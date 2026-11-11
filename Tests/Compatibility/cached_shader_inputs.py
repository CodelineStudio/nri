"""Compile the standalone corpus through persistent compatible target entries."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import time

from artifact_cache import ArtifactCache, CacheCapacityError, canonical, identity_key
from compile_shaders import COMPILER_FLAGS, CORPUS, digest, reflection_layout, validate_spirv


def compile_cached(driver, validator, artifacts, sources, output, cache, kind, forbid_compiler=False):
    started = time.perf_counter()
    driver, validator, artifacts, sources, output = (Path(p).resolve() for p in (driver, validator, artifacts, sources, output))
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((artifacts / 'manifest.json').read_bytes())
    if manifest['profile'] != 'spirv_1_3' or manifest['target'] != 'vulkan1.1' or manifest['compiler_options'] != COMPILER_FLAGS:
        raise ValueError('Artifact package does not describe the qualified compiler recipe')
    producer = manifest['input_qualification']
    if digest(driver) != producer['driver_sha256'] or digest(validator) != manifest['tools']['spirv_val']['sha256']:
        raise ValueError('Compiler driver or validator differs from the qualified package')
    for name, checksum in producer['runtime_dlls'].items():
        if digest(driver.parent / name) != checksum:
            raise ValueError('Compiler runtime differs from the qualified package')
    module_dir = artifacts / 'inputs' / 'modules'
    module_hashes = json.loads((artifacts / 'module-artifacts.json').read_bytes())
    if kind == 'module':
        for name, checksum in module_hashes.items():
            if digest(module_dir / name) != checksum:
                raise ValueError('Serialized module package is incomplete or changed')
    records = {}
    compiled = 0
    for name, file, entry, stage in CORPUS:
        if digest(artifacts / (name + '.reflection.json')) != manifest['shaders'][name]['reflection_sha256']:
            raise ValueError('Reference layout metadata changed')
        expected_layout = reflection_layout(artifacts / (name + '.reflection.json'))
        if kind == 'module':
            names = [Path(file).stem + '.slang-module']
            if file != 'graphics_state.slang':
                names.append('shared_math.slang-module')
            contents = {item: (module_dir / item).read_bytes() for item in names}
            inputs = {item: hashlib.sha256(data).hexdigest() for item, data in contents.items()}
            if any(inputs[item] != module_hashes[item] for item in names):
                raise ValueError('Serialized module changed while preparing input')
        else:
            names = [file] if file == 'graphics_state.slang' else [file, 'common.slang', 'shared_math.slang']
            contents = {item: (sources / item).read_bytes() for item in names}
            inputs = {item: hashlib.sha256(data).hexdigest() for item, data in contents.items()}
        identity = {'kind': 'target_shader', 'input_kind': kind, 'entry': entry, 'stage': stage,
                    'inputs': inputs, 'compiler': manifest['tools']['slangc']['version'],
                    'compiler_driver': producer['driver_sha256'], 'runtime': producer['runtime_dlls'],
                    'profile': manifest['profile'], 'environment': manifest['target'], 'capabilities': [0, 1],
                    'options': manifest['compiler_options'], 'defines': {}, 'specialization': {},
                    'reflection_abi': hashlib.sha256(canonical(expected_layout)).hexdigest(),
                    'validator': manifest['tools']['spirv_val']['sha256'], 'recipe_schema': 1,
                    'recipe_sha256': digest(Path(__file__)), 'validation_recipe_sha256': digest(Path(__file__).with_name('compile_shaders.py'))}

        def decode(payload):
            data = json.loads(payload)
            binary = base64.b64decode(data['spirv'], validate=True)
            validate_spirv(binary)
            if data['reflection'] != expected_layout:
                raise ValueError('Cached reflection ABI differs')
            return binary, data['reflection']

        def valid(payload):
            try:
                decode(payload)
                return True
            except (ValueError, KeyError, TypeError, IndexError, RuntimeError):
                return False

        generated = None

        def create():
            nonlocal compiled, generated
            if forbid_compiler:
                raise RuntimeError('Warm-only run unexpectedly requested compilation: ' + name)
            prefix = output / 'compile' / name
            # Compile the exact captured bytes used by the key, even if a live
            # source/include is edited while the compiler is starting.
            snapshot = output / 'input-snapshots' / name
            snapshot.mkdir(parents=True, exist_ok=True)
            for item, data in contents.items():
                (snapshot / item).write_bytes(data)
            input_path = snapshot / (Path(file).stem + '.slang-module' if kind == 'module' else file)
            serialization = snapshot if kind == 'module' else output / 'serialized'
            result = subprocess.run([str(driver), kind, str(input_path), entry, stage, str(prefix), str(serialization), manifest['profile']], capture_output=True, text=True, timeout=60)
            if result.returncode or result.stderr:
                raise RuntimeError('Compilation failed: ' + result.stderr)
            if digest(driver) != producer['driver_sha256'] or any(digest(driver.parent / item) != checksum for item, checksum in producer['runtime_dlls'].items()):
                raise ValueError('Compiler package changed during compilation')
            compiled += 1
            binary = prefix.with_suffix('.spv')
            subprocess.run([str(validator), '--target-env', manifest['target'], str(binary)], check=True, timeout=30, capture_output=True)
            layout = reflection_layout(prefix.with_suffix('.reflection.json'))
            generated = canonical({'spirv': base64.b64encode(binary.read_bytes()).decode('ascii'), 'reflection': layout})
            return generated

        begin = time.perf_counter()
        try:
            payload, status = cache.get_or_create(identity, create, valid)
        except (OSError, CacheCapacityError) as error:
            # Cache availability must not prevent compiling valid input. Reuse
            # work already completed before a publication/storage failure.
            payload = generated if generated is not None else create()
            status = 'uncached_' + type(error).__name__
        binary, layout = decode(payload)
        (output / (name + '.spv')).write_bytes(binary)
        (output / (name + '.reflection.json')).write_bytes(canonical(layout))
        records[name] = {'status': status, 'key': identity_key(identity), 'identity': identity,
                         'binary_sha256': hashlib.sha256(binary).hexdigest(), 'resolve_ms': (time.perf_counter() - begin) * 1000}
    receipt = {'kind': kind, 'compiler_invocations': compiled, 'elapsed_ms': (time.perf_counter() - started) * 1000, 'entries': records}
    (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('driver', 'validator', 'artifacts', 'sources', 'output', 'cache'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--kind', choices=('text', 'memory', 'module'), required=True)
    parser.add_argument('--forbid-compiler', action='store_true')
    args = parser.parse_args()
    receipt = compile_cached(args.driver, args.validator, args.artifacts, args.sources, args.output,
                             ArtifactCache(args.cache), args.kind, args.forbid_compiler)
    print(json.dumps({key: receipt[key] for key in ('kind', 'compiler_invocations', 'elapsed_ms')}))


if __name__ == '__main__':
    main()
