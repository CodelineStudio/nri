// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "common.slang"
[[vk::binding(1, 0)]] StructuredBuffer<uint4> results;

[shader("vertex")]
float4 vertexMain(float2 position : POSITION) : SV_Position {
    return mul(parameters.transform, float4(position, 0, 1));
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target0 {
    uint index = (uint(position.x) + uint(position.y) * 64) % parameters.count;
    uint value = results[index].x;
    return float4(float(value & 1), float((value >> 1) & 1), float((value >> 2) & 1), 1);
}
