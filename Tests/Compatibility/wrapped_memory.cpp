// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "NRI.h"
#include "Extensions/NRIWrapperVK.h"
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Fixture {
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::WrapperVKInterface wrapper = {};
    nri::Queue* queue = nullptr;
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::Fence* fence = nullptr;
    nri::Buffer* readback = nullptr;
    std::array<nri::Buffer*, 2> buffers{};
    nri::Memory* wrapped = nullptr;
    VkDevice nativeDevice = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memoryProperties = {};
    VkPhysicalDeviceProperties properties = {};
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkFlushMappedMemoryRanges Flush = nullptr;
    PFN_vkInvalidateMappedMemoryRanges Invalidate = nullptr;
    PFN_vkGetBufferMemoryRequirements Requirements = nullptr;
    std::atomic<uint32_t> errors{0}, warnings{0};
    uint32_t cycles = 0, providedCases = 0, nriMapCases = 0, unknownExtentCases = 0;
    uint32_t deviceId = 0, driver = 0, noncoherentCases = 0;
    bool ready = false, mapped = false, addressEnabled = false;

    static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* text, void* user) {
        auto& f = *(Fixture*)user;

        if (severity == nri::Message::ERROR)
            f.errors++;
        else if (severity == nri::Message::WARNING)
            f.warnings++;

        std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, text ? text : "");
    }

    static void NRI_CALL NoAbort(void*) {}

    bool Check(bool passed, const char* name) {
        if (!passed)
            std::fprintf(stderr, "Wrapped memory failed: %s\n", name);

        return passed;
    }

    bool Initialize(bool validation) {
        uint32_t count = 0;

        if (!Check(nri::nriEnumerateAdapters(nullptr, count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && count, "adapter count"))
            return false;

        std::vector<nri::AdapterDesc> adapters(count);

        if (!Check(nri::nriEnumerateAdapters(adapters.data(), count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && count, "adapters"))
            return false;

        nri::DeviceCreationDesc create = {};
        create.graphicsAPI = nri::GraphicsAPI::VK;
        create.adapterDesc = adapters.data();
        create.disableVKRayTracing = true;
        create.enableNRIValidation = create.enableGraphicsAPIValidation = validation;
        create.callbackInterface.MessageCallback = Message;
        create.callbackInterface.AbortExecution = NoAbort;
        create.callbackInterface.userArg = this;

        if (!Check(nri::nriCreateDevice(create, device) == nri::Result::SUCCESS, "device") ||
            !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core) == nri::Result::SUCCESS, "core") ||
            !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::WrapperVKInterface), &wrapper) == nri::Result::SUCCESS, "wrapper"))
            return false;

        ready = true;
        deviceId = core.GetDeviceDesc(*device).adapterDesc.deviceId;
        driver = core.GetDeviceDesc(*device).adapterDesc.driverVersion;
        nativeDevice = (VkDevice)core.GetDeviceNativeObject(device);
        auto instance = (VkInstance)wrapper.GetInstanceVK(*device);
        auto physical = (VkPhysicalDevice)wrapper.GetPhysicalDeviceVK(*device);
        auto instanceProc = (PFN_vkGetInstanceProcAddr)wrapper.GetInstanceProcAddrVK(*device);
        auto deviceProc = (PFN_vkGetDeviceProcAddr)wrapper.GetDeviceProcAddrVK(*device);

        if (!Check(nativeDevice && instance && physical && instanceProc && deviceProc, "native device and dispatch handles"))
            return false;

        auto getMemory = (PFN_vkGetPhysicalDeviceMemoryProperties)instanceProc(instance, "vkGetPhysicalDeviceMemoryProperties");
        auto getProperties = (PFN_vkGetPhysicalDeviceProperties)instanceProc(instance, "vkGetPhysicalDeviceProperties");

        if (!Check(getMemory && getProperties && deviceProc, "native dispatch"))
            return false;

        getMemory(physical, &memoryProperties);
        getProperties(physical, &properties);
        AllocateMemory = (PFN_vkAllocateMemory)deviceProc(nativeDevice, "vkAllocateMemory");
        FreeMemory = (PFN_vkFreeMemory)deviceProc(nativeDevice, "vkFreeMemory");
        MapMemory = (PFN_vkMapMemory)deviceProc(nativeDevice, "vkMapMemory");
        UnmapMemory = (PFN_vkUnmapMemory)deviceProc(nativeDevice, "vkUnmapMemory");
        Flush = (PFN_vkFlushMappedMemoryRanges)deviceProc(nativeDevice, "vkFlushMappedMemoryRanges");
        Invalidate = (PFN_vkInvalidateMappedMemoryRanges)deviceProc(nativeDevice, "vkInvalidateMappedMemoryRanges");
        Requirements = (PFN_vkGetBufferMemoryRequirements)deviceProc(nativeDevice, "vkGetBufferMemoryRequirements");

        if (!Check(AllocateMemory && FreeMemory && MapMemory && UnmapMemory && Flush && Invalidate && Requirements, "memory dispatch") ||
            !Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, 0, queue) == nri::Result::SUCCESS, "queue") ||
            !Check(core.CreateCommandAllocator(*queue, allocator) == nri::Result::SUCCESS, "allocator") ||
            !Check(core.CreateCommandBuffer(*allocator, command) == nri::Result::SUCCESS, "command") ||
            !Check(core.CreateFence(*device, nri::FenceDesc{}, fence) == nri::Result::SUCCESS, "fence"))
            return false;

        nri::BufferDesc desc = {};
        desc.size = 1024;

        if (!Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_READBACK, 0, desc, readback) == nri::Result::SUCCESS, "readback"))
            return false;

        // Probe an already bound NRI buffer, then compare with the enabled
        // device manifest in the runner. Physical support is not admission.
        addressEnabled = core.GetBufferDeviceAddress(*readback) != 0;

        return true;
    }

    void RetireWrapped() {
        for (auto*& buffer : buffers) {
            if (buffer)
                core.DestroyBuffer(buffer);

            buffer = nullptr;
        }

        if (wrapped)
            core.FreeMemory(wrapped);

        wrapped = nullptr;
    }

    bool Cycle(uint32_t cycle) {
        uint32_t mode = cycle % 3;
        uint32_t typeBits = ~0u;
        uint64_t alignment = std::max(uint64_t(properties.limits.nonCoherentAtomSize), uint64_t(properties.limits.minMemoryMapAlignment));
        uint64_t requirementSize = 0;

        for (auto*& buffer : buffers) {
            nri::BufferDesc desc = {};
            desc.size = 260;

            if (!Check(core.CreateBuffer(*device, desc, buffer) == nri::Result::SUCCESS, "buffer"))
                return false;

            nri::MemoryDesc nriRequirements = {};
            core.GetBufferMemoryDesc(*buffer, nri::MemoryLocation::HOST_UPLOAD, nriRequirements);

            if (!Check(!nriRequirements.mustBeDedicated, "shared fixture allocation supported"))
                return false;

            VkMemoryRequirements requirements = {};
            Requirements(nativeDevice, (VkBuffer)core.GetBufferNativeObject(buffer), &requirements);
            typeBits &= requirements.memoryTypeBits;
            alignment = std::max(alignment, requirements.alignment);
            requirementSize = std::max(requirementSize, requirements.size);
        }

        uint32_t type = UINT32_MAX;

        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
            auto flags = memoryProperties.memoryTypes[i].propertyFlags;

            if ((typeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                type = i;

                if (!(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                    break;
            }
        }

        if (!Check(type != UINT32_MAX, "host memory type"))
            return false;

        uint64_t span = ((requirementSize + alignment - 1) / alignment) * alignment;
        uint64_t offsets[] = {alignment, 2 * alignment + span};
        uint64_t mapOffset = 2 * alignment;
        uint64_t mapSize = 3 * alignment + 2 * span;
        uint64_t allocationSize = mapOffset + mapSize + 2 * alignment;
        bool noncoherent = !(memoryProperties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.pNext = addressEnabled ? &flags : nullptr;
        allocate.allocationSize = allocationSize;
        allocate.memoryTypeIndex = type;

        if (!Check(AllocateMemory(nativeDevice, &allocate, nullptr, &memory) == VK_SUCCESS, "native allocation"))
            return false;

        void* pointer = nullptr;

        if (!Check(MapMemory(nativeDevice, memory, 0, VK_WHOLE_SIZE, 0, &pointer) == VK_SUCCESS, "initialize native mapping"))
            return false;

        mapped = true;
        std::memset(pointer, 0xd7, size_t(allocationSize));
        VkMappedMemoryRange nativeRange = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        nativeRange.memory = memory;
        nativeRange.size = VK_WHOLE_SIZE;

        if (!Check(Flush(nativeDevice, 1, &nativeRange) == VK_SUCCESS, "initialize backing bytes"))
            return false;

        UnmapMemory(nativeDevice, memory);
        mapped = false;
        pointer = nullptr;

        if (mode != 1) {
            if (!Check(MapMemory(nativeDevice, memory, mapOffset, mode == 2 ? VK_WHOLE_SIZE : mapSize, 0, &pointer) == VK_SUCCESS, "caller partial mapping"))
                return false;

            mapped = true;
        }

        nri::MemoryVKDesc wrap = {};
        wrap.vkDeviceMemory = (uint64_t)memory;
        wrap.memoryTypeIndex = type;
        wrap.offset = mapOffset;
        wrap.size = mode == 2 ? VK_WHOLE_SIZE : mapSize;
        wrap.mappedMemory = pointer;

        if (!Check(wrapper.CreateMemoryVK(*device, wrap, wrapped) == nri::Result::SUCCESS, "wrap native memory"))
            return false;

        mapped = true;
        nri::BindBufferMemoryDesc bindings[] = {{buffers[0], wrapped, offsets[0]}, {buffers[1], wrapped, offsets[1]}};

        if (!Check(core.BindBufferMemory(bindings, 2) == nri::Result::SUCCESS, "bind wrapped mapping"))
            return false;

        std::array<std::array<uint8_t, 260>, 2> expected{};

        for (uint32_t i = 0; i < 2; i++) {
            for (uint32_t j = 0; j < 260; j++)
                expected[i][j] = uint8_t(j * 17 + cycle * 29 + i * 97);

            pointer = core.MapBuffer(*buffers[i], 0, nri::WHOLE_SIZE);

            if (!Check(pointer != nullptr, "wrapped full buffer map"))
                return false;

            std::memcpy(pointer, expected[i].data(), 260);
            core.UnmapBuffer(*buffers[i]);
            pointer = core.MapBuffer(*buffers[i], 3, 125);

            if (!Check(pointer != nullptr, "wrapped partial buffer map"))
                return false;

            for (uint32_t j = 3; j < 128; j++)
                expected[i][j] ^= 0x63;

            std::memcpy(pointer, expected[i].data() + 3, 125);
            core.UnmapBuffer(*buffers[i]);
        }

        core.ResetCommandAllocator(*allocator);

        if (!Check(core.BeginCommandBuffer(*command, nullptr) == nri::Result::SUCCESS, "begin"))
            return false;

        nri::GlobalBarrierDesc global = {};
        global.before = {nri::AccessBits::HOST_WRITE | nri::AccessBits::HOST_READ | nri::AccessBits::COPY_DESTINATION, nri::StageBits::HOST | nri::StageBits::COPY};
        global.after = {nri::AccessBits::COPY_SOURCE | nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        nri::BarrierDesc barriers = {};
        barriers.globals = &global;
        barriers.globalNum = 1;
        core.CmdBarrier(*command, barriers);

        for (uint32_t i = 0; i < 2; i++)
            core.CmdCopyBuffer(*command, *readback, i * 260, *buffers[i], 0, 260);

        global.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        global.after = {nri::AccessBits::HOST_READ, nri::StageBits::HOST};
        core.CmdBarrier(*command, barriers);

        if (!Check(core.EndCommandBuffer(*command) == nri::Result::SUCCESS, "end"))
            return false;

        nri::FenceSubmitDesc signal = {fence, cycle + 1u, nri::StageBits::ALL};
        nri::QueueSubmitDesc submit = {};
        submit.commandBuffers = &command;
        submit.commandBufferNum = 1;
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*queue, submit) == nri::Result::SUCCESS, "submit"))
            return false;

        core.Wait(*fence, cycle + 1);

        if (!Check(core.GetFenceValue(*fence) >= cycle + 1, "completion"))
            return false;

        pointer = core.MapBuffer(*readback, 0, 520);

        if (!Check(pointer != nullptr, "readback"))
            return false;

        bool exact = std::memcmp(pointer, expected.data(), 520) == 0;
        core.UnmapBuffer(*readback);
        RetireWrapped();
        nativeRange.offset = mapOffset;
        nativeRange.size = mode == 2 ? VK_WHOLE_SIZE : mapSize;

        if (!Check(Flush(nativeDevice, 1, &nativeRange) == VK_SUCCESS, "caller mapping survives wrapper destruction"))
            return false;

        UnmapMemory(nativeDevice, memory);
        mapped = false;

        if (!Check(MapMemory(nativeDevice, memory, 0, VK_WHOLE_SIZE, 0, &pointer) == VK_SUCCESS, "caller allocation survives wrapper destruction"))
            return false;

        mapped = true;
        nativeRange.offset = 0;
        nativeRange.size = VK_WHOLE_SIZE;

        if (!Check(Invalidate(nativeDevice, 1, &nativeRange) == VK_SUCCESS, "native final readback"))
            return false;

        for (uint64_t byte = 0; byte < allocationSize; byte++) {
            uint8_t value = 0xd7;

            for (uint32_t i = 0; i < 2; i++) {
                uint64_t base = mapOffset + offsets[i];

                if (byte >= base && byte < (base + 260))
                    value = expected[i][size_t(byte - base)];
            }

            exact &= ((const uint8_t*)pointer)[byte] == value;
        }

        UnmapMemory(nativeDevice, memory);
        mapped = false;
        FreeMemory(nativeDevice, memory, nullptr);
        memory = VK_NULL_HANDLE;

        if (!Check(exact && errors.load() == 0, "exact contents, untouched padding and native ownership"))
            return false;

        providedCases += mode == 0;
        nriMapCases += mode == 1;
        unknownExtentCases += mode == 2;
        noncoherentCases += noncoherent;
        cycles++;
        std::printf("NRI_WRAPPED_CASE {\"cycle\":%u,\"mode\":%u,\"mapping_offset\":%llu,\"mapping_size\":%llu,\"allocation_size\":%llu,\"buffer_offsets\":[%llu,%llu],\"atom_size\":%llu,\"memory_type\":%u,\"memory_flags\":%u}\n", cycle, mode, (unsigned long long)mapOffset, (unsigned long long)mapSize, (unsigned long long)allocationSize, (unsigned long long)offsets[0], (unsigned long long)offsets[1], (unsigned long long)properties.limits.nonCoherentAtomSize, type, memoryProperties.memoryTypes[type].propertyFlags);

        return true;
    }

    bool Cleanup() {
        bool passed = true;

        if (ready) {
            passed = core.DeviceWaitIdle(device) == nri::Result::SUCCESS;
            RetireWrapped();

            if (mapped)
                UnmapMemory(nativeDevice, memory);

            if (memory)
                FreeMemory(nativeDevice, memory, nullptr);

            if (readback)
                core.DestroyBuffer(readback);

            if (command)
                core.DestroyCommandBuffer(command);

            if (allocator)
                core.DestroyCommandAllocator(allocator);

            if (fence)
                core.DestroyFence(fence);
        }

        if (device)
            nri::nriDestroyDevice(device);

        return passed;
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

    Fixture fixture;
    bool passed = fixture.Initialize(validation);

    for (uint32_t cycle = 0; cycle < 24 && passed; cycle++)
        passed = fixture.Cycle(cycle);

    passed = fixture.Cleanup() && passed && fixture.errors.load() == 0 && fixture.warnings.load() == 0;
    std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_wrapped_memory\",\"status\":\"%s\",\"source_commit\":\"%s\",\"compiler\":\"%s\",\"device_id\":%u,\"driver_version_raw\":%u,\"validation_errors\":%u,\"validation_warnings\":%u,\"exact_cycles\":%u,\"provided_mapping_cases\":%u,\"nri_mapping_cases\":%u,\"unknown_extent_cases\":%u,\"native_noncoherent_cases\":%u,\"address_allocation\":%s,\"mapped_ranges_forced\":%s,\"legacy_sync_forced\":%s}\n", passed ? "passed" : "failed", NRI_COMPATIBILITY_SOURCE_COMMIT, NRI_COMPATIBILITY_COMPILER, fixture.deviceId, fixture.driver, fixture.errors.load(), fixture.warnings.load(), fixture.cycles, fixture.providedCases, fixture.nriMapCases, fixture.unknownExtentCases, fixture.noncoherentCases, fixture.addressEnabled ? "true" : "false", NRI_COMPATIBILITY_MAPPED_RANGES ? "true" : "false", NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false");

    return std::fflush(stdout) == 0 && passed ? 0 : 1;
}
