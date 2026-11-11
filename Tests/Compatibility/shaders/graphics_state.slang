// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
struct DrawParameters {
    float depth;
    float opacity;
    uint flags;
    uint padding;
};

[[vk::push_constant]] ConstantBuffer<DrawParameters> drawParameters;
[[vk::binding(0, 0)]] Texture2DArray<float4> image;
[[vk::binding(1, 0)]] SamplerState imageSampler;

struct Varying {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Varying stateVertexMain(float2 position : POSITION, float2 uv : TEXCOORD0, float2 instanceData : TEXCOORD1) {
    Varying result;
    result.position = float4(position.x * 0.5 + instanceData.x, position.y, drawParameters.depth, 1);
    result.uv = float2(uv.x * instanceData.y + (instanceData.y < 0 ? 1 : 0), uv.y);
    return result;
}

[shader("fragment")]
float4 stateFragmentMain(Varying input) : SV_Target0 {
    if ((drawParameters.flags & 2) != 0)
        return float4(1, 0, 1, 1);
    float4 result = image.SampleLevel(imageSampler, float3(input.uv, 0), 0);
    result.a *= drawParameters.opacity;
    if ((drawParameters.flags & 1) != 0)
        result.rgb *= result.a;
    return result;
}
