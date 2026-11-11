// © 2021 NVIDIA Corporation

static inline VkResult PresentNative(DeviceVK& device, VkQueue queue, const VkPresentInfoKHR& presentInfo) {
    VkResult result = device.GetDispatchTable().QueuePresentKHR(queue, &presentInfo);
#if NRI_COMPATIBILITY_ACQUIRE_RESULTS
    // Rejected presentation still enqueues its semaphore waits. Call Vulkan
    // first, then inject before retirement bookkeeping observes the result.
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        uint32_t ordinal = device.NextCompatibilityPresent();

        if (ordinal == 13)
            result = VK_ERROR_OUT_OF_DATE_KHR;
        else if (ordinal == 23)
            result = VK_ERROR_SURFACE_LOST_KHR;
    }
#endif

    return result;
}

QueueVK::~QueueVK() {
    // As with submitted resources, the caller must complete queue work before
    // device destruction. WaitIdle also waits for maintenance1 presentations.
    const auto& vk = m_Device.GetDispatchTable();

    for (VkFence fence : m_PresentFences)
        vk.DestroyFence(m_Device, fence, m_Device.GetVkAllocationCallbacks());
}

VkResult QueueVK::Present(const VkPresentInfoKHR& presentInfo) {
    const auto& vk = m_Device.GetDispatchTable();

    if (!m_Device.m_IsSupported.swapChainMaintenance1)
        return PresentNative(m_Device, m_Handle, presentInfo);

    // Reuse only a fence whose previous presentation payload has retired.
    // The vector contains accepted operations, never failed enqueue attempts.
    // Reserve before native acceptance so recording it cannot allocate afterward.
    m_PresentFences.reserve(m_PresentFences.size() + 1);
    VkFence fence = VK_NULL_HANDLE;

    for (size_t i = 0; i < m_PresentFences.size(); i++) {
        VkResult result = vk.GetFenceStatus(m_Device, m_PresentFences[i]);

        if (result == VK_NOT_READY)
            continue;

        if (result != VK_SUCCESS)
            return result;

        fence = m_PresentFences[i];
        result = vk.ResetFences(m_Device, 1, &fence);

        if (result != VK_SUCCESS)
            return result;

        m_PresentFences.erase(m_PresentFences.begin() + i);
#if NRI_COMPATIBILITY_DIAGNOSTICS
        m_PresentRetired++;
#endif
        break;
    }

    if (!fence) {
        VkFenceCreateInfo create = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkResult result = vk.CreateFence(m_Device, &create, m_Device.GetVkAllocationCallbacks(), &fence);

        if (result != VK_SUCCESS)
            return result;
    }

    VkSwapchainPresentFenceInfoEXT completion = {VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    completion.pNext = presentInfo.pNext;
    completion.swapchainCount = 1;
    completion.pFences = &fence;
    VkPresentInfoKHR tracked = presentInfo;
    tracked.pNext = &completion;
    VkResult result;
#if NRI_COMPATIBILITY_PRESENT_OOM
    if (!m_TestPresentFailed) {
        m_TestPresentFailed = true;
        result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    } else
#endif
        result = PresentNative(m_Device, m_Handle, tracked);
    const bool enqueued = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR ||
        result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT || result == VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT;

    if (enqueued) {
        m_PresentFences.push_back(fence);
#if NRI_COMPATIBILITY_DIAGNOSTICS
        m_PresentSubmitted++;
        m_PresentPeak = std::max(m_PresentPeak, (uint32_t)m_PresentFences.size());
#endif
    } else {
        // OOM leaves the present's objects untouched. Device loss permits native
        // cleanup; neither case produces a fence that a later idle should wait.
        vk.DestroyFence(m_Device, fence, m_Device.GetVkAllocationCallbacks());
    }

    return result;
}

Result QueueVK::Create(QueueType type, uint32_t familyIndex, VkQueue handle, Lock* sharedLock) {
    m_Type = type;
    m_FamilyIndex = familyIndex;
    m_Handle = handle;
    m_Lock = sharedLock ? sharedLock : &m_OwnLock;

    return Result::SUCCESS;
}

NRI_INLINE void QueueVK::SetDebugName(const char* name) {
    ExclusiveScope lock(*m_Lock);

    m_Device.SetDebugNameToTrivialObject(VK_OBJECT_TYPE_QUEUE, (uint64_t)m_Handle, name);
}

NRI_INLINE void QueueVK::BeginAnnotation(const char* name, uint32_t bgra) {
    ExclusiveScope lock(*m_Lock);

    VkDebugUtilsLabelEXT info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    info.pLabelName = name;
    info.color[0] = ((bgra >> 16) & 0xFF) / 255.0f;
    info.color[1] = ((bgra >> 8) & 0xFF) / 255.0f;
    info.color[2] = ((bgra >> 0) & 0xFF) / 255.0f;
    info.color[3] = 1.0f; // PIX sets alpha to 1

    const auto& vk = m_Device.GetDispatchTable();
    if (vk.QueueBeginDebugUtilsLabelEXT)
        vk.QueueBeginDebugUtilsLabelEXT(m_Handle, &info);
}

NRI_INLINE void QueueVK::EndAnnotation() {
    ExclusiveScope lock(*m_Lock);

    const auto& vk = m_Device.GetDispatchTable();
    if (vk.QueueEndDebugUtilsLabelEXT)
        vk.QueueEndDebugUtilsLabelEXT(m_Handle);
}

NRI_INLINE void QueueVK::Annotation(const char* name, uint32_t bgra) {
    ExclusiveScope lock(*m_Lock);

    VkDebugUtilsLabelEXT info = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    info.pLabelName = name;
    info.color[0] = ((bgra >> 16) & 0xFF) / 255.0f;
    info.color[1] = ((bgra >> 8) & 0xFF) / 255.0f;
    info.color[2] = ((bgra >> 0) & 0xFF) / 255.0f;
    info.color[3] = 1.0f; // PIX sets alpha to 1

    const auto& vk = m_Device.GetDispatchTable();
    if (vk.QueueInsertDebugUtilsLabelEXT)
        vk.QueueInsertDebugUtilsLabelEXT(m_Handle, &info);
}

NRI_INLINE void QueueVK::GetCalibratedTimestamps(uint64_t& timestampGPU, uint64_t& timestampCPU) {
    timestampGPU = 0;
    timestampCPU = 0;

    VkCalibratedTimestampInfoKHR timestampInfos[2] = {};
    {
        // GPU
        timestampInfos[0].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
        timestampInfos[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;

        // CPU
        timestampInfos[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
        timestampInfos[1].timeDomain = m_Device.GetCalibratedTimestampCPUTimeDomain();
    }

    uint64_t timestamps[2] = {};
    uint64_t maxDeviation = 0;

    const auto& vk = m_Device.GetDispatchTable();
    VkResult vkResult = vk.GetCalibratedTimestampsEXT(m_Device, 2, timestampInfos, timestamps, &maxDeviation);
    NRI_RETURN_VOID_ON_BAD_VKRESULT(&m_Device, vkResult, "GetCalibratedTimestampsKHR");

    timestampGPU = timestamps[0];
    timestampCPU = timestamps[1];
}

NRI_INLINE Result QueueVK::Submit(const QueueSubmitDesc& queueSubmitDesc) {
    if (m_Device.GetLegacyScheduler())
        return m_Device.GetLegacyScheduler()->Submit(*this, queueSubmitDesc);

    ExclusiveScope lock(*m_Lock);
    const VkPipelineStageFlags2 unsupportedStages = m_Device.GetUnsupportedPipelineStages();

    Scratch<VkSemaphoreSubmitInfo> waitSemaphores = NRI_ALLOCATE_SCRATCH(m_Device, VkSemaphoreSubmitInfo, queueSubmitDesc.waitFenceNum);
    for (uint32_t i = 0; i < queueSubmitDesc.waitFenceNum; i++) {
        waitSemaphores[i] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitSemaphores[i].semaphore = *(FenceVK*)queueSubmitDesc.waitFences[i].fence;
        waitSemaphores[i].value = queueSubmitDesc.waitFences[i].value;
        waitSemaphores[i].stageMask = GetPipelineStageFlags(queueSubmitDesc.waitFences[i].stages) & ~unsupportedStages;
    }

    Scratch<VkCommandBufferSubmitInfo> commandBuffers = NRI_ALLOCATE_SCRATCH(m_Device, VkCommandBufferSubmitInfo, queueSubmitDesc.commandBufferNum);
    for (uint32_t i = 0; i < queueSubmitDesc.commandBufferNum; i++) {
        commandBuffers[i] = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandBuffers[i].commandBuffer = *(CommandBufferVK*)queueSubmitDesc.commandBuffers[i];
    }

    Scratch<VkSemaphoreSubmitInfo> signalSemaphores = NRI_ALLOCATE_SCRATCH(m_Device, VkSemaphoreSubmitInfo, queueSubmitDesc.signalFenceNum);
    for (uint32_t i = 0; i < queueSubmitDesc.signalFenceNum; i++) {
        signalSemaphores[i] = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signalSemaphores[i].semaphore = *(FenceVK*)queueSubmitDesc.signalFences[i].fence;
        signalSemaphores[i].value = queueSubmitDesc.signalFences[i].value;
        signalSemaphores[i].stageMask = GetPipelineStageFlags(queueSubmitDesc.signalFences[i].stages) & ~unsupportedStages;
    }

    VkSubmitInfo2 submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submitInfo.waitSemaphoreInfoCount = queueSubmitDesc.waitFenceNum;
    submitInfo.pWaitSemaphoreInfos = waitSemaphores;
    submitInfo.commandBufferInfoCount = queueSubmitDesc.commandBufferNum;
    submitInfo.pCommandBufferInfos = commandBuffers;
    submitInfo.signalSemaphoreInfoCount = queueSubmitDesc.signalFenceNum;
    submitInfo.pSignalSemaphoreInfos = signalSemaphores;

    VkLatencySubmissionPresentIdNV presentId = {VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV};
    if (queueSubmitDesc.swapChain && m_Device.m_IsSupported.presentId) {
        presentId.presentID = queueSubmitDesc.presentId;
        submitInfo.pNext = &presentId;
    }

    const auto& vk = m_Device.GetDispatchTable();
    VkResult vkResult;

    if (m_Device.m_IsSupported.synchronization2)
        vkResult = vk.QueueSubmit2(m_Handle, 1, &submitInfo, VK_NULL_HANDLE);
    else {
        Scratch<VkSemaphore> waits = NRI_ALLOCATE_SCRATCH(m_Device, VkSemaphore, queueSubmitDesc.waitFenceNum);
        Scratch<uint64_t> waitValues = NRI_ALLOCATE_SCRATCH(m_Device, uint64_t, queueSubmitDesc.waitFenceNum);
        Scratch<VkPipelineStageFlags> waitStages = NRI_ALLOCATE_SCRATCH(m_Device, VkPipelineStageFlags, queueSubmitDesc.waitFenceNum);

        for (uint32_t i = 0; i < queueSubmitDesc.waitFenceNum; i++) {
            waits[i] = waitSemaphores[i].semaphore;
            waitValues[i] = waitSemaphores[i].value;
            waitStages[i] = GetLegacyPipelineStages(waitSemaphores[i].stageMask, false);
        }

        Scratch<VkSemaphore> signals = NRI_ALLOCATE_SCRATCH(m_Device, VkSemaphore, queueSubmitDesc.signalFenceNum);
        Scratch<uint64_t> signalValues = NRI_ALLOCATE_SCRATCH(m_Device, uint64_t, queueSubmitDesc.signalFenceNum);

        for (uint32_t i = 0; i < queueSubmitDesc.signalFenceNum; i++) {
            signals[i] = signalSemaphores[i].semaphore;
            signalValues[i] = signalSemaphores[i].value;
        }

        Scratch<VkCommandBuffer> commands = NRI_ALLOCATE_SCRATCH(m_Device, VkCommandBuffer, queueSubmitDesc.commandBufferNum);

        for (uint32_t i = 0; i < queueSubmitDesc.commandBufferNum; i++)
            commands[i] = commandBuffers[i].commandBuffer;

        // Timeline remains native in this path. Binary swap-chain entries use
        // the same arrays; Vulkan ignores their corresponding values.
        VkTimelineSemaphoreSubmitInfo timeline = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        timeline.pNext = submitInfo.pNext;
        timeline.waitSemaphoreValueCount = queueSubmitDesc.waitFenceNum;
        timeline.pWaitSemaphoreValues = waitValues;
        timeline.signalSemaphoreValueCount = queueSubmitDesc.signalFenceNum;
        timeline.pSignalSemaphoreValues = signalValues;

        VkSubmitInfo legacy = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        legacy.pNext = &timeline;
        legacy.waitSemaphoreCount = queueSubmitDesc.waitFenceNum;
        legacy.pWaitSemaphores = waits;
        legacy.pWaitDstStageMask = waitStages;
        legacy.commandBufferCount = queueSubmitDesc.commandBufferNum;
        legacy.pCommandBuffers = commands;
        legacy.signalSemaphoreCount = queueSubmitDesc.signalFenceNum;
        legacy.pSignalSemaphores = signals;
        vkResult = vk.QueueSubmit(m_Handle, 1, &legacy, VK_NULL_HANDLE);
    }
    NRI_RETURN_ON_BAD_VKRESULT(&m_Device, vkResult, "QueueSubmit");

    return Result::SUCCESS;
}

NRI_INLINE Result QueueVK::WaitIdle() {
    if (m_Device.GetLegacyScheduler()) {
        Result result = m_Device.GetLegacyScheduler()->Drain(this, false);

        if (result != Result::SUCCESS)
            return result;
    }

    ExclusiveScope lock(*m_Lock);

    const auto& vk = m_Device.GetDispatchTable();
    VkResult vkResult = vk.QueueWaitIdle(m_Handle);
    NRI_RETURN_ON_BAD_VKRESULT(&m_Device, vkResult, "QueueWaitIdle");

    if (!m_PresentFences.empty()) {
#if NRI_COMPATIBILITY_PRESENT_TIMEOUT
        if (!m_TestPresentTimedOut) {
            m_TestPresentTimedOut = true;
            vkResult = VK_TIMEOUT;
        } else
#endif
        vkResult = vk.WaitForFences(m_Device, (uint32_t)m_PresentFences.size(), m_PresentFences.data(), VK_TRUE, MsToUs(NRI_TIMEOUT_FENCE));
        NRI_RETURN_ON_BAD_VKRESULT(&m_Device, vkResult, "WaitForFences(presentation)");
        NRI_RETURN_ON_FAILURE(&m_Device, vkResult == VK_SUCCESS, Result::FAILURE, "Presentation retirement timed out; resources are still in use");

#if NRI_COMPATIBILITY_DIAGNOSTICS
        m_PresentRetired += m_PresentFences.size();
#endif

        for (VkFence fence : m_PresentFences)
            vk.DestroyFence(m_Device, fence, m_Device.GetVkAllocationCallbacks());

        m_PresentFences.clear();
    }

#if NRI_COMPATIBILITY_DIAGNOSTICS
    if (m_PresentSubmitted)
        NRI_REPORT_INFO(&m_Device, "NRI_PRESENT_RETIREMENT {\"submitted\":%llu,\"retired\":%llu,\"pending\":%u,\"peak_fences\":%u}", (unsigned long long)m_PresentSubmitted, (unsigned long long)m_PresentRetired, (uint32_t)m_PresentFences.size(), m_PresentPeak);
#endif

    return Result::SUCCESS;
}
