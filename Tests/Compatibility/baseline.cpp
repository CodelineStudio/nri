// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT

#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Diagnostics {
    std::atomic<uint32_t> errors{0};
    std::atomic<uint32_t> warnings{0};
};

static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* message, void* user) {
    auto& diagnostics = *static_cast<Diagnostics*>(user);

    if (severity == nri::Message::ERROR)
        ++diagnostics.errors;
    else if (severity == nri::Message::WARNING)
        ++diagnostics.warnings;

    std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, message ? message : "");
}

static void NRI_CALL DoNotAbort(void*) {
    // Report failures through the fixture result instead of an interactive debugger.
}

static std::string JsonString(const char* input) {
    std::string result = "\"";

    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; ++p) {
        if (*p == '\\' || *p == '"') {
            result += '\\';
            result += char(*p);
        } else if (*p < 32) {
            char escaped[7] = {};
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(*p));
            result += escaped;
        } else
            result += char(*p);
    }
    result += '"';

    return result;
}

struct Fixture {
    Diagnostics diagnostics;
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::Queue* queue = nullptr;
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::Fence* fence = nullptr;
    nri::PipelineLayout* layout = nullptr;
    nri::DescriptorPool* descriptorPool = nullptr;
    nri::Texture* texture = nullptr;
    nri::Descriptor* colorView = nullptr;
    nri::Descriptor* inputColorView = nullptr;
    bool renderExact = false;
    bool resolveExact = false;
    bool inputAttachmentExact = false;
    std::array<nri::Texture*, 3> attachmentTextures{};
    std::array<nri::Descriptor*, 3> attachmentViews{};
    nri::QueryPool* queries = nullptr;
    nri::Buffer* queryReadback = nullptr;
    bool imageExact = false;
    bool timestampsOrdered = false;
    std::array<nri::Buffer*, 3> buffers{};
    nri::AdapterDesc adapter = {};
    std::string failure;
    uint64_t completedValue = 0;
    bool coreReady = false;
    bool exactBytes = false;
    static constexpr uint32_t BYTE_COUNT = 4096;

    bool Check(nri::Result result, const char* operation) {
        if (result == nri::Result::SUCCESS)
            return true;

        failure = std::string(operation) + " returned " + std::to_string(int(result));

        return false;
    }

    bool Run(bool validation, uint32_t adapterIndex) {
        uint32_t adapterCount = 0;

        if (!Check(nri::nriEnumerateAdapters(nullptr, adapterCount, nri::GraphicsAPI::VK), "EnumerateAdapters(count)"))
            return false;

        if (adapterIndex >= adapterCount) {
            failure = "Requested Vulkan adapter is unavailable";

            return false;
        }
        std::vector<nri::AdapterDesc> adapters(adapterCount);

        if (!Check(nri::nriEnumerateAdapters(adapters.data(), adapterCount, nri::GraphicsAPI::VK), "EnumerateAdapters(list)"))
            return false;

        if (adapterIndex >= adapterCount) {
            failure = "Adapter list changed during enumeration";

            return false;
        }
        adapter = adapters[adapterIndex];
        nri::DeviceCreationDesc create = {};
        create.graphicsAPI = nri::GraphicsAPI::VK;
        create.adapterDesc = &adapter;
        create.enableNRIValidation = validation;
        create.enableGraphicsAPIValidation = validation;
        create.disableVKRayTracing = true;
        create.callbackInterface.MessageCallback = Message;
        create.callbackInterface.AbortExecution = DoNotAbort;
        create.callbackInterface.userArg = &diagnostics;

        if (!Check(nri::nriCreateDevice(create, device), "CreateDevice"))
            return false;

        if (!Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core), "GetCoreInterface"))
            return false;
        coreReady = true;
        adapter = core.GetDeviceDesc(*device).adapterDesc;

        const nri::DeviceDesc& capabilities = core.GetDeviceDesc(*device);

        if (!capabilities.wave.laneMinNum || capabilities.wave.laneMaxNum < capabilities.wave.laneMinNum || capabilities.memory.bufferMaxSize < BYTE_COUNT) {
            failure = "Invalid subgroup size or buffer allocation limits";

            return false;
        }

        if (!Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, 0, queue), "GetQueue"))
            return false;

        const nri::FormatSupportBits rgbaSupport = core.GetFormatSupport(*device, nri::Format::RGBA8_UNORM);

        if (!(rgbaSupport & nri::FormatSupportBits::TEXTURE) || !(rgbaSupport & nri::FormatSupportBits::COLOR_ATTACHMENT)) {
            failure = "Required RGBA8 sampled/color-attachment format support is missing";

            return false;
        }

        nri::DescriptorRangeDesc range = {};
        range.descriptorNum = 1;
        range.descriptorType = nri::DescriptorType::CONSTANT_BUFFER;
        range.shaderStages = nri::StageBits::COMPUTE_SHADER;
        nri::DescriptorSetDesc set = {};
        set.ranges = &range;
        set.rangeNum = 1;
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.descriptorSets = &set;
        layoutDesc.descriptorSetNum = 1;
        layoutDesc.shaderStages = nri::StageBits::COMPUTE_SHADER;
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum = 1;
        poolDesc.constantBufferMaxNum = 1;
        nri::DescriptorSet* descriptorSet = nullptr;

        if (!Check(core.CreatePipelineLayout(*device, layoutDesc, layout), "CreatePipelineLayout(fixed descriptor)") ||
            !Check(core.CreateDescriptorPool(*device, poolDesc, descriptorPool), "CreateDescriptorPool") ||
            !Check(core.AllocateDescriptorSets(*descriptorPool, *layout, 0, &descriptorSet, 1, 0), "AllocateDescriptorSets"))
            return false;

        nri::BufferDesc description = {};
        description.size = BYTE_COUNT;
        const std::array<nri::MemoryLocation, 3> locations = {nri::MemoryLocation::HOST_UPLOAD, nri::MemoryLocation::DEVICE, nri::MemoryLocation::HOST_READBACK};

        for (size_t i = 0; i < buffers.size(); ++i) {
            if (!Check(core.CreateCommittedBuffer(*device, locations[i], 0.0f, description, buffers[i]), "CreateCommittedBuffer"))
                return false;
        }

        // Exercise one nonzero mip and array layer; barriers must retain the
        // exact subresource and layouts during legacy translation.
        nri::TextureDesc image = {};
        image.type = nri::TextureType::TEXTURE_2D;
        image.usage = nri::TextureUsageBits::SHADER_RESOURCE | nri::TextureUsageBits::COLOR_ATTACHMENT | nri::TextureUsageBits::INPUT_ATTACHMENT;
        image.format = nri::Format::RGBA8_UNORM;
        image.width = 128;
        image.height = 32;
        image.mipNum = 2;
        image.layerNum = 2;
        nri::QueryPoolDesc queryDescription = {};
        queryDescription.queryType = nri::QueryType::TIMESTAMP;
        queryDescription.capacity = 2;
        nri::BufferDesc queryBuffer = {};
        queryBuffer.size = 2 * sizeof(uint64_t);

        if (!Check(core.CreateCommittedTexture(*device, nri::MemoryLocation::DEVICE, 0.0f, image, texture), "CreateCommittedTexture") ||
            !Check(core.CreateQueryPool(*device, queryDescription, queries), "CreateQueryPool") ||
            !Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_READBACK, 0.0f, queryBuffer, queryReadback), "CreateQueryReadback"))
            return false;

        std::array<uint8_t, BYTE_COUNT> expected = {};

        for (uint32_t i = 0; i < BYTE_COUNT; ++i)
            expected[i] = uint8_t((i * 73u) ^ (i >> 3u) ^ 0xa5u);

        void* mapped = core.MapBuffer(*buffers[0], 0, BYTE_COUNT);

        if (!mapped) {
            failure = "MapBuffer(upload) returned null";

            return false;
        }
        std::memcpy(mapped, expected.data(), BYTE_COUNT);
        core.UnmapBuffer(*buffers[0]);

        if (!Check(core.CreateCommandAllocator(*queue, allocator), "CreateCommandAllocator") ||
            !Check(core.CreateCommandBuffer(*allocator, command), "CreateCommandBuffer") ||
            !Check(core.CreateFence(*device, nri::FenceDesc{}, fence), "CreateFence") ||
            !Check(core.BeginCommandBuffer(*command, nullptr), "BeginCommandBuffer"))
            return false;

        core.CmdResetQueries(*command, *queries, 0, 2);
        core.CmdEndQuery(*command, *queries, 0);
        GlobalBarrier(nri::AccessBits::HOST_WRITE, nri::StageBits::HOST, nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY);
        Barrier(*buffers[1], nri::AccessBits::NONE, nri::StageBits::NONE, nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY);
        core.CmdCopyBuffer(*command, *buffers[1], 0, *buffers[0], 0, BYTE_COUNT);
        Barrier(*buffers[1], nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY);
        Barrier(*buffers[2], nri::AccessBits::NONE, nri::StageBits::NONE, nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY);
        core.CmdCopyBuffer(*command, *buffers[2], 0, *buffers[1], 0, BYTE_COUNT);
        // The second copy overwrites the readback with the same pattern through
        // the image, so separately verify the original buffer copy below first.
        GlobalBarrier(nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::HOST_READ, nri::StageBits::HOST);

        if (!Check(core.EndCommandBuffer(*command), "EndCommandBuffer"))
            return false;

        const nri::CommandBuffer* commands[] = {command};
        nri::FenceSubmitDesc signal = {};
        signal.fence = fence;
        signal.value = 1;
        signal.stages = nri::StageBits::ALL;
        nri::QueueSubmitDesc submit = {};
        submit.commandBuffers = commands;
        submit.commandBufferNum = 1;
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*queue, submit), "QueueSubmit(copy)"))
            return false;
        core.Wait(*fence, 1);
        completedValue = core.GetFenceValue(*fence);

        if (completedValue < 1) {
            failure = "Fence value did not reach copy completion";

            return false;
        }
        mapped = core.MapBuffer(*buffers[2], 0, BYTE_COUNT);

        if (!mapped) {
            failure = "MapBuffer(readback) returned null";

            return false;
        }
        exactBytes = std::memcmp(mapped, expected.data(), BYTE_COUNT) == 0;
        core.UnmapBuffer(*buffers[2]);

        if (!exactBytes) {
            failure = "Readback bytes differ from uploaded pattern";

            return false;
        }

        core.ResetCommandAllocator(*allocator);

        if (!Check(core.BeginCommandBuffer(*command, nullptr), "BeginCommandBuffer(image)"))
            return false;

        nri::TextureBarrierDesc imageBarrier = {};
        imageBarrier.texture = texture;
        imageBarrier.before = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::NONE};
        imageBarrier.after = {nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY};
        imageBarrier.mipOffset = 1;
        imageBarrier.mipNum = 1;
        imageBarrier.layerOffset = 1;
        imageBarrier.layerNum = 1;
        imageBarrier.planes = nri::PlaneBits::COLOR;
        nri::BarrierDesc imageBarriers = {};
        imageBarriers.textures = &imageBarrier;
        imageBarriers.textureNum = 1;
        core.CmdBarrier(*command, imageBarriers);
        nri::TextureRegionDesc region = {};
        region.width = 64;
        region.height = 16;
        region.depth = 1;
        region.mipOffset = 1;
        region.layerOffset = 1;
        region.planes = nri::PlaneBits::COLOR;
        nri::TextureDataLayoutDesc imageData = {};
        imageData.rowPitch = 256;
        imageData.slicePitch = BYTE_COUNT;
        core.CmdUploadBufferToTexture(*command, *texture, region, *buffers[0], imageData);
        imageBarrier.before = imageBarrier.after;
        imageBarrier.after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
        core.CmdBarrier(*command, imageBarriers);
        Barrier(*buffers[2], nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY);
        // Erase the earlier matching buffer result, so an omitted image copy
        // cannot accidentally pass by leaving the previous pattern untouched.
        core.CmdZeroBuffer(*command, *buffers[2], 0, BYTE_COUNT);
        Barrier(*buffers[2], nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY);
        core.CmdReadbackTextureToBuffer(*command, *buffers[2], imageData, *texture, region);
        core.CmdEndQuery(*command, *queries, 1);
        Barrier(*queryReadback, nri::AccessBits::NONE, nri::StageBits::NONE, nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY);
        core.CmdCopyQueries(*command, *queries, 0, 2, *queryReadback, 0);
        GlobalBarrier(nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::HOST_READ, nri::StageBits::HOST);

        if (!Check(core.EndCommandBuffer(*command), "EndCommandBuffer(image)"))
            return false;

        nri::FenceSubmitDesc wait = signal;
        signal.value = 2;
        submit.waitFences = &wait;
        submit.waitFenceNum = 1;

        if (!Check(core.QueueSubmit(*queue, submit), "QueueSubmit(image with wait)"))
            return false;
        core.Wait(*fence, 2);
        mapped = core.MapBuffer(*buffers[2], 0, BYTE_COUNT);

        if (!mapped) {
            failure = "MapBuffer(image readback) returned null";

            return false;
        }
        imageExact = std::memcmp(mapped, expected.data(), BYTE_COUNT) == 0;
        core.UnmapBuffer(*buffers[2]);
        mapped = core.MapBuffer(*queryReadback, 0, queryBuffer.size);

        if (!mapped) {
            failure = "MapBuffer(query readback) returned null";

            return false;
        }
        std::array<uint64_t, 2> timestamps = {};
        std::memcpy(timestamps.data(), mapped, sizeof(timestamps));
        core.UnmapBuffer(*queryReadback);
        timestampsOrdered = timestamps[0] != 0 && timestamps[1] >= timestamps[0];

        if (!imageExact || !timestampsOrdered) {
            failure = "Image readback or timestamp ordering mismatch";

            return false;
        }

        // Empty submission signals a later value. Repeated waiting for an older
        // value must remain valid; this is not a fan-out/future-wait stress test.
        wait.value = 2;
        signal.value = 3;
        submit.commandBuffers = nullptr;
        submit.commandBufferNum = 0;

        if (!Check(core.QueueSubmit(*queue, submit), "QueueSubmit(empty signal)"))
            return false;
        core.Wait(*fence, 3);
        core.Wait(*fence, 1);
        completedValue = core.GetFenceValue(*fence);

        if (completedValue < 3) {
            failure = "Fence value did not reach second signal";

            return false;
        }

        return RunRendering(false) && RunRendering(true) && RunResolveRendering();
    }

    bool RunRendering(bool inputAttachment) {
        nri::TextureViewDesc view = {};
        view.texture = texture;
        view.type = nri::TextureView::COLOR_ATTACHMENT;
        view.mipOffset = 1;
        view.mipNum = 1;
        view.layerOffset = 1;
        view.layerNum = 1;
        view.planes = nri::PlaneBits::COLOR;
        view.layout = inputAttachment ? nri::Layout::INPUT_ATTACHMENT : nri::Layout::COLOR_ATTACHMENT;
        nri::Descriptor*& targetView = inputAttachment ? inputColorView : colorView;

        if (!Check(core.CreateTextureView(view, targetView), "CreateTextureView(color)"))
            return false;

        core.ResetCommandAllocator(*allocator);

        if (!Check(core.BeginCommandBuffer(*command, nullptr), "BeginCommandBuffer(render)"))
            return false;

        nri::TextureBarrierDesc transition = {};
        transition.texture = texture;
        transition.mipOffset = 1;
        transition.mipNum = 1;
        transition.layerOffset = 1;
        transition.layerNum = 1;
        transition.planes = nri::PlaneBits::COLOR;
        transition.before = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
        transition.after = {nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT};

        if (inputAttachment) {
            transition.after.layout = nri::Layout::INPUT_ATTACHMENT;
            transition.after.access |= nri::AccessBits::INPUT_ATTACHMENT;
            transition.after.stages |= nri::StageBits::FRAGMENT_SHADER;
        }
        nri::BarrierDesc barriers = {};
        barriers.textures = &transition;
        barriers.textureNum = 1;
        core.CmdBarrier(*command, barriers);

        nri::AttachmentDesc color = {};
        color.descriptor = targetView;
        color.loadOp = nri::LoadOp::CLEAR;
        color.storeOp = nri::StoreOp::STORE;
        color.clearValue.color.f = {1.0f, 0.0f, 0.0f, 1.0f};
        nri::RenderingDesc rendering = {};
        rendering.colors = &color;
        rendering.colorNum = 1;
        core.CmdBeginRendering(*command, rendering);
        core.CmdEndRendering(*command);

        // Preserve the first pass outside an asymmetric green rectangle.
        // Repeat the LOAD pass to exercise render-pass/framebuffer cache reuse.
        color.loadOp = nri::LoadOp::LOAD;
        nri::ClearAttachmentDesc clear = {};
        clear.planes = nri::PlaneBits::COLOR;
        clear.value.color.f = {0.0f, 1.0f, 0.0f, 1.0f};
        nri::Rect rectangle = {7, 3, 9, 5};

        for (uint32_t pass = 0; pass < 2; pass++) {
            transition.before = transition.after;
            core.CmdBarrier(*command, barriers);
            core.CmdBeginRendering(*command, rendering);
            core.CmdClearAttachments(*command, &clear, 1, &rectangle, 1);
            core.CmdEndRendering(*command);
        }

        transition.before = transition.after;
        transition.after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
        core.CmdBarrier(*command, barriers);
        Barrier(*buffers[2], nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY);
        core.CmdZeroBuffer(*command, *buffers[2], 0, BYTE_COUNT);
        Barrier(*buffers[2], nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY);
        nri::TextureRegionDesc region = {};
        region.mipOffset = 1;
        region.layerOffset = 1;
        region.planes = nri::PlaneBits::COLOR;
        nri::TextureDataLayoutDesc data = {};
        data.rowPitch = 256;
        data.slicePitch = BYTE_COUNT;
        core.CmdReadbackTextureToBuffer(*command, *buffers[2], data, *texture, region);
        GlobalBarrier(nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::HOST_READ, nri::StageBits::HOST);

        if (!Check(core.EndCommandBuffer(*command), "EndCommandBuffer(render)"))
            return false;

        const nri::CommandBuffer* commands[] = {command};
        nri::FenceSubmitDesc signal = {};
        signal.fence = fence;
        signal.value = inputAttachment ? 5 : 4;
        signal.stages = nri::StageBits::ALL;
        nri::QueueSubmitDesc submit = {};
        submit.commandBuffers = commands;
        submit.commandBufferNum = 1;
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*queue, submit), "QueueSubmit(render)"))
            return false;

        core.Wait(*fence, signal.value);
        completedValue = core.GetFenceValue(*fence);
        const uint8_t* pixels = static_cast<const uint8_t*>(core.MapBuffer(*buffers[2], 0, BYTE_COUNT));

        if (!pixels) {
            failure = "MapBuffer(render readback) returned null";

            return false;
        }

        renderExact = true;

        for (uint32_t y = 0; y < 16; y++) {
            for (uint32_t x = 0; x < 64; x++) {
                const bool green = x >= 7 && x < 16 && y >= 3 && y < 8;
                const uint8_t expected[4] = {uint8_t(green ? 0 : 255), uint8_t(green ? 255 : 0), 0, 255};
                renderExact &= std::memcmp(pixels + y * 256 + x * 4, expected, 4) == 0;
            }
        }

        core.UnmapBuffer(*buffers[2]);

        if (!renderExact)
            failure = "Render clear/load/store or rectangular-clear pixel mismatch";

        if (inputAttachment)
            inputAttachmentExact = renderExact;

        return renderExact;
    }

    bool RunResolveRendering() {
        if (core.GetDeviceDesc(*device).other.viewMaxNum < 2) {
            failure = "The layered resolve fixture requires two-view multiview support";

            return false;
        }

        for (uint32_t i = 0; i < 3; i++) {
            const bool depth = i == 2;
            nri::TextureDesc desc = {};
            desc.type = nri::TextureType::TEXTURE_2D;
            desc.usage = depth ? nri::TextureUsageBits::DEPTH_STENCIL_ATTACHMENT : nri::TextureUsageBits::COLOR_ATTACHMENT;
            desc.format = depth ? nri::Format::D32_SFLOAT : nri::Format::RGBA8_UNORM;
            desc.width = 64;
            desc.height = 8;
            desc.layerNum = 2;
            desc.mipNum = 1;
            desc.sampleNum = i == 1 ? 1 : 4;

            if (!Check(core.CreateCommittedTexture(*device, nri::MemoryLocation::DEVICE, 0.0f, desc, attachmentTextures[i]), "CreateCommittedTexture(layered resolve)"))
                return false;

            nri::TextureViewDesc view = {};
            view.texture = attachmentTextures[i];
            view.type = depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT;
            view.mipNum = 1;
            view.layerNum = 2;
            view.planes = depth ? nri::PlaneBits::DEPTH : nri::PlaneBits::COLOR;

            if (!Check(core.CreateTextureView(view, attachmentViews[i]), "CreateTextureView(layered resolve)"))
                return false;
        }

        core.ResetCommandAllocator(*allocator);

        if (!Check(core.BeginCommandBuffer(*command, nullptr), "BeginCommandBuffer(layered resolve)"))
            return false;

        nri::TextureBarrierDesc transitions[3] = {};

        for (uint32_t i = 0; i < 3; i++) {
            transitions[i].texture = attachmentTextures[i];
            transitions[i].mipNum = 1;
            transitions[i].layerNum = 2;
            transitions[i].planes = i == 2 ? nri::PlaneBits::DEPTH : nri::PlaneBits::COLOR;
        }

        transitions[0].after = {nri::AccessBits::COLOR_ATTACHMENT | nri::AccessBits::RESOLVE_ATTACHMENT_SOURCE, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT | nri::StageBits::RESOLVE_ATTACHMENT};
        transitions[1].after = {nri::AccessBits::RESOLVE_ATTACHMENT_DESTINATION, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::RESOLVE_ATTACHMENT};
        transitions[2].after = {nri::AccessBits::DEPTH_STENCIL_ATTACHMENT, nri::Layout::DEPTH_STENCIL_ATTACHMENT, nri::StageBits::DEPTH_STENCIL_ATTACHMENT};
        nri::BarrierDesc barriers = {};
        barriers.textures = transitions;
        barriers.textureNum = 3;
        core.CmdBarrier(*command, barriers);

        nri::AttachmentDesc color = {};
        color.descriptor = attachmentViews[0];
        color.resolveDst = attachmentViews[1];
        color.loadOp = nri::LoadOp::CLEAR;
        color.storeOp = nri::StoreOp::STORE;
        color.resolveOp = nri::ResolveOp::AVERAGE;
        color.clearValue.color.f = {1.0f, 0.0f, 1.0f, 1.0f};
        nri::RenderingDesc rendering = {};
        rendering.colors = &color;
        rendering.colorNum = 1;
        rendering.viewMask = 3;
        rendering.depth.descriptor = attachmentViews[2];
        rendering.depth.loadOp = nri::LoadOp::CLEAR;
        rendering.depth.storeOp = nri::StoreOp::STORE;
        rendering.depth.clearValue.depthStencil.depth = 0.375f;
        core.CmdBeginRendering(*command, rendering);
        core.CmdEndRendering(*command);

        transitions[1].before = transitions[1].after;
        transitions[1].after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
        barriers.textures = &transitions[1];
        barriers.textureNum = 1;
        core.CmdBarrier(*command, barriers);
        Barrier(*buffers[2], nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY);
        core.CmdZeroBuffer(*command, *buffers[2], 0, BYTE_COUNT);
        Barrier(*buffers[2], nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY);

        for (uint16_t layer = 0; layer < 2; layer++) {
            nri::TextureRegionDesc region = {};
            region.layerOffset = layer;
            region.planes = nri::PlaneBits::COLOR;
            nri::TextureDataLayoutDesc data = {};
            data.offset = layer * 2048;
            data.rowPitch = 256;
            data.slicePitch = 2048;
            core.CmdReadbackTextureToBuffer(*command, *buffers[2], data, *attachmentTextures[1], region);
        }

        GlobalBarrier(nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY, nri::AccessBits::HOST_READ, nri::StageBits::HOST);

        if (!Check(core.EndCommandBuffer(*command), "EndCommandBuffer(layered resolve)"))
            return false;

        const nri::CommandBuffer* commands[] = {command};
        nri::FenceSubmitDesc signal = {};
        signal.fence = fence;
        signal.value = 6;
        signal.stages = nri::StageBits::ALL;
        nri::QueueSubmitDesc submit = {};
        submit.commandBuffers = commands;
        submit.commandBufferNum = 1;
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*queue, submit), "QueueSubmit(layered resolve)"))
            return false;

        core.Wait(*fence, signal.value);
        completedValue = core.GetFenceValue(*fence);
        const uint8_t* pixels = static_cast<const uint8_t*>(core.MapBuffer(*buffers[2], 0, BYTE_COUNT));

        if (!pixels) {
            failure = "MapBuffer(layered resolve) returned null";

            return false;
        }

        resolveExact = true;
        const uint8_t expected[4] = {255, 0, 255, 255};

        for (uint32_t offset = 0; offset < BYTE_COUNT; offset += 4)
            resolveExact &= std::memcmp(pixels + offset, expected, 4) == 0;

        core.UnmapBuffer(*buffers[2]);

        if (!resolveExact)
            failure = "Multiview MSAA color resolve pixel mismatch";

        return resolveExact;
    }

    void GlobalBarrier(nri::AccessBits beforeAccess, nri::StageBits beforeStage, nri::AccessBits afterAccess, nri::StageBits afterStage) {
        // Host access is expressed globally: NRI buffer usage validation does
        // not admit host access bits in a resource-scoped buffer barrier.
        nri::GlobalBarrierDesc global = {};
        global.before = {beforeAccess, beforeStage};
        global.after = {afterAccess, afterStage};
        nri::BarrierDesc barriers = {};
        barriers.globals = &global;
        barriers.globalNum = 1;
        core.CmdBarrier(*command, barriers);
    }

    void Barrier(nri::Buffer& buffer, nri::AccessBits beforeAccess, nri::StageBits beforeStage, nri::AccessBits afterAccess, nri::StageBits afterStage) {
        nri::BufferBarrierDesc bufferBarrier = {};
        bufferBarrier.buffer = &buffer;
        bufferBarrier.before = {beforeAccess, beforeStage};
        bufferBarrier.after = {afterAccess, afterStage};
        nri::BarrierDesc barriers = {};
        barriers.buffers = &bufferBarrier;
        barriers.bufferNum = 1;
        core.CmdBarrier(*command, barriers);
    }

    bool Cleanup() {
        bool idle = true;

        if (coreReady) {
            // Includes failure paths after submission. The external process
            // watchdog bounds a driver hang in Wait/DeviceWaitIdle/teardown.
            idle = Check(core.DeviceWaitIdle(device), "DeviceWaitIdle(cleanup)");

            if (command)
                core.DestroyCommandBuffer(command);

            if (allocator)
                core.DestroyCommandAllocator(allocator);

            if (fence)
                core.DestroyFence(fence);

            if (descriptorPool)
                core.DestroyDescriptorPool(descriptorPool);

            if (layout)
                core.DestroyPipelineLayout(layout);

            if (queries)
                core.DestroyQueryPool(queries);

            if (queryReadback)
                core.DestroyBuffer(queryReadback);

            for (nri::Descriptor* view : attachmentViews) {
                if (view)
                    core.DestroyDescriptor(view);
            }

            for (nri::Texture* attachment : attachmentTextures) {
                if (attachment)
                    core.DestroyTexture(attachment);
            }

            if (colorView)
                core.DestroyDescriptor(colorView);

            if (inputColorView)
                core.DestroyDescriptor(inputColorView);

            if (texture)
                core.DestroyTexture(texture);

            for (nri::Buffer* buffer : buffers) {
                if (buffer)
                    core.DestroyBuffer(buffer);
            }
        }

        if (device)
            nri::nriDestroyDevice(device);
        device = nullptr;

        return idle;
    }
};

} // namespace

int main(int argc, char** argv) {
    bool validation = false;
    uint32_t adapterIndex = 0;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--validation") == 0)
            validation = true;
        else if (std::strcmp(argv[i], "--adapter") == 0 && (i + 1) < argc) {
            char* end = nullptr;
            const char* value = argv[++i];
            errno = 0;
            const unsigned long long parsed = std::strtoull(value, &end, 10);

            if (!*value || *end || *value == '-' || errno == ERANGE || parsed > UINT32_MAX)
                return 2;
            adapterIndex = uint32_t(parsed);
        } else {
            std::fprintf(stderr, "Usage: NRICompatibilityBaseline [--validation] [--adapter INDEX]\n");

            return 2;
        }
    }

    const auto started = std::chrono::steady_clock::now();
    Fixture fixture;
    bool passed = fixture.Run(validation, adapterIndex);
    passed = fixture.Cleanup() && passed;
    passed = passed && fixture.diagnostics.errors.load() == 0;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const int written = std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_buffer_baseline\",\"status\":\"%s\",\"source_commit\":%s,\"compiler\":%s,\"nri_version\":%u,\"backend\":\"Vulkan\",\"adapter_index\":%u,\"adapter_name\":%s,\"vendor\":%u,\"device_id\":%u,\"driver_version_raw\":%u,\"validation_requested\":%s,\"native_validation_active\":null,\"validation_errors\":%u,\"validation_warnings\":%u,\"exact_bytes\":%s,\"byte_count\":%u,\"completed_fence_value\":%llu,\"elapsed_seconds\":%.6f,\"failure\":%s,\"image_exact\":%s,\"timestamps_ordered\":%s,\"render_exact\":%s,\"input_attachment_exact\":%s,\"resolve_exact\":%s,\"legacy_sync_forced\":%s,\"legacy_support_verified\":false,\"api_versions_and_enabled_extensions\":null}\n",
        passed ? "passed" : "failed", JsonString(NRI_COMPATIBILITY_SOURCE_COMMIT).c_str(), JsonString(NRI_COMPATIBILITY_COMPILER).c_str(), unsigned(NRI_VERSION), adapterIndex,
        JsonString(fixture.adapter.name).c_str(), unsigned(fixture.adapter.vendor), fixture.adapter.deviceId, fixture.adapter.driverVersion, validation ? "true" : "false",
        fixture.diagnostics.errors.load(), fixture.diagnostics.warnings.load(), fixture.exactBytes ? "true" : "false", Fixture::BYTE_COUNT,
        static_cast<unsigned long long>(fixture.completedValue), seconds, JsonString(fixture.failure.c_str()).c_str(),
        fixture.imageExact ? "true" : "false", fixture.timestampsOrdered ? "true" : "false", fixture.renderExact ? "true" : "false", fixture.inputAttachmentExact ? "true" : "false", fixture.resolveExact ? "true" : "false", NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false");

    // Deliver the receipt before process/static teardown; do not treat an
    // unsuccessful write as a passed test even if the GPU checks completed.
    if (written < 0 || std::fflush(stdout) != 0)
        return 3;

    return passed ? 0 : 1;
}
