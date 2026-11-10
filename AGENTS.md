# Codeline NRI fork guidance

This fork extends NRI to Vulkan **1.1 and newer** while retaining modern paths.
The initial Vulkan 1.0 proposal was superseded; do not implement a 1.0 baseline.
Read `Docs/CodelineVulkanCompatibility.md` before compatibility changes.

- Preserve upstream API/ABI/style and license notices. Do not add consumer engine
  headers, assets or private code to this reusable public dependency.
- Use `codeline/vulkan-1.1` for compatibility changes. Keep upstream `main` intact
  as the comparison baseline. Do not rewrite unrelated history or auto-update pins.
- Prove legal version/extension/feature negotiation, dispatch, pNext structures,
  synchronization and object lifetimes. Lowering a version check is not support.
- Vulkan 1.1 without synchronization2 and timeline extensions is required.
  Native modern paths remain preferred when genuinely supported and enabled.
- Preserve value-based fence semantics; binary waits consume semaphores and
  cannot naively emulate repeated timeline waits. Reject unsupported optional
  requests explicitly rather than dropping work or advertising false support.
- Require standalone tests, validation-clean execution, masked-path differential
  tests and real older-device receipts before a compatibility release claim.
- Record exact source/compiler/device/driver/features and pending hardware gaps.
  Successful compilation or a modern masked device is not legacy hardware proof.
- When used as the NextMU submodule, the consumer's authoritative checkpoint is
  `docs/graphics-migration/progress.json` in the parent repository. For standalone
  work record reproducible results beside the relevant compatibility change.
