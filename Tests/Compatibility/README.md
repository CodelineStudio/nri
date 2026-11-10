# Vulkan baseline fixture

This independent C++17 fixture tests the actual NRI public Vulkan path without
GameServer, a window, an asset catalog or a shader compiler. It enumerates/selects
an adapter, creates a device and graphics queue, uploads a deterministic 4096-byte
pattern, copies through device memory into readback memory, verifies every byte,
signals/waits/queries fence values 1 and 2 (including an empty submission and an
older repeated wait), and waits for idle before teardown. Errors return nonzero.
Callback messages go to stderr; stdout contains a JSON result.

## Build and run

From the NRI root, configure a separate build directory:

```text
cmake -S Tests/Compatibility -B _Build/Compatibility -DNRI_STATIC_LIBRARY=ON -DNRI_ENABLE_D3D11_SUPPORT=OFF -DNRI_ENABLE_D3D12_SUPPORT=OFF -DNRI_ENABLE_NVTX_SUPPORT=OFF -DNRI_ENABLE_WGPU_SUPPORT=OFF
cmake --build _Build/Compatibility --config Release --target NRICompatibilityBaseline
python Tests/Compatibility/run_baseline.py --exe _Build/Compatibility/Release/NRICompatibilityBaseline.exe --output _Results/baseline-001 --timeout 60
```

For single-configuration generators omit `Release/` from the executable path;
on non-Windows hosts omit `.exe`. Existing fetched dependencies can be reused with
`-DFETCHCONTENT_BASE_DIR=<absolute-existing-dependency-directory>`.
For an offline reuse that skips download stamp checks, instead pass explicit
`-DFETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS=<headers-source>` and
`-DFETCHCONTENT_SOURCE_DIR_VMA=<vma-source>` overrides for already verified sources.
NRI's own
static/shared configuration remains selectable. CMake records the checked-out
commit and compiler version; reconfigure after changing commits, and retain a
separate dirty-diff/build receipt for uncommitted implementation changes.

Use a new output directory for each run. The Python runner preserves stdout,
stderr and a receipt, bounds startup/waits/teardown, kills and reaps only its own
fixture process on timeout, and returns nonzero for failure or missing receipts.
It does not launch games, services or other child applications.

`--adapter INDEX` chooses the Vulkan enumeration index. `--validation` requests
both NRI and native graphics validation; it does not establish that a native
validation layer was available/enabled. NRI may filter missing instance layers.
The result therefore records `validation_requested` separately from the currently
unknown `native_validation_active`. Zero callback errors is **not** a native
validation-clean claim. Independently verified layer activation is required for
the later validation gate.

## Coverage boundary

This is an initial current-path transfer/fence baseline, not Vulkan 1.1 acceptance.
It does not force capabilities off, expose enabled extension/API-version details,
exercise WSI, execute shaders, or prove future waits, fan-out, cross-queue ordering,
device loss or memory pressure. Unknown API/extension metadata is recorded as
null, never inferred from NRI's version or the adapter name. Later fixtures must
cover those cases and real older drivers before claiming compatibility support.
