// © 2021 NVIDIA Corporation

#pragma once

namespace nri {

struct QueueVK final : public DebugNameBase {
    inline QueueVK(DeviceVK& device)
        : m_Device(device)
        , m_PresentFences(device.GetStdAllocator()) {
    }

    ~QueueVK();

    inline operator VkQueue() const {
        return m_Handle;
    }

    inline DeviceVK& GetDevice() const {
        return m_Device;
    }

    inline uint32_t GetFamilyIndex() const {
        return m_FamilyIndex;
    }

    inline QueueType GetType() const {
        return m_Type;
    }

    inline Lock& GetLock() {
        return *m_Lock;
    }

    Result Create(QueueType type, uint32_t familyIndex, VkQueue handle, Lock* sharedLock);

    //================================================================================================================
    // DebugNameBase
    //================================================================================================================

    void SetDebugName(const char* name) NRI_DEBUG_NAME_OVERRIDE;

    //================================================================================================================
    // NRI
    //================================================================================================================

    void BeginAnnotation(const char* name, uint32_t bgra);
    void EndAnnotation();
    void Annotation(const char* name, uint32_t bgra);
    void GetCalibratedTimestamps(uint64_t& timestampGPU, uint64_t& timestampCPU);
    Result Submit(const QueueSubmitDesc& queueSubmitDesc);
    Result WaitIdle();

    // Caller holds GetLock(). Tracks only maintenance1 presentation payloads;
    // ordinary GPU submission completion remains the caller's responsibility.
    VkResult Present(const VkPresentInfoKHR& presentInfo);

private:
    DeviceVK& m_Device;
    Vector<VkFence> m_PresentFences;
#if NRI_COMPATIBILITY_DIAGNOSTICS
    uint64_t m_PresentSubmitted = 0;
    uint64_t m_PresentRetired = 0;
    uint32_t m_PresentPeak = 0;
#endif
#if NRI_COMPATIBILITY_PRESENT_OOM
    bool m_TestPresentFailed = false;
#endif
#if NRI_COMPATIBILITY_PRESENT_TIMEOUT
    bool m_TestPresentTimedOut = false;
#endif
    VkQueue m_Handle = VK_NULL_HANDLE;
    Lock* m_Lock = nullptr; // lock actually used
    uint32_t m_FamilyIndex = INVALID_FAMILY_INDEX;
    QueueType m_Type = QueueType(-1);
    Lock m_OwnLock; // storage used when this wrapper owns the lock
};

} // namespace nri
