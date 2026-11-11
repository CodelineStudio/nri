"""Qualify deployed module rejection and isolated compiler-runtime packaging."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--driver', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    driver = args.driver.resolve(strict=True)
    artifacts = args.artifacts.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    modules = artifacts / 'inputs' / 'modules'
    source = Path(__file__).resolve().parent / 'shaders'
    cases = {}

    def run(name, executable, kind, input_path, package, entry='computeMain', expected_error=None, isolated=False):
        prefix = output / name / 'compute'
        prefix.parent.mkdir(parents=True, exist_ok=True)
        command = [str(executable), kind, str(input_path), entry, 'compute', str(prefix), str(package), 'spirv_1_3']
        environment = dict(os.environ)
        if isolated:
            environment['PATH'] = str(Path(environment['SYSTEMROOT']) / 'System32')
        result = subprocess.run(command, capture_output=True, text=True, timeout=60, cwd=prefix.parent, env=environment)
        (prefix.parent / 'stdout.log').write_text(result.stdout, encoding='utf-8')
        (prefix.parent / 'stderr.log').write_text(result.stderr, encoding='utf-8')
        if expected_error:
            if result.returncode != 1 or expected_error not in result.stderr or prefix.with_suffix('.spv').exists():
                raise RuntimeError(f'{name} did not reject its invalid input cleanly')
        else:
            if result.returncode or result.stderr:
                raise RuntimeError(f'{name} failed: {result.stderr}')
            if prefix.with_suffix('.spv').read_bytes() != (artifacts / 'compute.spv').read_bytes():
                raise RuntimeError(f'{name} generated different GPU code')
        cases[name] = {'exit_code': result.returncode, 'expected_error': expected_error, 'command': command}
        if not expected_error:
            cases[name]['compiler_run'] = json.loads(result.stdout)

    # Relocate only serialized artifacts; the driver filesystem rejects source
    # and all files outside this directory, even if original paths remain in IR.
    deployed = output / 'deployed-modules'
    shutil.copytree(modules, deployed)
    assert all(p.suffix == '.slang-module' for p in deployed.iterdir())
    run('module-only', driver, 'module', deployed / 'compute.slang-module', deployed)
    missing = output / 'missing-import'
    missing.mkdir()
    shutil.copyfile(deployed / 'compute.slang-module', missing / 'compute.slang-module')
    run('missing-import-rejected', driver, 'module', missing / 'compute.slang-module', missing, expected_error='module load')
    corrupt = output / 'corrupt-module'
    corrupt.mkdir()
    (corrupt / 'compute.slang-module').write_bytes(b'not a Slang module\0')
    run('corrupt-module-rejected', driver, 'module', corrupt / 'compute.slang-module', corrupt, expected_error='module load')
    run('missing-entry-rejected', driver, 'module', deployed / 'compute.slang-module', deployed, entry='missingMain', expected_error='entry point')

    if driver.suffix.lower() == '.exe':
        # The optimizer DLL is optional to the API, but required by this pinned
        # recipe. Its diagnostic must prevent silently accepting another route.
        incomplete = output / 'incomplete-runtime'
        incomplete.mkdir()
        executable = incomplete / driver.name
        shutil.copyfile(driver, executable)
        shutil.copyfile(driver.parent / 'slang-compiler.dll', incomplete / 'slang-compiler.dll')
        run('missing-optimizer-rejected', executable, 'text', source / 'compute.slang', output / 'incomplete-modules', expected_error='code generation', isolated=True)
        shutil.copyfile(driver.parent / 'slang-glslang.dll', incomplete / 'slang-glslang.dll')
        run('packaged-runtime', executable, 'module', deployed / 'compute.slang-module', deployed, isolated=True)

    manifest = {'status': 'passed', 'driver_sha256': hashlib.sha256(driver.read_bytes()).hexdigest(), 'cases': cases}
    (output / 'summary.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'status': 'passed', 'cases': len(cases)}))


if __name__ == '__main__':
    main()
