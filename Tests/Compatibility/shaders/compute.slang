// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "common.slang"
[[vk::binding(1, 0)]] RWStructuredBuffer<uint4> results;
groupshared uint neighbors[32];

[numthreads(32, 1, 1)]
[shader("compute")]
void computeMain(uint3 dispatchId : SV_DispatchThreadID, uint3 localId : SV_GroupThreadID) {
    uint index = dispatchId.x;
    neighbors[localId.x] = transformValue(index);
    GroupMemoryBarrierWithGroupSync();
    if (index < parameters.count) {
        float4 transformed = mul(parameters.transform, float4(float(index % 4), 1, 0, 1));
        results[index] = uint4(neighbors[localId.x], neighbors[(localId.x + 1) % 32], asuint(transformed.x), index ^ parameters.nonce);
    }
}
