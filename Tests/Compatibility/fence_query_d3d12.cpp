// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "NRI.h"
#include "Extensions/NRIWrapperD3D12.h"
#include "Extensions/NRISwapChain.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>

// Compile the real FenceD3D12 class/body with controlled native fence/event
// dependencies. No device is created, no OS wait runs, and no GPU is reset.
using HANDLE = void*;
using HRESULT = int32_t;
static constexpr HRESULT S_OK = 0;
static constexpr HRESULT E_FAIL = int32_t(0x80004005u);
static constexpr HRESULT E_OUTOFMEMORY = int32_t(0x8007000Eu);
static constexpr HRESULT DXGI_ERROR_DEVICE_REMOVED = int32_t(0x887A0005u);
static constexpr uint32_t WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = UINT32_MAX;
static constexpr uint32_t GENERIC_ALL = 0x10000000;
static constexpr bool FALSE = false;
static const HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(intptr_t(-1));
enum D3D12_FENCE_FLAGS { D3D12_FENCE_FLAG_NONE, D3D12_FENCE_FLAG_SHARED };

namespace {
struct EventState {
    HANDLE handle = reinterpret_cast<HANDLE>(uintptr_t(1));
    HRESULT setResult = S_OK;
    uint32_t waitResult = WAIT_OBJECT_0, resets = 0, sets = 0, waits = 0;
} events;
}

static HANDLE CreateEventA(void*, bool, bool, const char*) { return events.handle; }
static bool CloseHandle(HANDLE) { return true; }
static bool ResetEvent(HANDLE) { events.resets++; return true; }
static uint32_t WaitForSingleObjectEx(HANDLE, uint32_t, bool) { events.waits++; return events.waitResult; }

struct ID3D12Fence {
    std::array<uint64_t, 4> values{};
    uint32_t count = 1, queries = 0;
    uint64_t GetCompletedValue() { return values[std::min(queries++, count - 1)]; }
    HRESULT SetEventOnCompletion(uint64_t, HANDLE) { events.sets++; return events.setResult; }
};
struct ID3D12CommandQueue {
    HRESULT Signal(ID3D12Fence*, uint64_t) { return S_OK; }
    HRESULT Wait(ID3D12Fence*, uint64_t) { return S_OK; }
};

namespace nri {
struct DebugNameBase {};
template<class T> struct ComPtr {
    T* value = nullptr;
    ComPtr& operator=(T* pointer) { value = pointer; return *this; }
    operator T*() const { return value; }
    T* operator->() const { return value; }
    T* GetInterface() const { return value; }
};
struct NativeDevice {
    HRESULT CreateFence(uint64_t, D3D12_FENCE_FLAGS, ComPtr<ID3D12Fence>*) { return E_FAIL; }
    HRESULT CreateSharedHandle(ID3D12Fence*, void*, uint32_t, const char*, HANDLE*) { return E_FAIL; }
};
struct DeviceD3D12 {
    NativeDevice native;
    uint32_t reports = 0;
    Result reportedResult = Result::SUCCESS;
    NativeDevice* operator->() { return &native; }
    void ReportMessage(Message, Result failure, const char*, uint32_t, const char*, ...) {
        reports++;
        reportedResult = failure;
    }
};
struct QueueD3D12 {
    ID3D12CommandQueue native;
    operator ID3D12CommandQueue*() { return &native; }
};
// HRESULT mapping is a surrounding dependency, not the wait under test.
static Result TestResultFromHRESULT(HRESULT hr) {
    if (hr == E_OUTOFMEMORY)
        return Result::OUT_OF_MEMORY;

    if (hr == DXGI_ERROR_DEVICE_REMOVED)
        return Result::DEVICE_LOST;

    return Result::FAILURE;
}
}

#define IID_PPV_ARGS(pointer) pointer
#define NRI_INLINE inline
#define NRI_DEBUG_NAME_OVERRIDE
#define NRI_SET_D3D_DEBUG_OBJECT_NAME(object, name) ((void)0)
#define NRI_TIMEOUT_FENCE 1000
#define NRI_RETURN_ON_BAD_HRESULT(device, hr, operation) \
    do { \
        if ((hr) < 0) { \
            const Result failure = TestResultFromHRESULT(hr); \
            (device)->ReportMessage(Message::ERROR, failure, __FILE__, __LINE__, operation); \
            return failure; \
        } \
    } while (false)
#define NRI_RETURN_VOID_ON_BAD_HRESULT(device, hr, operation) \
    do { \
        if ((hr) < 0) { \
            (device)->ReportMessage(Message::ERROR, TestResultFromHRESULT(hr), __FILE__, __LINE__, operation); \
            return; \
        } \
    } while (false)
#define NRI_RETURN_ON_FAILURE(device, condition, result, ...) \
    do { \
        if (!(condition)) { \
            (device)->ReportMessage(Message::ERROR, result, __FILE__, __LINE__, __VA_ARGS__); \
            return result; \
        } \
    } while (false)

#include "FenceD3D12.h"
namespace nri {
#include "FenceD3D12.hpp"
}

uint32_t RunFenceD3D12WaitTests() {
    uint32_t failures = 0;
    auto check = [&](bool success, const char* name) {
        if (!success) {
            std::fprintf(stderr, "FAILED D3D12 fence wait: %s\n", name);
            failures++;
        }
    };
    auto run = [&](std::initializer_list<uint64_t> values, HANDLE event, uint32_t waitResult, HRESULT setResult,
                   uint64_t target, nri::Result expected, uint32_t queries, uint32_t waits, uint32_t sets, const char* name) {
        events = {};
        events.handle = event;
        events.waitResult = waitResult;
        events.setResult = setResult;
        ID3D12Fence native;
        native.count = uint32_t(values.size());
        std::copy(values.begin(), values.end(), native.values.begin());
        nri::DeviceD3D12 device;
        nri::FenceD3D12 fence(device);
        check(fence.Create(nri::FenceD3D12Desc{&native}) == nri::Result::SUCCESS, "wrap controlled fence");
        const auto result = fence.Wait(target);
        check(result == expected && native.queries == queries && events.waits == waits && events.sets == sets &&
            events.resets == sets && device.reports == (expected == nri::Result::SUCCESS ? 0u : 1u) &&
            device.reportedResult == expected, name);
    };
    const HANDLE event = reinterpret_cast<HANDLE>(uintptr_t(1));
    run({0}, event, WAIT_OBJECT_0, S_OK, 0, nri::Result::SUCCESS, 1, 0, 0, "zero already complete avoids event setup");
    run({7}, event, WAIT_OBJECT_0, S_OK, 4, nri::Result::SUCCESS, 1, 0, 0, "already complete avoids event setup");
    run({UINT64_MAX}, event, WAIT_OBJECT_0, S_OK, 0, nri::Result::DEVICE_LOST, 1, 0, 0, "removal before wait even for zero target");
    run({1, 2, 4}, nullptr, WAIT_OBJECT_0, S_OK, 4, nri::Result::SUCCESS, 3, 0, 0, "busy wait observes completion");
    run({0, UINT64_MAX}, nullptr, WAIT_OBJECT_0, S_OK, 4, nri::Result::DEVICE_LOST, 2, 0, 0, "busy wait exits on removal");
    run({0, UINT64_MAX}, INVALID_HANDLE_VALUE, WAIT_OBJECT_0, S_OK, 4, nri::Result::DEVICE_LOST, 2, 0, 0, "invalid event busy wait exits on removal");
    run({0, 4}, event, WAIT_OBJECT_0, S_OK, 4, nri::Result::SUCCESS, 2, 1, 1, "event completion verified");
    run({0, 3}, event, WAIT_OBJECT_0, S_OK, 4, nri::Result::FAILURE, 2, 1, 1, "premature event wake rejected");
    run({0, UINT64_MAX}, event, WAIT_OBJECT_0, S_OK, 4, nri::Result::DEVICE_LOST, 2, 1, 1, "removal during event wait");
    run({0, UINT64_MAX}, event, WAIT_TIMEOUT, S_OK, 4, nri::Result::DEVICE_LOST, 2, 1, 1, "removal dominates timeout");
    run({0, 0}, event, WAIT_TIMEOUT, S_OK, 4, nri::Result::FAILURE, 2, 1, 1, "event timeout preserved");
    run({0, 4}, event, WAIT_TIMEOUT, S_OK, 4, nri::Result::FAILURE, 2, 1, 1, "native timeout is not hidden by late completion");
    run({0, 0}, event, WAIT_FAILED, S_OK, 4, nri::Result::FAILURE, 2, 1, 1, "native wait error preserved");
    run({0}, event, WAIT_OBJECT_0, E_OUTOFMEMORY, 4, nri::Result::OUT_OF_MEMORY, 1, 0, 1, "SetEvent allocation error preserved");
    run({0}, event, WAIT_OBJECT_0, DXGI_ERROR_DEVICE_REMOVED, 4, nri::Result::DEVICE_LOST, 1, 0, 1, "SetEvent removal preserved");
    {
        nri::DeviceD3D12 device;
        nri::FenceD3D12 fence(device);
        check(fence.Wait(4) == nri::Result::SUCCESS && device.reports == 0, "absent swapchain fence retains no-op semantics");
    }
    std::printf("D3D12 fence wait checks: 16 cases, %u failures\n", failures);

    return failures;
}
