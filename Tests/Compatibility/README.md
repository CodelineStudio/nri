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
then compares all seven entries from the CLI, API disk text, API in-memory text
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

The shader corpus now contains seven entries. `indirect.slang` adds a single
32-thread producer workgroup: storage-buffer atomic increments and a device-memory
group barrier produce dispatch and both draw argument layouts. The consumer
compute shader never writes the argument buffer while it is consumed. Explicit
copy-to-storage, storage-to-indirect and indirect-to-readback barriers separate
these uses. Each of eight cycles validates all 64 words, including untouched
sentinels and a changing shared-module result, then checks the existing compute
records and every output pixel. Dispatch arguments start at byte16; single
nonindexed/indexed draws use byte32/64, first vertex1 and indexed first index2.
Grouped commands start at byte96/160. Indirect first instance stays zero. The
single-draw fallback requires neither `drawIndirectFirstInstance`, multi-draw nor
draw-count extensions; supported optional paths are selected as described below.

The eight graphics-state cases additionally use first index2, first instance1
and signed base vertex+1/-1 with deliberately different prefix/suffix data. The
same full-image oracle verifies vertex/instance fetch offsets, sampled textures,
alpha blending, stencil/depth and MSAA together. Raw IDs are verified in the baseline; local IDs and optional DrawParameters
builtins are qualified in a separately admitted variant as described below. Native and
extension-free Vulkan1.1 runs are required; masked hardware remains distinct
from genuine older-device acceptance. The shared pipeline cache now contains
19 baseline pipelines plus16per admitted optional graphics mode (up to67 total);
the API text/memory/module and persistent cache tests derive their
entry counts from the current corpus.

### Optional indirect features and effective draw limit

`other.drawIndirectMaxNum` is now1 when Vulkan `multiDrawIndirect` is disabled;
otherwise it retains the hardware limit. Validation rejects excessive `drawNum`
for both indexed and nonindexed ordinary indirect calls before backend dispatch.
Count-buffer commands are independently supported and do not use that ordinary
multi-draw limit; Vulkan bounds the GPU count by its physical limit instead. Public
API/ABI is unchanged. NRI does not silently split multi-draw commands: doing so
would change draw-ID semantics. Callers choose an appropriate supported path.

`NRI_COMPATIBILITY_NO_INDIRECT_OPTIONALS=ON` disables actual device creation bits
for multi-draw, indirect first instance and shader draw parameters, in either
aggregate or individual feature chains. `--require-no-indirect-optionals` checks
the observed manifest. Together with the extension-free profile this verifies
that the existing baseline does not accidentally rely on those optional features.

The shader fixture expands to64argument words and deliberately different third
commands. Cycles0/1 retain single indirect draws;2/3 draw the two triangles through
native multi-draw when admitted, otherwise two explicit single draws. Cycles4/5
use GPU count2 with maximum3 where supported;6/7 use GPU count0. The third command
would paint pixels outside the expected geometry if the count were ignored.
Without count support the fixture uses its known two-draw/empty scenario; this
is not a general GPU count emulation. All argument words and output pixels are
checked. Feature-selected path counters are enforced by the runner. On a limit1
validation-enabled profile, two deliberate oversized requests produce exactly
two expected NRI diagnostics and no native Vulkan error. Unexpected errors still
fail the fixture. Signed indexed offsets and other graphics-state checks remain.

Nonzero indirect firstInstance uses the fork's additive enabled-feature query
described below. Do not infer it from shaderFeatures.drawParameters or
drawIndirectMaxNum. Other backend admission remains separately qualified.

The modern masked profile keeps drawIndirectCount enabled while disabling
multiDrawIndirect. Count-buffer calls with maximum3 remain supported even though
the ordinary indirect limit is1. GPU count2 selects the two valid triangles and
count0 executes nothing. This profile reports4count calls,4ordinary split calls
and2expected validation rejections. It prevents conflating the independently
supported count-buffer and ordinary multi-draw commands. NRI's ordinary limit is
not a restriction on the count-buffer maximum parameter.

### Slang draw-ID semantics and per-entry capability admission

The state vertex shader uses `SV_VulkanVertexID` and `SV_VulkanInstanceID` to
verify raw Vulkan indices against independently supplied vertex/instance data.
Indexed cases include first index2, signed base vertex+1/-1 and first instance1.
Only Matrix/Shader capabilities are admitted for baseline entries. The verifier
requires actual VertexIndex/InstanceIndex builtins in this shader's binary.

`state_parameters_vertex` shares the geometry/UV helper and adds Slang local
`SV_VertexID`/`SV_InstanceID`, base vertex/instance and draw index checks. Its
DrawParameters capability and all five input builtins must appear in the binary.
Only this named entry admits that optional capability. CPU push data supplies
the expected signed base vertex; coordinate/instance data independently checks
local IDs. Direct commands test draw index0; the admitted multi-draw mode tests indices0/1. NRI's
`shaderFeatures.drawParameters` selects whether8additional graphics cases run;
unsupported devices create none of those pipelines. The runner verifies this
against actual enabled feature provenance. No global capability relaxation or
silent promotion of the baseline's requirements is allowed.

The shared16-byte push layout now names its final int32field `baseVertex` at
byte12. CLI,API text,memory and serialized-module reflection/binaries must match.
Cache keys include the entry's allowed capabilities and required device features.
Fifteen compiler-cache scenarios include replacing a valid baseline envelope's
binary with the optional binary and the reverse,while preserving checksums and
expected reflection metadata: both are rejected and only that entry is rebuilt.
The GPU consumer must still admit required features before pipeline creation;
a compatible cached optional binary does not itself authorize device execution.

These semantics follow Slang's official SPIR-V target documentation:
https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/a2-01-spirv-target-specific.html
Raw IDs include base offsets. Slang's ordinary HLSL-style IDs subtract them.
Do not blindly exchange these semantics in an existing shader. This qualification
covers Vulkan SPIR-V,not DXIL/Metal shader-ID behavior or production shader ports.

### Enabled Vulkan draw capabilities and indirect instances

`Extensions/NRIDrawCapabilitiesVK.h` adds `DrawCapabilitiesVKInterface` without
changing existing public tables or DeviceDesc. It reports the actual enabled
multiDrawIndirect, drawIndirectFirstInstance, shaderDrawParameters and
drawIndirectCount bits for NRI-created Vulkan devices. The immutable reference
remains valid until device destruction. Other backends and wrapped Vulkan
devices return UNSUPPORTED: wrappers provide no enabled-feature chain to NRI.
Consumers must obtain explicit capability information for wrapped devices from
their creator; physical-device support is not proof a feature was enabled.

The GPU fixture queries through both C++ and a compiled C translation unit and
compares results with the device-creation manifest. An additional raw-ID mode
draws two indirect indexed commands with firstInstance1/2 and signed base vertex.
It requires only drawIndirectFirstInstance; a further mode also requires
shaderDrawParameters and multiDrawIndirect and independently checks local IDs,
base offsets and DrawIndex0/1. Each admitted mode runs the existing eight
texture/alpha/depth/stencil/MSAA pixel cases. Unsupported modes create no pipelines.

`NRI_COMPATIBILITY_NO_INDIRECT_FIRST_INSTANCE` and
`NRI_COMPATIBILITY_NO_SHADER_DRAW_PARAMETERS` disable those device-creation bits
independently; the matching runner flags are `--require-no-indirect-first-instance`
and `--require-no-shader-draw-parameters`. The all-optionals-disabled legacy
profile remains supported. Modern and extension-free1.1 runs are differential
coverage on the available modern GPU, not genuine older-device acceptance.

### Runtime-generated and link-specialized inputs

The Slang input driver accepts an optional final link-input path. Text/memory
modes load that source as a distinct module; module mode loads both inputs from
serialized IR. `module-memory` loads a serialized base and a newly generated
specialization module from source, with the existing filesystem preventing base
source fallback. The caller must validate output and reflection before admission.
`NRI_SLANG_TRACE` enables phase diagnostics for compiler investigation. Windows
crash dialogs are suppressed so bounded subprocesses can report a crash status.

The GPU runner accepts `--compute-input`, `--compute-entry`, `--compute-xor` and
the required `--compute-validator`. It snapshots the supplied SPIR-V1.3 binary,
enforces baseline capabilities and runs Vulkan1.1 validation before pipeline
creation. The replacement compute shader must preserve the fixture's uniform,
storage and output contract; its fourth uint result is additionally XORed with
the independently supplied expected value. Exact storage/pixel/indirect checks
still apply. Receipts distinguish embedded shaders from the actual runtime
compute input, and driver-cache identities include the runtime binary and entry.
The generic fixture does not contain consumer-engine source or implement a
shipping compiler/cache service.

### Allocation failure recovery and memory lifetime

`NRICompatibilityMemoryLifetime` checks 64 cycles of two explicitly allocated
upload buffers and two images: whole/partial mapping, exact buffer and image
readback, command allocator reuse and resource destruction after confirmed GPU
completion. Ordinary profiles alternate direct allocation and VMA allocation.
The runner fixture name is `nri_vulkan_memory_lifetime`.

`NRI_COMPATIBILITY_FORCE_DEDICATED_MEMORY=ON` forces legal dedicated allocations
for this qualification. With `NRI_COMPATIBILITY_MEMORY_FAILURES=ON`, private
backend instrumentation rejects the second buffer allocation, second buffer
mapping and second image allocation. The fixture checks the returned errors,
retries the same batches and verifies their eventual contents. The runner's
`--require-memory-failures` requires all three recoveries and expected diagnostics.
Successful preallocated resources survive a later batch preparation failure;
failed mapping frees the new allocation before retry. Native validation must
report no errors or warnings, including during teardown.

Both switches default OFF and are standalone harness options, not production
features. Failure-profile builds are only intended for this fixture; restore
both switches before running the other fixtures. Injected errors do not prove
behavior under actual exhausted device memory. This fixture does not establish
noncoherent-memory coverage, descriptor reuse, overlapping in-flight resources
or real legacy-device support; those remain separate acceptance cases.

### Descriptor reset, copy and offset reuse

The shader execution fixture recreates its three buffer views and allocates
fresh sets from capacity-one copy-source and shader-visible pools each cycle.
After confirmed completion it resets the command allocator and both pools.
Uniform bindings alternate between two correctly aligned offsets; the unused
slot contains different, bounded parameters. Exact compute, indirect argument
and pixel results must reflect the newly copied descriptor range each cycle.
Receipts require eight cycles, fourteen pool resets and twenty-four range copies.

With NRI validation enabled, each cycle also attempts one excess set allocation,
one out-of-range update and one out-of-range copy. All twenty-four diagnostics
must be recognized before native execution, with subsequent exact output intact
and zero native validation errors/warnings. These checks cover completed-work
reuse; overlapping in-flight descriptor lifetimes remain a separate scenario.

### Validation mapping recovery

`NRIBufferValidationTests` compiles the actual `BufferVal.h` and `BufferVal.hpp`
against a controlled core interface. A backend returning null must leave the
buffer unmapped, permit an immediate retry, and reject an unmap until a map
succeeds. The same test checks nested maps, double unmaps, unbound buffers,
whole-buffer and last-byte access, and both offset and size addition overflow.
The native interface must not be called for rejected ranges. This is a test of
shared validation control flow; it does not emulate Vulkan cache coherency or
claim a real driver mapping failure.

### Mapped memory ranges

Explicitly bound host buffers retain the known native mapping end. Raw flush
and invalidate calls use the smallest complete atom interval that fits that
mapping, avoiding a WHOLE_SIZE range through unrelated allocations. VMA-backed
Memory objects use the mapped native block extent from VmaAllocationInfo2;
buffer-owned VMA allocations continue using VMA's own flush/invalidate helpers.
A final partial allocation atom still uses Vulkan WHOLE_SIZE when needed.
Wrapped buffers without extent metadata retain a conservative partial-tail
fallback; supplied native mappings must satisfy Vulkan atom and mapping rules.
No public struct or interface layout changes.

NRIMappedMemoryRangeTests covers twelve fixed cases: padded mappings, interior
and final atoms, subrange ends, absent wrapper extents and rounding overflow.
The memory fixture now uses padded direct/VMA allocations and nonzero binding
offsets, with whole-buffer, explicit full-buffer and partial-buffer maps.
NRI WHOLE_SIZE is zero; it is distinct from Vulkan VK_WHOLE_SIZE.

NRI_COMPATIBILITY_MAPPED_RANGES=ON forces the explicit-binding raw range path
on coherent host memory and records each requested native interval. The runner
flag --require-mapped-ranges requires 768 correctly bounded calls in 64 cycles,
plus 128 nonzero bindings (zero in the forced dedicated profile). Native
validation checks the calls; exact GPU readback still checks resource contents.
The switch defaults OFF. This is range instrumentation, not noncoherent cache
emulation or real noncoherent hardware acceptance. Dedicated allocation/map
failure recovery can be combined with this profile.

### Native wrapped memory and partial mappings

NRICompatibilityWrappedMemory runs 24 shared native-allocation cases: eight
caller-supplied partial mappings, eight finite partial mappings created by NRI,
and eight caller mappings declared with native VK_WHOLE_SIZE. Two NRI buffers
bind at different offsets inside each mapping. Exact GPU copies and a final
native remap check both payload bytes and all surrounding allocation padding.
Resources retire only after fence completion. After NRI wrapper destruction the
fixture flushes the still-live mapping, unmaps it, remaps the still-live native
allocation, and finally releases it through Vulkan. The native owner remains
responsible for unmapping/freeing wrapped memory, including a mapping initiated
by CreateMemoryVK when mappedMemory was not supplied.

The fixture resolves native entry points through NRI's wrapper dispatch. A bound
NRI buffer's address determines native allocation flags; the runner compares
this with the actual enabled feature manifest. Compatible host-visible native
memory is selected, preferring noncoherent memory if present; receipts record
its actual properties rather than inferring coherency from a requested location.

Use --fixture nri_vulkan_wrapped_memory with run_baseline.py. With
NRI_COMPATIBILITY_MAPPED_RANGES=ON and --require-mapped-ranges, the runner also
checks 192 requested intervals against the native mapping and binding manifests:
128 finite-extent calls and 64 unknown-extent calls. Unknown extents intentionally
retain the conservative final-atom fallback. This does not qualify arbitrary
wrapped BufferVK offsets absent from that public descriptor, overlapping in-flight
work, or noncoherent hardware when the selected memory is coherent.

### Overlapping in-flight resource generations

NRICompatibilityInflightResources reuses the validated Slang compute corpus.
Three independent slots hold command allocators/buffers, descriptor pools/sets,
uniform/storage/readback buffers and completion fences. Ninety-six submissions
wait for successive future values on a separate gate fence. A second graphics
queue releases one value at a time; no host-signal API is added.

The fixture proves the unreleased generations remain incomplete, validates all
64 compute records after completion, resets only that slot's allocator/pool,
and destroys/recreates its buffers and views while other slots remain pending.
It records 96 exact completions, 96 pool resets, 96 resource generations,
474 pending-value checks, a maximum of three pending slots, and 95 retirements
with other work still pending. Device idle occurs only at final/error cleanup;
accepted future waits are released before error cleanup to allow draining.
This tests pending work and completion-based reuse, not parallel shader execution
or performance. Host concurrency and actual memory pressure are separate cases.

Use --fixture nri_vulkan_inflight_resources with the standalone runner. This
fixture requires the Slang toolchain and two graphics queues on the selected
adapter, like the future-fence sequence fixture. That is a fixture requirement,
not a new Vulkan/engine device-admission minimum. Single-queue hardware needs its
own lifetime acceptance path; a masked modern device is not legacy hardware proof.

### Multiple swapchains and frame-slot reuse

On Windows, NRICompatibilityMultiPresentation uses two owned windows and one
graphics queue. Each swapchain has three independent allocator/command/acquire/
completion/readback slots and per-image present semaphores. Frames submit and
present immediately; the CPU waits and checks the exact image only when reusing
a slot or retiring its chain. No queue/device idle occurs between ordinary frames.

The bounded workload submits 800 frames, reuses 497 slots, and resizes/recreates
100 chains (50 per window), retaining the other chain each time. It alternates
VSYNC and tearing requests, verifies resized extents and every pixel, and
exercises 52 queue-idle and 50 device-idle retirements including cleanup. Its
six outstanding slots count submitted work whose readback has not yet been
verified; they do not assert six simultaneous GPU executions or incomplete fences.
The separate in-flight fixture proves incomplete work behind future dependencies.
This workload also tests resource reuse with only one requested graphics queue.

Use --fixture nri_vulkan_multi_presentation. On a maintenance1 profile, add
--require-present-retirement to require all 800 payloads retired, none pending,
and at most eight tracked native fences (eight presents between retirements).
On the baseline 1.1 profile, combine --require-renderpass1 --require-legacy-wsi
with the no-timeline/no-host-reset flags: KHR_swapchain is the only enabled
device extension. Without maintenance1, the existing idle-based teardown remains
a platform qualification limitation, not equivalent portable retirement proof.

Resizes are deliberate and occur after retirement. Unexpected surface loss fails
this exact-accounting fixture; injected acquire/present loss and recovery remain
covered by NRICompatibilityPresentation. This is desktop WSI evidence, not Android
lifecycle coverage, a performance result, or a long-duration soak.
