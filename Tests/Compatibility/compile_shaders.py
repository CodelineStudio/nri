"""Compile and validate the standalone Slang corpus before embedding GPU inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('slangc', 'spirv-val', 'spirv-dis', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
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
    flags = ['-target', 'spirv', '-profile', 'spirv_1_3', '-emit-spirv-directly', '-matrix-layout-column-major', '-fvk-use-entrypoint-name', '-O2', '-g0']
    manifest['compiler_options'] = flags
    arrays = []
    for name, file, entry, stage in (('compute', 'compute.slang', 'computeMain', 'compute'), ('vertex', 'draw.slang', 'vertexMain', 'vertex'), ('fragment', 'draw.slang', 'fragmentMain', 'fragment')):
        binary = output / (name + '.spv')
        reflection = output / (name + '.reflection.json')
        assembly = output / (name + '.spvasm')
        subprocess.run([str(args.slangc.resolve()), str(source / 'shaders' / file), '-entry', entry, '-stage', stage, *flags, '-reflection-json', str(reflection), '-o', str(binary)], check=True, timeout=60)
        subprocess.run([str(args.spirv_val.resolve()), '--target-env', 'vulkan1.1', str(binary)], check=True, timeout=30)
        subprocess.run([str(args.spirv_dis.resolve()), str(binary), '-o', str(assembly)], check=True, timeout=30)
        raw = binary.read_bytes()
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
                capabilities.append(words[cursor + 1])
            if opcode == 10:
                raise RuntimeError('Baseline corpus must not declare a SPIR-V extension')
            cursor += size
        if not capabilities or set(capabilities) - {0, 1}:
            raise RuntimeError('Baseline shader requires an unqualified optional capability')
        reflected = json.loads(reflection.read_bytes())
        entrypoints = reflected['entryPoints']
        if len(entrypoints) != 1 or entrypoints[0]['name'] != entry or entrypoints[0]['stage'] != stage:
            raise RuntimeError('Unexpected shader entry point or stage')
        if stage == 'compute' and entrypoints[0].get('threadGroupSize') != [32, 1, 1]:
            raise RuntimeError('Unexpected compute workgroup size')
        if stage == 'vertex' and entrypoints[0]['parameters'][0]['binding'] != {'kind': 'varyingInput', 'index': 0}:
            raise RuntimeError('Unexpected vertex input location')
        parameters = {item['name']: item for item in reflected['parameters']}
        uniform = parameters['parameters']
        if uniform['binding']['index'] != 0 or uniform['binding'].get('space', 0) != 0:
            raise RuntimeError('Unexpected constant-buffer binding')
        fields = uniform['type']['elementType']['fields']
        offsets = {item['name']: (item['binding']['offset'], item['binding']['size']) for item in fields}
        if offsets != {'count': (0, 4), 'multiplier': (4, 4), 'bias': (8, 4), 'nonce': (12, 4), 'transform': (16, 64)}:
            raise RuntimeError('CPU/uniform layout mismatch')
        if stage != 'vertex' and (parameters['results']['binding']['index'] != 1 or parameters['results']['binding'].get('space', 0) != 0):
            raise RuntimeError('Unexpected storage-buffer binding')
        manifest['shaders'][name] = {'entry': entry, 'stage': stage, 'binary_sha256': digest(binary), 'reflection_sha256': digest(reflection), 'capabilities': capabilities, 'uniform_offsets': offsets, 'byte_count': len(raw)}
        arrays.append('inline constexpr uint32_t ' + name + '[] = {' + ','.join(hex(word) for word in words) + '};')
    encoded = json.dumps(manifest, sort_keys=True, separators=(',', ':'))
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    header = '#pragma once\n#include <cstdint>\nnamespace shader_fixture {\n' + '\n'.join(arrays) + '\ninline constexpr char manifest[] = R"manifest(' + encoded + ')manifest";\n}\n'
    (output / 'compiled_shaders.h').write_text(header, encoding='utf-8')
    print('Compiled and validated three SPIR-V1.3 shaders; uniform offsets and baseline capabilities verified.')


if __name__ == '__main__':
    main()
