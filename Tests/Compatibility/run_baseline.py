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
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--adapter", type=int, default=0)
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--require-native-validation", action="store_true")
    parser.add_argument("--require-legacy-sync", action="store_true")
    parser.add_argument("--require-individual-features", action="store_true")
    parser.add_argument("--require-api-minor", type=int, choices=(2, 3, 4))
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
                if isinstance(candidate, dict) and candidate.get("fixture") == "nri_vulkan_buffer_baseline":
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
                if args.require_individual_features:
                    require_individual_features(manifest)
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
