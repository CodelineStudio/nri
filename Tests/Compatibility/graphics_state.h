// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#pragma once
#include "NRI.h"
#include <atomic>

bool RunGraphicsStates(nri::Device& device, const nri::CoreInterface& core, nri::Queue& queue, const std::atomic<uint32_t>& errors, uint32_t& completedCases, uint32_t& maxChannelError);
