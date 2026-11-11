"""Exercise real compiler cache hits/invalidation and admitted driver payloads."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

from artifact_cache import ArtifactCache, canonical, identity_key
from cached_pipeline_inputs import admit_pipeline, verified_pipeline_source
from cached_shader_inputs import compile_cached
from compile_shaders import CORPUS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('driver', 'validator', 'artifacts', 'sources', 'gpu-exe', 'pipeline-receipt', 'pipeline-data', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cache = ArtifactCache(output / 'cache')
    copied_sources = output / 'sources'
    shutil.copytree(args.sources, copied_sources)
    runs = {}

    def compile_run(label, kind, expected_compiles, sources=copied_sources, forbid=False):
        receipt = compile_cached(args.driver, args.validator, args.artifacts, sources, output / label, cache, kind, forbid)
        assert receipt['compiler_invocations'] == expected_compiles, (label, receipt['compiler_invocations'])
        runs[label] = receipt
        return receipt

    for kind in ('text', 'memory', 'module'):
        compile_run(kind + '-cold', kind, len(CORPUS))
        compile_run(kind + '-warm', kind, 0, forbid=True)
        for name, *_ in CORPUS:
            assert (output / (kind + '-warm') / (name + '.spv')).read_bytes() == (args.artifacts / (name + '.spv')).read_bytes()

    relocated = output / 'relocated'
    shutil.copytree(copied_sources, relocated)
    for path in relocated.glob('*.slang'):
        path.touch()
    compile_run('relocated-warm', 'text', 0, relocated, True)

    shared = copied_sources / 'shared_math.slang'
    original = shared.read_bytes()
    assert b'0x85ebca6bu' in original
    shared.write_bytes(original.replace(b'0x85ebca6bu', b'0x85ebca6du'))
    changed = compile_run('include-changed', 'text', sum(file != 'graphics_state.slang' for _, file, _, _ in CORPUS))
    assert changed['entries']['compute']['binary_sha256'] != runs['text-cold']['entries']['compute']['binary_sha256']
    assert all(changed['entries'][name]['status'] == 'hit' for name, file, _, _ in CORPUS if file == 'graphics_state.slang')
    shared.write_bytes(original)
    compile_run('include-restored', 'text', 0, forbid=True)

    target_identity = runs['text-cold']['entries']['compute']['identity']
    path = cache.path(target_identity)
    raw = path.read_bytes()
    path.write_bytes(raw[:-1] + bytes([raw[-1] ^ 1]))
    recovered = compile_run('target-corruption-repaired', 'text', 1)
    assert recovered['entries']['compute']['status'] == 'rebuilt_checksum'
    assert recovered['entries']['compute']['binary_sha256'] == runs['text-cold']['entries']['compute']['binary_sha256']
    cache.put(target_identity, b'{"spirv":"AA==","reflection":{}}')
    recovered = compile_run('target-invalid-format-repaired', 'text', 1)
    assert recovered['entries']['compute']['status'] == 'rebuilt_invalid_payload'

    # A checksum-valid payload must still obey its entry's capability contract.
    for target, replacement in [('state_vertex', 'state_parameters_vertex'), ('state_parameters_vertex', 'state_vertex')]:
        identity = runs['text-cold']['entries'][target]['identity']
        current, _ = cache.get(identity)
        data = json.loads(current)
        data['spirv'] = base64.b64encode((args.artifacts / (replacement + '.spv')).read_bytes()).decode('ascii')
        cache.put(identity, canonical(data))
        repaired = compile_run(target + '-capability-repaired', 'text', 1)
        assert repaired['entries'][target]['status'] == 'rebuilt_invalid_payload'
        assert repaired['entries'][target]['binary_sha256'] == runs['text-cold']['entries'][target]['binary_sha256']

    bounded = ArtifactCache(output / 'tiny-cache', max_entry_bytes=1)
    uncached = compile_cached(args.driver, args.validator, args.artifacts, copied_sources, output / 'capacity-fallback', bounded, 'text')
    assert uncached['compiler_invocations'] == len(CORPUS)
    assert all(entry['status'] == 'uncached_CacheCapacityError' for entry in uncached['entries'].values())
    runs['capacity-fallback'] = uncached
    held_root = output / 'busy-cache'
    busy = ArtifactCache(held_root, lock_timeout=0.05)
    process = subprocess.Popen([sys.executable, str(Path(__file__).with_name('test_artifact_cache.py')), '--worker', str(held_root), 'hold'])
    try:
        deadline = time.monotonic() + 5
        while not (held_root / 'held').exists() and time.monotonic() < deadline:
            time.sleep(0.01)
        assert (held_root / 'held').exists()
        uncached = compile_cached(args.driver, args.validator, args.artifacts, copied_sources, output / 'busy-fallback', busy, 'text')
        assert uncached['compiler_invocations'] == len(CORPUS)
        assert all(entry['status'] == 'uncached_TimeoutError' for entry in uncached['entries'].values())
        runs['busy-fallback'] = uncached
    finally:
        (held_root / 'release').touch()
        try:
            assert process.wait(timeout=5) == 0
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise

    # These are metadata invalidation checks, not claims of execution on another
    # compiler/device/driver. Actual changed include compilation is tested above.
    invalidations = []
    for field in target_identity:
        if field == 'kind':
            continue
        modified = dict(target_identity, **{field: {'changed': True}})
        assert cache.get(modified) == (None, 'missing')
        invalidations.append('target:' + field)

    receipt = json.loads(args.pipeline_receipt.read_bytes())
    assert hashlib.sha256(args.gpu_exe.read_bytes()).hexdigest() == receipt['binary_sha256']
    driver_data = args.pipeline_data.read_bytes()
    driver_identity = verified_pipeline_source(receipt, driver_data)
    valid = lambda data: admit_pipeline(data, driver_identity)
    payload, first_status = cache.get_or_create(driver_identity, lambda: driver_data, valid)
    assert payload == driver_data and first_status == 'rebuilt_missing'
    payload, second_status = cache.get_or_create(driver_identity, lambda: (_ for _ in ()).throw(AssertionError('warm driver cache rebuild')), valid)
    assert payload == driver_data and second_status == 'hit'
    for field in driver_identity:
        if field == 'kind':
            continue
        assert cache.get(dict(driver_identity, **{field: {'changed': True}})) == (None, 'missing')
        invalidations.append('pipeline:' + field)
    path = cache.path(driver_identity)
    raw = path.read_bytes()
    path.write_bytes(raw[:-1] + bytes([raw[-1] ^ 1]))
    payload, corrupt_status = cache.get_or_create(driver_identity, lambda: driver_data, valid)
    assert payload == driver_data and corrupt_status == 'rebuilt_checksum'
    # Check a checksum-valid envelope with an incompatible native header too.
    altered = bytearray(driver_data)
    altered[16] ^= 1
    cache.put(driver_identity, bytes(altered))
    payload, incompatible_status = cache.get_or_create(driver_identity, lambda: driver_data, valid)
    assert payload == driver_data and incompatible_status == 'rebuilt_invalid_payload'
    (output / 'admitted-pipeline.bin').write_bytes(payload)
    driver_summary = {'identity': driver_identity, 'key': identity_key(driver_identity), 'cold': first_status,
                      'warm': second_status, 'corrupt': corrupt_status, 'incompatible_header': incompatible_status,
                      'payload_sha256': hashlib.sha256(payload).hexdigest()}
    summary = {'status': 'passed', 'compiler_runs': {name: {'compiler_invocations': r['compiler_invocations'], 'elapsed_ms': r['elapsed_ms']} for name, r in runs.items()},
               'metadata_invalidation_checks': invalidations, 'driver_cache': driver_summary,
               'limits': 'Metadata mutations simulate invalidation, not another actual compiler/driver. Native GPU reload of admitted-pipeline.bin is a separate required run.'}
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': 'passed', 'compiler_scenarios': len(runs), 'metadata_invalidations': len(invalidations)}))


if __name__ == '__main__':
    main()
