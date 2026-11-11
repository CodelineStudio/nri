"""Bounded, checksummed fixture cache; independent of compiler and GPU lifetimes.

This is a standalone qualification implementation, not an NRI public API or the
consumer engine's shipping cache. Callers own complete compatibility identities.
"""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import time
import uuid


MAGIC = b'NRICACHE1'
MAX_HEADER = 64 * 1024
ENTRY_NAME = re.compile(r'[0-9a-f]{64}\.cache\Z')
PENDING_NAME = re.compile(r'pending_[0-9a-f]{32}\.tmp\Z')


class CacheCapacityError(ValueError):
    pass


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False).encode('utf-8')


def identity_key(identity):
    if not isinstance(identity, dict) or identity.get('kind') not in ('target_shader', 'driver_pipeline'):
        raise ValueError('Cache identity must explicitly select target_shader or driver_pipeline')
    return hashlib.sha256(canonical({'schema': 1, 'identity': identity})).hexdigest()


class ArtifactCache:
    def __init__(self, root, max_bytes=256 * 1024 * 1024, max_entry_bytes=128 * 1024 * 1024, lock_timeout=10):
        if max_bytes <= MAX_HEADER or max_entry_bytes <= 0 or not 0 < lock_timeout <= 60:
            raise ValueError('Invalid cache limits')
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.max_bytes = max_bytes
        self.max_entry_bytes = min(max_entry_bytes, max_bytes)
        self.lock_timeout = lock_timeout

    @contextmanager
    def _locked(self):
        # Never delete the lock file: another process may already hold its
        # handle. A process exit releases the OS lock, including interrupted IO.
        with (self.root / 'cache.lock').open('a+b') as lock:
            if os.fstat(lock.fileno()).st_size == 0:
                lock.write(b'\0')
                lock.flush()
            deadline = time.monotonic() + self.lock_timeout
            while True:
                lock.seek(0)
                try:
                    if os.name == 'nt':
                        import msvcrt
                        msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                    else:
                        import fcntl
                        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise TimeoutError('Cache lock timed out')
                    time.sleep(0.02)
            try:
                # No live writer can own a pending file while this lock is held.
                for path in self.root.iterdir():
                    if PENDING_NAME.fullmatch(path.name) and path.is_file() and not path.is_symlink():
                        path.unlink()
                yield
            finally:
                lock.seek(0)
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(lock.fileno(), fcntl.LOCK_UN)

    def path(self, identity):
        return self.root / (identity_key(identity) + '.cache')

    def _read(self, identity):
        path = self.path(identity)
        if path.is_symlink():
            return None, 'invalid_file'
        try:
            with path.open('rb') as file:
                size = os.fstat(file.fileno()).st_size
                prefix = file.read(len(MAGIC) + 4)
                if len(prefix) != len(MAGIC) + 4 or not prefix.startswith(MAGIC):
                    return None, 'format'
                header_size = struct.unpack_from('<I', prefix, len(MAGIC))[0]
                if not 0 < header_size <= MAX_HEADER or size > self.max_entry_bytes + MAX_HEADER + len(prefix):
                    return None, 'bounds'
                header = json.loads(file.read(header_size))
                if not isinstance(header, dict) or set(header) != {'schema', 'key', 'identity', 'payload_size', 'payload_sha256'}:
                    return None, 'metadata'
                if type(header['schema']) is not int or header['schema'] != 1 or header['key'] != identity_key(identity) or canonical(header['identity']) != canonical(identity):
                    return None, 'incompatible'
                payload_size = header['payload_size']
                if type(payload_size) is not int or not 0 <= payload_size <= self.max_entry_bytes or size != len(prefix) + header_size + payload_size:
                    return None, 'bounds'
                payload = file.read(payload_size)
                if len(payload) != payload_size or hashlib.sha256(payload).hexdigest() != header['payload_sha256']:
                    return None, 'checksum'
            os.utime(path, None)  # eviction only; timestamps never enter identity
            return payload, 'hit'
        except FileNotFoundError:
            return None, 'missing'
        except (ValueError, UnicodeError, TypeError, KeyError, RecursionError):
            return None, 'format'

    def get(self, identity):
        with self._locked():
            return self._read(identity)

    def _publish(self, identity, payload, before_replace=None):
        if not isinstance(payload, bytes):
            raise ValueError('Cache payload is not bytes')
        if len(payload) > self.max_entry_bytes:
            raise CacheCapacityError('Cache payload exceeds entry limit')
        header = canonical({'schema': 1, 'key': identity_key(identity), 'identity': identity,
                            'payload_size': len(payload), 'payload_sha256': hashlib.sha256(payload).hexdigest()})
        total = len(MAGIC) + 4 + len(header) + len(payload)
        if len(header) > MAX_HEADER or total > self.max_bytes:
            raise CacheCapacityError('Cache entry exceeds storage limit')
        path = self.path(identity)
        temporary = self.root / ('pending_' + uuid.uuid4().hex + '.tmp')
        try:
            with temporary.open('xb') as file:
                file.write(MAGIC + struct.pack('<I', len(header)))
                file.write(header)
                file.write(payload)
                file.flush()
                os.fsync(file.fileno())
            if before_replace:
                before_replace(temporary)  # bounded crash-injection hook for tests
            os.replace(temporary, path)
            self._prune(path)
        finally:
            if temporary.exists():
                temporary.unlink()

    def _prune(self, retained):
        entries = []
        for path in self.root.iterdir():
            if ENTRY_NAME.fullmatch(path.name) and path.is_file() and not path.is_symlink():
                info = path.stat()
                entries.append((info.st_mtime_ns, path.name, path, info.st_size))
        total = sum(entry[3] for entry in entries)
        for _, _, path, size in sorted(entries):
            if total <= self.max_bytes:
                break
            if path != retained:
                path.unlink()
                total -= size

    def put(self, identity, payload, before_replace=None):
        with self._locked():
            self._publish(identity, payload, before_replace)

    def get_or_create(self, identity, factory, validate=None):
        # One compile per key even with concurrent cold starts. This fixture
        # serializes all cache writers/compilers; shipping scheduling can refine
        # lock granularity while retaining these publication/invalidation rules.
        with self._locked():
            payload, reason = self._read(identity)
            if payload is not None:
                if validate is None or validate(payload):
                    return payload, 'hit'
                reason = 'invalid_payload'
            payload = factory()
            if validate is not None and not validate(payload):
                raise ValueError('Generated artifact fails admission checks')
            self._publish(identity, payload)
            return payload, 'rebuilt_' + reason
