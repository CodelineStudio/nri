// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace nri {

struct LegacyCompletionVK {
    LegacyCompletionVK(DeviceVK& device);
    ~LegacyCompletionVK();

    DeviceVK& device;
    VkFence fence = VK_NULL_HANDLE;
    Vector<VkSemaphore> retire;
    bool complete = false;
};

struct LegacyPointVK {
    uint64_t value = 0;
    QueueVK* queue = nullptr;
    VkSemaphore anchor = VK_NULL_HANDLE;
    std::shared_ptr<LegacyCompletionVK> completion;
    std::shared_ptr<LegacyCompletionVK> anchorCompletion;
};

struct LegacyFenceStateVK {
    LegacyFenceStateVK(const StdAllocator<uint8_t>& allocator, uint64_t initial);

    uint64_t initialValue;
    uint64_t completedValue;
    uint64_t submittedValue;
    Vector<LegacyPointVK> points;
    bool released = false;
};

struct LegacyFenceDependencyVK {
    std::shared_ptr<LegacyFenceStateVK> state;
    VkSemaphore binary = VK_NULL_HANDLE;
    uint64_t value = 0;
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
};

struct LegacyPacketVK {
    LegacyPacketVK(const StdAllocator<uint8_t>& allocator);

    QueueVK* queue = nullptr;
    Vector<VkCommandBuffer> commands;
    Vector<LegacyFenceDependencyVK> waits;
    Vector<LegacyFenceDependencyVK> signals;
};

// Only the fallback uses this scheduler. Every accepted packet owns its arrays.
// Lock order: scheduler mutex, then the existing physical-queue lock. Blocking
// host waits release the scheduler mutex; native queue submission never waits.
struct LegacyQueueSchedulerVK {
    LegacyQueueSchedulerVK(DeviceVK& device);
    ~LegacyQueueSchedulerVK();
    DeviceVK& GetDevice() const;

    std::shared_ptr<LegacyFenceStateVK> CreateFence(uint64_t initialValue);
    void ReleaseFence(const std::shared_ptr<LegacyFenceStateVK>& state);
    uint64_t GetValue(const std::shared_ptr<LegacyFenceStateVK>& state);
    Result Wait(const std::shared_ptr<LegacyFenceStateVK>& state, uint64_t value);
    Result Submit(QueueVK& queue, const QueueSubmitDesc& desc);
    Result Drain(QueueVK* queue, bool completion);
    void ReportStats();

private:
    Result Fail(VkResult result, const char* operation);
    Result CreateBinary(VkSemaphore& semaphore);
    Result CreateCompletion(std::shared_ptr<LegacyCompletionVK>& completion);
    Result NativeSubmit(QueueVK& queue, const VkSubmitInfo& info, const std::shared_ptr<LegacyCompletionVK>& completion);
    LegacyPointVK* FindPoint(LegacyFenceStateVK& state, uint64_t value);
    Result MakeDependency(LegacyPointVK& point, VkSemaphore& semaphore);
    void RetirePoint(LegacyPointVK& point);
    Result Poll(bool quiesce = false);
    Result Pump();
    bool Ready(const LegacyPacketVK& packet);
    Result Dispatch(LegacyPacketVK& packet);
    bool HasPending(QueueVK* queue) const;

private:
    DeviceVK& m_Device;
    Vector<std::shared_ptr<LegacyFenceStateVK>> m_Fences;
    Vector<std::shared_ptr<LegacyCompletionVK>> m_InFlight;
    Vector<std::shared_ptr<LegacyPacketVK>> m_Pending;
    Result m_Failure = Result::SUCCESS;
    std::mutex m_Mutex;
    std::condition_variable m_Wakeup;
#if NRI_COMPATIBILITY_FAIL_SUBMIT
    uint64_t m_SubmitCount = 0;
#endif
};

} // namespace nri
