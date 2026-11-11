// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "pipeline_cache_io.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {

constexpr uint64_t MAX_FIXTURE_CACHE_BYTES = 128 * 1024 * 1024;

static bool Check(bool valid, const char* operation) {
    if (!valid)
        std::fprintf(stderr, "Pipeline cache qualification failed: %s\n", operation);

    return valid;
}

} // namespace

bool LoadPipelineCache(nri::Device& device, const nri::CoreInterface& core, const char* input, bool requireHit, PipelineCacheRun& run) {
    run.control = core.GetDeviceDesc(device).features.pipelineCacheControl;
    run.requireHit = requireHit;

    if (!Check(core.GetDeviceDesc(device).features.pipelineCache && (!requireHit || run.control), "required cache capabilities"))
        return false;

    std::vector<uint8_t> bytes;

    if (input) {
        std::ifstream file(input, std::ios::binary | std::ios::ate);
        auto size = file.tellg();

        if (!Check(file && size >= 32 && uint64_t(size) <= MAX_FIXTURE_CACHE_BYTES, "input size"))
            return false;

        bytes.resize(size_t(size));
        file.seekg(0);
        file.read((char*)bytes.data(), size);

        if (!Check(bool(file), "read input"))
            return false;

        run.loadedBytes = bytes.size();
    }

    nri::PipelineCacheDesc desc = {bytes.empty() ? nullptr : bytes.data(), bytes.size()};
    auto begin = std::chrono::steady_clock::now();
    nri::Result result = core.CreatePipelineCache(device, desc, run.cache);
    run.createMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();

    return Check(result == nri::Result::SUCCESS && run.cache, "create from compatible input");
}

bool SavePipelineCache(nri::Device& device, const nri::CoreInterface& core, const char* output, PipelineCacheRun& run) {
    uint64_t size = 0;

    if (!Check(core.GetPipelineCacheData(*run.cache, nullptr, size) == nri::Result::SUCCESS && size >= 32 && size <= MAX_FIXTURE_CACHE_BYTES, "query data size"))
        return false;

    std::vector<uint8_t> bytes(size_t(size), uint8_t(0));

    if (!Check(core.GetPipelineCacheData(*run.cache, bytes.data(), size) == nri::Result::SUCCESS && size >= 32 && size <= bytes.size(), "read data"))
        return false;

    bytes.resize(size_t(size));
    run.savedBytes = size;

    // Rejections must happen before native creation: fabricated/stale headers
    // are not legal vkCreatePipelineCache inputs. The opaque payload is intact.
    const uint32_t offsets[] = {0, 4, 8, 12, 16, 31};

    for (uint32_t offset : offsets) {
        bytes[offset] ^= 1;
        nri::PipelineCache* rejected = nullptr;
        nri::PipelineCacheDesc desc = {bytes.data(), bytes.size()};
        nri::Result result = core.CreatePipelineCache(device, desc, rejected);
        bytes[offset] ^= 1;

        if (!Check(result == nri::Result::OUT_OF_DATE && !rejected, "stale header rejection")) {
            if (rejected)
                core.DestroyPipelineCache(rejected);

            return false;
        }

        run.rejectedHeaders++;
    }

    nri::PipelineCache* rejected = nullptr;
    nri::PipelineCacheDesc shortDesc = {bytes.data(), 31};

    if (!Check(core.CreatePipelineCache(device, shortDesc, rejected) == nri::Result::OUT_OF_DATE && !rejected, "short header rejection")) {
        if (rejected)
            core.DestroyPipelineCache(rejected);

        return false;
    }

    run.rejectedHeaders++;

    if (output) {
        // Raw fixture artifact only. A consumer cache must add checksums,
        // compatibility metadata and atomic publication before persisting it.
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        file.write((const char*)bytes.data(), std::streamsize(bytes.size()));
        file.close();

        if (!Check(bool(file), "write output"))
            return false;
    }

    return true;
}
