// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "graphics_state.h"
#include "compiled_shaders.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

struct DrawParameters {
    float depth, opacity;
    uint32_t flags, padding;
};
static_assert(sizeof(DrawParameters) == 16, "Push constant ABI");

static float Texel(int x, int y, uint32_t channel) {
    x = std::clamp(x, 0, 3);
    y = std::clamp(y, 0, 3);

    return float(channel == 0 ? x % 2 : (channel == 1 ? y % 2 : (x + y) % 2));
}

static float Sample(float u, float v, uint32_t channel, bool linear) {
    if (!linear)
        return Texel(int(std::floor(u * 4)), int(std::floor(v * 4)), channel);

    float x = u * 4 - 0.5f, y = v * 4 - 0.5f;
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - float(ix), fy = y - float(iy);
    float top = Texel(ix, iy, channel) * (1 - fx) + Texel(ix + 1, iy, channel) * fx;
    float bottom = Texel(ix, iy + 1, channel) * (1 - fx) + Texel(ix + 1, iy + 1, channel) * fx;

    return top * (1 - fy) + bottom * fy;
}

struct StateCase {
    nri::Device& device;
    const nri::CoreInterface& core;
    nri::Queue& queue;
    PipelineCacheRun& cache;
    uint8_t samples;
    bool linear, premultiplied;
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::Fence* fence = nullptr;
    nri::DescriptorPool* pool = nullptr;
    nri::DescriptorSet* set = nullptr;
    nri::PipelineLayout* layout = nullptr;
    std::array<nri::Pipeline*, 2> pipelines{};
    // Color, resolve (also allocated for 1x), depth/stencil, sampled mip/layer.
    std::array<nri::Texture*, 4> images{};
    std::array<nri::Descriptor*, 5> views{};
    // Texture upload, vertices, instances, indices, image readback.
    std::array<nri::Buffer*, 5> buffers{};
    uint32_t uploadRow = 0, uploadSlice = 0, readRow = 0, readSlice = 0;
    nri::Format depthFormat = nri::Format::D24_UNORM_S8_UINT;

    bool Check(bool valid, const char* operation) {
        if (!valid)
            std::fprintf(stderr, "Graphics state failed (%ux, linear=%u, premultiplied=%u): %s\n", samples, linear, premultiplied, operation);

        return valid;
    }

    bool Initialize() {
        const auto& alignment = core.GetDeviceDesc(device).memoryAlignment;
        uploadRow = ((16 + alignment.uploadBufferTextureRow - 1) / alignment.uploadBufferTextureRow) * alignment.uploadBufferTextureRow;
        uploadSlice = ((uploadRow * 4 + alignment.uploadBufferTextureSlice - 1) / alignment.uploadBufferTextureSlice) * alignment.uploadBufferTextureSlice;
        readRow = ((256 + alignment.uploadBufferTextureRow - 1) / alignment.uploadBufferTextureRow) * alignment.uploadBufferTextureRow;
        readSlice = ((readRow * 64 + alignment.uploadBufferTextureSlice - 1) / alignment.uploadBufferTextureSlice) * alignment.uploadBufferTextureSlice;

        if (!(core.GetFormatSupport(device, depthFormat) & nri::FormatSupportBits::DEPTH_STENCIL_ATTACHMENT))
            depthFormat = nri::Format::D32_SFLOAT_S8_UINT;

        if (!Check(core.CreateCommandAllocator(queue, allocator) == nri::Result::SUCCESS, "allocator") ||
            !Check(core.CreateCommandBuffer(*allocator, command) == nri::Result::SUCCESS, "command") ||
            !Check(core.CreateFence(device, nri::FenceDesc{}, fence) == nri::Result::SUCCESS, "fence"))
            return false;

        const uint64_t sizes[] = {uploadSlice, 8 * 4 * sizeof(float), 3 * 2 * sizeof(float), 8 * sizeof(uint16_t), readSlice};

        for (uint32_t i = 0; i < buffers.size(); i++) {
            nri::BufferDesc desc = {};
            desc.size = sizes[i];

            if (i == 1 || i == 2)
                desc.usage = nri::BufferUsageBits::VERTEX;
            else if (i == 3)
                desc.usage = nri::BufferUsageBits::INDEX;

            if (!Check(core.CreateCommittedBuffer(device, i == 4 ? nri::MemoryLocation::HOST_READBACK : nri::MemoryLocation::HOST_UPLOAD, 0.0f, desc, buffers[i]) == nri::Result::SUCCESS, "buffer"))
                return false;
        }

        // Prefix/suffix sentinels keep incorrect offsets in bounds and visibly wrong.
        const float vertices[] = {9, 9, 0, 0, 9, 9, 0, 0, -1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, 1, 1, 1, 0, 9, 9, 0, 0, 9, 9, 0, 0};
        const float instances[] = {9, 0, -0.5f, 1, 0.5f, -1};
        const uint16_t positive[] = {1, 1, 1, 2, 3, 3, 2, 4};
        const uint16_t negative[] = {1, 1, 3, 4, 5, 5, 4, 6};
        const uint16_t* indices = premultiplied ? negative : positive;
        const void* mesh[] = {vertices, instances, indices};

        for (uint32_t i = 0; i < 4; i++) {
            auto* mapped = (uint8_t*)core.MapBuffer(*buffers[i], 0, sizes[i]);

            if (!Check(mapped != nullptr, "upload mapping"))
                return false;

            if (i != 0)
                std::memcpy(mapped, mesh[i - 1], size_t(sizes[i]));
            else {
                std::memset(mapped, 0, size_t(sizes[i]));

                for (uint32_t y = 0; y < 4; y++) {
                    for (uint32_t x = 0; x < 4; x++) {
                        for (uint32_t c = 0; c < 3; c++)
                            mapped[y * uploadRow + x * 4 + c] = uint8_t(Texel(x, y, c) * 255);

                        mapped[y * uploadRow + x * 4 + 3] = 255;
                    }
                }
            }

            core.UnmapBuffer(*buffers[i]);
        }

        for (uint32_t i = 0; i < images.size(); i++) {
            nri::TextureDesc desc = {};
            desc.type = nri::TextureType::TEXTURE_2D;
            desc.width = desc.height = i == 3 ? 8 : 64;
            desc.depth = 1;
            desc.mipNum = desc.layerNum = i == 3 ? 2 : 1;
            desc.sampleNum = (i == 0 || i == 2) ? samples : 1;
            desc.format = i == 2 ? depthFormat : nri::Format::RGBA8_UNORM;
            desc.usage = i == 3 ? nri::TextureUsageBits::SHADER_RESOURCE : (i == 2 ? nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT : nri::TextureUsageBits::COLOR_ATTACHMENT);

            if (!Check(core.CreateCommittedTexture(device, nri::MemoryLocation::DEVICE, 0.0f, desc, images[i]) == nri::Result::SUCCESS, "texture"))
                return false;

            nri::TextureViewDesc view = {};
            view.texture = images[i];
            view.type = i == 3 ? nri::TextureView::TEXTURE_ARRAY : (i == 2 ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT);
            view.layout = i == 3 ? nri::Layout::SHADER_RESOURCE : (i == 2 ? nri::Layout::DEPTH_STENCIL_ATTACHMENT : nri::Layout::COLOR_ATTACHMENT);
            view.planes = i == 2 ? nri::PlaneBits::DEPTH | nri::PlaneBits::STENCIL : nri::PlaneBits::COLOR;
            view.mipNum = view.layerNum = 1;
            view.mipOffset = view.layerOffset = i == 3 ? 1 : 0;

            if (!Check(core.CreateTextureView(view, views[i]) == nri::Result::SUCCESS, "texture view"))
                return false;
        }

        nri::SamplerDesc sampler = {};
        sampler.filters.min = sampler.filters.mag = linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
        sampler.addressModes = {nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE};

        if (!Check(core.CreateSampler(device, sampler, views[4]) == nri::Result::SUCCESS, "sampler"))
            return false;

        nri::DescriptorRangeDesc ranges[2] = {};
        ranges[0].descriptorType = nri::DescriptorType::TEXTURE;
        ranges[1].descriptorType = nri::DescriptorType::SAMPLER;
        ranges[1].baseRegisterIndex = 1;
        ranges[0].descriptorNum = ranges[1].descriptorNum = 1;
        ranges[0].shaderStages = ranges[1].shaderStages = nri::StageBits::FRAGMENT_SHADER;
        nri::DescriptorSetDesc setDesc = {};
        setDesc.ranges = ranges;
        setDesc.rangeNum = 2;
        nri::RootConstantDesc root = {};
        root.size = sizeof(DrawParameters);
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 1;
        layoutDesc.rootConstants = &root;
        layoutDesc.rootConstantNum = 1;
        layoutDesc.descriptorSets = &setDesc;
        layoutDesc.descriptorSetNum = 1;
        layoutDesc.shaderStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER;
        layoutDesc.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum = poolDesc.textureMaxNum = poolDesc.samplerMaxNum = 1;

        if (!Check(core.CreatePipelineLayout(device, layoutDesc, layout) == nri::Result::SUCCESS, "layout") ||
            !Check(core.CreateDescriptorPool(device, poolDesc, pool) == nri::Result::SUCCESS, "pool") ||
            !Check(core.AllocateDescriptorSets(*pool, *layout, 0, &set, 1, 0) == nri::Result::SUCCESS, "set"))
            return false;

        for (uint32_t i = 0; i < 2; i++) {
            nri::UpdateDescriptorRangeDesc update = {};
            update.descriptorSet = set;
            update.rangeIndex = i;
            update.descriptors = &views[i + 3];
            update.descriptorNum = 1;
            core.UpdateDescriptorRanges(&update, 1);
        }

        nri::VertexAttributeDesc attributes[3] = {};

        for (uint32_t i = 0; i < 3; i++) {
            attributes[i].vk.location = i;
            attributes[i].format = nri::Format::RG32_SFLOAT;
            attributes[i].streamIndex = i == 2 ? 1 : 0;
            attributes[i].offset = i == 1 ? 8 : 0;
        }

        nri::VertexStreamDesc streams[2] = {};
        streams[0].stride = 16;
        streams[1].bindingSlot = 1;
        streams[1].stride = 8;
        streams[1].stepRate = nri::VertexStreamStepRate::PER_INSTANCE;
        nri::VertexInputDesc input = {};
        input.attributes = attributes;
        input.attributeNum = 3;
        input.streams = streams;
        input.streamNum = 2;
        nri::MultisampleDesc multisample = {};
        multisample.sampleNum = samples;
        multisample.sampleMask = nri::ALL;
        nri::ShaderDesc shaders[] = {
            {nri::StageBits::VERTEX_SHADER, shader_fixture::state_vertex, sizeof(shader_fixture::state_vertex), "stateVertexMain"},
            {nri::StageBits::FRAGMENT_SHADER, shader_fixture::state_fragment, sizeof(shader_fixture::state_fragment), "stateFragmentMain"}
        };

        for (uint32_t i = 0; i < pipelines.size(); i++) {
            nri::ColorAttachmentDesc color = {};
            color.format = nri::Format::RGBA8_UNORM;
            color.colorWriteMask = i == 0 ? nri::ColorWriteBits::NONE : nri::ColorWriteBits::RGBA;
            color.blendEnabled = i != 0;
            color.colorBlend = {premultiplied ? nri::BlendFactor::ONE : nri::BlendFactor::SRC_ALPHA, nri::BlendFactor::ONE_MINUS_SRC_ALPHA, nri::BlendOp::ADD};
            color.alphaBlend = {nri::BlendFactor::ONE, nri::BlendFactor::ONE_MINUS_SRC_ALPHA, nri::BlendOp::ADD};
            nri::GraphicsPipelineDesc desc = {};
            desc.pipelineLayout = layout;
            desc.cache = cache.cache;
            desc.flags = cache.requireHit ? nri::GraphicsPipelineBits::FAIL_ON_CACHE_MISS : nri::GraphicsPipelineBits::NONE;
            desc.vertexInput = &input;
            desc.inputAssembly.topology = nri::Topology::TRIANGLE_LIST;
            desc.multisample = &multisample;
            desc.shaders = shaders;
            desc.shaderNum = 2;
            desc.outputMerger.colors = &color;
            desc.outputMerger.colorNum = 1;
            desc.outputMerger.depthStencilFormat = depthFormat;
            desc.outputMerger.depth.compareOp = i == 0 ? nri::CompareOp::ALWAYS : nri::CompareOp::LESS;
            desc.outputMerger.depth.write = i == 0;
            auto& stencil = desc.outputMerger.stencil.front;
            stencil.compareOp = i == 0 ? nri::CompareOp::ALWAYS : nri::CompareOp::EQUAL;
            stencil.passOp = i == 0 ? nri::StencilOp::REPLACE : nri::StencilOp::KEEP;
            stencil.compareMask = stencil.writeMask = 255;
            desc.outputMerger.stencil.back = stencil;

            auto begin = std::chrono::steady_clock::now();
            auto result = core.CreateGraphicsPipeline(device, desc, pipelines[i]);
            cache.pipelineMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();

            if (!Check(result == nri::Result::SUCCESS, "pipeline"))
                return false;
        }

        return true;
    }

    void Draw(float depth, float opacity, uint32_t flags, uint8_t stencil) {
        DrawParameters parameters = {depth, opacity, flags, 0};
        nri::SetRootConstantsDesc constants = {};
        constants.data = &parameters;
        constants.size = sizeof(parameters);
        constants.bindPoint = nri::BindPoint::GRAPHICS;
        core.CmdSetRootConstants(*command, constants);
        core.CmdSetStencilReference(*command, stencil, stencil);
        core.CmdDrawIndexed(*command, {6, 2, 2, premultiplied ? -1 : 1, 1});
    }

    bool Execute(uint32_t& maxChannelError) {
        if (!Check(core.BeginCommandBuffer(*command, pool) == nri::Result::SUCCESS, "begin"))
            return false;

        nri::GlobalBarrierDesc host = {};
        host.before = {nri::AccessBits::HOST_WRITE, nri::StageBits::HOST};
        host.after = {nri::AccessBits::VERTEX_BUFFER | nri::AccessBits::INDEX_BUFFER | nri::AccessBits::COPY_SOURCE, nri::StageBits::ALL};
        nri::TextureBarrierDesc transitions[4] = {};

        for (uint32_t i = 0; i < 4; i++) {
            transitions[i].texture = images[i];
            transitions[i].mipNum = transitions[i].layerNum = 1;
            transitions[i].planes = i == 2 ? nri::PlaneBits::DEPTH | nri::PlaneBits::STENCIL : nri::PlaneBits::COLOR;
        }

        transitions[0].after = {nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT};

        if (samples > 1) {
            transitions[0].after.access |= nri::AccessBits::RESOLVE_ATTACHMENT_SOURCE;
            transitions[0].after.stages |= nri::StageBits::RESOLVE_ATTACHMENT;
        }
        transitions[1].after = {nri::AccessBits::RESOLVE_ATTACHMENT_DESTINATION, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::RESOLVE_ATTACHMENT};
        transitions[2].after = {nri::AccessBits::DEPTH_STENCIL_ATTACHMENT, nri::Layout::DEPTH_STENCIL_ATTACHMENT, nri::StageBits::DEPTH_STENCIL_ATTACHMENT};
        transitions[3].mipOffset = transitions[3].layerOffset = 1;
        transitions[3].after = {nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY};
        nri::BarrierDesc barriers = {};
        barriers.globals = &host;
        barriers.globalNum = 1;
        barriers.textures = transitions;
        barriers.textureNum = 4;
        core.CmdBarrier(*command, barriers);
        nri::TextureRegionDesc region = {};
        region.mipOffset = region.layerOffset = 1;
        region.planes = nri::PlaneBits::COLOR;
        nri::TextureDataLayoutDesc data = {};
        data.rowPitch = uploadRow;
        data.slicePitch = uploadSlice;
        core.CmdUploadBufferToTexture(*command, *images[3], region, *buffers[0], data);
        transitions[3].before = transitions[3].after;
        transitions[3].after = {nri::AccessBits::SHADER_RESOURCE, nri::Layout::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER};
        barriers.globalNum = 0;
        barriers.textures = &transitions[3];
        barriers.textureNum = 1;
        core.CmdBarrier(*command, barriers);
        core.CmdSetPipelineLayout(*command, nri::BindPoint::GRAPHICS, *layout);
        nri::SetDescriptorSetDesc binding = {};
        binding.descriptorSet = set;
        binding.bindPoint = nri::BindPoint::GRAPHICS;
        core.CmdSetDescriptorSet(*command, binding);
        nri::VertexBufferDesc vertices[] = {{buffers[1], 0, 16}, {buffers[2], 0, 8}};
        core.CmdSetVertexBuffers(*command, 0, vertices, 2);
        core.CmdSetIndexBuffer(*command, *buffers[3], 0, nri::IndexType::UINT16);
        nri::Viewport viewport = {0, 0, 64, 64, 0, 1, false};
        core.CmdSetViewports(*command, &viewport, 1);
        nri::AttachmentDesc color = {};
        color.descriptor = views[0];
        color.loadOp = nri::LoadOp::CLEAR;
        color.storeOp = nri::StoreOp::STORE;
        color.clearValue.color.f = {0, 0, 0, 1};

        if (samples > 1) {
            color.resolveDst = views[1];
            color.resolveOp = nri::ResolveOp::AVERAGE;
        }

        nri::RenderingDesc rendering = {};
        rendering.colors = &color;
        rendering.colorNum = 1;
        rendering.depth.descriptor = views[2];
        rendering.depth.loadOp = nri::LoadOp::CLEAR;
        rendering.depth.storeOp = nri::StoreOp::STORE;
        rendering.depth.clearValue.depthStencil = {1.0f, 0};
        core.CmdBeginRendering(*command, rendering);
        nri::Rect scissor = {16, 16, 32, 32};
        core.CmdSetScissors(*command, &scissor, 1);
        core.CmdSetPipeline(*command, *pipelines[0]);
        Draw(0.5f, 1.0f, 0, 1);
        scissor = {0, 0, 64, 64};
        core.CmdSetScissors(*command, &scissor, 1);
        core.CmdSetPipeline(*command, *pipelines[1]);
        Draw(0.25f, 1.0f, 2, 2); // must fail stencil everywhere
        Draw(0.75f, 1.0f, 2, 1); // must fail depth inside the mask
        Draw(0.25f, 0.5f, premultiplied ? 1 : 0, 1);
        core.CmdEndRendering(*command);
        uint32_t output = samples > 1 ? 1 : 0;
        transitions[output].before = transitions[output].after;
        transitions[output].after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
        barriers.textures = &transitions[output];
        core.CmdBarrier(*command, barriers);
        region.mipOffset = region.layerOffset = 0;
        data.rowPitch = readRow;
        data.slicePitch = readSlice;
        core.CmdReadbackTextureToBuffer(*command, *buffers[4], data, *images[output], region);
        host.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        host.after = {nri::AccessBits::HOST_READ, nri::StageBits::HOST};
        barriers.textureNum = 0;
        barriers.globalNum = 1;
        core.CmdBarrier(*command, barriers);

        if (!Check(core.EndCommandBuffer(*command) == nri::Result::SUCCESS, "end"))
            return false;

        nri::FenceSubmitDesc signal = {fence, 1, nri::StageBits::ALL};
        nri::QueueSubmitDesc submit = {};
        submit.commandBuffers = &command;
        submit.commandBufferNum = 1;
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(queue, submit) == nri::Result::SUCCESS, "submit"))
            return false;

        core.Wait(*fence, 1);

        if (!Check(core.GetFenceValue(*fence) >= 1, "completion"))
            return false;

        const auto* pixels = (const uint8_t*)core.MapBuffer(*buffers[4], 0, readSlice);

        if (!Check(pixels != nullptr, "readback"))
            return false;

        bool exact = true;

        for (uint32_t y = 0; y < 64; y++) {
            for (uint32_t x = 0; x < 64; x++) {
                bool inside = x >= 16 && x < 48 && y >= 16 && y < 48;
                float u = (float(x % 32) + 0.5f) / 32;

                if (x >= 32)
                    u = 1 - u;

                for (uint32_t c = 0; c < 4; c++) {
                    int expected = c == 3 ? 255 : (inside ? int(std::lround(Sample(u, (float(y) + 0.5f) / 64, c, linear) * 127.5f)) : 0);
                    uint32_t error = uint32_t(std::abs(int(pixels[y * readRow + x * 4 + c]) - expected));
                    maxChannelError = std::max(maxChannelError, error);

                    if (error > (inside && c != 3 ? 1u : 0u)) {
                        if (exact)
                            std::fprintf(stderr, "State pixel %u,%u channel%u: got%u expected%d\n", x, y, c, pixels[y * readRow + x * 4 + c], expected);

                        exact = false;
                    }
                }
            }
        }

        core.UnmapBuffer(*buffers[4]);

        return Check(exact, "texture/filter/blend/depth/stencil/MSAA pixels");
    }

    bool Cleanup() {
        if (core.DeviceWaitIdle(&device) != nri::Result::SUCCESS)
            return false;

        if (command)
            core.DestroyCommandBuffer(command);

        if (allocator)
            core.DestroyCommandAllocator(allocator);

        for (auto* pipeline : pipelines) {
            if (pipeline)
                core.DestroyPipeline(pipeline);
        }

        if (pool)
            core.DestroyDescriptorPool(pool);

        if (layout)
            core.DestroyPipelineLayout(layout);

        for (auto* view : views) {
            if (view)
                core.DestroyDescriptor(view);
        }

        for (auto* image : images) {
            if (image)
                core.DestroyTexture(image);
        }

        for (auto* buffer : buffers) {
            if (buffer)
                core.DestroyBuffer(buffer);
        }

        if (fence)
            core.DestroyFence(fence);

        return true;
    }
};

} // namespace

bool RunGraphicsStates(nri::Device& device, const nri::CoreInterface& core, nri::Queue& queue, const std::atomic<uint32_t>& errors, PipelineCacheRun& cache, uint32_t& completedCases, uint32_t& maxChannelError) {
    for (uint32_t i = 0; i < 8; i++) {
        StateCase state{device, core, queue, cache, uint8_t((i & 4) ? 4 : 1), (i & 2) != 0, (i & 1) != 0};
        bool passed = state.Initialize() && state.Execute(maxChannelError);
        passed = state.Cleanup() && passed && errors.load() == 0;

        if (!passed)
            return false;

        completedCases++;
    }

    return true;
}
