import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

from artifact_cache import ArtifactCache, MAGIC, identity_key


IDENTITY = {'kind': 'target_shader', 'compiler': 'pinned', 'entry': 'compute', 'sources': {'main': 'abc', 'include': 'def'}, 'profile': 'spirv_1_3', 'layout': 'abi1'}


def worker(root, mode):
    cache = ArtifactCache(root)
    if mode == 'crash':
        cache.put(IDENTITY, b'new incomplete generation', before_replace=lambda _: os._exit(23))
    elif mode == 'create':
        def create():
            with (Path(root) / 'compilations.log').open('ab') as log:
                log.write(b'compiled\n')
            time.sleep(0.05)
            return b'one complete result'
        cache.get_or_create(IDENTITY, create)
    elif mode == 'hold':
        with cache._locked():
            (Path(root) / 'held').write_text('locked')
            deadline = time.monotonic() + 30
            while not (Path(root) / 'release').exists() and time.monotonic() < deadline:
                time.sleep(0.01)


class ArtifactCacheTests(unittest.TestCase):
    def setUp(self):
        parent = Path(os.environ.get('NRI_CACHE_TEST_ROOT', tempfile.gettempdir())).resolve()
        parent.mkdir(parents=True, exist_ok=True)
        self.root = parent / ('nri_cache_' + uuid.uuid4().hex)
        self.cache = ArtifactCache(self.root, max_bytes=100000, max_entry_bytes=80000)

    def process(self, mode):
        return subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--worker', str(self.root), mode], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def finish(self, process):
        try:
            output = process.communicate(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.communicate()
            self.fail('Owned cache worker timed out')
        return process.returncode, output

    def test_roundtrip_and_warm_factory_bypass(self):
        payload, status = self.cache.get_or_create(IDENTITY, lambda: b'compiled')
        self.assertEqual((payload, status), (b'compiled', 'rebuilt_missing'))
        self.assertEqual(self.cache.get_or_create(IDENTITY, lambda: self.fail('warm compile')), (b'compiled', 'hit'))
        reordered = dict(reversed(list(IDENTITY.items())))
        reordered['sources'] = dict(reversed(list(IDENTITY['sources'].items())))
        self.assertEqual(identity_key(reordered), identity_key(IDENTITY))
        self.assertEqual(self.cache.get(reordered), (b'compiled', 'hit'))

    def test_required_identity_changes_miss(self):
        self.cache.put(IDENTITY, b'good')
        for field in ('compiler', 'entry', 'sources', 'profile', 'layout'):
            changed = dict(IDENTITY, **{field: 'changed'})
            self.assertEqual(self.cache.get(changed), (None, 'missing'))
        driver = {'kind': 'driver_pipeline', 'vendor': 4318, 'device': 11141, 'driver': 123, 'uuid': 'native_uuid', 'profile': 'native', 'pipelines': 'states', 'shaders': 'targets'}
        self.cache.put(driver, b'driver data')
        for field in driver:
            if field == 'kind':
                continue
            self.assertEqual(self.cache.get(dict(driver, **{field: 'changed'})), (None, 'missing'))

    def test_corruption_truncation_and_repair(self):
        self.cache.put(IDENTITY, b'valid binary data')
        path = self.cache.path(IDENTITY)
        complete = path.read_bytes()
        for corrupted in (complete[:-1], complete + b'trailing', b'wrong', complete[:-1] + bytes([complete[-1] ^ 1]), MAGIC + struct.pack('<I', 0xffffffff)):
            path.write_bytes(corrupted)
            self.assertIsNone(self.cache.get(IDENTITY)[0])
            payload, reason = self.cache.get_or_create(IDENTITY, lambda: b'recovered')
            self.assertEqual(payload, b'recovered')
            self.assertTrue(reason.startswith('rebuilt_'))
        self.assertEqual(self.cache.get(IDENTITY), (b'recovered', 'hit'))

    def test_wrong_metadata_and_payload_never_accepted(self):
        self.cache.put(IDENTITY, b'payload')
        path = self.cache.path(IDENTITY)
        raw = path.read_bytes()
        length = struct.unpack_from('<I', raw, len(MAGIC))[0]
        header = json.loads(raw[len(MAGIC) + 4:len(MAGIC) + 4 + length])
        header['identity']['compiler'] = 'stale'
        encoded = json.dumps(header).encode()
        path.write_bytes(MAGIC + struct.pack('<I', len(encoded)) + encoded + b'payload')
        self.assertEqual(self.cache.get(IDENTITY), (None, 'incompatible'))

    def test_storage_bounds_and_unrelated_files(self):
        unrelated = self.root / 'user-note.txt'
        unrelated.write_bytes(b'untouched')
        for i in range(5):
            self.cache.put(dict(IDENTITY, variant=i), bytes([i]) * 40000)
        self.assertLessEqual(sum(p.stat().st_size for p in self.root.glob('*.cache')), 100000)
        self.assertEqual(self.cache.get(dict(IDENTITY, variant=4)), (bytes([4]) * 40000, 'hit'))
        self.assertEqual(unrelated.read_bytes(), b'untouched')
        with self.assertRaises(ValueError):
            self.cache.put(IDENTITY, b'x' * 80001)

    def test_interrupted_publication_preserves_previous_entry(self):
        self.cache.put(IDENTITY, b'old complete generation')
        code, _ = self.finish(self.process('crash'))
        self.assertEqual(code, 23)
        self.assertEqual(len(list(self.root.glob('pending_*.tmp'))), 1)
        self.assertEqual(self.cache.get(IDENTITY), (b'old complete generation', 'hit'))
        self.assertEqual(list(self.root.glob('pending_*.tmp')), [])

    def test_concurrent_cold_compilation_publishes_once(self):
        processes = [self.process('create') for _ in range(4)]
        for process in processes:
            code, output = self.finish(process)
            self.assertEqual(code, 0, output)
        self.assertEqual((self.root / 'compilations.log').read_bytes(), b'compiled\n')
        self.assertEqual(self.cache.get(IDENTITY), (b'one complete result', 'hit'))

    def test_lock_timeout_is_bounded_and_recoverable(self):
        process = self.process('hold')
        try:
            deadline = time.monotonic() + 5
            while not (self.root / 'held').exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue((self.root / 'held').exists())
            waiting = ArtifactCache(self.root, lock_timeout=0.05)
            with self.assertRaises(TimeoutError):
                waiting.get(IDENTITY)
        finally:
            (self.root / 'release').touch()
            code, output = self.finish(process)
            self.assertEqual(code, 0, output)
        self.assertEqual(self.cache.get(IDENTITY), (None, 'missing'))

    def test_checksum_valid_but_semantically_invalid_payload_rebuilds(self):
        self.cache.put(IDENTITY, b'wrong representation')
        self.assertEqual(self.cache.get_or_create(IDENTITY, lambda: b'valid', validate=lambda data: data == b'valid'), (b'valid', 'rebuilt_invalid_payload'))
        with self.assertRaises(ValueError):
            self.cache.get_or_create(dict(IDENTITY, variant='invalid'), lambda: b'bad', validate=lambda _: False)


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--worker':
        worker(sys.argv[2], sys.argv[3])
    else:
        unittest.main()
