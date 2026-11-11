"""Bound the standalone fixture process and preserve stdout, stderr and receipt."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import struct
import sys
import time
from capability_manifest import parse_manifest, require_individual_features


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--fixture", choices=("nri_vulkan_buffer_baseline", "nri_vulkan_fence_sequences", "nri_vulkan_query_reset", "nri_vulkan_presentation", "nri_vulkan_shader_execution", "nri_vulkan_memory_lifetime", "nri_vulkan_wrapped_memory"), default="nri_vulkan_buffer_baseline")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--adapter", type=int, default=0)
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--require-native-validation", action="store_true")
    parser.add_argument("--require-legacy-sync", action="store_true")
    parser.add_argument("--require-no-indirect-optionals", action="store_true")
    parser.add_argument("--require-no-indirect-first-instance", action="store_true")
    parser.add_argument("--require-no-shader-draw-parameters", action="store_true")
    parser.add_argument("--require-no-timeline", action="store_true")
    parser.add_argument("--require-no-host-reset", action="store_true")
    parser.add_argument("--require-legacy-wsi", action="store_true")
    parser.add_argument("--require-legacy-surface", action="store_true")
    parser.add_argument("--require-present-retirement", action="store_true")
    parser.add_argument("--require-shader-inputs", action="store_true")
    parser.add_argument("--cache-input", type=Path)
    parser.add_argument("--cache-output", type=Path)
    parser.add_argument("--require-pipeline-cache-hit", action="store_true")
    parser.add_argument("--require-individual-features", action="store_true")
    parser.add_argument("--require-basic-optionals", action="store_true")
    parser.add_argument("--require-renderpass1", action="store_true")
    parser.add_argument("--require-renderpass2", action="store_true")
    parser.add_argument("--require-api-minor", type=int, choices=(1, 2, 3, 4))
    parser.add_argument('--compute-input', type=Path)
    parser.add_argument('--compute-entry', default='computeMain')
    parser.add_argument('--compute-xor', type=int, default=0)
    parser.add_argument('--compute-validator', type=Path)
    parser.add_argument("--require-memory-failures", action="store_true")
    parser.add_argument("--require-mapped-ranges", action="store_true")
    args = parser.parse_args()
    if args.require_mapped_ranges and args.fixture not in ("nri_vulkan_memory_lifetime", "nri_vulkan_wrapped_memory"):
        parser.error("Mapped range requirements need the memory lifetime fixture")
    if args.require_memory_failures and args.fixture != "nri_vulkan_memory_lifetime":
        parser.error("Memory failure requirements need the memory lifetime fixture")
    if args.compute_input and (args.fixture != 'nri_vulkan_shader_execution' or not args.compute_validator):
        parser.error('External compute input requires shader fixture and SPIR-V validator')
    if not 0 <= args.compute_xor <= 0xffffffff or (args.compute_xor and not args.compute_input):
        parser.error('Compute xor must be uint32 and requires an external shader')
    if (args.cache_input or args.cache_output or args.require_pipeline_cache_hit) and args.fixture != 'nri_vulkan_shader_execution':
        parser.error('Pipeline cache arguments require the shader fixture')
    if not 0 < args.timeout <= 600 or args.adapter < 0:
        parser.error("timeout must be in (0,600] seconds and adapter must be nonnegative")
    exe = args.exe.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    command = [str(exe), "--adapter", str(args.adapter)]
    if args.validation or args.require_native_validation:
        command.append("--validation")
    if args.cache_input:
        command += ['--cache-input', str(args.cache_input.resolve(strict=True))]
    if args.cache_output:
        args.cache_output.parent.mkdir(parents=True, exist_ok=True)
        command += ['--cache-output', str(args.cache_output.resolve())]
    if args.require_pipeline_cache_hit:
        command.append('--cache-hit-only')
    compute_identity = None
    if args.compute_input:
        from compile_shaders import validate_spirv
        raw = args.compute_input.read_bytes()
        validate_spirv(raw)
        captured = args.output.resolve() / 'runtime-compute.spv'
        captured.write_bytes(raw)
        validator = args.compute_validator.resolve(strict=True)
        subprocess.run([str(validator), '--target-env', 'vulkan1.1', str(captured)], check=True, capture_output=True, timeout=30)
        command += ['--compute-input', str(captured), '--compute-entry', args.compute_entry, '--compute-xor', str(args.compute_xor)]
        compute_identity = {'sha256': hashlib.sha256(raw).hexdigest(), 'validator_sha256': hashlib.sha256(validator.read_bytes()).hexdigest(), 'entry': args.compute_entry, 'xor': args.compute_xor}
    started = time.monotonic()
    receipt = {
        "schema_version": 1,
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "command": command,
        "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
        "platform": platform.platform(),
        "timeout_seconds": args.timeout,
        "timed_out": False,
        "status": "failed",
    }
    receipt["runtime_compute"] = compute_identity
    stdout = b""
    stderr = b""
    try:
        # The fixture does not spawn children. subprocess.run kills and reaps only
        # this owned process on timeout, including hangs during GPU teardown.
        process = subprocess.run(command, capture_output=True, timeout=args.timeout, check=False)
        stdout, stderr = process.stdout, process.stderr
        receipt["exit_code"] = process.returncode
        candidates = []
        for line in stdout.decode("utf-8", errors="replace").splitlines():
            try:
                candidate = json.loads(line)
                if isinstance(candidate, dict) and candidate.get("fixture") == args.fixture:
                    candidates.append(candidate)
            except json.JSONDecodeError:
                pass
        if len(candidates) == 1:
            receipt["fixture"] = candidates[0]
            if process.returncode == 0 and candidates[0].get("status") == "passed":
                receipt["status"] = "passed"
        else:
            receipt["failure"] = "Expected exactly one structured fixture result"
        if receipt["status"] == "passed":
            try:
                manifest = parse_manifest(stderr.decode("utf-8", errors="replace"))
                receipt["capability_manifest"] = manifest
                receipt["fixture"]["api_versions_and_enabled_extensions"] = manifest
                if args.fixture == 'nri_vulkan_wrapped_memory':
                    wrapped = receipt['fixture']
                    if any(wrapped.get(k) != v for k, v in {'exact_cycles': 24, 'provided_mapping_cases': 8, 'nri_mapping_cases': 8, 'unknown_extent_cases': 8}.items()):
                        raise ValueError('Wrapped mapping cases incomplete')
                    if wrapped.get('address_allocation') != bool(manifest['device']['device_address']):
                        raise ValueError('Native allocation flags disagree with enabled address feature')
                    cases = [json.loads(line.split('NRI_WRAPPED_CASE ', 1)[1]) for line in stdout.decode('utf-8').splitlines() if line.startswith('NRI_WRAPPED_CASE ')]
                    if len(cases) != 24 or any(c['cycle'] != i or c['mode'] != i % 3 or not c['memory_flags'] & 2 for i, c in enumerate(cases)):
                        raise ValueError('Wrapped native memory manifest incomplete')
                    if wrapped.get('native_noncoherent_cases') != sum(not c['memory_flags'] & 4 for c in cases):
                        raise ValueError('Native noncoherent coverage counter mismatch')
                    receipt['wrapped_memory_cases'] = cases
                    if args.require_mapped_ranges and not wrapped.get('mapped_ranges_forced'):
                        raise ValueError('Wrapped range instrumentation inactive')
                    if wrapped.get('mapped_ranges_forced'):
                        ranges = [json.loads(line.split('NRI_MAPPED_RANGE ', 1)[1]) for line in stderr.decode('utf-8').splitlines() if line.startswith('NRI_MAPPED_RANGE ')]
                        if len(ranges) != 192:
                            raise ValueError('Wrapped native mapped call coverage incomplete')
                        for i, item in enumerate(ranges):
                            case = cases[i // 8]
                            base = case['mapping_offset'] + case['buffer_offsets'][(i % 8) // 4]
                            offset, size = (0, 260) if i % 4 < 2 else (3, 125)
                            mapped_end = 0 if case['mode'] == 2 else case['mapping_offset'] + case['mapping_size']
                            if any(item[k] != v for k, v in {'memory_offset': base, 'buffer_size': 260, 'mapped_end': mapped_end, 'request_offset': offset, 'request_size': size, 'atom_size': case['atom_size']}.items()):
                                raise ValueError('Wrapped mapping/binding metadata mismatch')
                            atom = case['atom_size']
                            start = ((base + offset) // atom) * atom
                            end = ((base + offset + size + atom - 1) // atom) * atom
                            expected_size = end - start if end <= (mapped_end or base + 260) else 0xffffffffffffffff
                            if item['offset'] != start or item['size'] != expected_size or start < case['mapping_offset']:
                                raise ValueError('Wrapped range escaped its mapping or included unnecessary atoms')
                        receipt['wrapped_ranges'] = {'calls': len(ranges), 'known_extent_calls': 128, 'unknown_extent_calls': 64, 'whole_size_calls': sum(r['size'] == 0xffffffffffffffff for r in ranges)}
                if args.fixture == 'nri_vulkan_memory_lifetime':
                    memory = receipt['fixture']
                    expected = 3 if memory.get('memory_failures') else 0
                    if memory.get('exact_cycles') != 64 or memory.get('recoveries') != expected or memory.get('expected_errors') != expected:
                        raise ValueError('Memory retirement/readback or failure recovery incomplete')
                    if args.require_memory_failures and not (memory.get('forced_dedicated') and memory.get('memory_failures')):
                        raise ValueError('Memory failure profile not active')
                    if args.require_mapped_ranges and not memory.get('mapped_ranges_forced'):
                        raise ValueError('Explicit mapped range instrumentation is not active')
                    if memory.get('mapped_ranges_forced'):
                        ranges = [json.loads(line.split('NRI_MAPPED_RANGE ', 1)[1]) for line in stderr.decode('utf-8').splitlines() if line.startswith('NRI_MAPPED_RANGE ')]
                        expected_nonzero = 0 if memory.get('forced_dedicated') else 128
                        if len(ranges) != 768 or memory.get('nonzero_bindings') != expected_nonzero:
                            raise ValueError('Padded allocation/offset/range coverage incomplete')
                        for i, item in enumerate(ranges):
                            expected_request = (0, 1024) if i % 6 < 4 else (5, 500)
                            if (item['request_offset'], item['request_size']) != expected_request or (expected_nonzero and item['memory_offset'] == 0) or item['buffer_size'] != 1024:
                                raise ValueError('Unexpected mapped range sequence')
                            atom = item['atom_size']
                            begin = item['memory_offset'] + item['request_offset']
                            end = begin + item['request_size']
                            start_atom = begin // atom
                            end_atom = (end + atom - 1) // atom
                            expected_offset = start_atom * atom
                            expected_size = (end_atom - start_atom) * atom if end_atom * atom <= item['mapped_end'] else 0xffffffffffffffff
                            if item['offset'] != expected_offset or item['size'] != expected_size:
                                raise ValueError('Mapped range includes unrelated atoms or exceeds mapped extent')
                        receipt['mapped_ranges'] = {'calls': len(ranges), 'nonzero_bindings': memory['nonzero_bindings'], 'whole_size_calls': sum(item['size'] == 0xffffffffffffffff for item in ranges), 'atom_sizes': sorted(set(item['atom_size'] for item in ranges)), 'coherent_memory_instrumentation': True}

                if args.fixture == 'nri_vulkan_shader_execution' and (receipt['fixture'].get('exact_cycles') != 8 or receipt['fixture'].get('indirect_cycles') != 8 or receipt['fixture'].get('indexed_offset_cases') != 8 or receipt['fixture'].get('graphics_state_cases') != 8 or receipt['fixture'].get('max_channel_error', 256) > 1):
                    raise ValueError('Shader/graphics-state output checks did not pass')
                if args.fixture == 'nri_vulkan_shader_execution':
                    records = [json.loads(line.split('NRI_PIPELINE_CACHE ', 1)[1]) for line in stdout.decode('utf-8').splitlines() if line.startswith('NRI_PIPELINE_CACHE ')]
                    if len(records) != 1 or records[0].get('rejected_headers') != 7 or records[0].get('saved_bytes', 0) < 32:
                        raise ValueError('Pipeline cache serialization/rejection checks did not pass')
                    cache = receipt['pipeline_cache'] = records[0]
                    if args.require_pipeline_cache_hit and (not cache.get('cache_control') or not cache.get('hit_required') or cache.get('loaded_bytes', 0) < 32):
                        raise ValueError('Loaded cache without no-compilation pipeline qualification')
                    for label, path in [('input', args.cache_input), ('output', args.cache_output)]:
                        if path:
                            data = path.read_bytes()
                            if len(data) != cache['loaded_bytes' if label == 'input' else 'saved_bytes'] or len(data) < 32:
                                raise ValueError('Cache artifact size differs from native receipt')
                            size, version, vendor, device = struct.unpack_from('<4I', data)
                            cache[label] = {'sha256': hashlib.sha256(data).hexdigest(), 'header_size': size, 'header_version': version, 'vendor_id': vendor, 'device_id': device, 'pipeline_cache_uuid': data[16:32].hex()}
                if args.fixture == 'nri_vulkan_shader_execution':
                    reuse = [json.loads(line.split('NRI_DESCRIPTOR_REUSE ', 1)[1]) for line in stdout.decode('utf-8').splitlines() if line.startswith('NRI_DESCRIPTOR_REUSE ')]
                    if len(reuse) != 1 or any(reuse[0].get(k) != v for k, v in {'cycles': 8, 'pool_resets': 14, 'range_copies': 24}.items()) or reuse[0].get('uniform_stride', 0) < 80:
                        raise ValueError('Descriptor reset/copy/reuse qualification incomplete')
                    if reuse[0].get('expected_rejections') != (24 if args.validation or args.require_native_validation else 0):
                        raise ValueError('Descriptor bounds rejection coverage incomplete')
                    receipt['descriptor_reuse'] = reuse[0]
                    records = [json.loads(line.split('NRI_INDIRECT_FEATURES ', 1)[1]) for line in stdout.decode('utf-8').splitlines() if line.startswith('NRI_INDIRECT_FEATURES ')]
                    if len(records) != 1:
                        raise ValueError('Missing indirect feature execution counters')
                    indirect = receipt['indirect_features'] = records[0]
                    device = manifest['device']
                    compute_records = [json.loads(line.split('NRI_COMPUTE_INPUT ', 1)[1]) for line in stdout.decode('utf-8').splitlines() if line.startswith('NRI_COMPUTE_INPUT ')]
                    if compute_records != [{'external': bool(args.compute_input), 'xor': args.compute_xor}]:
                        raise ValueError('Runtime compute selection was not executed')
                    query_records = [json.loads(line.split('NRI_DRAW_CAPABILITIES ', 1)[1]) for line in stdout.decode('utf-8').splitlines() if line.startswith('NRI_DRAW_CAPABILITIES ')]
                    fields = ('multi_draw_indirect', 'draw_indirect_first_instance', 'shader_draw_parameters', 'draw_indirect_count')
                    if len(query_records) != 1 or query_records[0] != {key: device[key] for key in fields}:
                        raise ValueError('Public enabled draw capabilities differ from actual device creation')
                    receipt['draw_capabilities'] = query_records[0]
                    instance_cases = 8 if device['draw_indirect_first_instance'] else 0
                    multi_parameter_cases = 8 if all(device[key] for key in fields[:3]) else 0
                    if receipt['fixture'].get('indirect_first_instance_cases') != instance_cases or receipt['fixture'].get('multi_draw_parameter_cases') != multi_parameter_cases:
                        raise ValueError('Indirect firstInstance or multi-draw DrawIndex execution incomplete')
                    parameter_cases = 8 if device.get('shader_draw_parameters') == 1 else 0
                    if receipt['fixture'].get('draw_parameter_cases') != parameter_cases:
                        raise ValueError('Draw-parameter capability-selected execution was incomplete')
                    multi = device.get('multi_draw_indirect')
                    count = device['draw_indirect_count']
                    if multi not in (0, 1) or indirect['draw_limit'] < 1 or (multi == 0 and indirect['draw_limit'] != 1):
                        raise ValueError('Indirect draw limit does not match enabled native feature')
                    count_calls = 4 if count else 0
                    expected = (2 if count else 4, count_calls, 0, 0) if indirect['draw_limit'] >= 2 else (0, count_calls, 4 if count else 8, 2 if args.validation or args.require_native_validation else 0)
                    actual = tuple(indirect[k] for k in ('multi_draw_calls', 'count_draw_calls', 'split_draw_calls', 'expected_rejections'))
                    if actual != expected:
                        raise ValueError('Indirect capability-selected execution was incomplete')
                if args.require_no_indirect_first_instance and manifest['device'].get('draw_indirect_first_instance') != 0:
                    raise ValueError('Indirect firstInstance was not disabled')
                if args.require_no_shader_draw_parameters and manifest['device'].get('shader_draw_parameters') != 0:
                    raise ValueError('Shader draw parameters were not disabled')
                if args.require_no_indirect_optionals:
                    if any(manifest['device'].get(k) != 0 for k in ('multi_draw_indirect', 'draw_indirect_first_instance', 'shader_draw_parameters')):
                        raise ValueError('Indirect optional features were not disabled before device creation')
                if args.require_shader_inputs:
                    inputs = receipt['fixture'].get('shader_manifest', {}).get('input_qualification', {})
                    entries = inputs.get('entries', {})
                    if args.fixture != 'nri_vulkan_shader_execution' or inputs.get('embedded_input') != 'serialized_module' or set(entries) != {'compute', 'arguments', 'vertex', 'fragment', 'state_vertex', 'state_parameters_vertex', 'state_fragment'}:
                        raise ValueError('Serialized shader input qualification is absent')
                    for name, entry in entries.items():
                        if entry.get('input_kinds') != ['text', 'memory', 'module'] or (not name.startswith('state_') and entry.get('module_reads', 0) < 1):
                            raise ValueError('Shader input routes or shared module dependency were not qualified')
                if args.require_present_retirement:
                    records = [json.loads(line.split('NRI_PRESENT_RETIREMENT ', 1)[1]) for line in stderr.decode('utf-8').splitlines() if 'NRI_PRESENT_RETIREMENT ' in line]
                    if args.fixture != 'nri_vulkan_presentation' or not records:
                        raise ValueError('Presentation retirement was not observed')
                    for record in records:
                        if any(type(record.get(key)) is not int or record[key] < 0 for key in ('submitted', 'retired', 'pending', 'peak_fences')):
                            raise ValueError('Invalid presentation retirement counters')
                        if record['retired'] + record['pending'] != record['submitted']:
                            raise ValueError('Unaccounted presentation payloads')
                    final = records[-1]
                    if final['submitted'] != receipt['fixture']['exact_frames'] or final['pending'] != 0 or not 1 <= final['peak_fences'] <= 4:
                        raise ValueError('Presentation payloads did not retire within the fixture bound')
                    receipt['presentation_retirement'] = final
                if args.require_no_timeline and manifest['device']['timeline'] != 0:
                    raise ValueError("Required no-timeline profile is not active")
                if args.require_no_timeline and 'VK_KHR_timeline_semaphore' in manifest['device']['extensions']:
                    raise ValueError("Timeline semaphore extension is still enabled")
                if args.require_no_host_reset and (manifest['device'].get('host_query_reset') != 0 or 'VK_EXT_host_query_reset' in manifest['device']['extensions']):
                    raise ValueError("Required no-host-reset profile is not active")
                if args.require_legacy_wsi or args.require_legacy_surface:
                    if {'VK_KHR_get_surface_capabilities2', 'VK_EXT_surface_maintenance1'}.intersection(manifest['instance']['extensions']):
                        raise ValueError("Legacy surface queries were not selected")
                    if 'VK_EXT_swapchain_maintenance1' in manifest['device']['extensions']:
                        raise ValueError("Swapchain maintenance requires the absent instance extension")
                if args.require_legacy_wsi:
                    if set(manifest['device']['extensions']) != {'VK_KHR_swapchain'}:
                        raise ValueError("Legacy WSI profile must enable only the swapchain device extension")
                if args.require_individual_features:
                    require_individual_features(manifest)
                if args.require_renderpass1 or args.require_renderpass2:
                    required = {'VK_KHR_timeline_semaphore', 'VK_EXT_host_query_reset'}
                    if args.require_no_timeline:
                        required.remove('VK_KHR_timeline_semaphore')
                    if args.require_no_host_reset:
                        required.remove('VK_EXT_host_query_reset')
                    if args.require_renderpass2:
                        required |= {'VK_KHR_create_renderpass2', 'VK_KHR_depth_stencil_resolve'}
                    if set(manifest['device']['extensions']) != required or manifest['device']['effective_minor'] != 1 or manifest['device']['dynamic_rendering'] != 0:
                        raise ValueError("Required legacy renderpass profile is not active")
                    if args.fixture == 'nri_vulkan_shader_execution':
                        if receipt['fixture'].get('exact_cycles') != 8:
                            raise ValueError('Shader execution cycles did not pass')
                    elif args.fixture == 'nri_vulkan_buffer_baseline' and not all(receipt['fixture'].get(key) for key in ('render_exact', 'input_attachment_exact', 'resolve_exact')):
                        raise ValueError("Rendering pixel checks did not pass")
                if args.require_basic_optionals:
                    fields = ('device_address', 'descriptor_indexing', 'draw_indirect_count', 'format_feature_flags2')
                    if any(manifest['device'].get(key) != 0 for key in fields):
                        raise ValueError("Required basic optional-feature profile is not active")
                    forbidden = {'VK_KHR_buffer_device_address', 'VK_KHR_draw_indirect_count', 'VK_EXT_descriptor_indexing', 'VK_EXT_descriptor_heap', 'VK_EXT_mutable_descriptor_type', 'VK_KHR_shader_untyped_pointers', 'VK_KHR_format_feature_flags2', 'VK_EXT_host_image_copy', 'VK_KHR_maintenance4', 'VK_EXT_subgroup_size_control'}
                    if forbidden.intersection(manifest['device']['extensions']):
                        raise ValueError("Disabled optional extension was enabled")
                if args.require_api_minor is not None and manifest['device']['effective_minor'] != args.require_api_minor:
                    raise ValueError("Required effective API minor version is not active")
                if args.require_legacy_sync and (manifest["device"]["synchronization2"] != 0 or not receipt["fixture"].get("legacy_sync_forced")):
                    raise ValueError("Required forced legacy synchronization is not active")
                active = manifest["instance"]["native_validation_active"]
                receipt["fixture"]["native_validation_active"] = active
                if args.require_native_validation and not active:
                    raise ValueError("Required native Vulkan validation layer is not active")
            except (ValueError, KeyError, TypeError) as error:
                receipt["status"] = "failed"
                receipt["failure"] = str(error)
    except subprocess.TimeoutExpired as error:
        stdout, stderr = error.stdout or b"", error.stderr or b""
        receipt["timed_out"] = True
        receipt["exit_code"] = None
        receipt["failure"] = "Owned fixture process timed out and was killed"
    except OSError as error:
        receipt["exit_code"] = None
        receipt["failure"] = str(error)
    receipt["elapsed_seconds"] = time.monotonic() - started
    (args.output / "stdout.log").write_bytes(stdout)
    (args.output / "stderr.log").write_bytes(stderr)
    (args.output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt))
    return 0 if receipt["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
