// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRISwapChain.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CLOSE) {
        PostQuitMessage(0);

        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

static bool PumpMessages() {
    MSG message = {};

    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT)
            return false;

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return true;
}

struct Fixture {
    std::atomic<uint32_t> errors{0};
    std::atomic<uint32_t> warnings{0};
    HWND window = nullptr;
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::SwapChainInterface wsi = {};
    nri::Queue* queues[2] = {};
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::SwapChain* swapChain = nullptr;
    nri::Buffer* readback = nullptr;
    nri::Fence* acquire = nullptr;
    nri::Fence* completed = nullptr;
    nri::Fence* gate = nullptr;
    std::vector<nri::Descriptor*> views;
    std::vector<nri::Fence*> release;
    std::vector<bool> presented;
    nri::Texture* const* textures = nullptr;
    uint32_t deviceId = 0;
    uint32_t driverVersion = 0;
    uint32_t frames = 0;
    uint32_t cycles = 0;
    uint32_t notReady = 0;
    uint32_t delayedPresents = 0;
    uint32_t outOfDate = 0;
    bool needsRecreation = false;
    bool coreReady = false;
    bool wsiReady = false;

    static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* text, void* user) {
        auto& fixture = *(Fixture*)user;

        if (severity == nri::Message::ERROR)
            fixture.errors++;
        else if (severity == nri::Message::WARNING)
            fixture.warnings++;

        std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, text ? text : "");
    }

    static void NRI_CALL DoNotAbort(void*) {
    }

    bool Check(bool success, const char* operation) {
        if (!success)
            std::fprintf(stderr, "Presentation fixture failed: %s\n", operation);

        return success;
    }

    bool Initialize(bool validation, uint32_t adapterIndex) {
        WNDCLASSW windowClass = {};
        windowClass.lpfnWndProc = WindowProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"NRICompatibilityPresentation";

        if (!Check(RegisterClassW(&windowClass) != 0, "register window class"))
            return false;

        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass.lpszClassName, L"NRI compatibility test", WS_OVERLAPPEDWINDOW,
            80, 80, 240, 180, nullptr, nullptr, windowClass.hInstance, nullptr);

        if (!Check(window != nullptr, "create owned test window"))
            return false;

        ShowWindow(window, SW_SHOWNOACTIVATE);
        uint32_t count = 0;

        if (!Check(nri::nriEnumerateAdapters(nullptr, count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && adapterIndex < count, "enumerate adapters"))
            return false;

        std::vector<nri::AdapterDesc> adapters(count);

        if (!Check(nri::nriEnumerateAdapters(adapters.data(), count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && adapters[adapterIndex].queueNum[size_t(nri::QueueType::GRAPHICS)] >= 2, "two graphics queues required"))
            return false;

        nri::QueueFamilyDesc family = {};
        family.queueType = nri::QueueType::GRAPHICS;
        family.queueNum = 2;
        nri::DeviceCreationDesc create = {};
        create.graphicsAPI = nri::GraphicsAPI::VK;
        create.adapterDesc = &adapters[adapterIndex];
        create.queueFamilies = &family;
        create.queueFamilyNum = 1;
        create.enableNRIValidation = validation;
        create.enableGraphicsAPIValidation = validation;
        create.disableVKRayTracing = true;
        create.callbackInterface.MessageCallback = Message;
        create.callbackInterface.AbortExecution = DoNotAbort;
        create.callbackInterface.userArg = this;

        if (!Check(nri::nriCreateDevice(create, device) == nri::Result::SUCCESS, "create device") ||
            !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core) == nri::Result::SUCCESS, "core interface"))
            return false;

        coreReady = true;
        const auto& desc = core.GetDeviceDesc(*device);
        deviceId = desc.adapterDesc.deviceId;
        driverVersion = desc.adapterDesc.driverVersion;

        if (!Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::SwapChainInterface), &wsi) == nri::Result::SUCCESS, "swapchain interface"))
            return false;

        wsiReady = true;

        for (uint32_t i = 0; i < 2; i++) {
            if (!Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, i, queues[i]) == nri::Result::SUCCESS, "queue"))
                return false;
        }

        nri::FenceDesc binary = {};
        binary.initialValue = nri::SWAPCHAIN_SEMAPHORE;

        return Check(core.CreateCommandAllocator(*queues[0], allocator) == nri::Result::SUCCESS, "allocator") &&
            Check(core.CreateCommandBuffer(*allocator, command) == nri::Result::SUCCESS, "command") &&
            Check(core.CreateFence(*device, binary, acquire) == nri::Result::SUCCESS, "acquire semaphore") &&
            Check(core.CreateFence(*device, nri::FenceDesc{}, completed) == nri::Result::SUCCESS, "completion") &&
            Check(core.CreateFence(*device, nri::FenceDesc{}, gate) == nri::Result::SUCCESS, "future gate");
    }

    bool CreateChain(uint32_t cycle) {
        RECT rectangle = {0, 0, LONG(128 + (cycle % 3) * 32), LONG(96 + (cycle % 2) * 32)};
        AdjustWindowRectEx(&rectangle, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);

        if (!Check(SetWindowPos(window, nullptr, 0, 0, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0 && PumpMessages(), "resize owned window"))
            return false;

        GetClientRect(window, &rectangle);
        nri::SwapChainDesc desc = {};
        desc.window.windows.hwnd = window;
        desc.queue = queues[0];
        desc.width = nri::Dim_t(rectangle.right);
        desc.height = nri::Dim_t(rectangle.bottom);
        desc.textureNum = 3;
        desc.format = nri::SwapChainFormat::BT709_G22_8BIT;
        desc.flags = (cycle % 2) ? nri::SwapChainBits::VSYNC : nri::SwapChainBits::ALLOW_TEARING;

        if (!Check(wsi.CreateSwapChain(*device, desc, swapChain) == nri::Result::SUCCESS, "create swapchain"))
            return false;

        uint32_t count = 0;
        textures = wsi.GetSwapChainTextures(*swapChain, count);
        views.resize(count);
        release.resize(count);
        presented.assign(count, false);
        nri::FenceDesc binary = {};
        binary.initialValue = nri::SWAPCHAIN_SEMAPHORE;

        for (uint32_t i = 0; i < count; i++) {
            const auto& texture = core.GetTextureDesc(*textures[i]);

            if (!Check(texture.width == desc.width && texture.height == desc.height, "resized image extent"))
                return false;

            nri::TextureViewDesc view = {};
            view.texture = textures[i];
            view.type = nri::TextureView::COLOR_ATTACHMENT;
            view.layout = nri::Layout::COLOR_ATTACHMENT;
            view.mipNum = view.layerNum = 1;
            view.planes = nri::PlaneBits::COLOR;

            if (!Check(core.CreateTextureView(view, views[i]) == nri::Result::SUCCESS, "image view") ||
                !Check(core.CreateFence(*device, binary, release[i]) == nri::Result::SUCCESS, "per-image present semaphore"))
                return false;
        }

        const auto& alignment = core.GetDeviceDesc(*device).memoryAlignment;
        uint32_t row = ((uint32_t(desc.width) * 4 + alignment.uploadBufferTextureRow - 1) / alignment.uploadBufferTextureRow) * alignment.uploadBufferTextureRow;
        nri::BufferDesc buffer = {};
        buffer.size = ((row * uint32_t(desc.height) + alignment.uploadBufferTextureSlice - 1) / alignment.uploadBufferTextureSlice) * alignment.uploadBufferTextureSlice;

        return Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_READBACK, 0.0f, buffer, readback) == nri::Result::SUCCESS, "readback buffer");
    }

    bool Frame(uint32_t cycle, uint32_t frame) {
        needsRecreation = false;
        uint32_t index = UINT32_MAX;
        nri::Result result = nri::Result::NOT_READY;

        for (uint32_t attempt = 0; attempt < 5 && result == nri::Result::NOT_READY; attempt++) {
            if (!PumpMessages())
                return false;

            result = wsi.AcquireNextTexture(*swapChain, *acquire, index);

            if (result == nri::Result::NOT_READY) {
                notReady++;

                if (!Check(index == UINT32_MAX, "no-image acquire preserves output"))
                    return false;
            }
        }

        if (result == nri::Result::OUT_OF_DATE) {
            outOfDate++;
            needsRecreation = Check(index == UINT32_MAX, "out-of-date acquire preserves output");

            return false;
        }

        if (!Check(result == nri::Result::SUCCESS && index < views.size(), "acquire drawable image"))
            return false;

        const auto& desc = core.GetTextureDesc(*textures[index]);
        bool bgra = desc.format == nri::Format::BGRA8_UNORM || desc.format == nri::Format::BGRA8_SRGB;

        if (!Check(bgra || desc.format == nri::Format::RGBA8_UNORM || desc.format == nri::Format::RGBA8_SRGB, "8-bit RGBA/BGRA image"))
            return false;

        core.ResetCommandAllocator(*allocator);

        if (!Check(core.BeginCommandBuffer(*command, nullptr) == nri::Result::SUCCESS, "begin frame"))
            return false;

        nri::TextureBarrierDesc transition = {};
        transition.texture = textures[index];
        transition.mipNum = transition.layerNum = 1;
        transition.planes = nri::PlaneBits::COLOR;
        transition.before = {nri::AccessBits::NONE, presented[index] ? nri::Layout::PRESENT : nri::Layout::UNDEFINED, nri::StageBits::NONE};
        transition.after = {nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT};
        nri::BarrierDesc barriers = {};
        barriers.textures = &transition;
        barriers.textureNum = 1;
        core.CmdBarrier(*command, barriers);

        nri::AttachmentDesc color = {};
        color.descriptor = views[index];
        color.loadOp = nri::LoadOp::CLEAR;
        color.storeOp = nri::StoreOp::STORE;
        color.clearValue.color.f = {float(cycle % 2), float(frame % 2), 1.0f, 1.0f};
        nri::RenderingDesc rendering = {};
        rendering.colors = &color;
        rendering.colorNum = 1;
        core.CmdBeginRendering(*command, rendering);
        core.CmdEndRendering(*command);
        transition.before = transition.after;
        transition.after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
        core.CmdBarrier(*command, barriers);

        uint32_t rowAlignment = core.GetDeviceDesc(*device).memoryAlignment.uploadBufferTextureRow;
        uint32_t sliceAlignment = core.GetDeviceDesc(*device).memoryAlignment.uploadBufferTextureSlice;
        nri::TextureDataLayoutDesc data = {};
        data.rowPitch = ((uint32_t(desc.width) * 4 + rowAlignment - 1) / rowAlignment) * rowAlignment;
        data.slicePitch = ((data.rowPitch * uint32_t(desc.height) + sliceAlignment - 1) / sliceAlignment) * sliceAlignment;
        nri::TextureRegionDesc region = {};
        region.planes = nri::PlaneBits::COLOR;
        core.CmdReadbackTextureToBuffer(*command, *readback, data, *textures[index], region);
        transition.before = transition.after;
        transition.after = {nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE};
        core.CmdBarrier(*command, barriers);
        nri::GlobalBarrierDesc host = {};
        host.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        host.after = {nri::AccessBits::HOST_READ, nri::StageBits::HOST};
        nri::BarrierDesc hostBarrier = {};
        hostBarrier.globals = &host;
        hostBarrier.globalNum = 1;
        core.CmdBarrier(*command, hostBarrier);

        if (!Check(core.EndCommandBuffer(*command) == nri::Result::SUCCESS, "end frame"))
            return false;

        nri::FenceSubmitDesc waits[2] = {};
        waits[0] = {acquire, 0, nri::StageBits::ALL};
        waits[1] = {gate, cycle + 1, nri::StageBits::ALL};
        nri::FenceSubmitDesc signals[2] = {};
        signals[0] = {release[index], 0, nri::StageBits::ALL};
        signals[1] = {completed, frames + 1, nri::StageBits::ALL};
        nri::QueueSubmitDesc submit = {};
        submit.commandBuffers = &command;
        submit.commandBufferNum = 1;
        submit.waitFences = waits;
        submit.waitFenceNum = frame == 0 ? 2 : 1;
        submit.signalFences = signals;
        submit.signalFenceNum = 2;

        if (!Check(core.QueueSubmit(*queues[0], submit) == nri::Result::SUCCESS, "submit frame before future producer"))
            return false;

        nri::Result producerResult = nri::Result::SUCCESS;
        std::thread producer;

        if (frame == 0) {
            delayedPresents++;
            producer = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                nri::QueueSubmitDesc signal = {};
                signal.signalFences = &waits[1];
                signal.signalFenceNum = 1;
                producerResult = core.QueueSubmit(*queues[1], signal);
            });
        }

        // Native Vulkan presentation requires every dependency's producer to
        // have been submitted (VUID 03268). The forced scheduler test additionally
        // proves that its host-deferred Present boundary waits for that condition.
        if (!NRI_COMPATIBILITY_DEFERRED_PRESENT && producer.joinable())
            producer.join();

        result = wsi.QueuePresent(*swapChain, *release[index], frames + 1);

        if (producer.joinable())
            producer.join();

        if (result == nri::Result::OUT_OF_DATE) {
            outOfDate++;
            needsRecreation = true;
        }

        if (!Check((result == nri::Result::SUCCESS || needsRecreation) && producerResult == nri::Result::SUCCESS, "present after delayed producer"))
            return false;

        core.Wait(*completed, frames + 1);

        if (!Check(core.GetFenceValue(*completed) >= frames + 1, "frame GPU completion"))
            return false;

        const auto* bytes = (const uint8_t*)core.MapBuffer(*readback, 0, data.slicePitch);

        if (!Check(bytes != nullptr, "map image readback"))
            return false;

        const uint8_t red = uint8_t((cycle % 2) * 255);
        const uint8_t green = uint8_t((frame % 2) * 255);
        const std::array<uint8_t, 4> pixel = {uint8_t(bgra ? 255 : red), green, uint8_t(bgra ? red : 255), 255};
        bool exact = true;

        for (uint32_t y = 0; y < desc.height && exact; y++) {
            for (uint32_t x = 0; x < desc.width; x++) {
                if (std::memcmp(bytes + y * data.rowPitch + x * 4, pixel.data(), 4) != 0) {
                    exact = false;
                    break;
                }
            }
        }

        core.UnmapBuffer(*readback);
        presented[index] = true;
        frames++;

        return Check(exact, "exact presented-image readback") && errors.load() == 0;
    }

    bool DestroyChain() {
        if (!coreReady)
            return true;

        bool idle = core.DeviceWaitIdle(device) == nri::Result::SUCCESS;

        for (auto* view : views) {
            if (view)
                core.DestroyDescriptor(view);
        }

        views.clear();

        for (auto* semaphore : release) {
            if (semaphore)
                core.DestroyFence(semaphore);
        }

        release.clear();

        if (swapChain && wsiReady)
            wsi.DestroySwapChain(swapChain);

        swapChain = nullptr;

        if (readback)
            core.DestroyBuffer(readback);

        readback = nullptr;

        return idle;
    }

    bool Cleanup() {
        bool idle = DestroyChain();

        if (coreReady) {
            if (command)
                core.DestroyCommandBuffer(command);

            if (allocator)
                core.DestroyCommandAllocator(allocator);

            for (auto* fence : {acquire, completed, gate}) {
                if (fence)
                    core.DestroyFence(fence);
            }
        }

        if (device)
            nri::nriDestroyDevice(device);

        if (window)
            DestroyWindow(window);

        UnregisterClassW(L"NRICompatibilityPresentation", GetModuleHandleW(nullptr));

        return idle;
    }
};

} // namespace

int main(int argc, char** argv) {
    bool validation = false;
    uint32_t adapterIndex = 0;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--validation") == 0)
            validation = true;
        else if (std::strcmp(argv[i], "--adapter") == 0 && (i + 1) < argc) {
            char* end = nullptr;
            const char* value = argv[++i];
            errno = 0;
            unsigned long long parsed = std::strtoull(value, &end, 10);

            if (!*value || *end || *value == '-' || errno == ERANGE || parsed > UINT32_MAX)
                return 2;

            adapterIndex = uint32_t(parsed);
        } else
            return 2;
    }

    Fixture fixture;
    bool passed = fixture.Initialize(validation, adapterIndex);

    for (uint32_t cycle = 0; cycle < 100 && passed; cycle++) {
        passed = fixture.CreateChain(cycle);

        for (uint32_t frame = 0; frame < 4 && passed; frame++) {
            passed = fixture.Frame(cycle, frame);

            if (!passed && fixture.needsRecreation)
                passed = fixture.DestroyChain() && fixture.CreateChain(cycle) && fixture.Frame(cycle, frame);

            if (passed && fixture.needsRecreation)
                passed = fixture.DestroyChain() && fixture.CreateChain(cycle);
        }

        passed = fixture.DestroyChain() && passed;

        if (passed)
            fixture.cycles++;

        if ((cycle + 1) % 10 == 0)
            std::fprintf(stderr, "WSI progress: %u recreations, %u exact frames\n", fixture.cycles, fixture.frames);
    }

    passed = fixture.Cleanup() && passed && fixture.errors.load() == 0;

    if (NRI_COMPATIBILITY_ACQUIRE_RESULTS)
        passed = passed && fixture.notReady == 208 && fixture.outOfDate == 4;

    int written = std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_presentation\",\"status\":\"%s\",\"source_commit\":\"%s\",\"compiler\":\"%s\",\"device_id\":%u,\"driver_version_raw\":%u,\"validation_errors\":%u,\"validation_warnings\":%u,\"resize_cycles\":%u,\"exact_frames\":%u,\"no_image_acquires\":%u,\"delayed_present_producers\":%u,\"out_of_date_recoveries\":%u,\"host_deferred_present\":%s,\"acquire_results_injected\":%s,\"legacy_sync_forced\":%s}\n",
        passed ? "passed" : "failed", NRI_COMPATIBILITY_SOURCE_COMMIT, NRI_COMPATIBILITY_COMPILER, fixture.deviceId, fixture.driverVersion, fixture.errors.load(), fixture.warnings.load(), fixture.cycles, fixture.frames, fixture.notReady, fixture.delayedPresents, fixture.outOfDate, NRI_COMPATIBILITY_DEFERRED_PRESENT ? "true" : "false", NRI_COMPATIBILITY_ACQUIRE_RESULTS ? "true" : "false", NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false");

    if (written < 0 || std::fflush(stdout) != 0)
        return 3;

    return passed ? 0 : 1;
}
