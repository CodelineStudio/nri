"""Bound the standalone fixture process and preserve stdout, stderr and receipt."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import time
from capability_manifest import parse_manifest, require_individual_features


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--fixture", choices=("nri_vulkan_buffer_baseline", "nri_vulkan_fence_sequences", "nri_vulkan_query_reset", "nri_vulkan_presentation", "nri_vulkan_shader_execution"), default="nri_vulkan_buffer_baseline")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--adapter", type=int, default=0)
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--require-native-validation", action="store_true")
    parser.add_argument("--require-legacy-sync", action="store_true")
    parser.add_argument("--require-no-timeline", action="store_true")
    parser.add_argument("--require-no-host-reset", action="store_true")
    parser.add_argument("--require-legacy-wsi", action="store_true")
    parser.add_argument("--require-legacy-surface", action="store_true")
    parser.add_argument("--require-present-retirement", action="store_true")
    parser.add_argument("--require-shader-inputs", action="store_true")
    parser.add_argument("--require-individual-features", action="store_true")
    parser.add_argument("--require-basic-optionals", action="store_true")
    parser.add_argument("--require-renderpass1", action="store_true")
    parser.add_argument("--require-renderpass2", action="store_true")
    parser.add_argument("--require-api-minor", type=int, choices=(1, 2, 3, 4))
    args = parser.parse_args()
    if not 0 < args.timeout <= 600 or args.adapter < 0:
        parser.error("timeout must be in (0,600] seconds and adapter must be nonnegative")
    exe = args.exe.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=False)
    command = [str(exe), "--adapter", str(args.adapter)]
    if args.validation or args.require_native_validation:
        command.append("--validation")
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
                if args.fixture == 'nri_vulkan_shader_execution' and (receipt['fixture'].get('exact_cycles') != 8 or receipt['fixture'].get('graphics_state_cases') != 8 or receipt['fixture'].get('max_channel_error', 256) > 1):
                    raise ValueError('Shader/graphics-state output checks did not pass')
                if args.require_shader_inputs:
                    inputs = receipt['fixture'].get('shader_manifest', {}).get('input_qualification', {})
                    entries = inputs.get('entries', {})
                    if args.fixture != 'nri_vulkan_shader_execution' or inputs.get('embedded_input') != 'serialized_module' or set(entries) != {'compute', 'vertex', 'fragment', 'state_vertex', 'state_fragment'}:
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
                    elif not all(receipt['fixture'].get(key) for key in ('render_exact', 'input_attachment_exact', 'resolve_exact')):
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
