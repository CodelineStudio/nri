// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRIWrapperVK.h"
#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

struct Fixture {
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::WrapperVKInterface wrapper = {};
    nri::Queue* queues[2] = {};
    nri::Fence* fences[3] = {};
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::Buffer* upload = nullptr;
    nri::Buffer* readback = nullptr;
    VkDevice nativeDevice = VK_NULL_HANDLE;
    PFN_vkDeviceWaitIdle nativeIdle = nullptr;
    std::atomic<uint32_t> errors{0}, warnings{0}, expectedErrors{0}, lossReports{0}, oomReports{0}, started{0}, returned{0};
    uint32_t deviceId = 0, driver = 0;
    bool ready = false, pendingAccepted = false, rejected = false, drained = false, exact = false, pollObserved = false, promoted = false;

    static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* text, void* user) {
        auto& f = *(Fixture*)user;

        if (severity == nri::Message::ERROR && text &&
            ((NRI_COMPATIBILITY_POLL_LOSS_MODE == 0 && std::strstr(text, "Legacy queue scheduler: injected QueueSubmit failed (-4)")) ||
             (NRI_COMPATIBILITY_POLL_LOSS_MODE != 0 && std::strstr(text, "Legacy queue scheduler: GetFenceStatus failed (-4)")))) {
            f.expectedErrors++;
            f.lossReports++;
        } else if (severity == nri::Message::ERROR && NRI_COMPATIBILITY_POLL_LOSS_MODE == 2 && text && std::strstr(text, "Legacy queue scheduler: injected QueueSubmit failed (-2)")) {
            f.expectedErrors++;
            f.oomReports++;
        }
        else if (severity == nri::Message::ERROR)
            f.errors++;
        else if (severity == nri::Message::WARNING)
            f.warnings++;

        std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, text ? text : "");
    }

    static void NRI_CALL NoAbort(void*) {}

    bool Check(bool passed, const char* operation) {
        if (!passed)
            std::fprintf(stderr, "Scheduler loss failed: %s\n", operation);

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
        nri::DeviceCreationDesc desc = {};
        desc.graphicsAPI = nri::GraphicsAPI::VK;
        desc.adapterDesc = adapters.data();
        desc.queueFamilies = &family;
        desc.queueFamilyNum = 1;
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

        if (!Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::WrapperVKInterface), &wrapper) == nri::Result::SUCCESS, "wrapper"))
            return false;

        nativeDevice = (VkDevice)core.GetDeviceNativeObject(device);
        auto getProc = (PFN_vkGetDeviceProcAddr)wrapper.GetDeviceProcAddrVK(*device);

        if (!Check(nativeDevice && getProc, "native cleanup dispatch"))
            return false;

        nativeIdle = (PFN_vkDeviceWaitIdle)getProc(nativeDevice, "vkDeviceWaitIdle");

        if (!Check(nativeIdle != nullptr, "native idle"))
            return false;

        for (uint32_t i = 0; i < 2; i++) {
            if (!Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, i, queues[i]) == nri::Result::SUCCESS, "queue"))
                return false;
        }

        for (auto*& fence : fences) {
            if (!Check(core.CreateFence(*device, nri::FenceDesc{}, fence) == nri::Result::SUCCESS, "fence"))
                return false;
        }

        nri::BufferDesc buffer = {};
        buffer.size = 4096;

        return Check(core.CreateCommandAllocator(*queues[1], allocator) == nri::Result::SUCCESS, "allocator") &&
            Check(core.CreateCommandBuffer(*allocator, command) == nri::Result::SUCCESS, "command") &&
            Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_UPLOAD, 0, buffer, upload) == nri::Result::SUCCESS, "upload") &&
            Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_READBACK, 0, buffer, readback) == nri::Result::SUCCESS, "readback");
    }

    bool StartCopy() {
        auto* values = (uint32_t*)core.MapBuffer(*upload, 0, 4096);

        if (!Check(values != nullptr, "map upload"))
            return false;

        for (uint32_t i = 0; i < 1024; i++)
            values[i] = i * 1664525u + 1013904223u;

        core.UnmapBuffer(*upload);

        if (!Check(core.BeginCommandBuffer(*command, nullptr) == nri::Result::SUCCESS, "begin"))
            return false;

        nri::GlobalBarrierDesc global = {};
        global.before = {nri::AccessBits::HOST_WRITE, nri::StageBits::HOST};
        global.after = {nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY};
        nri::BarrierDesc barriers = {};
        barriers.globals = &global;
        barriers.globalNum = 1;
        core.CmdBarrier(*command, barriers);
        core.CmdCopyBuffer(*command, *readback, 0, *upload, 0, 4096);
        global.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        global.after = {nri::AccessBits::HOST_READ, nri::StageBits::HOST};
        core.CmdBarrier(*command, barriers);

        if (!Check(core.EndCommandBuffer(*command) == nri::Result::SUCCESS, "end"))
            return false;

        nri::FenceSubmitDesc first = {fences[0], 1, nri::StageBits::ALL};
        nri::QueueSubmitDesc copy = {};
        copy.commandBuffers = &command;
        copy.commandBufferNum = 1;
        copy.signalFences = &first;
        copy.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*queues[1], copy) == nri::Result::SUCCESS, "first native submission"))
            return false;

        return true;
    }

    bool Run() {
        if (!StartCopy())
            return false;

        nri::FenceSubmitDesc gate = {fences[1], 9, nri::StageBits::ALL};
        nri::FenceSubmitDesc done = {fences[2], 9, nri::StageBits::ALL};
        nri::QueueSubmitDesc future = {};
        future.waitFences = &gate;
        future.waitFenceNum = 1;
        future.signalFences = &done;
        future.signalFenceNum = 1;
        pendingAccepted = core.QueueSubmit(*queues[0], future) == nri::Result::SUCCESS;

        if (!Check(pendingAccepted, "accepted deferred packet"))
            return false;

        std::array<std::thread, 2> waiters;

        for (auto& waiter : waiters) {
            waiter = std::thread([&] {
                started++;
                core.Wait(*fences[2], 9);
                returned++;
            });
        }

        while (started.load() != waiters.size())
            std::this_thread::yield();

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        bool stillWaiting = returned.load() == 0;
        nri::QueueSubmitDesc producer = {};
        producer.signalFences = &gate;
        producer.signalFenceNum = 1;
        nri::Result failure = core.QueueSubmit(*queues[1], producer);

        for (auto& waiter : waiters)
            waiter.join();

        if (!Check(stillWaiting && returned.load() == 2 && failure == nri::Result::DEVICE_LOST, "loss releases host waits"))
            return false;

        rejected = core.QueueSubmit(*queues[1], producer) == nri::Result::DEVICE_LOST;
        drained = core.DeviceWaitIdle(device) == nri::Result::DEVICE_LOST;

        if (!Check(rejected && drained && core.GetFenceValue(*fences[0]) == 1 && core.GetFenceValue(*fences[1]) == 0 && core.GetFenceValue(*fences[2]) == 0, "sticky loss and confirmed completion only"))
            return false;

        return VerifyCopy() && Check(expectedErrors.load() == 1 && errors.load() == 0, "accepted output survives later rejection");
    }

    bool VerifyCopy() {
        const auto* result = (const uint32_t*)core.MapBuffer(*readback, 0, 4096);

        if (!Check(result != nullptr, "read completed copy"))
            return false;

        exact = true;

        for (uint32_t i = 0; i < 1024; i++)
            exact &= result[i] == i * 1664525u + 1013904223u;

        core.UnmapBuffer(*readback);

        return Check(exact, "exact accepted copy output");
    }

    bool RunPollLoss() {
        if (!StartCopy())
            return false;

        nri::QueueSubmitDesc empty = {};

        if (NRI_COMPATIBILITY_POLL_LOSS_MODE == 2) {
            if (!Check(core.QueueSubmit(*queues[1], empty) == nri::Result::OUT_OF_MEMORY, "initial submission OOM"))
                return false;
        }

        std::array<std::thread, 2> waiters;

        for (auto& waiter : waiters) {
            waiter = std::thread([&] {
                core.Wait(*fences[0], 1);
                returned++;
            });
        }

        for (auto& waiter : waiters)
            waiter.join();

        nri::Result initial = NRI_COMPATIBILITY_POLL_LOSS_MODE == 2 ? nri::Result::OUT_OF_MEMORY : nri::Result::DEVICE_LOST;

        if (!Check(returned.load() == 2 && core.GetFenceValue(*fences[0]) == 0 && core.QueueSubmit(*queues[1], empty) == initial, "failed polling does not publish completion"))
            return false;

        nri::Result firstDrain = core.DeviceWaitIdle(device);
        pollObserved = firstDrain == nri::Result::DEVICE_LOST && lossReports.load() == 1;

        if (NRI_COMPATIBILITY_POLL_LOSS_MODE == 2) {
            promoted = pollObserved && oomReports.load() == 1 && core.GetFenceValue(*fences[0]) == 0;

            if (!Check(promoted, "poll loss supersedes prior OOM without claiming completion"))
                return false;
        }

        rejected = core.QueueSubmit(*queues[1], empty) == nri::Result::DEVICE_LOST;
        drained = core.DeviceWaitIdle(device) == nri::Result::DEVICE_LOST;

        if (!Check(pollObserved && rejected && drained && core.GetFenceValue(*fences[0]) == 1, "native-completed copy retires while failure stays sticky"))
            return false;

        return VerifyCopy() && Check(expectedErrors.load() == (NRI_COMPATIBILITY_POLL_LOSS_MODE == 2 ? 2u : 1u) && errors.load() == 0, "expected fault reports only");
    }

    bool Cleanup() {
        // The injected loss never reaches Vulkan. Independently quiesce the
        // healthy native device before freeing objects, even after a failed check.
        if (ready && (!nativeIdle || nativeIdle(nativeDevice) != VK_SUCCESS))
            return false;

        if (ready) {
            core.DeviceWaitIdle(device);

            if (command)
                core.DestroyCommandBuffer(command);

            if (allocator)
                core.DestroyCommandAllocator(allocator);

            for (auto* buffer : {upload, readback}) {
                if (buffer)
                    core.DestroyBuffer(buffer);
            }

            for (auto* fence : fences) {
                if (fence)
                    core.DestroyFence(fence);
            }
        }

        if (device)
            nri::nriDestroyDevice(device);

        return true;
    }
};

} // namespace

int main(int argc, char** argv) {
    if (NRI_COMPATIBILITY_POLL_LOSS_MODE == 0 && (NRI_COMPATIBILITY_FAIL_SUBMIT != 2 || !NRI_COMPATIBILITY_SUBMIT_DEVICE_LOST)) {
        std::fprintf(stderr, "Requires FAIL_SUBMIT=2 and SUBMIT_DEVICE_LOST=ON\n");

        return 2;
    }

    bool validation = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--validation") == 0)
            validation = true;
        else if (std::strcmp(argv[i], "--adapter") == 0 && (i + 1) < argc && std::strcmp(argv[++i], "0") == 0) {}
        else
            return 2;
    }

    Fixture f;
    bool passed = f.Initialize(validation) && (NRI_COMPATIBILITY_POLL_LOSS_MODE ? f.RunPollLoss() : f.Run());
    passed = f.Cleanup() && passed && f.errors.load() == 0 && f.warnings.load() == 0;
    std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_scheduler_loss\",\"status\":\"%s\",\"source_commit\":\"%s\",\"compiler\":\"%s\",\"device_id\":%u,\"driver_version_raw\":%u,\"validation_errors\":%u,\"validation_warnings\":%u,\"expected_errors\":%u,\"returned_waiters\":%u,\"poll_loss_mode\":%u,\"loss_reports\":%u,\"oom_reports\":%u,\"poll_observed\":%s,\"loss_promoted\":%s,\"pending_accepted\":%s,\"later_rejected\":%s,\"failure_drained\":%s,\"accepted_copy_exact\":%s,\"legacy_sync_forced\":%s}\n",
        passed ? "passed" : "failed", NRI_COMPATIBILITY_SOURCE_COMMIT, NRI_COMPATIBILITY_COMPILER, f.deviceId, f.driver, f.errors.load(), f.warnings.load(), f.expectedErrors.load(), f.returned.load(), NRI_COMPATIBILITY_POLL_LOSS_MODE, f.lossReports.load(), f.oomReports.load(), f.pollObserved ? "true" : "false", f.promoted ? "true" : "false", f.pendingAccepted ? "true" : "false", f.rejected ? "true" : "false", f.drained ? "true" : "false", f.exact ? "true" : "false", NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false");

    return std::fflush(stdout) == 0 && passed ? 0 : 1;
}
