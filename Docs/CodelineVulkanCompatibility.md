# Vulkan 1.1+ compatibility workstream

Status: partial implementation; full Vulkan 1.1 compatibility is not implemented
or qualified yet. Legacy barrier, timestamp and queue-submission paths are
implemented with native or emulated timeline fences; see `Tests/Compatibility` for the
feature-disabled fixture. Discovery and owned-device initialization now negotiate
Vulkan1.1+, with individual feature/property structures and version/extension
selected dispatch. Fixed descriptors, legacy format queries and subgroup/buffer
limit normalization have basic-profile coverage on a modern device.

Renderpass1 now supports the existing single-subpass cache, color/input/depth
attachments, color resolves, multiview and dependencies. Legacy input attachment
layouts use GENERAL consistently in barriers, descriptors and pipeline/render-pass
descriptions. Depth-only attachments do not request nonexistent stencil clears.
The fixture verifies clear/load/store and rectangular clears, input-attachment
layouts, and two-view 4x MSAA color resolve with a depth attachment on renderpass1,
renderpass2 and native dynamic rendering. Shader execution is still pending.

Owned devices without timeline semaphores now use `LegacyQueueSchedulerVK`.
It retains FIFO order per physical queue, copies accepted submission arrays,
defers unresolved future waits, and uses VkFence completion plus binary semaphore
dependencies. Binary payload splitting preserves fanout and repeated waits;
cross-queue signals retain transitive memory dependencies. Completed old points
are retired while the newest completed anchor remains reusable. No ordinary
submission uses a queue-idle substitute. Host waits release the scheduler mutex.
Native timeline paths remain selected when that feature is enabled.

Tests cover transfer/render readback, future waits, two consumers, concurrent host
waits, partial host query resets, 128 alternating-queue retirement cycles and an
injected submit failure after partial native submission. Masked API1.1 runs remove
the timeline extension entirely; a modern sync2/no-timeline combination is also
covered. Shared timeline handles, wrapped no-timeline devices and low-latency
timeline interop are unsupported on the emulated path. WSI integration has source
changes for deferred work and acquisition locking, but runtime WSI acceptance,
real device-loss handling, native interop and broader stress remain pending.

**Temporary implementation prerequisite remaining:** host query reset. Device
creation reports its missing fallback explicitly. A synchronous reset on an
ordinary graphics queue is insufficient: the two-queue fixture demonstrates a
legal host-reset call while that queue awaits a future producer.
The modern GPU requesting API1.1 is not a real older-driver acceptance result.
Baseline upstream: `a20efc8f7c6e4e6045acd6f29bf90b0d26ac3df5`.
Fork branch: `codeline/vulkan-1.1`. Vulkan 1.0 and GLES are outside this work.

## Contract

Provide a real Vulkan 1.1 baseline without requiring synchronization2 or timeline
semaphores. Preserve native modern paths and existing other backends. Optional
features/limits remain explicit: support does not imply all NRI optional
operations are available on every device. Do not silently discard unsupported
work, falsify capabilities or rely on externally installed development layers.

## Planned sequence

1. Audit discovery and device/wrapper creation, legal feature/property chains,
   function aliases and dependency minimums. Build an independent graphics,
   compute, readback and WSI fixture harness with version/feature receipts.
2. Negotiate 1.1+ throughout adapter enumeration and device creation. Replace
   unconditional Vulkan11/12 aggregate structures (introduced in1.2) with legal
   individual structures where needed. Make dispatch reflect enabled features.
3. Add legacy barrier/timestamp/submission translation and a formally specified
   value-fence fallback. Test waits-before-signals, repeated waits, fan-out,
   cross-queue ordering, CPU waits, completion, destruction and device loss.
4. Qualify renderpass1 drawing/input reads and the existing descriptor/format
   fallbacks; complete query reset, memory flags/VMA negotiation,
   optional indirect/address operations and presentation extension fallbacks.
5. Qualify native1.1, extension-enabled1.1,1.2 and modern profiles. Test actual
   older Android devices and modern drivers with legacy paths forced, plus other
   backend regression fixtures. Keep missing hardware pending.

Core1.1 already supplies properties2, memory-requirements2, bind-memory2 and
negative viewport support: do not add unnecessary Vulkan1.0 emulation.
Shaders targeting the baseline use SPIR-V1.3 and enabled capabilities only;
run `spirv-val --target-env vulkan1.1` and readback/pixel tests.

## Evidence and release

For every fixture record source/dependency/compiler versions, platform, device,
driver, loader/API version, features/extensions, selected paths, command,
duration, output, timeout/errors and validation log. Separate implementation,
build, masked-device and actual-device verification. No release support claim
until the required hardware matrix passes. Keep upstream changes reviewable and
run both compatibility and modern regressions before updating an accepted pin.

The consumer integration plan and full progress ledger live in the parent
NextMU repository when this fork is used there; this public document records
only the reusable dependency's contract.
