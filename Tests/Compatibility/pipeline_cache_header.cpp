// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "PipelineCacheHeaderVK.h"

#include <array>
#include <cstdio>

int main() {
    VkPhysicalDeviceProperties properties = {};
    properties.vendorID = 0x12345678;
    properties.deviceID = 0x90abcdef;
    std::array<uint8_t, 34> storage{};
    uint8_t* header = storage.data() + 1; // deliberately unaligned
    const uint32_t words[] = {32, 1, properties.vendorID, properties.deviceID};

    for (uint32_t i = 0; i < 4; i++) {
        for (uint32_t byte = 0; byte < 4; byte++)
            header[i * 4 + byte] = uint8_t(words[i] >> (byte * 8));
    }

    for (uint32_t i = 0; i < VK_UUID_SIZE; i++)
        header[16 + i] = properties.pipelineCacheUUID[i] = uint8_t(0x80 + i);

    if (!nri::IsPipelineCacheCompatible(header, 32, properties) || !nri::IsPipelineCacheCompatible(header, 33, properties) || nri::IsPipelineCacheCompatible(nullptr, 32, properties))
        return 1;

    for (uint32_t size = 0; size < 32; size++) {
        if (nri::IsPipelineCacheCompatible(header, size, properties))
            return 2;
    }

    for (uint32_t i = 0; i < 32; i++) {
        header[i] ^= 1;

        if (nri::IsPipelineCacheCompatible(header, 32, properties))
            return 3;

        header[i] ^= 1;
    }

    std::puts("Pipeline cache header: unaligned little-endian input, truncation, version, vendor, device and UUID checks passed.");

    return 0;
}
