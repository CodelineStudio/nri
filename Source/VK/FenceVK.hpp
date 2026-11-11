// © 2021 NVIDIA Corporation

FenceVK::~FenceVK() {
    if (m_LegacyState)
        m_Device.GetLegacyScheduler()->ReleaseFence(m_LegacyState);

    CloseSharedHandle(m_SharedHandle);

    if (m_OwnsNativeObjects && m_Handle) {
        const auto& vk = m_Device.GetDispatchTable();
        vk.DestroySemaphore(m_Device, m_Handle, m_Device.GetVkAllocationCallbacks());
    }
}

Result FenceVK::Create(const FenceDesc& fenceDesc) {
    if (m_Device.GetLegacyScheduler() && fenceDesc.initialValue != SWAPCHAIN_SEMAPHORE) {
        if (fenceDesc.shared)
            return Result::UNSUPPORTED;

        m_LegacyState = m_Device.GetLegacyScheduler()->CreateFence(fenceDesc.initialValue);

        return Result::SUCCESS;
    }

    VkSemaphoreTypeCreateInfo semaphoreTypeCreateInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    semaphoreTypeCreateInfo.semaphoreType = fenceDesc.initialValue == SWAPCHAIN_SEMAPHORE ? VK_SEMAPHORE_TYPE_BINARY : VK_SEMAPHORE_TYPE_TIMELINE;
    semaphoreTypeCreateInfo.initialValue = fenceDesc.initialValue == SWAPCHAIN_SEMAPHORE ? 0 : fenceDesc.initialValue;

    VkExportSemaphoreCreateInfo exportInfo = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exportInfo.handleTypes = SHARED_SEMAPHORE_HANDLE_TYPE;

    if (fenceDesc.shared) {
        if (!m_Device.IsSharedFenceSupported(semaphoreTypeCreateInfo.semaphoreType))
            return Result::UNSUPPORTED;

        semaphoreTypeCreateInfo.pNext = &exportInfo;
    }

    VkSemaphoreCreateInfo semaphoreCreateInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphoreCreateInfo.pNext = m_Device.GetLegacyScheduler() ? (fenceDesc.shared ? (const void*)&exportInfo : nullptr) : (const void*)&semaphoreTypeCreateInfo;

    const auto& vk = m_Device.GetDispatchTable();
    VkResult vkResult = vk.CreateSemaphore((VkDevice)m_Device, &semaphoreCreateInfo, m_Device.GetVkAllocationCallbacks(), &m_Handle);
    NRI_RETURN_ON_BAD_VKRESULT(&m_Device, vkResult, "vkCreateSemaphore");

    return Result::SUCCESS;
}

Result FenceVK::Create(const FenceVKDesc& fenceVKDesc) {
    if (m_Device.GetLegacyScheduler())
        return Result::UNSUPPORTED;

    m_OwnsNativeObjects = false;
    m_Handle = (VkSemaphore)fenceVKDesc.vkTimelineSemaphore;

    return Result::SUCCESS;
}

Result FenceVK::GetSharedHandle(SharedHandle& sharedHandle) const {
    if (m_LegacyState)
        return Result::UNSUPPORTED;

    return m_Device.GetSharedSemaphoreHandle(m_Handle, m_SharedHandle, sharedHandle);
}

NRI_INLINE void FenceVK::SetDebugName(const char* name) {
    if (m_LegacyState)
        return;

    m_Device.SetDebugNameToTrivialObject(VK_OBJECT_TYPE_SEMAPHORE, (uint64_t)m_Handle, name);
}

NRI_INLINE uint64_t FenceVK::GetFenceValue() const {
    if (m_LegacyState)
        return m_Device.GetLegacyScheduler()->GetValue(m_LegacyState);

    uint64_t value = 0;

    const auto& vk = m_Device.GetDispatchTable();
    VkResult vkResult = vk.GetSemaphoreCounterValue((VkDevice)m_Device, m_Handle, &value);

    if (vkResult < 0) {
        Result result = GetResultFromVkResult(vkResult);
        m_Device.ReportMessage(Message::ERROR, result, __FILE__, __LINE__, "GetSemaphoreCounterValue(): failed, result = 0x%08X (%d)!", vkResult, vkResult);

        return 0;
    }

    return value;
}

NRI_INLINE Result FenceVK::Wait(uint64_t value) {
    if (m_LegacyState)
        return m_Device.GetLegacyScheduler()->Wait(m_LegacyState, value);

    VkSemaphoreWaitInfo semaphoreWaitInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    semaphoreWaitInfo.semaphoreCount = 1;
    semaphoreWaitInfo.pSemaphores = &m_Handle;
    semaphoreWaitInfo.pValues = &value;

    const auto& vk = m_Device.GetDispatchTable();
    VkResult vkResult = vk.WaitSemaphores((VkDevice)m_Device, &semaphoreWaitInfo, MsToUs(NRI_TIMEOUT_FENCE));
    NRI_RETURN_ON_BAD_VKRESULT(&m_Device, vkResult, "WaitSemaphores");
    NRI_RETURN_ON_FAILURE(&m_Device, vkResult == VK_SUCCESS, Result::FAILURE, "WaitSemaphores() timed out!");

    return Result::SUCCESS;
}
