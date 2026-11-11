// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#pragma once
#include "NRI.h"

struct PipelineCacheRun {
    nri::PipelineCache* cache = nullptr;
    uint64_t loadedBytes = 0, savedBytes = 0;
    uint32_t rejectedHeaders = 0;
    bool control = false, requireHit = false;
    double createMilliseconds = 0, pipelineMilliseconds = 0;
};

bool LoadPipelineCache(nri::Device& device, const nri::CoreInterface& core, const char* input, bool requireHit, PipelineCacheRun& run);
bool SavePipelineCache(nri::Device& device, const nri::CoreInterface& core, const char* output, PipelineCacheRun& run);
