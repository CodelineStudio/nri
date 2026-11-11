// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#pragma once
#include "NRI.h"
#include "pipeline_cache_io.h"
#include <atomic>

bool RunGraphicsStates(nri::Device& device, const nri::CoreInterface& core, nri::Queue& queue, const std::atomic<uint32_t>& errors, PipelineCacheRun& cache, uint32_t& completedCases, uint32_t& maxChannelError, uint32_t& parameterCases, uint32_t& indirectCases, uint32_t& multiParameterCases);
