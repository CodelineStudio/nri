// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "compiled_shaders.h"
#include "graphics_state.h"
#include "pipeline_cache_io.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Parameters {
    uint32_t count, multiplier, bias, nonce;
    float transform[16];
};
static_assert(sizeof(Parameters) == 80 && offsetof(Parameters, transform) == 16, "Uniform ABI");

static uint32_t ExpectedValue(uint32_t index, const Parameters& parameters) {
    uint32_t value = index * parameters.multiplier + parameters.bias;
    value ^= value >> 16;

    return value * 0x85ebca6bu;
}

struct Fixture {
    std::atomic<uint32_t> errors{0}, warnings{0};
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::Queue* queue = nullptr;
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::Fence* fence = nullptr;
    nri::PipelineLayout* layout = nullptr;
    nri::DescriptorPool* pool = nullptr;
    nri::DescriptorSet* set = nullptr;
    nri::Pipeline* compute = nullptr;
    nri::Pipeline* graphics = nullptr;
    PipelineCacheRun pipelineCache;
    nri::Texture* image = nullptr;
    nri::Descriptor* color = nullptr;
    std::array<nri::Descriptor*, 2> descriptors{};
    // Uniform upload, device storage, storage readback, image readback, vertices.
    std::array<nri::Buffer*, 5> buffers{};
    uint32_t rowPitch = 0, slicePitch = 0;
    uint32_t completedCycles = 0, deviceId = 0, driver = 0;
    bool ready = false;

    static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* text, void* user) {
        auto& fixture = *(Fixture*)user;

        if (severity == nri::Message::ERROR)
            fixture.errors++;
        else if (severity == nri::Message::WARNING)
            fixture.warnings++;

        std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, text ? text : "");
    }

    static void NRI_CALL DoNotAbort(void*) {}

    bool Check(bool condition, const char* operation) {
        if (!condition)
            std::fprintf(stderr, "Shader execution failed: %s\n", operation);

        return condition;
    }

    bool Initialize(bool validation, uint32_t adapterIndex, const char* cacheInput, bool requireCacheHit) {
        uint32_t count = 0;

        if (!Check(nri::nriEnumerateAdapters(nullptr, count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && adapterIndex < count, "enumerate adapters"))
            return false;

        std::vector<nri::AdapterDesc> adapters(count);

        if (!Check(nri::nriEnumerateAdapters(adapters.data(), count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && adapterIndex < count, "adapter details"))
            return false;

        nri::DeviceCreationDesc create = {};
        create.graphicsAPI = nri::GraphicsAPI::VK;
        create.adapterDesc = &adapters[adapterIndex];
        create.enableNRIValidation = create.enableGraphicsAPIValidation = validation;
        create.disableVKRayTracing = true;
        create.callbackInterface = {Message, DoNotAbort, this};

        if (!Check(nri::nriCreateDevice(create, device) == nri::Result::SUCCESS, "create device") ||
            !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core) == nri::Result::SUCCESS, "core interface"))
            return false;

        ready = true;
        const auto& desc = core.GetDeviceDesc(*device);
        deviceId = desc.adapterDesc.deviceId;
        driver = desc.adapterDesc.driverVersion;

        if (!LoadPipelineCache(*device, core, cacheInput, requireCacheHit, pipelineCache))
            return false;

        if (!Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, 0, queue) == nri::Result::SUCCESS, "graphics queue") ||
            !Check(core.CreateCommandAllocator(*queue, allocator) == nri::Result::SUCCESS, "allocator") ||
            !Check(core.CreateCommandBuffer(*allocator, command) == nri::Result::SUCCESS, "command buffer") ||
            !Check(core.CreateFence(*device, nri::FenceDesc{}, fence) == nri::Result::SUCCESS, "completion fence"))
            return false;

        rowPitch = ((256 + desc.memoryAlignment.uploadBufferTextureRow - 1) / desc.memoryAlignment.uploadBufferTextureRow) * desc.memoryAlignment.uploadBufferTextureRow;
        slicePitch = ((rowPitch * 64 + desc.memoryAlignment.uploadBufferTextureSlice - 1) / desc.memoryAlignment.uploadBufferTextureSlice) * desc.memoryAlignment.uploadBufferTextureSlice;
        const uint64_t sizes[] = {sizeof(Parameters), 64 * 16, 64 * 16, slicePitch, 6 * 2 * sizeof(float)};

        for (uint32_t i = 0; i < buffers.size(); i++) {
            nri::BufferDesc buffer = {};
            buffer.size = sizes[i];

            if (i == 0)
                buffer.usage = nri::BufferUsageBits::CONSTANT;
            else if (i == 1) {
                buffer.usage = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE | nri::BufferUsageBits::SHADER_RESOURCE;
                buffer.structureStride = 16;
            } else if (i == 4)
                buffer.usage = nri::BufferUsageBits::VERTEX;

            nri::MemoryLocation memory = (i == 0 || i == 4) ? nri::MemoryLocation::HOST_UPLOAD : (i == 1 ? nri::MemoryLocation::DEVICE : nri::MemoryLocation::HOST_READBACK);

            if (!Check(core.CreateCommittedBuffer(*device, memory, 0.0f, buffer, buffers[i]) == nri::Result::SUCCESS, "buffer"))
                return false;
        }

        const float vertices[] = {-1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1};
        void* mapped = core.MapBuffer(*buffers[4], 0, sizeof(vertices));

        if (!Check(mapped != nullptr, "vertex upload"))
            return false;

        std::memcpy(mapped, vertices, sizeof(vertices));
        core.UnmapBuffer(*buffers[4]);
        nri::TextureDesc texture = {};
        texture.type = nri::TextureType::TEXTURE_2D;
        texture.format = nri::Format::RGBA8_UNORM;
        texture.width = texture.height = 64;
        texture.depth = texture.mipNum = texture.layerNum = texture.sampleNum = 1;
        texture.usage = nri::TextureUsageBits::COLOR_ATTACHMENT;

        if (!Check(core.CreateCommittedTexture(*device, nri::MemoryLocation::DEVICE, 0.0f, texture, image) == nri::Result::SUCCESS, "color texture"))
            return false;

        nri::TextureViewDesc view = {};
        view.texture = image;
        view.type = nri::TextureView::COLOR_ATTACHMENT;
        view.layout = nri::Layout::COLOR_ATTACHMENT;
        view.mipNum = view.layerNum = 1;
        view.planes = nri::PlaneBits::COLOR;

        if (!Check(core.CreateTextureView(view, color) == nri::Result::SUCCESS, "color view"))
            return false;

        nri::DescriptorRangeDesc ranges[2] = {};

        for (uint32_t i = 0; i < 2; i++) {
            ranges[i].baseRegisterIndex = i;
            ranges[i].descriptorNum = 1;
            ranges[i].descriptorType = i == 0 ? nri::DescriptorType::CONSTANT_BUFFER : nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
        }

        nri::DescriptorSetDesc setDesc = {};
        setDesc.ranges = ranges;
        setDesc.rangeNum = 2;
        nri::PipelineLayoutDesc layoutDesc = {};
        layoutDesc.rootRegisterSpace = 1;
        layoutDesc.descriptorSets = &setDesc;
        layoutDesc.descriptorSetNum = 1;
        layoutDesc.shaderStages = nri::StageBits::VERTEX_SHADER | nri::StageBits::FRAGMENT_SHADER | nri::StageBits::COMPUTE_SHADER;
        layoutDesc.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
        nri::DescriptorPoolDesc poolDesc = {};
        poolDesc.descriptorSetMaxNum = poolDesc.constantBufferMaxNum = poolDesc.storageStructuredBufferMaxNum = 1;

        if (!Check(core.CreatePipelineLayout(*device, layoutDesc, layout) == nri::Result::SUCCESS, "pipeline layout") ||
            !Check(core.CreateDescriptorPool(*device, poolDesc, pool) == nri::Result::SUCCESS, "descriptor pool") ||
            !Check(core.AllocateDescriptorSets(*pool, *layout, 0, &set, 1, 0) == nri::Result::SUCCESS, "descriptor set"))
            return false;

        for (uint32_t i = 0; i < 2; i++) {
            nri::BufferViewDesc bufferView = {};
            bufferView.buffer = buffers[i];
            bufferView.type = i == 0 ? nri::BufferView::CONSTANT_BUFFER : nri::BufferView::STORAGE_STRUCTURED_BUFFER;
            bufferView.size = sizes[i];

            if (!Check(core.CreateBufferView(bufferView, descriptors[i]) == nri::Result::SUCCESS, "buffer view"))
                return false;

            nri::UpdateDescriptorRangeDesc update = {};
            update.descriptorSet = set;
            update.rangeIndex = i;
            update.descriptors = &descriptors[i];
            update.descriptorNum = 1;
            core.UpdateDescriptorRanges(&update, 1);
        }

        nri::ComputePipelineDesc computeDesc = {};
        computeDesc.pipelineLayout = layout;
        computeDesc.shader = {nri::StageBits::COMPUTE_SHADER, shader_fixture::compute, sizeof(shader_fixture::compute), "computeMain"};
        computeDesc.cache = pipelineCache.cache;
        computeDesc.flags = pipelineCache.requireHit ? nri::ComputePipelineBits::FAIL_ON_CACHE_MISS : nri::ComputePipelineBits::NONE;
        auto pipelineBegin = std::chrono::steady_clock::now();

        if (!Check(core.CreateComputePipeline(*device, computeDesc, compute) == nri::Result::SUCCESS, "compute pipeline"))
            return false;

        nri::VertexAttributeDesc attribute = {};
        attribute.format = nri::Format::RG32_SFLOAT;
        nri::VertexStreamDesc stream = {};
        stream.stride = 2 * sizeof(float);
        nri::VertexInputDesc input = {};
        input.attributes = &attribute;
        input.attributeNum = 1;
        input.streams = &stream;
        input.streamNum = 1;
        nri::ColorAttachmentDesc target = {};
        target.format = nri::Format::RGBA8_UNORM;
        target.colorWriteMask = nri::ColorWriteBits::RGBA;
        nri::ShaderDesc shaders[] = {
            {nri::StageBits::VERTEX_SHADER, shader_fixture::vertex, sizeof(shader_fixture::vertex), "vertexMain"},
            {nri::StageBits::FRAGMENT_SHADER, shader_fixture::fragment, sizeof(shader_fixture::fragment), "fragmentMain"}
        };
        nri::GraphicsPipelineDesc graphicsDesc = {};
        graphicsDesc.pipelineLayout = layout;
        graphicsDesc.vertexInput = &input;
        graphicsDesc.inputAssembly.topology = nri::Topology::TRIANGLE_LIST;
        graphicsDesc.outputMerger.colors = &target;
        graphicsDesc.outputMerger.colorNum = 1;
        graphicsDesc.shaders = shaders;
        graphicsDesc.shaderNum = 2;
        graphicsDesc.cache = pipelineCache.cache;
        graphicsDesc.flags = pipelineCache.requireHit ? nri::GraphicsPipelineBits::FAIL_ON_CACHE_MISS : nri::GraphicsPipelineBits::NONE;
        bool pipelinesCreated = Check(core.CreateGraphicsPipeline(*device, graphicsDesc, graphics) == nri::Result::SUCCESS, "graphics pipeline");
        pipelineCache.pipelineMilliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pipelineBegin).count();

        return pipelinesCreated;
    }

    bool Execute(uint32_t cycle) {
        Parameters parameters = {64, 1664525, 1013904223u + cycle, 0xa53c9e17u ^ cycle, {}};
        parameters.transform[0] = parameters.transform[5] = 0.5f;
        parameters.transform[10] = parameters.transform[15] = 1;
        parameters.transform[12] = 0.25f;
        parameters.transform[13] = -0.25f;
        void* mapped = core.MapBuffer(*buffers[0], 0, sizeof(parameters));

        if (!Check(mapped != nullptr, "uniform upload"))
            return false;

        std::memcpy(mapped, &parameters, sizeof(parameters));
        core.UnmapBuffer(*buffers[0]);
        core.ResetCommandAllocator(*allocator);

        if (!Check(core.BeginCommandBuffer(*command, pool) == nri::Result::SUCCESS, "begin commands"))
            return false;

        nri::GlobalBarrierDesc host = {};
        host.before = {nri::AccessBits::HOST_WRITE, nri::StageBits::HOST};
        host.after = {nri::AccessBits::CONSTANT_BUFFER | nri::AccessBits::VERTEX_BUFFER, nri::StageBits::ALL};
        nri::BufferBarrierDesc storage = {};
        storage.buffer = buffers[1];
        storage.before = {cycle ? nri::AccessBits::COPY_SOURCE : nri::AccessBits::NONE, cycle ? nri::StageBits::COPY : nri::StageBits::NONE};
        storage.after = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER};
        nri::BarrierDesc barriers = {};
        barriers.globals = &host;
        barriers.globalNum = 1;
        barriers.buffers = &storage;
        barriers.bufferNum = 1;
        core.CmdBarrier(*command, barriers);
        core.CmdSetPipelineLayout(*command, nri::BindPoint::COMPUTE, *layout);
        nri::SetDescriptorSetDesc binding = {};
        binding.descriptorSet = set;
        binding.bindPoint = nri::BindPoint::COMPUTE;
        core.CmdSetDescriptorSet(*command, binding);
        core.CmdSetPipeline(*command, *compute);
        core.CmdDispatch(*command, {2, 1, 1});

        storage.before = storage.after;
        storage.after = {nri::AccessBits::SHADER_RESOURCE, nri::StageBits::FRAGMENT_SHADER};
        nri::TextureBarrierDesc transition = {};
        transition.texture = image;
        transition.mipNum = transition.layerNum = 1;
        transition.planes = nri::PlaneBits::COLOR;
        transition.before = {cycle ? nri::AccessBits::COPY_SOURCE : nri::AccessBits::NONE, cycle ? nri::Layout::COPY_SOURCE : nri::Layout::UNDEFINED, cycle ? nri::StageBits::COPY : nri::StageBits::NONE};
        transition.after = {nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT};
        barriers.globalNum = 0;
        barriers.textures = &transition;
        barriers.textureNum = 1;
        core.CmdBarrier(*command, barriers);
        core.CmdSetPipelineLayout(*command, nri::BindPoint::GRAPHICS, *layout);
        binding.bindPoint = nri::BindPoint::GRAPHICS;
        core.CmdSetDescriptorSet(*command, binding);
        core.CmdSetPipeline(*command, *graphics);
        nri::VertexBufferDesc vertex = {buffers[4], 0, 2 * sizeof(float)};
        core.CmdSetVertexBuffers(*command, 0, &vertex, 1);
        nri::Viewport viewport = {0, 0, 64, 64, 0, 1, false};
        nri::Rect scissor = {32, 16, 32, 32};
        core.CmdSetViewports(*command, &viewport, 1);
        core.CmdSetScissors(*command, &scissor, 1);
        nri::AttachmentDesc attachment = {};
        attachment.descriptor = color;
        attachment.loadOp = nri::LoadOp::CLEAR;
        attachment.storeOp = nri::StoreOp::STORE;
        nri::RenderingDesc rendering = {};
        rendering.colors = &attachment;
        rendering.colorNum = 1;
        core.CmdBeginRendering(*command, rendering);
        core.CmdDraw(*command, {6, 1, 0, 0});
        core.CmdEndRendering(*command);
        storage.before = storage.after;
        storage.after = {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY};
        transition.before = transition.after;
        transition.after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
        core.CmdBarrier(*command, barriers);
        core.CmdCopyBuffer(*command, *buffers[2], 0, *buffers[1], 0, 64 * 16);
        nri::TextureDataLayoutDesc data = {};
        data.rowPitch = rowPitch;
        data.slicePitch = slicePitch;
        nri::TextureRegionDesc region = {};
        region.planes = nri::PlaneBits::COLOR;
        core.CmdReadbackTextureToBuffer(*command, *buffers[3], data, *image, region);
        host.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        host.after = {nri::AccessBits::HOST_READ, nri::StageBits::HOST};
        barriers.globalNum = 1;
        barriers.bufferNum = barriers.textureNum = 0;
        core.CmdBarrier(*command, barriers);

        if (!Check(core.EndCommandBuffer(*command) == nri::Result::SUCCESS, "end commands"))
            return false;

        nri::FenceSubmitDesc signal = {fence, cycle + 1, nri::StageBits::ALL};
        nri::QueueSubmitDesc submit = {};
        submit.commandBuffers = &command;
        submit.commandBufferNum = 1;
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*queue, submit) == nri::Result::SUCCESS, "submit shaders"))
            return false;

        core.Wait(*fence, cycle + 1);

        if (!Check(core.GetFenceValue(*fence) >= cycle + 1, "shader completion"))
            return false;

        const auto* values = (const uint32_t*)core.MapBuffer(*buffers[2], 0, 64 * 16);

        if (!Check(values != nullptr, "storage readback"))
            return false;

        bool exact = true;

        for (uint32_t i = 0; i < 64; i++) {
            float transformed = float(i % 4) * 0.5f + 0.25f;
            uint32_t bits;
            std::memcpy(&bits, &transformed, sizeof(bits));
            uint32_t neighbor = (i / 32) * 32 + (i + 1) % 32;
            const uint32_t expected[] = {ExpectedValue(i, parameters), ExpectedValue(neighbor, parameters), bits, i ^ parameters.nonce};
            exact &= std::memcmp(values + i * 4, expected, sizeof(expected)) == 0;
        }

        core.UnmapBuffer(*buffers[2]);

        if (!Check(exact, "integer/group-barrier/matrix compute readback"))
            return false;

        const auto* pixels = (const uint8_t*)core.MapBuffer(*buffers[3], 0, slicePitch);

        if (!Check(pixels != nullptr, "pixel readback"))
            return false;

        for (uint32_t y = 0; y < 64; y++) {
            for (uint32_t x = 0; x < 64; x++) {
                std::array<uint8_t, 4> expected{};

                if (x >= 32 && x < 56 && y >= 24 && y < 48) {
                    uint32_t value = ExpectedValue((x + y * 64) % 64, parameters);
                    expected = {uint8_t((value & 1) * 255), uint8_t(((value >> 1) & 1) * 255), uint8_t(((value >> 2) & 1) * 255), 255};
                }

                if (std::memcmp(pixels + y * rowPitch + x * 4, expected.data(), 4) != 0) {
                    if (exact)
                        std::fprintf(stderr, "Pixel mismatch at %u,%u\n", x, y);

                    exact = false;
                }
            }
        }

        core.UnmapBuffer(*buffers[3]);

        if (!Check(exact && errors.load() == 0, "matrix/scissor/compute-to-fragment pixels"))
            return false;

        completedCycles++;

        return true;
    }

    bool Cleanup() {
        if (ready) {
            if (core.DeviceWaitIdle(device) != nri::Result::SUCCESS)
                return false;

            if (command)
                core.DestroyCommandBuffer(command);

            if (allocator)
                core.DestroyCommandAllocator(allocator);

            if (compute)
                core.DestroyPipeline(compute);

            if (graphics)
                core.DestroyPipeline(graphics);

            if (pipelineCache.cache)
                core.DestroyPipelineCache(pipelineCache.cache);

            if (pool)
                core.DestroyDescriptorPool(pool);

            if (layout)
                core.DestroyPipelineLayout(layout);

            for (auto* descriptor : descriptors) {
                if (descriptor)
                    core.DestroyDescriptor(descriptor);
            }

            if (color)
                core.DestroyDescriptor(color);

            if (image)
                core.DestroyTexture(image);

            for (auto* buffer : buffers) {
                if (buffer)
                    core.DestroyBuffer(buffer);
            }

            if (fence)
                core.DestroyFence(fence);
        }

        if (device)
            nri::nriDestroyDevice(device);

        return true;
    }
};

} // namespace

int main(int argc, char** argv) {
    bool validation = false;
    uint32_t adapter = 0;
    const char* cacheInput = nullptr;
    const char* cacheOutput = nullptr;
    bool requireCacheHit = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--validation") == 0)
            validation = true;
        else if (std::strcmp(argv[i], "--cache-input") == 0 && (i + 1) < argc)
            cacheInput = argv[++i];
        else if (std::strcmp(argv[i], "--cache-output") == 0 && (i + 1) < argc)
            cacheOutput = argv[++i];
        else if (std::strcmp(argv[i], "--cache-hit-only") == 0)
            requireCacheHit = true;
        else if (std::strcmp(argv[i], "--adapter") == 0 && (i + 1) < argc) {
            char* end = nullptr;
            const char* value = argv[++i];
            errno = 0;
            unsigned long long parsed = std::strtoull(value, &end, 10);

            if (!*value || *end || *value == '-' || errno == ERANGE || parsed > UINT32_MAX)
                return 2;

            adapter = uint32_t(parsed);
        } else
            return 2;
    }

    Fixture fixture;
    bool passed = fixture.Initialize(validation, adapter, cacheInput, requireCacheHit);

    for (uint32_t cycle = 0; cycle < 8 && passed; cycle++)
        passed = fixture.Execute(cycle);

    uint32_t graphicsCases = 0, maxChannelError = 0;

    if (passed)
        passed = RunGraphicsStates(*fixture.device, fixture.core, *fixture.queue, fixture.errors, fixture.pipelineCache, graphicsCases, maxChannelError);

    if (passed)
        passed = SavePipelineCache(*fixture.device, fixture.core, cacheOutput, fixture.pipelineCache);

    passed = fixture.Cleanup() && passed && fixture.errors.load() == 0;
    const auto& cache = fixture.pipelineCache;
    std::printf("NRI_PIPELINE_CACHE {\"loaded_bytes\":%llu,\"saved_bytes\":%llu,\"rejected_headers\":%u,\"cache_control\":%s,\"hit_required\":%s,\"create_ms\":%.3f,\"pipeline_ms\":%.3f}\n", (unsigned long long)cache.loadedBytes, (unsigned long long)cache.savedBytes, cache.rejectedHeaders, cache.control ? "true" : "false", cache.requireHit ? "true" : "false", cache.createMilliseconds, cache.pipelineMilliseconds);
    int written = std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_shader_execution\",\"status\":\"%s\",\"source_commit\":\"%s\",\"compiler\":\"%s\",\"device_id\":%u,\"driver_version_raw\":%u,\"validation_errors\":%u,\"validation_warnings\":%u,\"exact_cycles\":%u,\"graphics_state_cases\":%u,\"max_channel_error\":%u,\"legacy_sync_forced\":%s,\"shader_manifest\":%s}\n", passed ? "passed" : "failed", NRI_COMPATIBILITY_SOURCE_COMMIT, NRI_COMPATIBILITY_COMPILER, fixture.deviceId, fixture.driver, fixture.errors.load(), fixture.warnings.load(), fixture.completedCycles, graphicsCases, maxChannelError, NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false", shader_fixture::manifest);

    if (written < 0 || std::fflush(stdout) != 0)
        return 3;

    return passed ? 0 : 1;
}
