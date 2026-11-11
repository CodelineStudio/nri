// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
struct Parameters {
    uint count;
    uint multiplier;
    uint bias;
    uint nonce;
    float4x4 transform;
};

[[vk::binding(0, 0)]] ConstantBuffer<Parameters> parameters;

uint transformValue(uint index) {
    uint value = index * parameters.multiplier + parameters.bias;
    value ^= value >> 16;
    return value * 0x85ebca6bu;
}
