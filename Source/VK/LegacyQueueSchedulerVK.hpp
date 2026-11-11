// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT

LegacyCompletionVK::LegacyCompletionVK(DeviceVK& owner)
    : device(owner)
    , retire(owner.GetStdAllocator()) {
}

LegacyCompletionVK::~LegacyCompletionVK() {
    const auto& vk = device.GetDispatchTable();

    for (VkSemaphore semaphore : retire)
        vk.DestroySemaphore(device, semaphore, device.GetVkAllocationCallbacks());

    if (fence)
        vk.DestroyFence(device, fence, device.GetVkAllocationCallbacks());

    if (commandPool)
        vk.DestroyCommandPool(device, commandPool, device.GetVkAllocationCallbacks());
}

LegacyFenceStateVK::LegacyFenceStateVK(const StdAllocator<uint8_t>& allocator, uint64_t initial)
    : initialValue(initial)
    , completedValue(initial)
    , submittedValue(initial)
    , points(allocator) {
}

LegacyPacketVK::LegacyPacketVK(const StdAllocator<uint8_t>& allocator)
    : commands(allocator)
    , waits(allocator)
    , signals(allocator) {
}

LegacyQueueSchedulerVK::LegacyQueueSchedulerVK(DeviceVK& device)
    : m_Device(device)
    , m_Fences(device.GetStdAllocator())
    , m_InFlight(device.GetStdAllocator())
    , m_Pending(device.GetStdAllocator()) {
}

LegacyQueueSchedulerVK::~LegacyQueueSchedulerVK() {
    // Device destruction requires the caller to finish accepted work. No worker
    // or native callbacks can outlive this scheduler or its owning VkDevice.
    for (auto& state : m_Fences) {
        for (LegacyPointVK& point : state->points)
            RetirePoint(point);
    }
}

DeviceVK& LegacyQueueSchedulerVK::GetDevice() const {
    return m_Device;
}

Result LegacyQueueSchedulerVK::Fail(VkResult result, const char* operation) {
    if (result == VK_SUCCESS)
        return Result::SUCCESS;

    const Result failure = GetResultFromVkResult(result);

    if (m_Failure == Result::SUCCESS || (failure == Result::DEVICE_LOST && m_Failure != failure)) {
        m_Failure = failure;
        m_Device.ReportMessage(Message::ERROR, m_Failure, __FILE__, __LINE__, "Legacy queue scheduler: %s failed (%d)", operation, result);
        m_Wakeup.notify_all();
    }

    return m_Failure;
}

Result LegacyQueueSchedulerVK::CreateBinary(VkSemaphore& semaphore) {
    VkSemaphoreCreateInfo info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    const auto& vk = m_Device.GetDispatchTable();

    return Fail(vk.CreateSemaphore(m_Device, &info, m_Device.GetVkAllocationCallbacks(), &semaphore), "CreateSemaphore");
}

Result LegacyQueueSchedulerVK::CreateCompletion(std::shared_ptr<LegacyCompletionVK>& completion) {
    completion = std::allocate_shared<LegacyCompletionVK>(m_Device.GetStdAllocator(), m_Device);
    VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const auto& vk = m_Device.GetDispatchTable();

    return Fail(vk.CreateFence(m_Device, &info, m_Device.GetVkAllocationCallbacks(), &completion->fence), "CreateFence");
}

Result LegacyQueueSchedulerVK::NativeSubmit(QueueVK& queue, const VkSubmitInfo& info, const std::shared_ptr<LegacyCompletionVK>& completion) {
#if NRI_COMPATIBILITY_FAIL_SUBMIT
    if (++m_SubmitCount == NRI_COMPATIBILITY_FAIL_SUBMIT) {
#if NRI_COMPATIBILITY_SUBMIT_DEVICE_LOST
        return Fail(VK_ERROR_DEVICE_LOST, "injected QueueSubmit");
#else
        return Fail(VK_ERROR_OUT_OF_DEVICE_MEMORY, "injected QueueSubmit");
#endif
    }
#endif

    ExclusiveScope lock(queue.GetLock());
    const auto& vk = m_Device.GetDispatchTable();
    Result result = Fail(vk.QueueSubmit(queue, 1, &info, completion->fence), "QueueSubmit");

    if (result == Result::SUCCESS)
        m_InFlight.push_back(completion);

    return result;
}

std::shared_ptr<LegacyFenceStateVK> LegacyQueueSchedulerVK::CreateFence(uint64_t initialValue) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto state = std::allocate_shared<LegacyFenceStateVK>(m_Device.GetStdAllocator(), m_Device.GetStdAllocator(), initialValue);
    m_Fences.push_back(state);

    return state;
}

void LegacyQueueSchedulerVK::ReleaseFence(const std::shared_ptr<LegacyFenceStateVK>& state) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    state->released = true;
    Poll();
}

void LegacyQueueSchedulerVK::RetirePoint(LegacyPointVK& point) {
    if (!point.anchor)
        return;

    if (point.anchorCompletion->complete)
        m_Device.GetDispatchTable().DestroySemaphore(m_Device, point.anchor, m_Device.GetVkAllocationCallbacks());
    else
        point.anchorCompletion->retire.push_back(point.anchor);

    point.anchor = VK_NULL_HANDLE;
}

LegacyPointVK* LegacyQueueSchedulerVK::FindPoint(LegacyFenceStateVK& state, uint64_t value) {
    for (LegacyPointVK& point : state.points) {
        if (point.value >= value)
            return &point;
    }

    return nullptr;
}

Result LegacyQueueSchedulerVK::Poll(bool quiesce) {
    if (m_Failure != Result::SUCCESS && !quiesce)
        return m_Failure;

    const auto& vk = m_Device.GetDispatchTable();

    for (size_t i = 0; i < m_InFlight.size();) {
        auto& completion = m_InFlight[i];
        VkResult status = vk.GetFenceStatus(m_Device, completion->fence);

#if NRI_COMPATIBILITY_POLL_LOSS_MODE
        // Fault simulation only: wait for actual native completion before
        // returning a synthetic loss. Mode 2 withholds logical completion until
        // the separate submit OOM has been observed, then exercises escalation.
        if (status == VK_SUCCESS && !m_TestPollLoss) {
            if (NRI_COMPATIBILITY_POLL_LOSS_MODE == 2 && m_Failure == Result::SUCCESS)
                status = VK_NOT_READY;
            else {
                m_TestPollLoss = true;
                status = VK_ERROR_DEVICE_LOST;
                NRI_REPORT_INFO(&m_Device, "NRI_LEGACY_POLL_FAULT {\"mode\":%u,\"native_completed\":true}", NRI_COMPATIBILITY_POLL_LOSS_MODE);
            }
        }
#endif

        if (status == VK_NOT_READY) {
            i++;
            continue;
        }

        if (status != VK_SUCCESS)
            return Fail(status, "GetFenceStatus");

        completion->complete = true;

        for (VkSemaphore semaphore : completion->retire)
            vk.DestroySemaphore(m_Device, semaphore, m_Device.GetVkAllocationCallbacks());

        completion->retire.clear();
        m_InFlight.erase(m_InFlight.begin() + i);
    }

    for (size_t i = 0; i < m_Fences.size();) {
        auto& state = m_Fences[i];

        for (const LegacyPointVK& point : state->points) {
            if (!point.completion->complete)
                break;

            state->completedValue = std::max(state->completedValue, point.value);
        }

        // Keep the most recent completed signal as a reusable memory dependency,
        // plus outstanding signals. A completed value alone is not a semaphore.
        while (state->points.size() > 1 && state->points[1].value <= state->completedValue) {
            RetirePoint(state->points.front());
            state->points.erase(state->points.begin());
        }

        if (state->released && state.use_count() == 1) {
            for (LegacyPointVK& point : state->points)
                RetirePoint(point);

            m_Fences.erase(m_Fences.begin() + i);
        } else
            i++;
    }

    return Result::SUCCESS;
}

Result LegacyQueueSchedulerVK::MakeDependency(LegacyPointVK& point, VkSemaphore& semaphore) {
    std::shared_ptr<LegacyCompletionVK> bridge;
    Result result = CreateCompletion(bridge);

    if (result != Result::SUCCESS)
        return result;

    VkSemaphore nextAnchor = VK_NULL_HANDLE;
    result = CreateBinary(nextAnchor);

    if (result != Result::SUCCESS)
        return result;

    result = CreateBinary(semaphore);

    if (result != Result::SUCCESS) {
        m_Device.GetDispatchTable().DestroySemaphore(m_Device, nextAnchor, m_Device.GetVkAllocationCallbacks());

        return result;
    }

    // Split an existing payload into a retained anchor and one consumer payload.
    // Repeated/fanout waits never consume the same binary signal twice. Synthetic
    // work is appended to already-submitted native work, before any deferred FIFO
    // head; no native queue contains an unresolved emulated future wait.
    VkSemaphore signals[] = {nextAnchor, semaphore};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &point.anchor;
    info.pWaitDstStageMask = &stage;
    info.signalSemaphoreCount = 2;
    info.pSignalSemaphores = signals;
    result = NativeSubmit(*point.queue, info, bridge);

    if (result != Result::SUCCESS) {
        m_Device.GetDispatchTable().DestroySemaphore(m_Device, nextAnchor, m_Device.GetVkAllocationCallbacks());
        m_Device.GetDispatchTable().DestroySemaphore(m_Device, semaphore, m_Device.GetVkAllocationCallbacks());
        semaphore = VK_NULL_HANDLE;

        return result;
    }

    bridge->retire.push_back(point.anchor);
    bridge->retire.push_back(semaphore);
    point.anchor = nextAnchor;
    point.anchorCompletion = bridge;

    return Result::SUCCESS;
}

bool LegacyQueueSchedulerVK::Ready(const LegacyPacketVK& packet) {
    for (const auto& wait : packet.waits) {
        if (wait.state && wait.value > wait.state->initialValue && !FindPoint(*wait.state, wait.value))
            return false;
    }

    return true;
}

Result LegacyQueueSchedulerVK::Dispatch(LegacyPacketVK& packet) {
    std::shared_ptr<LegacyCompletionVK> completion;
    Result result = CreateCompletion(completion);

    if (result != Result::SUCCESS)
        return result;

    Vector<VkSemaphore> waits(m_Device.GetStdAllocator());
    Vector<VkPipelineStageFlags> stages(m_Device.GetStdAllocator());
    Vector<VkSemaphore> signals(m_Device.GetStdAllocator());
    Vector<std::shared_ptr<LegacyCompletionVK>> dependencyProducers(m_Device.GetStdAllocator());

    for (const auto& wait : packet.waits) {
        VkSemaphore semaphore = wait.binary;

        if (wait.state) {
            if (wait.value <= wait.state->initialValue)
                continue;

            LegacyPointVK* point = FindPoint(*wait.state, wait.value);
            NRI_CHECK(point, "Ready submission has no signal point");
            result = MakeDependency(*point, semaphore);

            if (result != Result::SUCCESS)
                return result;

            dependencyProducers.push_back(point->anchorCompletion);
        } else {
            dependencyProducers.push_back(nullptr);
        }

        waits.push_back(semaphore);
        stages.push_back(wait.stages);
    }

    for (const auto& signal : packet.signals) {
        VkSemaphore semaphore = signal.binary;

        if (signal.state) {
            if (signal.value <= signal.state->submittedValue)
                return Fail(VK_ERROR_UNKNOWN, "non-increasing emulated signal");

            // Preserve a transitive memory dependency when a logical fence moves
            // between physical queues. Completed old points can then be retired
            // while the newest anchor still covers every earlier signal.
            if (!signal.state->points.empty()) {
                LegacyPointVK& previous = signal.state->points.back();

                if ((VkQueue)*previous.queue != (VkQueue)*packet.queue) {
                    VkSemaphore dependency = VK_NULL_HANDLE;
                    result = MakeDependency(previous, dependency);

                    if (result != Result::SUCCESS)
                        return result;

                    waits.push_back(dependency);
                    stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                    dependencyProducers.push_back(previous.anchorCompletion);
                }
            }

            result = CreateBinary(semaphore);

            if (result != Result::SUCCESS)
                return result;

            completion->retire.push_back(semaphore);
        }

        signals.push_back(semaphore);
    }

    VkSubmitInfo info = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.waitSemaphoreCount = (uint32_t)waits.size();
    info.pWaitSemaphores = waits.data();
    info.pWaitDstStageMask = stages.data();
    info.commandBufferCount = (uint32_t)packet.commands.size();
    info.pCommandBuffers = packet.commands.data();
    info.signalSemaphoreCount = (uint32_t)signals.size();
    info.pSignalSemaphores = signals.data();
    result = NativeSubmit(*packet.queue, info, completion);

    if (result != Result::SUCCESS)
        return result;

    for (size_t i = 0; i < waits.size(); i++) {
        if (dependencyProducers[i]) {
            auto& retire = dependencyProducers[i]->retire;
            retire.erase(std::find(retire.begin(), retire.end(), waits[i]));
            completion->retire.push_back(waits[i]);
        }
    }

    for (size_t i = 0; i < packet.signals.size(); i++) {
        auto& signal = packet.signals[i];

        if (signal.state) {
            completion->retire.erase(std::find(completion->retire.begin(), completion->retire.end(), signals[i]));
            signal.state->submittedValue = signal.value;
            signal.state->points.push_back({signal.value, packet.queue, signals[i], completion, completion});
        }
    }

    return Result::SUCCESS;
}

Result LegacyQueueSchedulerVK::Pump() {
    Result result = Poll();

    if (result != Result::SUCCESS)
        return result;

    bool progressed;
    bool anyProgress = false;

    do {
        progressed = false;

        for (size_t i = 0; i < m_Pending.size();) {
            auto& packet = m_Pending[i];
            bool isHead = true;

            for (size_t earlier = 0; earlier < i; earlier++) {
                if ((VkQueue)*m_Pending[earlier]->queue == (VkQueue)*packet->queue) {
                    isHead = false;
                    break;
                }
            }

            if (!isHead || !Ready(*packet)) {
                i++;
                continue;
            }

            result = Dispatch(*packet);

            if (result != Result::SUCCESS)
                return result;

            m_Pending.erase(m_Pending.begin() + i);
            progressed = true;
            anyProgress = true;
        }
    } while (progressed);

    if (anyProgress)
        m_Wakeup.notify_all();

    return Result::SUCCESS;
}

Result LegacyQueueSchedulerVK::Submit(QueueVK& queue, const QueueSubmitDesc& desc) {
    std::lock_guard<std::mutex> lock(m_Mutex);

    if (m_Failure != Result::SUCCESS)
        return m_Failure;

    auto packet = std::allocate_shared<LegacyPacketVK>(m_Device.GetStdAllocator(), m_Device.GetStdAllocator());
    packet->queue = &queue;

    for (uint32_t i = 0; i < desc.commandBufferNum; i++)
        packet->commands.push_back(*(CommandBufferVK*)desc.commandBuffers[i]);

    auto copyDependencies = [&](Vector<LegacyFenceDependencyVK>& target, const FenceSubmitDesc* source, uint32_t num) {
        for (uint32_t i = 0; i < num; i++) {
            const FenceVK& fence = *(FenceVK*)source[i].fence;
            VkPipelineStageFlags2 scope = GetPipelineStageFlags(source[i].stages) & ~m_Device.GetUnsupportedPipelineStages();
            target.push_back({fence.GetLegacyState(), fence, source[i].value, GetLegacyPipelineStages(scope, false)});
        }
    };
    copyDependencies(packet->waits, desc.waitFences, desc.waitFenceNum);
    copyDependencies(packet->signals, desc.signalFences, desc.signalFenceNum);
    m_Pending.push_back(std::move(packet));

    return Pump();
}

uint64_t LegacyQueueSchedulerVK::GetValue(const std::shared_ptr<LegacyFenceStateVK>& state) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    Pump();

    return state->completedValue;
}

Result LegacyQueueSchedulerVK::Wait(const std::shared_ptr<LegacyFenceStateVK>& state, uint64_t value) {
    std::unique_lock<std::mutex> lock(m_Mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(NRI_TIMEOUT_FENCE);

    for (;;) {
        Result result = Pump();

        if (result != Result::SUCCESS)
            return result;

        if (state->completedValue >= value)
            return Result::SUCCESS;

        if (std::chrono::steady_clock::now() >= deadline) {
            NRI_REPORT_ERROR(&m_Device, "Emulated fence wait timed out");

            return Result::FAILURE;
        }

        m_Wakeup.wait_for(lock, std::chrono::milliseconds(1));
    }
}

bool LegacyQueueSchedulerVK::HasPending(QueueVK* queue) const {
    for (const auto& packet : m_Pending) {
        if (!queue || (VkQueue)*packet->queue == (VkQueue)*queue)
            return true;
    }

    return false;
}

void LegacyQueueSchedulerVK::ReportStats() {
#if NRI_COMPATIBILITY_DIAGNOSTICS
    std::lock_guard<std::mutex> lock(m_Mutex);
    Poll();
    Vector<const LegacyCompletionVK*> completions(m_Device.GetStdAllocator());
    uint32_t points = 0;
    uint32_t semaphores = 0;
    auto addCompletion = [&](const std::shared_ptr<LegacyCompletionVK>& completion) {
        if (std::find(completions.begin(), completions.end(), completion.get()) == completions.end())
            completions.push_back(completion.get());
    };

    for (const auto& completion : m_InFlight)
        addCompletion(completion);

    for (const auto& state : m_Fences) {
        for (const auto& point : state->points) {
            points++;
            semaphores += point.anchor != VK_NULL_HANDLE;
            addCompletion(point.completion);
            addCompletion(point.anchorCompletion);
        }
    }

    for (const auto* completion : completions)
        semaphores += (uint32_t)completion->retire.size();

    NRI_REPORT_INFO(&m_Device, "NRI_LEGACY_SCHEDULER {\"pending_packets\":%u,\"in_flight\":%u,\"logical_fences\":%u,\"signal_points\":%u,\"native_fences\":%u,\"binary_semaphores\":%u}",
        (uint32_t)m_Pending.size(), (uint32_t)m_InFlight.size(), (uint32_t)m_Fences.size(), points, (uint32_t)completions.size(), semaphores);
#endif
}

Result LegacyQueueSchedulerVK::Drain(QueueVK* queue, bool completion) {
    std::unique_lock<std::mutex> lock(m_Mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(NRI_TIMEOUT_FENCE);

    for (;;) {
        Result result = Pump();

        if (result != Result::SUCCESS) {
            // Accepted but not submitted work cannot execute after a terminal
            // error. Submitted work still owns its objects until completion.
            m_Pending.clear();
            Result pollResult = Poll(true);

            if (pollResult != Result::SUCCESS)
                return pollResult;

            if (m_InFlight.empty()) {
#if NRI_COMPATIBILITY_DIAGNOSTICS
                NRI_REPORT_INFO(&m_Device, "NRI_LEGACY_FAILURE_DRAIN {\"pending_packets\":%u,\"in_flight\":%u}", (uint32_t)m_Pending.size(), (uint32_t)m_InFlight.size());
#endif

                return result;
            }
        }

        if (result == Result::SUCCESS && !HasPending(queue) && (!completion || m_InFlight.empty()))
            return Result::SUCCESS;

        if (std::chrono::steady_clock::now() >= deadline) {
            NRI_REPORT_ERROR(&m_Device, "Deferred queue drain timed out");

            return Result::FAILURE;
        }

        m_Wakeup.wait_for(lock, std::chrono::milliseconds(1));
    }
}

Result LegacyQueueSchedulerVK::ResetQueries(VkQueryPool queryPool, uint32_t offset, uint32_t num) {
    std::unique_lock<std::mutex> lock(m_Mutex);

    if (m_Failure != Result::SUCCESS)
        return m_Failure;

    // All logical future waits stay on the host on this profile, even when the
    // device supports native timelines. Submit reset work ahead of deferred FIFO
    // heads, never through QueueVK::Submit. Every already-native dependency has
    // an accepted producer, so a future user signal cannot block this reset.
    QueueVK& queue = *m_Device.GetQueryResetQueue();
    const auto& vk = m_Device.GetDispatchTable();
    std::shared_ptr<LegacyCompletionVK> completion;
    Result result = CreateCompletion(completion);

    if (result != Result::SUCCESS)
        return result;

    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = queue.GetFamilyIndex();
    result = Fail(vk.CreateCommandPool(m_Device, &poolInfo, m_Device.GetVkAllocationCallbacks(), &completion->commandPool), "query reset CreateCommandPool");

    if (result != Result::SUCCESS)
        return result;

    VkCommandBufferAllocateInfo allocation = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = completion->commandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    result = Fail(vk.AllocateCommandBuffers(m_Device, &allocation, &command), "query reset AllocateCommandBuffers");

    if (result != Result::SUCCESS)
        return result;

    VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = Fail(vk.BeginCommandBuffer(command, &beginInfo), "query reset BeginCommandBuffer");

    if (result != Result::SUCCESS)
        return result;

    vk.CmdResetQueryPool(command, queryPool, offset, num);
    result = Fail(vk.EndCommandBuffer(command), "query reset EndCommandBuffer");

    if (result != Result::SUCCESS)
        return result;

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    result = NativeSubmit(queue, submit, completion);

    if (result != Result::SUCCESS)
        return result;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(NRI_TIMEOUT_FENCE);

    while (!completion->complete) {
        result = Poll(true);

        if (result != Result::SUCCESS)
            return result;

        if (std::chrono::steady_clock::now() >= deadline && !completion->complete)
            return Fail(VK_ERROR_UNKNOWN, "query reset completion timeout");

        if (!completion->complete)
            m_Wakeup.wait_for(lock, std::chrono::milliseconds(1));
    }

    return m_Failure;
}
