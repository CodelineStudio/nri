// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstring>
#include <vulkan/vulkan.h>

namespace nri {

static inline uint32_t ReadPipelineCacheHeaderWord(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

// Vulkan specifies a packed 32-byte little-endian header, independent of host
// byte order/alignment. This checks native compatibility, not payload integrity.
static inline bool IsPipelineCacheCompatible(const void* data, uint64_t size, const VkPhysicalDeviceProperties& properties) {
    if (!data || size < 32)
        return false;

    const auto* bytes = (const uint8_t*)data;

    return ReadPipelineCacheHeaderWord(bytes) == 32
        && ReadPipelineCacheHeaderWord(bytes + 4) == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        && ReadPipelineCacheHeaderWord(bytes + 8) == properties.vendorID
        && ReadPipelineCacheHeaderWord(bytes + 12) == properties.deviceID
        && std::memcmp(bytes + 16, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

} // namespace nri
