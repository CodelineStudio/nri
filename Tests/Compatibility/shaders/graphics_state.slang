// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
struct DrawParameters {
    float depth;
    float opacity;
    uint flags;
    int baseVertex;
};

[[vk::push_constant]] ConstantBuffer<DrawParameters> drawParameters;
[[vk::binding(0, 0)]] Texture2DArray<float4> image;
[[vk::binding(1, 0)]] SamplerState imageSampler;

struct Varying {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Varying buildStateVertex(float2 position, float2 uv, float2 instanceData, uint vertexIndex, uint instanceIndex, bool valid) {
    uint corner = uint(position.x > 0) + uint(position.y > 0) * 2;
    valid = valid && vertexIndex == corner + 2;
    valid = valid && ((instanceIndex == 1 && all(instanceData == float2(-0.5, 1))) ||
                      (instanceIndex == 2 && all(instanceData == float2(0.5, -1))));
    Varying result;
    result.position = valid ? float4(position.x * 0.5 + instanceData.x, position.y, drawParameters.depth, 1) : float4(9, 9, 0, 1);
    result.uv = float2(uv.x * instanceData.y + (instanceData.y < 0 ? 1 : 0), uv.y);
    return result;
}

[shader("vertex")]
Varying stateVertexMain(float2 position : POSITION, float2 uv : TEXCOORD0, float2 instanceData : TEXCOORD1,
                       uint vertexIndex : SV_VulkanVertexID, uint instanceIndex : SV_VulkanInstanceID) {
    return buildStateVertex(position, uv, instanceData, vertexIndex, instanceIndex, true);
}

[shader("vertex")]
Varying stateParametersVertexMain(float2 position : POSITION, float2 uv : TEXCOORD0, float2 instanceData : TEXCOORD1,
                                 int vertexId : SV_VertexID, uint instanceId : SV_InstanceID,
                                 int baseVertex : SV_StartVertexLocation, uint baseInstance : SV_StartInstanceLocation,
                                 uint drawIndex : SV_DrawIndex) {
    uint corner = uint(position.x > 0) + uint(position.y > 0) * 2;
    int expectedLocalVertex = int(corner) + (drawParameters.baseVertex > 0 ? 1 : 3);
    uint expectedLocalInstance = instanceData.x < 0 ? 0 : 1;
    bool valid = vertexId == expectedLocalVertex && instanceId == expectedLocalInstance &&
                 baseVertex == drawParameters.baseVertex && baseInstance == 1 && drawIndex == 0;
    return buildStateVertex(position, uv, instanceData, uint(vertexId + baseVertex), instanceId + baseInstance, valid);
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
