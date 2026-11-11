# Vulkan baseline fixture

This independent C++17 fixture tests the actual NRI public Vulkan path without
GameServer, a window, an asset catalog or a shader compiler. It enumerates/selects
an adapter, creates a device and graphics queue, uploads a deterministic 4096-byte
pattern, copies through device memory into readback memory, verifies every byte,
also copies the pattern through mip 1 / array layer 1 of a texture, checks GPU
timestamp ordering, signals/waits/queries fence values 1 through 3 (including
queued waits, an empty submission and an older repeated wait), and waits for idle
before teardown. Errors return nonzero.
It also checks basic format/limit queries and creates a fixed descriptor layout,
pool and set. These checks do not execute a shader or establish draw correctness.
Rendering checks exercise clear/load/store, an asymmetric rectangular clear and
cache reuse at mip1/layer1, first as an ordinary color attachment and then with
an input-attachment layout. A two-view 4x MSAA pass includes a depth attachment
and resolves both color layers; every output pixel is checked. These additional
submissions advance the fence through value6. Input attachment shader reads and
depth values are not verified by these clear/resolve tests. The layered fixture
requires multiview with at least two views and 4x RGBA8/D32 sample support.
Callback messages go to stderr; stdout contains a JSON result.

## Future waits and host query reset

Build `NRICompatibilityFenceSequences` and use the same bounded runner with
`--exe <path-to-NRICompatibilityFenceSequences> --fixture nri_vulkan_fence_sequences`.
This fixture requires two distinct graphics queues. It starts a fence at5,
submits a consumer waiting for7 before submitting its producer, and verifies
that pending work is not reported completed. While queue0 is waiting, it resets
unrelated queries on the host; the query commands were recorded beforehand but
have not been submitted. Queue1 then copies a deterministic pattern and signals9.
Both consumers must read back the exact bytes. Two concurrent host waiters and
repeated waits for older values must succeed without consuming completion state.
A later partial host reset updates query0 while query1 retains its exact result.

Run both legacy synchronization and native modern profiles with native validation.
These tests run on native timeline semaphores or the no-timeline scheduler;
host query reset remains native. They establish behavior for the remaining query
reset implementation as well as regression coverage for fence emulation.
In particular, an implementation of host reset that submits to queue0 and waits
would depend on the future producer the caller cannot submit until reset returns.
The fixture's timeout still applies to submission, reset, all waits and cleanup.
The fixture additionally alternates a logical fence between its two physical
queues for128 signals, including repeated old-value waits. Every16 cycles it
samples idle retirement. Diagnostic `NRI_LEGACY_SCHEDULER` lines report retained
packets, native completions, signal points and binary semaphores; these counts
cover scheduler-owned synchronization objects, not total CPU/GPU memory.

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

The fixture build enables private `NRI_COMPATIBILITY_DIAGNOSTICS` only on its
NRI Vulkan target. After successful instance/device creation, the actual enabled
versions, extensions and key features are emitted through the callback. The
runner requires complete provenance; missing or mismatched records fail the run.
Wrapped external devices are not currently covered by this provenance path.

`--adapter INDEX` chooses the Vulkan enumeration index. `--validation` requests
both NRI and native graphics validation; it does not establish that a native
validation layer was available/enabled. NRI may filter missing instance layers.
The executable's raw result leaves activation unknown; the runner fills it from
the successfully created instance's actual layer list. `--require-native-validation`
requests validation and fails unless that list confirms activation. This does
not imply synchronization/GPU-assisted validation settings were enabled.

## Coverage boundary

Configure with `-DNRI_COMPATIBILITY_FORCE_LEGACY_SYNC=ON` to disable the actual
synchronization2 feature before device creation. This control exists only in the
standalone fixture build. The three synchronization2 dispatch pointers remain
null; buffer/image barriers, timestamps and queue submission must use the legacy
commands. Use `--require-legacy-sync` to reject a wrongly configured run. Timeline
semaphores remain native unless NO_TIMELINE or NO_HOST_RESET is enabled. Configure OFF and rerun
for the modern comparison.
Build `NRILegacySynchronizationTests` and run CTest for Release-safe scope
conversion checks, including split stage/access bits and unknown upper bits.

`-DNRI_COMPATIBILITY_INDIVIDUAL_FEATURES=ON` exercises the pre-1.2 individual
feature/property structures instead of Vulkan11/12 aggregates. The latter were
introduced in 1.2 despite their names. Optional promoted features use explicitly
enabled extension structures; queried results are normalized into local values.
These local aggregates are never linked into the individual query/create chains.
`--require-individual-features` checks actual chain receipts for the required
core1.1 structures, missing/duplicate records and forbidden aggregate structures.

`-DNRI_COMPATIBILITY_API_MINOR=1`, `2` or `3` caps the test instance API; `0` (default)
keeps native negotiation. The runner's `--require-api-minor` verifies the actual
effective device API. These are independent controls: combine individual
structures, API2 and forced legacy synchronization, then test all controls off
for the modern regression. API1.1 naturally selects the individual structures,
so `INDIVIDUAL_FEATURES` can stay OFF in that profile. A capped modern device does
not establish older-driver compatibility. The core-only
subgroupBroadcastDynamicId feature is disabled on the individual path, and
framebufferIntegerColorSampleCounts remains unknown because it has no pre-1.2
query equivalent. Format-specific image queries remain authoritative.

`-DNRI_COMPATIBILITY_BASIC_OPTIONALS=ON` requires API1.1 and removes the address,
descriptor indexing, indirect-count, descriptor heap, mutable descriptor,
untyped-pointer, format-feature-flags2, host-image-copy, maintenance4 and subgroup
size-control extensions before querying/enabling features. Use
`--require-basic-optionals --require-api-minor 1 --require-individual-features`
to check the actual manifest. Combine with FORCE_LEGACY_SYNC ON and
`--require-legacy-sync`. Fixed descriptor creation omits unsupported binding-flag
structures, unavailable optional entry points stay null, format queries use core
properties2, and buffer/subgroup limits use their core1.1 fallback values.

`-DNRI_COMPATIBILITY_RENDERPASS1=ON` with API_MINOR=1 keeps only the timeline and
host-query-reset device extensions. All newer rendering extensions are removed
before feature queries and device creation, forcing vkCreateRenderPass and core
copy/format/descriptor paths. `--require-renderpass1` verifies that exact manifest
and the rendering readback results. For comparison, use RENDERPASS1=OFF and
`-DNRI_COMPATIBILITY_RENDERPASS2=ON` with `--require-renderpass2`: this additionally
enables create_renderpass2 and depth_stencil_resolve. The two options are mutually
exclusive. Both require an NRI-owned API1.1 device. Combine with legacy sync, and
turn both options off with API_MINOR=0 for the native dynamic-rendering comparison.

`-DNRI_COMPATIBILITY_NO_TIMELINE=ON` removes the timeline extension and disables
both aggregate and individual native timeline features. `--require-no-timeline`
checks their absence. Test API1.1 with legacy synchronization and renderpass1,
then native API with sync2 and no timeline, then all controls OFF. Emulated fence
dependencies retain binary signal anchors; modern timeline entry points stay null
when that feature is disabled. Host query reset stays native unless separately disabled.

`-DNRI_COMPATIBILITY_NO_HOST_RESET=ON` removes the host-query-reset extension and
disables its feature. `--require-no-host-reset` checks both. On this profile the
scheduler also emulates ordinary fences even when the native timeline feature is
enabled. This keeps future waits on the host so synchronous reset commands can
run ahead of deferred work without deadlock. The reset waits for its own native
completion, preserves partial ranges and leaves no deferred query side effects.
Combine API1.1, RENDERPASS1, NO_TIMELINE, NO_HOST_RESET and legacy sync for a fixture
with no device extensions. Also test native API/timeline/sync2 with NO_HOST_RESET,
then all controls OFF.

`NRICompatibilityQueryReset` (`--fixture nri_vulkan_query_reset`) requests only a
COPY queue. It imports a native two-query timestamp pool, records timestamps
before host reset, performs concurrent disjoint resets, and directly checks
native availability after each call. Across 64 cycles, resetting query zero must
leave query one's available result exactly unchanged. It logs actual queue-family
flags so a transfer-only family is observed rather than assumed. Native Vulkan
and NRI validation are enabled; raw Vulkan recording/results inspect the imported
pool without introducing native queue submissions. The no-host-reset profile
therefore exercises creation and cleanup of its internal reset-capable queue.

`-DNRI_COMPATIBILITY_FAIL_SUBMIT=N` injects a device-memory failure before the Nth
scheduler native submission (0 disables it). For the two-queue sequence N=3 fails
the first consumer after producer and payload-split submissions when host reset is
native. With emulated host reset the submit ordinals differ. For the copy-only
query fixture N=1 fails the first reset submission. The runner must
report failure, with the injected diagnostic and no native lifetime errors. Save
this expected-failure receipt; it must not be relabeled as a normal passing run.

Neither native timeline nor host query reset is now a device-creation prerequisite
for owned devices. Full Vulkan1.1 support still needs the remaining fixture
families and actual legacy hardware.

The legacy path merges barrier execution scopes, expands split scopes to their
legacy equivalents, and uses conservative all-commands / memory-access scopes
for extension bits with no legacy spelling. Queue signals use the whole legacy
submission scope. It does not add queue-idle waits.

This is transfer/synchronization and offscreen attachment qualification, not
Vulkan 1.1 acceptance.
The baseline does not exercise WSI, execute shaders, or prove future waits and
fan-out by itself; the separate fence-sequence fixture covers the cases described
above. Real device loss and broader memory pressure remain pending. The manifest records actual NRI-created device
configuration; it is never inferred from NRI's version or adapter name. Later fixtures must
cover those cases and real older drivers before claiming compatibility support.
