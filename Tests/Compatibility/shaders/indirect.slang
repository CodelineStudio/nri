// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "common.slang"
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> arguments;

[numthreads(32, 1, 1)]
[shader("compute")]
void argumentsMain(uint3 localId : SV_GroupThreadID) {
    uint previous;
    InterlockedAdd(arguments[0], 1, previous);
    DeviceMemoryBarrierWithGroupSync();

    if (localId.x == 0) {
        uint count = arguments[0];
        arguments[3] = transformValue(0);
        arguments[4] = count / 16;
        arguments[5] = arguments[6] = 1;
        arguments[8] = count - 26;
        arguments[9] = arguments[10] = 1;
        arguments[11] = 0;
        arguments[16] = count - 26;
        arguments[17] = 1;
        arguments[18] = 2;
        arguments[19] = 1;
        arguments[20] = 0;
    }
}
