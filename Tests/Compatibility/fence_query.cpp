// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "NRI.h"
#include "Extensions/NRIWrapperVK.h"
#include "Extensions/NRISwapChain.h"
#include <vulkan/vulkan.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

// Compile the actual FenceVK class/body. Only device, scheduler and Vulkan
// dispatch are controlled doubles; no GPU reset or driver fault is induced.
namespace nri {

struct DebugNameBase {};
struct LegacyFenceStateVK {};
struct LegacyQueueSchedulerVK {
    uint32_t queries = 0;
    std::shared_ptr<LegacyFenceStateVK> CreateFence(uint64_t) { return std::make_shared<LegacyFenceStateVK>(); }
    void ReleaseFence(const std::shared_ptr<LegacyFenceStateVK>&) {}
    uint64_t GetValue(const std::shared_ptr<LegacyFenceStateVK>&) { queries++; return 73; }
    Result Wait(const std::shared_ptr<LegacyFenceStateVK>&, uint64_t) { return Result::SUCCESS; }
};

// Conversion is a surrounding dependency, not the behavior under test.
static Result GetResultFromVkResult(VkResult result) {
    if (result == VK_ERROR_DEVICE_LOST)
        return Result::DEVICE_LOST;

    if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
        return Result::OUT_OF_MEMORY;

    return result < 0 ? Result::FAILURE : Result::SUCCESS;
}

static void CloseSharedHandle(void*) {}
static uint64_t MsToUs(uint64_t value) { return value * 1000000; }

struct DeviceVK {
    struct Dispatch {
        PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue = Query;
        PFN_vkCreateSemaphore CreateSemaphore = Create;
        PFN_vkDestroySemaphore DestroySemaphore = Destroy;
        PFN_vkWaitSemaphores WaitSemaphores = Wait;
    } dispatch;
    LegacyQueueSchedulerVK* scheduler = nullptr;
    VkResult result = VK_SUCCESS;
    uint64_t output = 0;
    bool writeOutput = true;
    uint32_t queries = 0, reports = 0;
    Result reportedResult = Result::SUCCESS;
    Message severity = Message::INFO;
    char message[256] = {};

    operator VkDevice() const { return (VkDevice)this; }
    const Dispatch& GetDispatchTable() const { return dispatch; }
    const VkAllocationCallbacks* GetVkAllocationCallbacks() const { return nullptr; }
    LegacyQueueSchedulerVK* GetLegacyScheduler() const { return scheduler; }
    bool IsSharedFenceSupported(VkSemaphoreType) const { return false; }
    Result GetSharedSemaphoreHandle(VkSemaphore, void*&, SharedHandle&) { return Result::UNSUPPORTED; }
    void SetDebugNameToTrivialObject(VkObjectType, uint64_t, const char*) {}

    void ReportMessage(Message kind, Result failure, const char*, uint32_t, const char* format, ...) {
        reports++;
        reportedResult = failure;
        severity = kind;
        va_list args;
        va_start(args, format);
        std::vsnprintf(message, sizeof(message), format, args);
        va_end(args);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Query(VkDevice handle, VkSemaphore, uint64_t* value) {
        auto& device = *(DeviceVK*)handle;
        device.queries++;

        if (device.writeOutput)
            *value = device.output;

        return device.result;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*) { return VK_ERROR_UNKNOWN; }
    static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
    static VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice handle, const VkSemaphoreWaitInfo*, uint64_t) { return ((DeviceVK*)handle)->result; }
};

} // namespace nri

#define NRI_INLINE inline
#define NRI_DEBUG_NAME_OVERRIDE
#define SHARED_SEMAPHORE_HANDLE_TYPE VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT
#define NRI_TIMEOUT_FENCE 1000
#define NRI_RETURN_ON_BAD_VKRESULT(device, result, operation) \
    do { \
        if ((result) < 0) { \
            (device)->ReportMessage(Message::ERROR, GetResultFromVkResult(result), __FILE__, __LINE__, operation); \
            return GetResultFromVkResult(result); \
        } \
    } while (false)
#define NRI_RETURN_ON_FAILURE(device, condition, result, ...) \
    do { \
        if (!(condition)) \
            return result; \
    } while (false)

#include "FenceVK.h"
namespace nri {
#include "FenceVK.hpp"
}

int main() {
    uint32_t failures = 0;
    auto check = [&](bool success, const char* name) {
        if (!success) {
            std::fprintf(stderr, "FAILED: %s\n", name);
            failures++;
        }
    };

    for (VkResult failure : {VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_UNKNOWN}) {
        for (bool writeOutput : {false, true}) {
            nri::DeviceVK device;
            nri::FenceVK fence(device);
            check(fence.Create(nri::FenceVKDesc{1}) == nri::Result::SUCCESS, "wrap counter semaphore");
            device.result = failure;
            device.writeOutput = writeOutput;
            device.output = UINT64_MAX;
            check(fence.GetFenceValue() == 0, "failed query cannot publish unspecified counter output");
            check(device.queries == 1 && device.reports == 1 && device.severity == nri::Message::ERROR && device.reportedResult == nri::GetResultFromVkResult(failure), "native query failure reported once with mapped result");
            check(std::strstr(device.message, "GetSemaphoreCounterValue") != nullptr, "query failure identifies operation");
        }
    }

    {
        nri::DeviceVK device;
        nri::FenceVK fence(device);
        check(fence.Create(nri::FenceVKDesc{1}) == nri::Result::SUCCESS, "wrap success semaphore");

        for (uint64_t value : {uint64_t(0), uint64_t(17), uint64_t(0x8000000000000001ull)}) {
            device.output = value;
            check(fence.GetFenceValue() == value && device.reports == 0, "successful counter preserves full width without reports");
        }

        device.result = VK_ERROR_OUT_OF_HOST_MEMORY;
        check(fence.GetFenceValue() == 0, "temporary host allocation failure");
        device.result = VK_SUCCESS;
        device.output = 29;
        check(fence.GetFenceValue() == 29 && device.reports == 1, "subsequent successful query is not poisoned");
    }

    {
        nri::DeviceVK device;
        nri::LegacyQueueSchedulerVK scheduler;
        device.scheduler = &scheduler;
        nri::FenceVK fence(device);
        check(fence.Create(nri::FenceDesc{}) == nri::Result::SUCCESS, "logical fence");
        check(fence.GetFenceValue() == 73 && scheduler.queries == 1 && device.queries == 0 && device.reports == 0, "legacy scheduler remains sole completion source");
    }

    std::printf("Fence query checks: %u failures\n", failures);

    return failures ? 1 : 0;
}
