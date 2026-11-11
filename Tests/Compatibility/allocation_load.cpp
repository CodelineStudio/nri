// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRIHelper.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Slot {
    nri::Buffer* buffer = nullptr;
    nri::Memory* memory = nullptr;
    uint64_t size = 0;
    uint32_t seed = 0;
};

static uint32_t Pattern(uint32_t word, uint32_t seed) {
    uint32_t value = word * 1664525u + seed;
    value ^= value >> 16;

    return value * 0x85ebca6bu;
}

struct Fixture {
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::HelperInterface helper = {};
    nri::Queue* queue = nullptr;
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::Fence* fence = nullptr;
    nri::Buffer* upload = nullptr;
    nri::Buffer* readback = nullptr;
    std::array<Slot, 32> slots{};
    std::array<uint32_t, 3> allocations{};
    std::atomic<uint32_t> errors{0}, warnings{0};
    uint64_t unit = 0, liveBytes = 0, peakBytes = 0, verifiedBytes = 0, uploadedBytes = 0;
    uint64_t sequence = 0, initialBudget = 0, initialUsage = 0, peakUsage = 0, finalUsage = 0;
    uint32_t samples = 0, checks = 0, cycles = 0, deviceId = 0, driver = 0;
    bool ready = false, budgetSupported = false;

    static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* text, void* user) {
        auto& f = *(Fixture*)user;

        if (severity == nri::Message::ERROR)
            f.errors++;
        else if (severity == nri::Message::WARNING)
            f.warnings++;

        std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, text ? text : "");
    }

    static void NRI_CALL NoAbort(void*) {}

    bool Check(bool passed, const char* operation) {
        if (!passed)
            std::fprintf(stderr, "Allocation load failed: %s\n", operation);

        return passed;
    }

    bool Snapshot(uint32_t cycle, const char* phase) {
        nri::VideoMemoryInfo info = {};
        nri::Result result = helper.QueryVideoMemoryInfo(*device, nri::MemoryLocation::DEVICE, info);

        if (!Check(result == nri::Result::SUCCESS || result == nri::Result::UNSUPPORTED, "budget query"))
            return false;

        bool supported = result == nri::Result::SUCCESS;

        if (samples == 0) {
            budgetSupported = supported;
            initialBudget = info.budgetSize;
            initialUsage = info.usageSize;
        } else if (!Check(supported == budgetSupported, "consistent budget capability"))
            return false;

        finalUsage = info.usageSize;
        peakUsage = std::max(peakUsage, info.usageSize);
        samples++;
        std::fprintf(stderr, "NRI_ALLOCATION_LOAD_MEMORY {\"cycle\":%u,\"phase\":\"%s\",\"budget_supported\":%s,\"budget_bytes\":%llu,\"usage_bytes\":%llu,\"live_payload_bytes\":%llu}\n",
            cycle, phase, supported ? "true" : "false", (unsigned long long)info.budgetSize, (unsigned long long)info.usageSize, (unsigned long long)liveBytes);

        return true;
    }

    bool Initialize(bool validation) {
        uint32_t count = 0;

        if (!Check(nri::nriEnumerateAdapters(nullptr, count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && count, "adapter count"))
            return false;

        std::vector<nri::AdapterDesc> adapters(count);

        if (!Check(nri::nriEnumerateAdapters(adapters.data(), count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && count, "adapters"))
            return false;

        nri::DeviceCreationDesc desc = {};
        desc.graphicsAPI = nri::GraphicsAPI::VK;
        desc.adapterDesc = adapters.data();
        desc.disableVKRayTracing = true;
        desc.enableNRIValidation = desc.enableGraphicsAPIValidation = validation;
        desc.callbackInterface.MessageCallback = Message;
        desc.callbackInterface.AbortExecution = NoAbort;
        desc.callbackInterface.userArg = this;

        if (!Check(nri::nriCreateDevice(desc, device) == nri::Result::SUCCESS, "device") ||
            !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core) == nri::Result::SUCCESS, "core"))
            return false;

        ready = true;
        deviceId = core.GetDeviceDesc(*device).adapterDesc.deviceId;
        driver = core.GetDeviceDesc(*device).adapterDesc.driverVersion;

        if (!Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::HelperInterface), &helper) == nri::Result::SUCCESS, "helper") || !Snapshot(0, "admission"))
            return false;

        // Limit requested device-buffer payload, not allocator block caches.
        // Budget availability is a snapshot, never a reservation or an OOM test.
        uint64_t capacity = adapters[0].videoMemorySize;
        uint64_t cap = capacity ? capacity / 16 : 80ull * 1024 * 1024;
        cap = std::min<uint64_t>(cap, 320ull * 1024 * 1024);

        if (budgetSupported)
            cap = std::min(cap, initialBudget > initialUsage ? (initialBudget - initialUsage) / 4 : uint64_t(0));

        unit = (cap / 80) & ~uint64_t(65535);

        if (!Check(unit >= 65536 && unit <= 4 * 1024 * 1024, "bounded payload admission"))
            return false;

        if (!Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, 0, queue) == nri::Result::SUCCESS, "queue") ||
            !Check(core.CreateCommandAllocator(*queue, allocator) == nri::Result::SUCCESS, "allocator") ||
            !Check(core.CreateCommandBuffer(*allocator, command) == nri::Result::SUCCESS, "command") ||
            !Check(core.CreateFence(*device, nri::FenceDesc{}, fence) == nri::Result::SUCCESS, "fence"))
            return false;

        nri::BufferDesc buffer = {};
        buffer.size = unit * 4;

        return Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_UPLOAD, 0, buffer, upload) == nri::Result::SUCCESS, "upload") &&
            Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_READBACK, 0, buffer, readback) == nri::Result::SUCCESS, "readback");
    }

    bool Allocate(uint32_t index, uint32_t generation) {
        auto& slot = slots[index];
        slot.size = unit * (1 + index % 4);
        slot.seed = 1013904223u + index * 104729u + generation * 0x9e3779b9u;
        nri::BufferDesc buffer = {};
        buffer.size = slot.size;
        uint32_t route = index % 3;

        if (route == 0) {
            if (!Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::DEVICE, 0, buffer, slot.buffer) == nri::Result::SUCCESS, "committed allocation"))
                return false;
        } else {
            if (!Check(core.CreateBuffer(*device, buffer, slot.buffer) == nri::Result::SUCCESS, "explicit buffer"))
                return false;

            nri::MemoryDesc requirements = {};
            core.GetBufferMemoryDesc(*slot.buffer, nri::MemoryLocation::DEVICE, requirements);
            nri::AllocateMemoryDesc allocation = {};
            allocation.size = requirements.size;
            allocation.type = requirements.type;
            allocation.vma.enable = route == 2;
            allocation.vma.alignment = requirements.alignment;

            if (!Check(core.AllocateMemory(*device, allocation, slot.memory) == nri::Result::SUCCESS, "explicit allocation"))
                return false;

            nri::BindBufferMemoryDesc binding = {slot.buffer, slot.memory, 0};

            if (!Check(core.BindBufferMemory(&binding, 1) == nri::Result::SUCCESS, "explicit binding"))
                return false;
        }

        liveBytes += slot.size;
        peakBytes = std::max(peakBytes, liveBytes);
        allocations[route]++;

        return Check(liveBytes <= unit * 80, "payload limit");
    }

    bool Transfer(Slot& slot, bool write) {
        if (write) {
            auto* values = (uint32_t*)core.MapBuffer(*upload, 0, slot.size);

            if (!Check(values != nullptr, "upload map"))
                return false;

            for (uint32_t i = 0; i < slot.size / 4; i++)
                values[i] = Pattern(i, slot.seed);

            core.UnmapBuffer(*upload);
        }

        core.ResetCommandAllocator(*allocator);

        if (!Check(core.BeginCommandBuffer(*command, nullptr) == nri::Result::SUCCESS, "begin"))
            return false;

        nri::GlobalBarrierDesc global = {};
        global.before = {nri::AccessBits::HOST_WRITE | nri::AccessBits::HOST_READ | nri::AccessBits::COPY_SOURCE | nri::AccessBits::COPY_DESTINATION, nri::StageBits::HOST | nri::StageBits::COPY};
        global.after = {nri::AccessBits::COPY_SOURCE | nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        nri::BarrierDesc barriers = {};
        barriers.globals = &global;
        barriers.globalNum = 1;
        core.CmdBarrier(*command, barriers);

        if (write) {
            core.CmdCopyBuffer(*command, *slot.buffer, 0, *upload, 0, slot.size);
            global.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
            global.after = {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY};
            core.CmdBarrier(*command, barriers);
        }

        core.CmdCopyBuffer(*command, *readback, 0, *slot.buffer, 0, slot.size);
        global.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        global.after = {nri::AccessBits::HOST_READ, nri::StageBits::HOST};
        core.CmdBarrier(*command, barriers);

        if (!Check(core.EndCommandBuffer(*command) == nri::Result::SUCCESS, "end"))
            return false;

        nri::FenceSubmitDesc signal = {fence, sequence + 1, nri::StageBits::ALL};
        nri::QueueSubmitDesc submit = {};
        submit.commandBuffers = &command;
        submit.commandBufferNum = 1;
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*queue, submit) == nri::Result::SUCCESS, "submit"))
            return false;

        sequence++;
        core.Wait(*fence, sequence);

        if (!Check(core.GetFenceValue(*fence) >= sequence && errors.load() == 0, "completion"))
            return false;

        const auto* values = (const uint32_t*)core.MapBuffer(*readback, 0, slot.size);

        if (!Check(values != nullptr, "readback map"))
            return false;

        bool exact = true;

        for (uint32_t i = 0; i < slot.size / 4; i++) {
            if (values[i] != Pattern(i, slot.seed)) {
                exact = false;
                break;
            }
        }

        core.UnmapBuffer(*readback);

        if (!Check(exact, "full allocation contents"))
            return false;

        checks++;
        verifiedBytes += slot.size;
        uploadedBytes += write ? slot.size : 0;

        return true;
    }

    void Retire(Slot& slot) {
        if (slot.buffer)
            core.DestroyBuffer(slot.buffer);

        if (slot.memory)
            core.FreeMemory(slot.memory);

        slot = {};
    }

    bool Run() {
        for (uint32_t cycle = 0; cycle < 4; cycle++) {
            if (!Snapshot(cycle, "before"))
                return false;

            for (uint32_t i = 0; i < slots.size(); i++) {
                if (!Allocate(i, cycle * 2) || !Transfer(slots[i], true))
                    return false;
            }

            if (!Snapshot(cycle, "full"))
                return false;

            for (uint32_t i = 1; i < slots.size(); i += 2) {
                liveBytes -= slots[i].size;
                Retire(slots[i]);
            }

            if (!Snapshot(cycle, "holes"))
                return false;

            for (uint32_t i = 1; i < slots.size(); i += 2) {
                if (!Allocate(i, cycle * 2 + 1) || !Transfer(slots[i], true))
                    return false;
            }

            if (!Snapshot(cycle, "refilled"))
                return false;

            for (uint32_t i = 0; i < slots.size(); i += 2) {
                if (!Transfer(slots[i], false))
                    return false;
            }

            for (auto& slot : slots)
                Retire(slot);

            liveBytes = 0;

            if (!Snapshot(cycle, "released"))
                return false;

            cycles++;
        }

        return true;
    }

    bool Cleanup() {
        if (ready) {
            if (!Check(core.DeviceWaitIdle(device) == nri::Result::SUCCESS, "cleanup idle"))
                return false; // Preserve resources until owned process exit if retirement failed.

            for (auto& slot : slots)
                Retire(slot);

            liveBytes = 0;

            if (upload)
                core.DestroyBuffer(upload);

            if (readback)
                core.DestroyBuffer(readback);

            if (command)
                core.DestroyCommandBuffer(command);

            if (allocator)
                core.DestroyCommandAllocator(allocator);

            if (fence)
                core.DestroyFence(fence);

            if (helper.QueryVideoMemoryInfo && !Snapshot(4, "cleanup"))
                return false;
        }

        if (device)
            nri::nriDestroyDevice(device);

        return true;
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

    Fixture f;
    bool passed = f.Initialize(validation) && f.Run();
    passed = f.Cleanup() && passed && f.errors.load() == 0 && f.warnings.load() == 0;
    passed = passed && f.cycles == 4 && f.checks == 256 && f.allocations == std::array<uint32_t, 3>{64, 68, 60};
    passed = passed && f.peakBytes == f.unit * 80 && f.verifiedBytes == f.unit * 640 && f.uploadedBytes == f.unit * 512 && f.samples == 22;
    std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_allocation_load\",\"status\":\"%s\",\"source_commit\":\"%s\",\"compiler\":\"%s\",\"device_id\":%u,\"driver_version_raw\":%u,\"validation_errors\":%u,\"validation_warnings\":%u,\"cycles\":%u,\"full_checks\":%u,\"committed_allocations\":%u,\"raw_allocations\":%u,\"vma_allocations\":%u,\"unit_bytes\":%llu,\"peak_live_payload_bytes\":%llu,\"verified_bytes\":%llu,\"uploaded_bytes\":%llu,\"budget_samples\":%u,\"budget_supported\":%s,\"initial_budget_bytes\":%llu,\"initial_usage_bytes\":%llu,\"peak_usage_bytes\":%llu,\"final_usage_bytes\":%llu,\"legacy_sync_forced\":%s}\n",
        passed ? "passed" : "failed", NRI_COMPATIBILITY_SOURCE_COMMIT, NRI_COMPATIBILITY_COMPILER, f.deviceId, f.driver, f.errors.load(), f.warnings.load(), f.cycles, f.checks, f.allocations[0], f.allocations[1], f.allocations[2], (unsigned long long)f.unit, (unsigned long long)f.peakBytes, (unsigned long long)f.verifiedBytes, (unsigned long long)f.uploadedBytes, f.samples, f.budgetSupported ? "true" : "false", (unsigned long long)f.initialBudget, (unsigned long long)f.initialUsage, (unsigned long long)f.peakUsage, (unsigned long long)f.finalUsage, NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false");

    return std::fflush(stdout) == 0 && passed ? 0 : 1;
}
