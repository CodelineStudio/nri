// © 2021 NVIDIA Corporation

BufferVal::~BufferVal() {
    if (m_Memory)
        m_Memory->Unbind(*this);
}

NRI_INLINE void* BufferVal::Map(uint64_t offset, uint64_t size) {
    if (size == WHOLE_SIZE)
        size = m_Desc.size;

    NRI_RETURN_ON_FAILURE(&m_Device, m_IsBoundToMemory, nullptr, "the buffer is not bound to memory");
    NRI_RETURN_ON_FAILURE(&m_Device, !m_IsMapped, nullptr, "the buffer is already mapped (D3D11 doesn't support nested calls)");
    NRI_RETURN_ON_FAILURE(&m_Device, offset <= m_Desc.size && size <= (m_Desc.size - offset), nullptr, "out of bounds");

    void* mappedMemory = GetCoreInterfaceImpl().MapBuffer(*GetImpl(), offset, size);
    m_IsMapped = mappedMemory != nullptr;

    return mappedMemory;
}

NRI_INLINE void BufferVal::Unmap() {
    NRI_RETURN_ON_FAILURE(&m_Device, m_IsMapped, ReturnVoid(), "the buffer is not mapped");

    m_IsMapped = false;

    GetCoreInterfaceImpl().UnmapBuffer(*GetImpl());
}

NRI_INLINE uint64_t BufferVal::GetDeviceAddress() const {
    return GetCoreInterfaceImpl().GetBufferDeviceAddress(*GetImpl());
}
