// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "MappedMemoryRangeVK.h"

#include <cstdio>
#include <limits>

int main() {
    struct Case {
        const char* name;
        uint64_t memoryOffset, bufferSize, mappedEnd, atomSize, offset, size, expectedOffset, expectedSize;
    };
    const Case cases[] = {
        {"buffer end inside larger mapping", 256, 128, 4096, 64, 127, 1, 320, 64},
        {"aligned buffer end without extent", 256, 128, 0, 64, 127, 1, 320, 64},
        {"partial final atom inside mapping", 256, 100, 4096, 64, 99, 1, 320, 64},
        {"unknown partial final atom", 0, 100, 0, 64, 99, 1, 64, VK_WHOLE_SIZE},
        {"partial allocation final atom", 0, 100, 100, 64, 99, 1, 64, VK_WHOLE_SIZE},
        {"mapped subrange end", 256, 64, 320, 64, 5, 59, 256, 64},
        {"unaligned interior range", 256, 1024, 4096, 64, 5, 500, 256, 512},
        {"single interior byte", 256, 1024, 4096, 64, 66, 1, 320, 64},
        {"whole logical buffer excludes neighbors", 256, 1024, 8192, 64, 0, 1024, 256, 1024},
        {"large VMA block offset", 65536, 100, 1048576, 256, 0, 100, 65536, 256},
        {"atom size one", 17, 33, 64, 1, 3, 7, 20, 7},
        {"rounding near integer limit", 0, std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint64_t>::max(), 64, std::numeric_limits<uint64_t>::max() - 3, 2, std::numeric_limits<uint64_t>::max() - 63, VK_WHOLE_SIZE},
    };
    uint32_t failures = 0;

    for (const auto& c : cases) {
        auto range = nri::BuildMappedMemoryRange(VK_NULL_HANDLE, c.memoryOffset, c.bufferSize, c.mappedEnd, c.atomSize, c.offset, c.size);

        if (range.sType != VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE || range.pNext != nullptr || range.offset != c.expectedOffset || range.size != c.expectedSize) {
            std::fprintf(stderr, "FAILED: %s: offset=%llu size=%llu\n", c.name, (unsigned long long)range.offset, (unsigned long long)range.size);
            failures++;
        }
    }

    std::printf("Mapped memory range tests: %u cases, %u failures\n", uint32_t(sizeof(cases) / sizeof(cases[0])), failures);

    return failures ? 1 : 0;
}
