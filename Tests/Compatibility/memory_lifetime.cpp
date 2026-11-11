// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Fixture {
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::Queue* queue = nullptr;
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::Fence* fence = nullptr;
    nri::Buffer* readback = nullptr;
    std::array<nri::Buffer*, 2> buffers{};
    std::array<nri::Texture*, 2> images{};
    std::array<nri::Memory*, 4> memory{};
    std::atomic<uint32_t> errors{0}, warnings{0}, expectedErrors{0};
    uint32_t cycles = 0, recoveries = 0, deviceId = 0, driver = 0, nonzeroBindings = 0;
    bool ready = false, expectingFailure = false;

    static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* text, void* user) {
        auto& f = *(Fixture*)user;

        if (severity == nri::Message::ERROR && f.expectingFailure && text &&
            (std::strstr(text, "vkAllocateMemory(dedicated)") || std::strstr(text, "vkMapMemory(dedicated)")))
            f.expectedErrors++;
        else if (severity == nri::Message::ERROR)
            f.errors++;
        else if (severity == nri::Message::WARNING)
            f.warnings++;

        std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, text ? text : "");
    }

    static void NRI_CALL NoAbort(void*) {}

    bool Check(bool success, const char* operation) {
        if (!success)
            std::fprintf(stderr, "Memory qualification failed: %s\n", operation);

        return success;
    }

    bool Initialize(bool validation) {
        uint32_t count = 0;

        if (!Check(nri::nriEnumerateAdapters(nullptr, count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && count, "adapter count"))
            return false;

        std::vector<nri::AdapterDesc> adapters(count);

        if (!Check(nri::nriEnumerateAdapters(adapters.data(), count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && count, "adapter list"))
            return false;

        nri::DeviceCreationDesc create = {};
        create.graphicsAPI = nri::GraphicsAPI::VK;
        create.adapterDesc = adapters.data();
        create.disableVKRayTracing = true;
        create.enableNRIValidation = create.enableGraphicsAPIValidation = validation;
        create.callbackInterface.MessageCallback = Message;
        create.callbackInterface.AbortExecution = NoAbort;
        create.callbackInterface.userArg = this;

        if (!Check(nri::nriCreateDevice(create, device) == nri::Result::SUCCESS, "device") ||
            !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core) == nri::Result::SUCCESS, "core"))
            return false;

        ready = true;
        const auto& desc = core.GetDeviceDesc(*device);
        deviceId = desc.adapterDesc.deviceId;
        driver = desc.adapterDesc.driverVersion;

        if (!Check(desc.memoryAlignment.uploadBufferTextureRow <= 256 && desc.memoryAlignment.uploadBufferTextureSlice <= 1024, "fixture copy alignment") ||
            !Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, 0, queue) == nri::Result::SUCCESS, "queue") ||
            !Check(core.CreateCommandAllocator(*queue, allocator) == nri::Result::SUCCESS, "allocator") ||
            !Check(core.CreateCommandBuffer(*allocator, command) == nri::Result::SUCCESS, "command") ||
            !Check(core.CreateFence(*device, nri::FenceDesc{}, fence) == nri::Result::SUCCESS, "fence"))
            return false;

        nri::BufferDesc read = {};
        read.size = 4096;

        return Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_READBACK, 0, read, readback) == nri::Result::SUCCESS, "readback");
    }

    bool Allocate(uint32_t index, const nri::MemoryDesc& desc, bool vma) {
        if (!Check(!NRI_COMPATIBILITY_FORCE_DEDICATED_MEMORY || desc.mustBeDedicated, "dedicated fixture profile"))
            return false;

        nri::AllocateMemoryDesc allocate = {};
        allocate.size = desc.size;

        if (index < 2 && !desc.mustBeDedicated)
            allocate.size += 2 * uint64_t(desc.alignment) + (index == 0 ? 1024 : 0);

        allocate.type = desc.type;
        allocate.vma.enable = vma;
        allocate.vma.alignment = desc.alignment;

        return Check(core.AllocateMemory(*device, allocate, memory[index]) == nri::Result::SUCCESS, "memory object");
    }

    bool Cycle(uint32_t cycle) {
        std::array<std::array<uint8_t, 1024>, 2> expected{};
        nri::BindBufferMemoryDesc bufferBindings[2] = {};
        nri::BindTextureMemoryDesc imageBindings[2] = {};

        for (uint32_t i = 0; i < 2; i++) {
            nri::BufferDesc desc = {};
            desc.size = 1024;

            if (!Check(core.CreateBuffer(*device, desc, buffers[i]) == nri::Result::SUCCESS, "buffer"))
                return false;

            nri::MemoryDesc requirements = {};
            core.GetBufferMemoryDesc(*buffers[i], nri::MemoryLocation::HOST_UPLOAD, requirements);

            if (!Allocate(i, requirements, (cycle & 1) != 0))
                return false;

            uint64_t offset = requirements.mustBeDedicated ? 0 : requirements.alignment;
            bufferBindings[i] = {buffers[i], memory[i], offset};
            nonzeroBindings += offset != 0;
            nri::TextureDesc image = {};
            image.type = nri::TextureType::TEXTURE_2D;
            image.format = nri::Format::RGBA8_UNORM;
            image.width = image.height = 4;
            image.depth = image.mipNum = image.layerNum = image.sampleNum = 1;
            image.usage = nri::TextureUsageBits::SHADER_RESOURCE;

            if (!Check(core.CreateTexture(*device, image, images[i]) == nri::Result::SUCCESS, "image"))
                return false;

            core.GetTextureMemoryDesc(*images[i], nri::MemoryLocation::DEVICE, requirements);

            if (!Allocate(i + 2, requirements, (cycle & 1) != 0))
                return false;

            imageBindings[i] = {images[i], memory[i + 2], 0};
        }

        if (NRI_COMPATIBILITY_MEMORY_FAILURES && cycle == 0) {
            expectingFailure = true;
            nri::Result allocation = core.BindBufferMemory(bufferBindings, 2);
            nri::Result mapping = core.BindBufferMemory(bufferBindings, 2);
            expectingFailure = false;

            if (!Check(allocation == nri::Result::OUT_OF_MEMORY && mapping == nri::Result::FAILURE, "buffer allocation/map failure propagation"))
                return false;

            recoveries += 2;
        }

        if (!Check(core.BindBufferMemory(bufferBindings, 2) == nri::Result::SUCCESS, "buffer batch retry"))
            return false;

        if (NRI_COMPATIBILITY_MEMORY_FAILURES && cycle == 0) {
            expectingFailure = true;
            nri::Result allocation = core.BindTextureMemory(imageBindings, 2);
            expectingFailure = false;

            if (!Check(allocation == nri::Result::OUT_OF_MEMORY, "image allocation failure propagation"))
                return false;

            recoveries++;
        }

        if (!Check(core.BindTextureMemory(imageBindings, 2) == nri::Result::SUCCESS, "image batch retry"))
            return false;

        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t byte = 0; byte < 1024; byte++)
                expected[i][byte] = uint8_t(byte * 13 + cycle * 17 + i * 43);

            void* mapped = core.MapBuffer(*buffers[i], 0, nri::WHOLE_SIZE);

            if (!Check(mapped != nullptr, "whole size map"))
                return false;

            core.UnmapBuffer(*buffers[i]);
            mapped = core.MapBuffer(*buffers[i], 0, 1024);

            if (!Check(mapped != nullptr, "initial map"))
                return false;

            std::memcpy(mapped, expected[i].data(), 1024);
            core.UnmapBuffer(*buffers[i]);
            mapped = core.MapBuffer(*buffers[i], 5, 500);

            if (!Check(mapped != nullptr, "partial map"))
                return false;

            for (uint32_t byte = 5; byte < 505; byte++)
                expected[i][byte] ^= 0x93;

            std::memcpy(mapped, expected[i].data() + 5, 500);
            core.UnmapBuffer(*buffers[i]);
        }

        core.ResetCommandAllocator(*allocator);

        if (!Check(core.BeginCommandBuffer(*command, nullptr) == nri::Result::SUCCESS, "begin"))
            return false;

        nri::GlobalBarrierDesc global = {};
        global.before = {nri::AccessBits::HOST_WRITE | nri::AccessBits::HOST_READ | nri::AccessBits::COPY_DESTINATION, nri::StageBits::HOST | nri::StageBits::COPY};
        global.after = {nri::AccessBits::COPY_SOURCE | nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        nri::BarrierDesc barriers = {};
        barriers.globals = &global;
        barriers.globalNum = 1;
        core.CmdBarrier(*command, barriers);

        for (uint32_t i = 0; i < 2; i++) {
            core.CmdCopyBuffer(*command, *readback, i * 1024, *buffers[i], 0, 1024);
            nri::TextureBarrierDesc image = {};
            image.texture = images[i];
            image.before = {nri::AccessBits::NONE, nri::Layout::UNDEFINED, nri::StageBits::NONE};
            image.after = {nri::AccessBits::COPY_DESTINATION, nri::Layout::COPY_DESTINATION, nri::StageBits::COPY};
            image.mipNum = image.layerNum = 1;
            image.planes = nri::PlaneBits::COLOR;
            nri::BarrierDesc imageBarriers = {};
            imageBarriers.textures = &image;
            imageBarriers.textureNum = 1;
            core.CmdBarrier(*command, imageBarriers);
            nri::TextureRegionDesc region = {};
            region.width = region.height = 4;
            region.depth = 1;
            region.planes = nri::PlaneBits::COLOR;
            nri::TextureDataLayoutDesc data = {};
            data.rowPitch = 256;
            data.slicePitch = 1024;
            core.CmdUploadBufferToTexture(*command, *images[i], region, *buffers[i], data);
            image.before = image.after;
            image.after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
            core.CmdBarrier(*command, imageBarriers);
            data.offset = 2048 + i * 1024;
            core.CmdReadbackTextureToBuffer(*command, *readback, data, *images[i], region);
        }

        global.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        global.after = {nri::AccessBits::HOST_READ, nri::StageBits::HOST};
        core.CmdBarrier(*command, barriers);

        if (!Check(core.EndCommandBuffer(*command) == nri::Result::SUCCESS, "end"))
            return false;

        nri::FenceSubmitDesc signal = {fence, cycle + 1u, nri::StageBits::ALL};
        nri::QueueSubmitDesc submit = {};
        submit.commandBuffers = &command;
        submit.commandBufferNum = 1;
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*queue, submit) == nri::Result::SUCCESS, "submit"))
            return false;

        core.Wait(*fence, cycle + 1);

        if (!Check(core.GetFenceValue(*fence) >= cycle + 1, "completion before retirement"))
            return false;

        const auto* bytes = (const uint8_t*)core.MapBuffer(*readback, 0, 4096);

        if (!Check(bytes != nullptr, "readback map"))
            return false;

        bool exact = true;

        for (uint32_t i = 0; i < 2; i++) {
            exact &= std::memcmp(bytes + i * 1024, expected[i].data(), 1024) == 0;

            for (uint32_t row = 0; row < 4; row++)
                exact &= std::memcmp(bytes + 2048 + i * 1024 + row * 256, expected[i].data() + row * 256, 16) == 0;
        }

        core.UnmapBuffer(*readback);

        if (!Check(exact, "buffer/image bytes after retry and partial mapping"))
            return false;

        Retire();
        cycles++;

        return errors.load() == 0;
    }

    void Retire() {
        for (auto*& buffer : buffers) {
            if (buffer)
                core.DestroyBuffer(buffer);

            buffer = nullptr;
        }

        for (auto*& image : images) {
            if (image)
                core.DestroyTexture(image);

            image = nullptr;
        }

        for (auto*& allocation : memory) {
            if (allocation)
                core.FreeMemory(allocation);

            allocation = nullptr;
        }
    }

    bool Cleanup() {
        bool passed = true;

        if (ready) {
            passed = core.DeviceWaitIdle(device) == nri::Result::SUCCESS;
            Retire();

            if (readback)
                core.DestroyBuffer(readback);

            if (command)
                core.DestroyCommandBuffer(command);

            if (allocator)
                core.DestroyCommandAllocator(allocator);

            if (fence)
                core.DestroyFence(fence);
        }

        if (device)
            nri::nriDestroyDevice(device);

        return passed;
    }
};

} // namespace

int main(int argc, char** argv) {
    bool validation = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--validation") == 0)
            validation = true;
        else if (std::strcmp(argv[i], "--adapter") == 0 && (i + 1) < argc && std::strcmp(argv[++i], "0") == 0) {}
        else
            return 2;
    }

    Fixture fixture;
    bool passed = fixture.Initialize(validation);

    for (uint32_t cycle = 0; cycle < 64 && passed; cycle++)
        passed = fixture.Cycle(cycle);

    passed = fixture.Cleanup() && passed && fixture.errors.load() == 0 && fixture.warnings.load() == 0;
    passed &= fixture.expectedErrors.load() == (NRI_COMPATIBILITY_MEMORY_FAILURES ? 3u : 0u);
    std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_memory_lifetime\",\"status\":\"%s\",\"source_commit\":\"%s\",\"compiler\":\"%s\",\"device_id\":%u,\"driver_version_raw\":%u,\"validation_errors\":%u,\"validation_warnings\":%u,\"exact_cycles\":%u,\"expected_errors\":%u,\"recoveries\":%u,\"nonzero_bindings\":%u,\"mapped_ranges_forced\":%s,\"forced_dedicated\":%s,\"memory_failures\":%s,\"legacy_sync_forced\":%s}\n", passed ? "passed" : "failed", NRI_COMPATIBILITY_SOURCE_COMMIT, NRI_COMPATIBILITY_COMPILER, fixture.deviceId, fixture.driver, fixture.errors.load(), fixture.warnings.load(), fixture.cycles, fixture.expectedErrors.load(), fixture.recoveries, fixture.nonzeroBindings, NRI_COMPATIBILITY_MAPPED_RANGES ? "true" : "false", NRI_COMPATIBILITY_FORCE_DEDICATED_MEMORY ? "true" : "false", NRI_COMPATIBILITY_MEMORY_FAILURES ? "true" : "false", NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false");

    return std::fflush(stdout) == 0 && passed ? 0 : 1;
}
