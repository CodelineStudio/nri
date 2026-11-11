// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT

#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRIWrapperVK.h"
#include <vulkan/vulkan.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

struct Fixture {
    std::atomic<uint32_t> errors{0};
    std::atomic<uint32_t> warnings{0};
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::WrapperVKInterface wrapper = {};
    nri::Queue* queue = nullptr;
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::Fence* fence = nullptr;
    nri::QueryPool* queries = nullptr;
    VkDevice nativeDevice = VK_NULL_HANDLE;
    VkQueryPool nativePool = VK_NULL_HANDLE;
    PFN_vkDestroyQueryPool destroyPool = nullptr;
    uint32_t familyFlags = 0;
    uint32_t cycles = 0;
    uint32_t deviceId = 0;
    uint32_t driverVersion = 0;
    bool coreReady = false;

    static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* text, void* user) {
        auto& fixture = *(Fixture*)user;

        if (severity == nri::Message::ERROR)
            fixture.errors++;
        else if (severity == nri::Message::WARNING)
            fixture.warnings++;

        std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, text ? text : "");
    }

    static void NRI_CALL DoNotAbort(void*) {
    }

    bool Check(bool passed, const char* operation) {
        if (!passed)
            std::fprintf(stderr, "Query reset fixture failed: %s\n", operation);

        return passed;
    }

    bool Run(bool validation, uint32_t adapterIndex) {
        uint32_t count = 0;

        if (!Check(nri::nriEnumerateAdapters(nullptr, count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && adapterIndex < count, "enumerate adapters"))
            return false;

        std::vector<nri::AdapterDesc> adapters(count);

        if (!Check(nri::nriEnumerateAdapters(adapters.data(), count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS, "adapter list"))
            return false;

        nri::QueueFamilyDesc family = {};
        family.queueType = nri::QueueType::COPY;
        family.queueNum = 1;
        nri::DeviceCreationDesc create = {};
        create.graphicsAPI = nri::GraphicsAPI::VK;
        create.adapterDesc = &adapters[adapterIndex];
        create.queueFamilies = &family;
        create.queueFamilyNum = 1;
        create.enableNRIValidation = validation;
        create.enableGraphicsAPIValidation = validation;
        create.disableVKRayTracing = true;
        create.callbackInterface.MessageCallback = Message;
        create.callbackInterface.AbortExecution = DoNotAbort;
        create.callbackInterface.userArg = this;

        if (!Check(nri::nriCreateDevice(create, device) == nri::Result::SUCCESS, "create copy-only device") ||
            !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core) == nri::Result::SUCCESS, "core interface"))
            return false;

        coreReady = true;
        const auto& desc = core.GetDeviceDesc(*device);
        deviceId = desc.adapterDesc.deviceId;
        driverVersion = desc.adapterDesc.driverVersion;

        if (!Check(desc.features.timestampCopyQueue, "copy queue timestamps supported") ||
            !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::WrapperVKInterface), &wrapper) == nri::Result::SUCCESS, "wrapper interface") ||
            !Check(core.GetQueue(*device, nri::QueueType::COPY, 0, queue) == nri::Result::SUCCESS, "copy queue") ||
            !Check(core.CreateCommandAllocator(*queue, allocator) == nri::Result::SUCCESS, "allocator") ||
            !Check(core.CreateCommandBuffer(*allocator, command) == nri::Result::SUCCESS, "command buffer") ||
            !Check(core.CreateFence(*device, nri::FenceDesc{}, fence) == nri::Result::SUCCESS, "fence"))
            return false;

        nativeDevice = (VkDevice)core.GetDeviceNativeObject(device);
        auto getDeviceProc = (PFN_vkGetDeviceProcAddr)wrapper.GetDeviceProcAddrVK(*device);
        auto getInstanceProc = (PFN_vkGetInstanceProcAddr)wrapper.GetInstanceProcAddrVK(*device);
        auto getFamilies = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)getInstanceProc((VkInstance)wrapper.GetInstanceVK(*device), "vkGetPhysicalDeviceQueueFamilyProperties");
        auto createPool = (PFN_vkCreateQueryPool)getDeviceProc(nativeDevice, "vkCreateQueryPool");
        destroyPool = (PFN_vkDestroyQueryPool)getDeviceProc(nativeDevice, "vkDestroyQueryPool");
        auto writeTimestamp = (PFN_vkCmdWriteTimestamp)getDeviceProc(nativeDevice, "vkCmdWriteTimestamp");
        auto getResults = (PFN_vkGetQueryPoolResults)getDeviceProc(nativeDevice, "vkGetQueryPoolResults");

        if (!Check(getFamilies && createPool && destroyPool && writeTimestamp && getResults, "core Vulkan dispatch"))
            return false;

        uint32_t familyCount = 0;
        VkPhysicalDevice physical = (VkPhysicalDevice)wrapper.GetPhysicalDeviceVK(*device);
        getFamilies(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> properties(familyCount);
        getFamilies(physical, &familyCount, properties.data());
        familyFlags = properties.at(wrapper.GetQueueFamilyIndexVK(*queue)).queueFlags;

        VkQueryPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        poolInfo.queryCount = 2;

        if (!Check(createPool(nativeDevice, &poolInfo, nullptr, &nativePool) == VK_SUCCESS, "native query pool"))
            return false;

        nri::QueryPoolVKDesc imported = {};
        imported.vkQueryPool = (VKNonDispatchableHandle)nativePool;
        imported.vkQueryType = VK_QUERY_TYPE_TIMESTAMP;

        if (!Check(wrapper.CreateQueryPoolVK(*device, imported, queries) == nri::Result::SUCCESS, "import query pool"))
            return false;

        for (uint32_t cycle = 0; cycle < 64; cycle++) {
            core.ResetCommandAllocator(*allocator);

            if (!Check(core.BeginCommandBuffer(*command, nullptr) == nri::Result::SUCCESS, "begin"))
                return false;

            // Record before the host reset. Use raw query commands solely to
            // inspect the same imported pool's native availability afterward.
            VkCommandBuffer nativeCommand = (VkCommandBuffer)core.GetCommandBufferNativeObject(command);
            writeTimestamp(nativeCommand, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, nativePool, 0);
            writeTimestamp(nativeCommand, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, nativePool, 1);

            if (!Check(core.EndCommandBuffer(*command) == nri::Result::SUCCESS, "end"))
                return false;

            // Concurrent, disjoint host ranges are legal. No other query use is
            // pending, and both calls must have synchronous native side effects.
            std::thread first([&] { core.ResetQueries(*queries, 0, 1); });
            std::thread second([&] { core.ResetQueries(*queries, 1, 1); });
            first.join();
            second.join();

            if (!Check(errors.load() == 0, "host reset diagnostics"))
                return false;

            uint64_t results[4] = {};
            VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;

            if (!Check(getResults(nativeDevice, nativePool, 0, 2, sizeof(results), results, 2 * sizeof(uint64_t), flags) == VK_NOT_READY && results[1] == 0 && results[3] == 0, "both reset ranges unavailable immediately"))
                return false;

            nri::FenceSubmitDesc signal = {};
            signal.fence = fence;
            signal.value = cycle + 1;
            signal.stages = nri::StageBits::ALL;
            nri::QueueSubmitDesc submit = {};
            submit.commandBuffers = &command;
            submit.commandBufferNum = 1;
            submit.signalFences = &signal;
            submit.signalFenceNum = 1;

            if (!Check(core.QueueSubmit(*queue, submit) == nri::Result::SUCCESS, "copy timestamp submission"))
                return false;

            core.Wait(*fence, signal.value);

            if (!Check(core.GetFenceValue(*fence) >= signal.value, "copy completion") ||
                !Check(getResults(nativeDevice, nativePool, 0, 2, sizeof(results), results, 2 * sizeof(uint64_t), flags) == VK_SUCCESS && results[1] && results[3] && results[2] >= results[0], "native query results"))
                return false;

            const uint64_t retained = results[2];
            core.ResetQueries(*queries, 0, 1);
            std::memset(results, 0, sizeof(results));

            if (!Check(getResults(nativeDevice, nativePool, 0, 2, sizeof(results), results, 2 * sizeof(uint64_t), flags) == VK_NOT_READY && results[1] == 0 && results[3] && results[2] == retained, "partial reset preserves other native result"))
                return false;

            cycles++;
        }

        return true;
    }

    bool Cleanup() {
        bool idle = true;

        if (coreReady) {
            idle = core.DeviceWaitIdle(device) == nri::Result::SUCCESS;

            if (queries)
                core.DestroyQueryPool(queries);

            if (command)
                core.DestroyCommandBuffer(command);

            if (allocator)
                core.DestroyCommandAllocator(allocator);

            if (fence)
                core.DestroyFence(fence);
        }

        if (nativePool)
            destroyPool(nativeDevice, nativePool, nullptr);

        if (device)
            nri::nriDestroyDevice(device);

        return idle;
    }
};

} // namespace

int main(int argc, char** argv) {
    bool validation = false;
    uint32_t adapterIndex = 0;

    for (int i = 1; i < argc; i++) {
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
        } else
            return 2;
    }

    Fixture fixture;
    bool passed = fixture.Run(validation, adapterIndex);
    passed = fixture.Cleanup() && passed && fixture.errors.load() == 0;
    int written = std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_query_reset\",\"status\":\"%s\",\"source_commit\":\"%s\",\"compiler\":\"%s\",\"device_id\":%u,\"driver_version_raw\":%u,\"validation_errors\":%u,\"validation_warnings\":%u,\"copy_queue_family_flags\":%u,\"partial_reset_cycles\":%u,\"legacy_sync_forced\":%s}\n",
        passed ? "passed" : "failed", NRI_COMPATIBILITY_SOURCE_COMMIT, NRI_COMPATIBILITY_COMPILER, fixture.deviceId, fixture.driverVersion, fixture.errors.load(), fixture.warnings.load(), fixture.familyFlags, fixture.cycles, NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false");

    if (written < 0 || std::fflush(stdout) != 0)
        return 3;

    return passed ? 0 : 1;
}
