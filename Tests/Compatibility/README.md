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

## Win32 presentation fixture

`NRICompatibilityPresentation` uses `--fixture nri_vulkan_presentation`. It owns
a small nonactivating window, renders and reads back every pixel of 400 frames,
and performs 100 alternating-size recreations. FIFO and uncapped requests
alternate; swapchain-returned extents are checked. Present semaphores are indexed
by acquired image, and command/acquire reuse waits for confirmed GPU completion.
A five-millisecond delayed producer on a second physical graphics queue exercises
the first frame of each cycle. On a forced emulated-fence profile Present drains
the deferred submission; on the native path the producer is submitted before
Present, as required by VUID-vkQueuePresentKHR-pWaitSemaphores-03268. This fixture
currently uses one frame in flight and requires two graphics queues.

`-DNRI_COMPATIBILITY_LEGACY_WSI=ON` disables the instance capabilities2 and surface
maintenance extensions. With API_MINOR=1 and RENDERPASS1 it retains KHR_swapchain
in the otherwise minimal device extension profile. Add NO_TIMELINE,
NO_HOST_RESET and FORCE_LEGACY_SYNC for the baseline. The runner option
`--require-legacy-wsi` checks that only KHR_swapchain remains and both newer
instance extensions are absent. `--require-legacy-surface` checks just the surface
fallback and its maintenance dependency, for a native-device/legacy-surface
combination. Turn LEGACY_WSI off for the normal modern comparison.

`-DNRI_COMPATIBILITY_ACQUIRE_RESULTS=ON` also enables bounded WSI result injection:
each new swapchain first returns TIMEOUT, then NOT_READY without acquiring an
image; its first real successful acquire is reported as SUBOPTIMAL. Two global
acquire ordinals return OUT_OF_DATE and SURFACE_LOST before calling Vulkan. Two
present ordinals report those errors after a real present, preserving the required
consumption of present-wait semaphores. The fixture requires 208 no-image returns
with unchanged output indices, four error recoveries and all 400 exact readbacks.
These injections are test-only and do not simulate the OS lifecycle itself.

The initial Win32 run exposed a missing HINSTANCE in surface creation, now fixed.
The first modern test incorrectly presented before its future timeline producer
was submitted; that fixture error is corrected, without changing native timeline
semantics. Keep the failed receipts. Unexpected validation errors stop subsequent
frames. The runner bounds the process and destroys only its owned test window.

Retirement boundary: the fixture uses DeviceWaitIdle before deleting each chain's
objects. On maintenance1, NRI now includes all presentation completion fences in
this wait and reuses only signaled fences during normal presentation. Run with
`--require-present-retirement` to require 400 accepted and retired payloads, zero
pending records and a peak of at most four retained native fences for this fixture.
`-DNRI_COMPATIBILITY_PRESENT_OOM=ON` rejects the first maintenance present before
native enqueue; the fixture must fail promptly with the expected NRI error and
validation-clean cleanup, without waiting for that unsubmitted fence.
`-DNRI_COMPATIBILITY_PRESENT_TIMEOUT=ON` times out the first retirement wait;
the fixture must report failure, preserve objects, then retire them on its cleanup
retry without Vulkan lifetime errors. If that retry also fails, the owned process
exits with the objects retained instead of destroying potentially live resources.
These injections remain off in normal builds. These fences do not prove displayed
scanout completion; they prove that presentation payloads can safely be released.

Without maintenance1, the idle convention is runtime evidence, not a portable
presentation completion proof. Legacy platform qualification, real surface loss, minimization, Android lifecycle,
multiple frames in flight and scanout inspection remain separate acceptance work.

## Slang graphics and compute fixture

`NRICompatibilityShaderExecution` executes a compiled compute shader followed by
vertex/fragment shaders, for eight changing uniform inputs. It checks 64 uint4
storage results per cycle (integer overflow, workgroup neighbor exchange/barrier,
matrix transform and nonce) and all 4096 RGBA pixels (vertex input, off-center
column-major transform, top-left viewport, scissor and compute-to-fragment reads).
Both fixed descriptor bindings and the 80-byte uniform layout are checked against
Slang reflection.

Eight additional graphics cases combine nearest/linear clamp sampling,
straight/premultiplied alpha and 1x/4x MSAA. Each samples a nonzero mip/layer of
a texture array using separate texture/sampler descriptors and a reflected
16-byte push constant. Indexed two-instance draws use an ordinary per-instance
vertex stream; the right instance mirrors the texture. A scissored depth/stencil
mask precedes deliberate stencil and depth rejection draws, then alpha blending
and attachment color resolve. All 4096 pixels are checked against a CPU sampling
oracle: alpha and untouched pixels are exact, with at most one UNORM8 unit of
RGB rounding tolerance inside the mask. `graphics_state_cases` must equal eight;
`max_channel_error` reports the observed maximum. Unexpected validation errors
stop subsequent cases. Geometry covers full pixels; fractional-edge MSAA quality
and other sample counts are not qualified by this test.

This remains a bounded corpus: optional capabilities, indirect/base-offset draw
semantics, production shader families, text/module packaging and caches still
need qualification. These reusable shaders are not the consumer's Noesis backend.

Pins are in `shader_toolchain.json`. Use the official Slang 2026.21 package and
build `spirv-val`/`spirv-dis` from its recorded Khronos revisions. The Windows
validator build uses `SPIRV-Headers_SOURCE_DIR`, `SPIRV_SKIP_TESTS=ON`, and Release
targets `spirv-val spirv-dis`; no global SDK installation is required. Source
archive checksums and actual tool/runtime hashes belong in the run evidence.
Configure the standalone project with all three absolute paths:

```
-DNRI_COMPATIBILITY_SLANGC=<slangc>
-DNRI_COMPATIBILITY_SPIRV_VAL=<spirv-val>
-DNRI_COMPATIBILITY_SPIRV_DIS=<spirv-dis>
```

The build runs `compile_shaders.py`, emitting direct SPIR-V1.3, reflection JSON,
disassembly, a tool/source/binary manifest and an embedded header in `shaders/`
under the build directory. Every binary passes `spirv-val --target-env vulkan1.1`;
the baseline allows only Shader/Matrix capabilities and no extension declarations.
The executable embeds the manifest, including compiler/runtime hashes, so a GPU
receipt identifies its actual shader bytes rather than whichever files exist later.
Windows compiler DLLs are explicit build dependencies; other host tool/runtime
packaging still requires qualification. This is build-time qualification, not the
consumer's eventual persistent cache or mobile runtime compiler implementation.

Run with `--fixture nri_vulkan_shader_execution` and the new executable. For the
extension-free profile, configure API_MINOR=1, RENDERPASS1, NO_TIMELINE,
NO_HOST_RESET and FORCE_LEGACY_SYNC. The runner accepts `--require-renderpass1`
for this fixture and requires all eight exact cycles plus eight graphics cases
as well as the profile. Switch RENDERPASS1 off and RENDERPASS2 on, with the other
settings unchanged, to qualify the KHR renderpass2 path using
`--require-renderpass2`. Restore all forced profile options for the native run.

The first expanded fixture incorrectly requested resolve-source access for its
single-sample color image. NRI validation rejected the barrier and native
validation reported the resulting missing transitions. Resolve-source access
and stage now apply only to the multisample source; keep the failed receipt.

The initial vertex-ID probe compiled valid Vulkan1.1 SPIR-V but was rejected by
the stricter corpus capability check: Slang 2026.21 implements `SV_VertexID` using
BaseVertex and adds optional DrawParameters. Ordinary vertex input avoids that
extra requirement in this baseline. Do not blindly allow it during migration:
qualify the device feature or select a verified alternative shader convention.
This output contains RowMajor decoration even under the column-major option;
the non-symmetric GPU transform test verifies the CPU memory contract, rather
than inferring it from that decoration alone.

### API text and serialized-module inputs

Optionally configure `-DNRI_COMPATIBILITY_SLANG_SDK=<pinned-slang-package>` to
build `NRICompatibilitySlangInputs` against the package's `slang::slang` CMake
target (the actual Windows library is `slang-compiler.dll`). The corpus build
then compares all five entries from the CLI, API disk text, API in-memory text
and API serialized modules. Every route must produce byte-identical SPIR-V and
matching static reflection layouts. CLI-only per-entry resource-liveness lists
are excluded from the JSON comparison; identical target code establishes matching
resource use. The serialized-module result is the one embedded in the GPU fixture.

`common.slang` imports the reusable `shared_math` module. Entry points have explicit
stage attributes so serialized multi-entry modules retain their intended entries.
The deployed module loader uses an isolated filesystem that exposes only
`.slang-module` files from the supplied directory. Original source paths stored
in IR cannot be opened. The fixture records loaded/imported modules and denied
file reads. In-memory text exercises the API route needed for generated inputs;
it does not qualify a consumer's particle code generator.

Run GPU qualification with `--require-shader-inputs` to reject stale executables
that do not contain this input-route evidence. `input-runs.json` separates session
initialization, load, serialization, link and codegen timings; these single cold
process observations are not a shipping startup benchmark. `module-artifacts.json`
records exact intermediate bytes separately from the deterministic target manifest.
Relocated source paths can change the serialized module bytes even when every
target binary is identical. Do not infer portable IR byte reproducibility from
target determinism or place current checkout paths in target-cache lookup keys.

Run `check_shader_inputs.py --driver <NRICompatibilitySlangInputs> --artifacts
<build/shaders> --output <new-evidence-directory>` for module-only deployment,
missing-import/corrupt-module/missing-entry rejection and, on Windows, isolated
runtime-DLL packaging. Slang can return success while diagnosing a missing
optimizer DLL; this qualification driver rejects all diagnostics. Its Windows
package includes `slang-compiler.dll` and `slang-glslang.dll`, and the build checks
both hashes against the pinned CLI package. Other platforms remain unqualified.

Do not use `ISession::isBinaryModuleUpToDate` as the persistent compatibility key:
the pinned API documents that compiler/option hashes are not compared when primary
source is unavailable. A shipping module/cache manifest must enforce the consumer's
compiler, options, dependencies, target capabilities and binding ABI separately.
Persistent target/pipeline caches and production engine integration remain pending.

### Driver pipeline cache round trips

The shader fixture now shares one NRI pipeline cache across its compute pipeline
and 17 graphics pipeline creations. `run_baseline.py --cache-output <file>` saves
a raw driver artifact after execution; a subsequent process can load it with
`--cache-input <file>`. The receipt records exact bytes/hash, native vendor/device/
cache UUID, cache creation time and aggregate CPU pipeline creation time.
`--require-pipeline-cache-hit` requests FAIL_ON_CACHE_MISS on every creation and
requires enabled pipelineCacheControl. It is intentionally unavailable in the
extension-free Vulkan1.1 profile. Legacy reload still verifies native data
acceptance and identical GPU output; it does not prove that compilation was skipped.
Native success proves no compilation was required, but driver-internal caches may
also contribute, so do not attribute every hit exclusively to the supplied blob.

NRI now checks the defined 32-byte little-endian Vulkan cache header against the
physical device before native creation, returning OUT_OF_DATE for unsupported
header versions, malformed/truncated headers or mismatched vendor/device/UUID.
The prior output-size heuristic could miss incompatible headers or misclassify
driver-compacted compatible data. The fixture verifies seven header rejections
without passing those fabricated headers to Vulkan; NRIPipelineCacheHeaderTests
also checks every truncated header size and unaligned input.

Header compatibility is not opaque-payload integrity. These CLI files are trusted
raw test artifacts, not a finished persistent cache. A consumer must verify its
own checksum and compiler/shader/pipeline/profile/driver metadata, then publish
atomically with storage bounds and concurrent writer handling. Corrupt payloads
must never be passed to the driver merely because their header matches.
One cold/reload pair is correctness evidence, not a performance benchmark.

### Persistent artifact cache qualification

`artifact_cache.py` implements the standalone cache envelope: canonical typed
compatibility metadata, schema/key/size/SHA-256 checks, bounded reads/storage,
fsynced temporary files and atomic replacement. A process-owned OS lock serializes
readers/writers and cold compilation; it is released on process exit. Pending
files from interrupted writers are reclaimed only while holding that lock.
Eviction uses access time only, never timestamps or checkout paths in cache keys.
Managed entries are bounded after publication; temporary publication can require
one additional bounded entry. Unrelated files are left alone. This simple global
lock is a qualification policy, not a claim of optimal shipping concurrency.

`cached_shader_inputs.py` keys target results by exact logical source/dependency
or module hashes, input kind, compiler/runtime/options, entry/stage/profile,
capabilities, static reflection ABI and validation recipe. It captures the input
bytes before compilation, so live edits cannot put new source under an old key.
Cache hits check the envelope, SPIR-V representation/capabilities and expected
layout without launching the compiler or validator. Cold output runs spirv-val.
Compiler package changes during compilation are rejected. Busy/unavailable or
over-capacity caches can return validated uncached output; root initialization
errors remain caller-visible. Invalid compiler output is never cached as success.

`cached_pipeline_inputs.py` additionally identifies device/driver/native UUID,
actual enabled device profile, shader hashes and the fixture executable containing
all pipeline states. It admits driver bytes only after envelope integrity and
native-header compatibility checks. A shipping engine should serialize its actual
PSO descriptions rather than using the fixture executable as their identity.

Run `qualify_artifact_cache.py` with the pinned `--driver`, `--validator`, compiled
`--artifacts`, shader `--sources`, matching `--gpu-exe`, cold `--pipeline-receipt`
and `--pipeline-data`, plus a fresh `--output` directory. It checks cold/warm text,
memory and module compilation, relocation, real shared-include edits, corruption,
semantic admission, busy/full-cache fallback and driver-envelope recovery. Warm
runs forbid compiler invocation. Metadata mutation checks model invalidation;
they are not execution evidence for another compiler/GPU/driver. Run the output
`admitted-pipeline.bin` through the GPU fixture separately. `test_artifact_cache.py`
also covers interrupted publication, four concurrent cold callers, bounded lock
timeouts and storage eviction. These paths are qualified on Windows; other OS
locking/filesystem behavior and power-loss durability remain unverified.

These Python components qualify the persistent format and compiler/NRI contracts.
They do not replace the consumer's native cache service, shipping shader resolver,
platform packaging or production renderer. The full migration gate still applies.

This is transfer/synchronization and offscreen attachment qualification, not
Vulkan 1.1 acceptance.
The baseline does not exercise WSI, execute shaders, or prove future waits and
fan-out by itself; the separate fence-sequence fixture covers the cases described
above. Real device loss and broader memory pressure remain pending. The manifest records actual NRI-created device
configuration; it is never inferred from NRI's version or adapter name. Later fixtures must
cover those cases and real older drivers before claiming compatibility support.

### Atomic work generation and draw offsets

The shader corpus now contains six entries. `indirect.slang` adds a single
32-thread producer workgroup: storage-buffer atomic increments and a device-memory
group barrier produce dispatch and both draw argument layouts. The consumer
compute shader never writes the argument buffer while it is consumed. Explicit
copy-to-storage, storage-to-indirect and indirect-to-readback barriers separate
these uses. Each of eight cycles validates all 24 words, including untouched
sentinels and a changing shared-module result, then checks the existing compute
records and every output pixel. Dispatch arguments start at byte16; alternating
nonindexed/indexed draws use byte32/64, first vertex1 and indexed first index2.
Indirect first instance stays zero, and each call submits one draw: this baseline
does not require `drawIndirectFirstInstance`, multi-draw or draw-count extensions.

The eight graphics-state cases additionally use first index2, first instance1
and signed base vertex+1/-1 with deliberately different prefix/suffix data. The
same full-image oracle verifies vertex/instance fetch offsets, sampled textures,
alpha blending, stencil/depth and MSAA together. This does not qualify shader
`SV_VertexID`/`SV_InstanceID` or optional DrawParameters builtins. Native and
extension-free Vulkan1.1 runs are required; masked hardware remains distinct
from genuine older-device acceptance. The shared pipeline cache now contains
19 pipelines; the API text/memory/module and persistent cache tests derive their
entry counts from the current corpus.
