// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "NRI.h"

#include <array>
#include <cstdio>
#include <limits>

// Compile the real validation class and implementation with a controlled core
// interface. Only the surrounding device/native buffer are test doubles.
namespace nri {

struct Buffer {
    BufferDesc desc = {};
    std::array<uint8_t, 256> bytes{};
    uint32_t maps = 0, unmaps = 0;
    uint64_t offset = 0, size = 0;
    bool failNext = false;

    Buffer() {
        desc.size = bytes.size();
    }
};

struct DeviceVal {
    CoreInterface core = {};
    uint32_t errors = 0;

    static const BufferDesc& NRI_CALL GetDesc(const Buffer& buffer) {
        return buffer.desc;
    }

    static void* NRI_CALL Map(Buffer& buffer, uint64_t offset, uint64_t size) {
        buffer.maps++;
        buffer.offset = offset;
        buffer.size = size;

        if (buffer.failNext) {
            buffer.failNext = false;

            return nullptr;
        }

        if (offset > buffer.desc.size || size > (buffer.desc.size - offset))
            return nullptr;

        return buffer.bytes.data() + offset;
    }

    static void NRI_CALL Unmap(Buffer& buffer) {
        buffer.unmaps++;
    }

    DeviceVal() {
        core.GetBufferDesc = GetDesc;
        core.MapBuffer = Map;
        core.UnmapBuffer = Unmap;
    }
};

struct ObjectVal {
    ObjectVal(DeviceVal& device, void* impl)
        : m_Device(device)
        , m_Impl(impl) {
    }

    const CoreInterface& GetCoreInterfaceImpl() const {
        return m_Device.core;
    }

protected:
    DeviceVal& m_Device;
    void* m_Impl;
};

struct BufferVal;
struct MemoryVal {
    void Unbind(BufferVal&) {}
};

inline void ReturnVoid() {}

} // namespace nri

#define NRI_INLINE inline
#define NRI_RETURN_ON_FAILURE(device, condition, result, ...) \
    do { \
        if (!(condition)) { \
            (device)->errors++; \
            return result; \
        } \
    } while (false)

#include "BufferVal.h"
namespace nri {
#include "BufferVal.hpp"
}

struct Fixture {
    nri::DeviceVal device;
    nri::Buffer native;
    nri::BufferVal buffer;

    Fixture(bool bound = true)
        : buffer(device, &native, bound) {
    }
};

int main() {
    uint32_t failures = 0;
    auto check = [&](bool passed, const char* name) {
        if (!passed) {
            std::fprintf(stderr, "FAILED: %s\n", name);
            failures++;
        }
    };

    {
        Fixture f;
        f.native.failNext = true;
        check(f.buffer.Map(7, 20) == nullptr && f.native.maps == 1 && f.device.errors == 0, "backend failure returned");
        check(f.buffer.Map(7, 20) == f.native.bytes.data() + 7 && f.native.maps == 2 && f.device.errors == 0, "retry after failure without unmap");
        f.buffer.Unmap();
        check(f.native.unmaps == 1 && f.device.errors == 0, "successful retry unmaps once");
    }
    {
        Fixture f;
        f.native.failNext = true;
        check(f.buffer.Map(0, 64) == nullptr, "failed map setup");
        f.buffer.Unmap();
        check(f.native.unmaps == 0 && f.device.errors == 1, "failed map does not permit backend unmap");
    }
    {
        Fixture f;
        check(f.buffer.Map(0, nri::WHOLE_SIZE) == f.native.bytes.data() && f.native.size == 256, "whole buffer map");
        check(f.buffer.Map(0, 1) == nullptr && f.native.maps == 1 && f.device.errors == 1, "nested map rejected");
        f.buffer.Unmap();
        check(f.buffer.Map(255, 1) == f.native.bytes.data() + 255 && f.native.maps == 2, "last byte map after unmap");
        f.buffer.Unmap();
        f.buffer.Unmap();
        check(f.native.unmaps == 2 && f.device.errors == 2, "double unmap rejected");
    }
    {
        Fixture f(false);
        check(f.buffer.Map(0, 1) == nullptr && f.native.maps == 0 && f.device.errors == 1, "unbound buffer rejected");
    }
    {
        Fixture f;
        check(f.buffer.Map(256, 1) == nullptr && f.native.maps == 0 && f.device.errors == 1, "range beyond end rejected");
        check(f.buffer.Map(std::numeric_limits<uint64_t>::max() - 3, 8) == nullptr && f.native.maps == 0 && f.device.errors == 2, "offset addition overflow rejected");
    }
    {
        Fixture f;
        check(f.buffer.Map(4, std::numeric_limits<uint64_t>::max() - 1) == nullptr && f.native.maps == 0 && f.device.errors == 1, "size addition overflow rejected");
        check(f.buffer.Map(0, 1) == f.native.bytes.data() && f.native.maps == 1, "rejected range leaves retry available");
        f.buffer.Unmap();
    }

    std::printf("Buffer validation mapping tests: %u failures\n", failures);

    return failures ? 1 : 0;
}
