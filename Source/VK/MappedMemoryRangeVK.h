// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

namespace nri {

// Inputs describe a valid buffer range in persistently mapped memory. A zero
// mappedEnd means the wrapper did not supply its mapping extent. Prefer the
// smallest atom-complete range; WHOLE_SIZE is needed only when rounding would
// extend beyond the known mapping (or beyond the buffer when extent is unknown).
static inline VkMappedMemoryRange BuildMappedMemoryRange(VkDeviceMemory memory, uint64_t memoryOffset, uint64_t bufferSize, uint64_t mappedEnd, uint64_t atomSize, uint64_t offset, uint64_t size) {
    uint64_t rangeBegin = memoryOffset + offset;
    uint64_t rangeEnd = rangeBegin + size;
    uint64_t endBound = mappedEnd ? mappedEnd : memoryOffset + bufferSize;
    uint64_t padding = (0 - rangeEnd) & (atomSize - 1);

    VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory;
    range.offset = rangeBegin & ~(atomSize - 1);
    range.size = (rangeEnd <= endBound && padding <= (endBound - rangeEnd)) ? rangeEnd + padding - range.offset : VK_WHOLE_SIZE;

    return range;
}

} // namespace nri
