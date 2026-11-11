// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT

#include "LegacySynchronizationVK.h"

using namespace nri;

// These are specification-level expectations, including bits that a narrowing
// cast would lose. They run in Release as compile-time checks.
static_assert(GetLegacyPipelineStages(0, true) == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
static_assert(GetLegacyPipelineStages(0, false) == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_COPY_BIT, true) == VK_PIPELINE_STAGE_TRANSFER_BIT);
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_CLEAR_BIT, true) == VK_PIPELINE_STAGE_TRANSFER_BIT);
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_RESOLVE_BIT, true) == VK_PIPELINE_STAGE_TRANSFER_BIT);
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_BLIT_BIT, true) == VK_PIPELINE_STAGE_TRANSFER_BIT);
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, false) == VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, false) == VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT, true) == VK_PIPELINE_STAGE_TRANSFER_BIT);
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, false) == VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, false) == (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT));
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, true) == (VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT));
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, false) == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
static_assert(GetLegacyPipelineStages(VkPipelineStageFlags2(1) << 63, true) == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
static_assert(GetLegacyPipelineStages(VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, true) == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
static_assert(GetLegacyAccess(0) == 0);
static_assert(GetLegacyAccess(VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT) == (VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT));
static_assert(GetLegacyAccess(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_BINDING_TABLE_READ_BIT_KHR) == VK_ACCESS_SHADER_READ_BIT);
static_assert(GetLegacyAccess(VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) == VK_ACCESS_SHADER_WRITE_BIT);
static_assert(GetLegacyAccess(VkAccessFlags2(1) << 63) == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
static_assert(GetLegacyAccess(VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR) == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));

int main() {
    return 0;
}
