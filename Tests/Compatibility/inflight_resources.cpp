// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "compiled_shaders.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Parameters {
    uint32_t count, multiplier, bias, nonce;
    float transform[16];
};
static_assert(sizeof(Parameters) == 80 && offsetof(Parameters, transform) == 16, "Uniform ABI");

static uint32_t ExpectedValue(uint32_t index, const Parameters& p) {
    uint32_t value = index * p.multiplier + p.bias;
    value ^= value >> 16;

    return value * 0x85ebca6bu;
}

struct Slot {
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::DescriptorPool* pool = nullptr;
    nri::DescriptorSet* set = nullptr;
    nri::Fence* done = nullptr;
    std::array<nri::Buffer*, 3> buffers{};
    std::array<nri::Descriptor*, 2> views{};
    Parameters parameters = {};
    uint64_t value = 0;
    uint32_t job = 0;
    bool pending = false;
};

struct Fixture {
    static constexpr uint32_t JOBS = 96;
    std::atomic<uint32_t> errors{0}, warnings{0};
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::Queue* consumer = nullptr;
    nri::Queue* producer = nullptr;
    nri::PipelineLayout* layout = nullptr;
    nri::Pipeline* pipeline = nullptr;
    nri::Fence* gate = nullptr;
    std::array<Slot, 3> slots{};
    uint64_t lastReleased = 0, lastSubmitted = 0;
    uint32_t submitted = 0, completed = 0, blockedBatches = 0, pendingChecks = 0;
    uint32_t poolResets = 0, resourceGenerations = 0, retiredWithOthersPending = 0;
    uint32_t deviceId = 0, driver = 0, maxPending = 0;
    bool ready = false;

    static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* text, void* user) {
        auto& f = *(Fixture*)user;

        if (severity == nri::Message::ERROR)
            f.errors++;
        else if (severity == nri::Message::WARNING)
            f.warnings++;

        std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, text ? text : "");
    }

    static void NRI_CALL NoAbort(void*) {}

    bool Check(bool passed, const char* name) {
        if (!passed)
            std::fprintf(stderr, "In-flight resources failed: %s\n", name);

        return passed;
    }

    bool Initialize(bool validation) {
        uint32_t count = 0;

        if (!Check(nri::nriEnumerateAdapters(nullptr, count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && count, "adapter count"))
            return false;

        std::vector<nri::AdapterDesc> adapters(count);

        if (!Check(nri::nriEnumerateAdapters(adapters.data(), count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && count, "adapters") ||
            !Check(adapters[0].queueNum[size_t(nri::QueueType::GRAPHICS)] >= 2, "fixture requires two graphics queues"))
            return false;

        nri::QueueFamilyDesc family = {};
        family.queueType = nri::QueueType::GRAPHICS;
        family.queueNum = 2;
        nri::DeviceCreationDesc create = {};
        create.graphicsAPI = nri::GraphicsAPI::VK;
        create.adapterDesc = adapters.data();
        create.queueFamilies = &family;
        create.queueFamilyNum = 1;
        create.disableVKRayTracing = true;
        create.enableNRIValidation = create.enableGraphicsAPIValidation = validation;
        create.callbackInterface.MessageCallback = Message;
        create.callbackInterface.AbortExecution = NoAbort;
        create.callbackInterface.userArg = this;

        if (!Check(nri::nriCreateDevice(create, device) == nri::Result::SUCCESS, "device") ||
            !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core) == nri::Result::SUCCESS, "core"))
            return false;

        ready = true;
        deviceId = core.GetDeviceDesc(*device).adapterDesc.deviceId;
        driver = core.GetDeviceDesc(*device).adapterDesc.driverVersion;

        if (!Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, 0, consumer) == nri::Result::SUCCESS, "consumer queue") ||
            !Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, 1, producer) == nri::Result::SUCCESS, "producer queue") ||
            !Check(core.CreateFence(*device, nri::FenceDesc{}, gate) == nri::Result::SUCCESS, "gate fence"))
            return false;

        nri::DescriptorRangeDesc ranges[2] = {};

        for (uint32_t i = 0; i < 2; i++) {
            ranges[i].baseRegisterIndex = i;
            ranges[i].descriptorNum = 1;
            ranges[i].descriptorType = i == 0 ? nri::DescriptorType::CONSTANT_BUFFER : nri::DescriptorType::STORAGE_STRUCTURED_BUFFER;
        }

        nri::DescriptorSetDesc set = {};
        set.ranges = ranges;
        set.rangeNum = 2;
        nri::PipelineLayoutDesc desc = {};
        desc.descriptorSets = &set;
        desc.descriptorSetNum = 1;
        desc.rootRegisterSpace = 1;
        desc.shaderStages = nri::StageBits::COMPUTE_SHADER;
        desc.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;

        if (!Check(core.CreatePipelineLayout(*device, desc, layout) == nri::Result::SUCCESS, "layout"))
            return false;

        nri::ComputePipelineDesc compute = {};
        compute.pipelineLayout = layout;
        compute.shader = {nri::StageBits::COMPUTE_SHADER, shader_fixture::compute, sizeof(shader_fixture::compute), "computeMain"};

        if (!Check(core.CreateComputePipeline(*device, compute, pipeline) == nri::Result::SUCCESS, "pipeline"))
            return false;

        nri::DescriptorPoolDesc pool = {};
        pool.descriptorSetMaxNum = pool.constantBufferMaxNum = pool.storageStructuredBufferMaxNum = 1;

        for (auto& slot : slots) {
            if (!Check(core.CreateCommandAllocator(*consumer, slot.allocator) == nri::Result::SUCCESS, "allocator") ||
                !Check(core.CreateCommandBuffer(*slot.allocator, slot.command) == nri::Result::SUCCESS, "command") ||
                !Check(core.CreateDescriptorPool(*device, pool, slot.pool) == nri::Result::SUCCESS, "pool") ||
                !Check(core.CreateFence(*device, nri::FenceDesc{}, slot.done) == nri::Result::SUCCESS, "completion fence"))
                return false;
        }

        return true;
    }

    bool PrepareAndSubmit(Slot& slot, uint32_t job) {
        if (!Check(!slot.pending && core.GetFenceValue(*slot.done) >= slot.value, "slot completion before reuse"))
            return false;

        slot.parameters = {64, 1664525u + job * 2, 1013904223u + job * 104729, 0xa53c9e17u ^ (job * 0x9e3779b9u), {}};
        slot.parameters.transform[0] = slot.parameters.transform[5] = 0.5f;
        slot.parameters.transform[10] = slot.parameters.transform[15] = 1;
        slot.parameters.transform[12] = 0.25f + float(job % 4) * 0.125f;
        slot.parameters.transform[13] = -0.25f;

        for (uint32_t i = 0; i < 3; i++) {
            nri::BufferDesc buffer = {};
            buffer.size = i == 0 ? sizeof(Parameters) : 1024;
            buffer.usage = i == 0 ? nri::BufferUsageBits::CONSTANT : (i == 1 ? nri::BufferUsageBits::SHADER_RESOURCE_STORAGE : nri::BufferUsageBits::NONE);
            buffer.structureStride = i == 1 ? 16 : 0;
            auto location = i == 0 ? nri::MemoryLocation::HOST_UPLOAD : (i == 1 ? nri::MemoryLocation::DEVICE : nri::MemoryLocation::HOST_READBACK);

            if (!Check(core.CreateCommittedBuffer(*device, location, 0, buffer, slot.buffers[i]) == nri::Result::SUCCESS, "fresh buffer generation"))
                return false;
        }

        void* mapped = core.MapBuffer(*slot.buffers[0], 0, sizeof(Parameters));

        if (!Check(mapped != nullptr, "parameter map"))
            return false;

        std::memcpy(mapped, &slot.parameters, sizeof(Parameters));
        core.UnmapBuffer(*slot.buffers[0]);

        if (!Check(core.AllocateDescriptorSets(*slot.pool, *layout, 0, &slot.set, 1, 0) == nri::Result::SUCCESS, "fresh descriptor generation"))
            return false;

        for (uint32_t i = 0; i < 2; i++) {
            nri::BufferViewDesc view = {};
            view.buffer = slot.buffers[i];
            view.type = i == 0 ? nri::BufferView::CONSTANT_BUFFER : nri::BufferView::STORAGE_STRUCTURED_BUFFER;
            view.size = i == 0 ? sizeof(Parameters) : 1024;

            if (!Check(core.CreateBufferView(view, slot.views[i]) == nri::Result::SUCCESS, "fresh view"))
                return false;

            nri::UpdateDescriptorRangeDesc update = {};
            update.descriptorSet = slot.set;
            update.rangeIndex = i;
            update.descriptors = &slot.views[i];
            update.descriptorNum = 1;
            core.UpdateDescriptorRanges(&update, 1);
        }

        if (!Check(core.BeginCommandBuffer(*slot.command, slot.pool) == nri::Result::SUCCESS, "begin"))
            return false;

        nri::GlobalBarrierDesc host = {};
        host.before = {nri::AccessBits::HOST_WRITE, nri::StageBits::HOST};
        host.after = {nri::AccessBits::CONSTANT_BUFFER, nri::StageBits::COMPUTE_SHADER};
        nri::BufferBarrierDesc storage = {};
        storage.buffer = slot.buffers[1];
        storage.before = {nri::AccessBits::NONE, nri::StageBits::NONE};
        storage.after = {nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::StageBits::COMPUTE_SHADER};
        nri::BarrierDesc barriers = {};
        barriers.globals = &host;
        barriers.globalNum = 1;
        barriers.buffers = &storage;
        barriers.bufferNum = 1;
        core.CmdBarrier(*slot.command, barriers);
        core.CmdSetPipelineLayout(*slot.command, nri::BindPoint::COMPUTE, *layout);
        core.CmdSetPipeline(*slot.command, *pipeline);
        nri::SetDescriptorSetDesc set = {};
        set.descriptorSet = slot.set;
        set.bindPoint = nri::BindPoint::COMPUTE;
        core.CmdSetDescriptorSet(*slot.command, set);
        core.CmdDispatch(*slot.command, {2, 1, 1});
        barriers.globalNum = 0;
        storage.before = storage.after;
        storage.after = {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY};
        core.CmdBarrier(*slot.command, barriers);
        core.CmdCopyBuffer(*slot.command, *slot.buffers[2], 0, *slot.buffers[1], 0, 1024);
        host.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        host.after = {nri::AccessBits::HOST_READ, nri::StageBits::HOST};
        barriers.globalNum = 1;
        barriers.bufferNum = 0;
        core.CmdBarrier(*slot.command, barriers);

        if (!Check(core.EndCommandBuffer(*slot.command) == nri::Result::SUCCESS, "end"))
            return false;

        uint64_t value = job / slots.size() + 1;
        nri::FenceSubmitDesc wait = {gate, job + 1u, nri::StageBits::ALL};
        nri::FenceSubmitDesc signal = {slot.done, value, nri::StageBits::ALL};
        nri::QueueSubmitDesc submit = {};
        submit.waitFences = &wait;
        submit.waitFenceNum = 1;
        submit.commandBuffers = &slot.command;
        submit.commandBufferNum = 1;
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*consumer, submit) == nri::Result::SUCCESS, "future-dependent submission"))
            return false;

        slot.job = job;
        slot.value = value;
        slot.pending = true;
        lastSubmitted = job + 1;
        submitted++;
        resourceGenerations++;

        return true;
    }

    bool CheckPending(uint32_t& count) {
        count = 0;

        for (auto& slot : slots) {
            if (slot.pending) {
                count++;
                pendingChecks++;

                if (!Check(core.GetFenceValue(*slot.done) < slot.value, "unreleased work did not complete early"))
                    return false;
            }
        }

        maxPending = std::max(maxPending, count);

        return true;
    }

    bool Release(uint64_t value) {
        nri::FenceSubmitDesc signal = {gate, value, nri::StageBits::ALL};
        nri::QueueSubmitDesc submit = {};
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*producer, submit) == nri::Result::SUCCESS, "release from independent queue"))
            return false;

        lastReleased = value;

        return true;
    }

    void DestroyResources(Slot& slot) {
        for (auto*& view : slot.views) {
            if (view)
                core.DestroyDescriptor(view);

            view = nullptr;
        }

        for (auto*& buffer : slot.buffers) {
            if (buffer)
                core.DestroyBuffer(buffer);

            buffer = nullptr;
        }
    }

    bool Run() {
        for (uint32_t job = 0; job < slots.size(); job++) {
            if (!PrepareAndSubmit(slots[job], job))
                return false;
        }

        for (uint32_t job = 0; job < JOBS; job++) {
            uint32_t pending = 0;

            if (!CheckPending(pending) || !Check(pending != 0, "pending batch") || !Release(job + 1))
                return false;

            blockedBatches++;
            auto& slot = slots[job % slots.size()];
            core.Wait(*slot.done, slot.value);

            if (!Check(slot.job == job && core.GetFenceValue(*slot.done) >= slot.value, "matching completed generation"))
                return false;

            slot.pending = false;
            const auto* values = (const uint32_t*)core.MapBuffer(*slot.buffers[2], 0, 1024);

            if (!Check(values != nullptr, "readback map"))
                return false;

            bool exact = true;

            for (uint32_t i = 0; i < 64; i++) {
                float transformed = float(i % 4) * 0.5f + slot.parameters.transform[12];
                uint32_t bits;
                std::memcpy(&bits, &transformed, sizeof(bits));
                uint32_t neighbor = (i / 32) * 32 + (i + 1) % 32;
                const uint32_t expected[] = {ExpectedValue(i, slot.parameters), ExpectedValue(neighbor, slot.parameters), bits, i ^ slot.parameters.nonce};
                exact &= std::memcmp(values + i * 4, expected, sizeof(expected)) == 0;
            }

            core.UnmapBuffer(*slot.buffers[2]);

            if (!Check(exact && errors.load() == 0, "independent generation output") || !CheckPending(pending))
                return false;

            retiredWithOthersPending += pending != 0;
            core.ResetCommandAllocator(*slot.allocator);
            core.ResetDescriptorPool(*slot.pool);
            poolResets++;
            slot.set = nullptr;
            DestroyResources(slot);
            completed++;

            if ((job + slots.size()) < JOBS && !PrepareAndSubmit(slot, job + uint32_t(slots.size())))
                return false;
        }

        return Check(submitted == JOBS && completed == JOBS && maxPending == 3 && retiredWithOthersPending == JOBS - 1, "overlap and retirement coverage");
    }

    bool Cleanup() {
        bool passed = true;

        if (ready) {
            // On a failed check, release accepted future waits before teardown.
            if (lastSubmitted > lastReleased)
                passed = Release(lastSubmitted);

            passed = core.DeviceWaitIdle(device) == nri::Result::SUCCESS && passed;

            for (auto& slot : slots) {
                if (slot.command)
                    core.DestroyCommandBuffer(slot.command);

                if (slot.allocator)
                    core.DestroyCommandAllocator(slot.allocator);

                if (slot.pool)
                    core.DestroyDescriptorPool(slot.pool);

                DestroyResources(slot);

                if (slot.done)
                    core.DestroyFence(slot.done);
            }

            if (pipeline)
                core.DestroyPipeline(pipeline);

            if (layout)
                core.DestroyPipelineLayout(layout);

            if (gate)
                core.DestroyFence(gate);
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
    bool passed = fixture.Initialize(validation) && fixture.Run();
    passed = fixture.Cleanup() && passed && fixture.errors.load() == 0 && fixture.warnings.load() == 0;
    std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_inflight_resources\",\"status\":\"%s\",\"source_commit\":\"%s\",\"compiler\":\"%s\",\"device_id\":%u,\"driver_version_raw\":%u,\"validation_errors\":%u,\"validation_warnings\":%u,\"submitted_jobs\":%u,\"completed_jobs\":%u,\"blocked_batches\":%u,\"pending_checks\":%u,\"max_pending\":%u,\"pool_resets\":%u,\"resource_generations\":%u,\"retired_with_others_pending\":%u,\"legacy_sync_forced\":%s,\"shader_manifest\":%s}\n", passed ? "passed" : "failed", NRI_COMPATIBILITY_SOURCE_COMMIT, NRI_COMPATIBILITY_COMPILER, fixture.deviceId, fixture.driver, fixture.errors.load(), fixture.warnings.load(), fixture.submitted, fixture.completed, fixture.blockedBatches, fixture.pendingChecks, fixture.maxPending, fixture.poolResets, fixture.resourceGenerations, fixture.retiredWithOthersPending, NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false", shader_fixture::manifest);

    return std::fflush(stdout) == 0 && passed ? 0 : 1;
}
