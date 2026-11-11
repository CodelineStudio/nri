"""Compile and validate the standalone Slang corpus before embedding GPU inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


COMPILER_FLAGS = ['-target', 'spirv', '-profile', 'spirv_1_3', '-emit-spirv-directly', '-matrix-layout-column-major', '-fvk-use-entrypoint-name', '-O2', '-g0']

CORPUS = (
    ('compute', 'compute.slang', 'computeMain', 'compute'),
    ('vertex', 'draw.slang', 'vertexMain', 'vertex'),
    ('fragment', 'draw.slang', 'fragmentMain', 'fragment'),
    ('state_vertex', 'graphics_state.slang', 'stateVertexMain', 'vertex'),
    ('state_fragment', 'graphics_state.slang', 'stateFragmentMain', 'fragment'),
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def reflection_layout(path):
    reflected = json.loads(path.read_bytes())
    # CLI adds per-entry liveness from its compile request. ProgramLayout.toJson
    # does not emit that list; static parameter/entry layouts must still match.
    # Identical target binaries independently establish identical resource use.
    for entry in reflected['entryPoints']:
        entry.pop('bindings', None)
    return reflected


def validate_spirv(raw):
    if len(raw) < 20 or len(raw) % 4:
        raise ValueError('Malformed SPIR-V byte count')
    words = struct.unpack('<' + 'I' * (len(raw) // 4), raw)
    if words[:2] != (0x07230203, 0x00010300):
        raise RuntimeError('Expected SPIR-V 1.3 binary')
    capabilities = []
    cursor = 5
    while cursor < len(words):
        size, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        if size == 0 or cursor + size > len(words):
            raise RuntimeError('Malformed SPIR-V instruction')
        if opcode == 17:
            if size != 2:
                raise ValueError('Malformed SPIR-V capability')
            capabilities.append(words[cursor + 1])
        if opcode == 10:
            raise RuntimeError('Baseline corpus must not declare a SPIR-V extension')
        cursor += size
    if not capabilities or set(capabilities) - {0, 1}:
        raise RuntimeError('Baseline shader requires an unqualified optional capability')
    return words, capabilities


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('slangc', 'spirv-val', 'spirv-dis', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--input-driver', type=Path, help='Also qualify Slang API text/memory/serialized inputs; embed the serialized-module result')
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    lock = source / 'shader_toolchain.json'
    pins = json.loads(lock.read_bytes())
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = {'schema_version': 1, 'target': 'vulkan1.1', 'profile': 'spirv_1_3', 'tools': {}, 'shaders': {}}
    for name in ('slangc', 'spirv_val', 'spirv_dis'):
        path = getattr(args, name).resolve(strict=True)
        version = subprocess.check_output([str(path), '-version' if name == 'slangc' else '--version'], text=True, stderr=subprocess.STDOUT, timeout=30).strip()
        manifest['tools'][name] = {'sha256': digest(path), 'version': version}
        if name == 'slangc':
            if version != pins['slang']['version']:
                raise RuntimeError('This qualification corpus requires the pinned Slang version')
            manifest['tools'][name]['runtime_dlls'] = {dll.name: digest(dll) for dll in sorted(path.parent.glob('*.dll'))}
    manifest['sources'] = {p.name: digest(p) for p in sorted((source / 'shaders').glob('*.slang'))}
    manifest['recipe_sha256'] = digest(Path(__file__))
    manifest['toolchain_lock_sha256'] = digest(lock)
    flags = COMPILER_FLAGS
    manifest['compiler_options'] = flags
    input_runs = {}
    if args.input_driver:
        driver = args.input_driver.resolve(strict=True)
        manifest['input_qualification'] = {'driver_sha256': digest(driver), 'runtime_dlls': {dll.name: digest(dll) for dll in sorted(driver.parent.glob('slang*.dll'))}, 'embedded_input': 'serialized_module', 'entries': {}}
        if driver.suffix.lower() == '.exe':
            for runtime in ('slang-compiler.dll', 'slang-glslang.dll'):
                if manifest['input_qualification']['runtime_dlls'].get(runtime) != manifest['tools']['slangc']['runtime_dlls'].get(runtime):
                    raise RuntimeError('API runtime does not match the pinned CLI package: ' + runtime)
    arrays = []
    for name, file, entry, stage in CORPUS:
        binary = output / (name + '.spv')
        reflection = output / (name + '.reflection.json')
        assembly = output / (name + '.spvasm')
        subprocess.run([str(args.slangc.resolve()), str(source / 'shaders' / file), '-entry', entry, '-stage', stage, *flags, '-reflection-json', str(reflection), '-o', str(binary)], check=True, timeout=60)
        if args.input_driver:
            runs = {}
            module_dir = output / 'inputs' / 'modules'
            for kind in ('text', 'memory', 'module'):
                prefix = output / 'inputs' / kind / name
                input_path = module_dir / (Path(file).stem + '.slang-module') if kind == 'module' else source / 'shaders' / file
                command = [str(driver), kind, str(input_path), entry, stage, str(prefix), str(module_dir), 'spirv_1_3']
                result = subprocess.run(command, capture_output=True, text=True, timeout=60)
                if result.returncode != 0 or result.stderr:
                    raise RuntimeError(f'Slang API {kind}/{name} failed: {result.stderr}')
                runs[kind] = json.loads(result.stdout)
                api_binary = prefix.with_suffix('.spv')
                api_reflection = prefix.with_suffix('.reflection.json')
                subprocess.run([str(args.spirv_val.resolve()), '--target-env', 'vulkan1.1', str(api_binary)], check=True, timeout=30)
                if api_binary.read_bytes() != binary.read_bytes():
                    raise RuntimeError(f'Slang API {kind}/{name} binary differs from pinned CLI output')
                if reflection_layout(api_reflection) != reflection_layout(reflection):
                    raise RuntimeError(f'Slang API {kind}/{name} reflection differs from pinned CLI output')
            if file != 'graphics_state.slang' and runs['module']['module_reads'] < 1:
                raise RuntimeError('Imported shared module was not loaded from the deployed artifact')
            # Embed the independently loaded module result, not the comparison CLI binary.
            binary.write_bytes(api_binary.read_bytes())
            reflection.write_bytes(api_reflection.read_bytes())
            input_runs[name] = runs
            manifest['input_qualification']['entries'][name] = {'binary_sha256': digest(binary), 'reflection_sha256': digest(reflection), 'input_kinds': ['text', 'memory', 'module'], 'loaded_modules': runs['module']['loaded_modules'], 'module_reads': runs['module']['module_reads'], 'denied_reads': runs['module']['denied_reads']}
        subprocess.run([str(args.spirv_val.resolve()), '--target-env', 'vulkan1.1', str(binary)], check=True, timeout=30)
        subprocess.run([str(args.spirv_dis.resolve()), str(binary), '-o', str(assembly)], check=True, timeout=30)
        raw = binary.read_bytes()
        words, capabilities = validate_spirv(raw)
        reflected = json.loads(reflection.read_bytes())
        entrypoints = reflected['entryPoints']
        if len(entrypoints) != 1 or entrypoints[0]['name'] != entry or entrypoints[0]['stage'] != stage:
            raise RuntimeError('Unexpected shader entry point or stage')
        if stage == 'compute' and entrypoints[0].get('threadGroupSize') != [32, 1, 1]:
            raise RuntimeError('Unexpected compute workgroup size')
        if stage == 'vertex' and entrypoints[0]['parameters'][0]['binding'] != {'kind': 'varyingInput', 'index': 0}:
            raise RuntimeError('Unexpected vertex input location')
        parameters = {item['name']: item for item in reflected['parameters']}
        state = name.startswith('state_')
        uniform = parameters['drawParameters' if state else 'parameters']
        kind = 'pushConstantBuffer' if state else 'descriptorTableSlot'
        if uniform['binding']['kind'] != kind or uniform['binding']['index'] != 0 or uniform['binding'].get('space', 0) != 0:
            raise RuntimeError('Unexpected parameter binding')
        fields = uniform['type']['elementType']['fields']
        offsets = {item['name']: (item['binding']['offset'], item['binding']['size']) for item in fields}
        expected = {'depth': (0, 4), 'opacity': (4, 4), 'flags': (8, 4), 'padding': (12, 4)} if state else {'count': (0, 4), 'multiplier': (4, 4), 'bias': (8, 4), 'nonce': (12, 4), 'transform': (16, 64)}
        if offsets != expected:
            raise RuntimeError('CPU/uniform layout mismatch')
        resources = {'image': 0, 'imageSampler': 1} if state else {'results': 1}
        if stage != 'vertex':
            for resource, binding in resources.items():
                if parameters[resource]['binding']['index'] != binding or parameters[resource]['binding'].get('space', 0) != 0:
                    raise RuntimeError('Unexpected resource binding')
        if state and stage == 'vertex' and [item['binding']['index'] for item in entrypoints[0]['parameters']] != [0, 1, 2]:
            raise RuntimeError('Unexpected vertex/instance input locations')
        manifest['shaders'][name] = {'entry': entry, 'stage': stage, 'binary_sha256': digest(binary), 'reflection_sha256': digest(reflection), 'capabilities': capabilities, 'uniform_offsets': offsets, 'byte_count': len(raw)}
        arrays.append('inline constexpr uint32_t ' + name + '[] = {' + ','.join(hex(word) for word in words) + '};')
    encoded = json.dumps(manifest, sort_keys=True, separators=(',', ':'))
    if args.input_driver:
        (output / 'input-runs.json').write_text(json.dumps(input_runs, indent=2) + '\n', encoding='utf-8')
        modules = {p.name: digest(p) for p in sorted((output / 'inputs' / 'modules').glob('*.slang-module'))}
        (output / 'module-artifacts.json').write_text(json.dumps(modules, indent=2) + '\n', encoding='utf-8')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    header = '#pragma once\n#include <cstdint>\nnamespace shader_fixture {\n' + '\n'.join(arrays) + '\ninline constexpr char manifest[] = R"manifest(' + encoded + ')manifest";\n}\n'
    (output / 'compiled_shaders.h').write_text(header, encoding='utf-8')
    print('Compiled and validated five SPIR-V1.3 shaders; parameter offsets and baseline capabilities verified.')


if __name__ == '__main__':
    main()
