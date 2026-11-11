// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRISwapChain.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
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

struct FrameSlot {
    nri::CommandAllocator* allocator = nullptr;
    nri::CommandBuffer* command = nullptr;
    nri::Buffer* readback = nullptr;
    nri::Fence* acquire = nullptr;
    nri::Fence* done = nullptr;
    uint64_t value = 0;
    uint32_t rowPitch = 0, slicePitch = 0, width = 0, height = 0;
    std::array<uint8_t, 4> pixel{};
    bool submitted = false;
};

struct Chain {
    HWND window = nullptr;
    nri::SwapChain* swapChain = nullptr;
    nri::Texture* const* textures = nullptr;
    std::vector<nri::Descriptor*> views;
    std::vector<nri::Fence*> release;
    std::vector<bool> presented;
    std::array<FrameSlot, 3> slots{};
    uint32_t id = 0, generation = 0, frames = 0;
};

struct Fixture {
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::SwapChainInterface wsi = {};
    nri::Queue* queue = nullptr;
    std::array<Chain, 2> chains{};
    std::atomic<uint32_t> errors{0}, warnings{0};
    uint32_t deviceId = 0, driver = 0, submitted = 0, exact = 0, recreations = 0;
    uint32_t maxOutstanding = 0, slotReuses = 0, retainedOtherChain = 0, noImage = 0;
    uint32_t queueIdleChecks = 0, deviceIdleChecks = 0;
    bool ready = false;

    static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* text, void* user) {
        auto& f = *(Fixture*)user;

        if (severity == nri::Message::ERROR)
            f.errors++;
        else if (severity == nri::Message::WARNING)
            f.warnings++;

        std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, text ? text : "");
    }

    static void NRI_CALL NoAbort(void*) {}

    bool Check(bool passed, const char* operation) {
        if (!passed)
            std::fprintf(stderr, "Multiple swapchains failed: %s\n", operation);

        return passed;
    }

    bool Initialize(bool validation) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WindowProcedure;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"NRICompatibilityMultiPresentation";

        if (!Check(RegisterClassW(&wc) != 0, "window class"))
            return false;

        for (uint32_t i = 0; i < chains.size(); i++) {
            auto& chain = chains[i];
            chain.id = i;
            chain.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"NRI multiframe test", WS_OVERLAPPEDWINDOW,
                80 + int(i) * 280, 80, 240, 180, nullptr, nullptr, wc.hInstance, nullptr);

            if (!Check(chain.window != nullptr, "owned window"))
                return false;

            ShowWindow(chain.window, SW_SHOWNOACTIVATE);
        }

        uint32_t count = 0;

        if (!Check(nri::nriEnumerateAdapters(nullptr, count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && count, "adapter count"))
            return false;

        std::vector<nri::AdapterDesc> adapters(count);

        if (!Check(nri::nriEnumerateAdapters(adapters.data(), count, nri::GraphicsAPI::VK) == nri::Result::SUCCESS && count, "adapters"))
            return false;

        nri::QueueFamilyDesc family = {};
        family.queueType = nri::QueueType::GRAPHICS;
        family.queueNum = 1;
        nri::DeviceCreationDesc create = {};
        create.graphicsAPI = nri::GraphicsAPI::VK;
        create.adapterDesc = adapters.data();
        create.queueFamilies = &family;
        create.queueFamilyNum = 1;
        create.disableVKRayTracing = true;
        create.enableNRIValidation = create.enableGraphicsAPIValidation = validation;
        create.callbackInterface.MessageCallback = Message;
        create.callbackInterface.AbortExecution = NoAbort;
        create.callbackInterface.userArg = this;

        if (!Check(nri::nriCreateDevice(create, device) == nri::Result::SUCCESS, "device") ||
            !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core) == nri::Result::SUCCESS, "core"))
            return false;

        ready = true;
        deviceId = core.GetDeviceDesc(*device).adapterDesc.deviceId;
        driver = core.GetDeviceDesc(*device).adapterDesc.driverVersion;

        return Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::SwapChainInterface), &wsi) == nri::Result::SUCCESS, "WSI") &&
            Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, 0, queue) == nri::Result::SUCCESS, "single graphics queue");
    }

    bool CreateChain(Chain& chain) {
        RECT rect = {0, 0, LONG(128 + ((chain.generation + chain.id) % 3) * 32), LONG(96 + (chain.generation % 2) * 32)};
        AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);

        if (!Check(SetWindowPos(chain.window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0 && PumpMessages(), "resize"))
            return false;

        GetClientRect(chain.window, &rect);
        nri::SwapChainDesc desc = {};
        desc.window.windows.hwnd = chain.window;
        desc.queue = queue;
        desc.width = nri::Dim_t(rect.right);
        desc.height = nri::Dim_t(rect.bottom);
        desc.textureNum = 4;
        desc.format = nri::SwapChainFormat::BT709_G22_8BIT;
        desc.flags = (chain.generation % 2) ? nri::SwapChainBits::VSYNC : nri::SwapChainBits::ALLOW_TEARING;

        if (!Check(wsi.CreateSwapChain(*device, desc, chain.swapChain) == nri::Result::SUCCESS, "swapchain"))
            return false;

        uint32_t count = 0;
        chain.textures = wsi.GetSwapChainTextures(*chain.swapChain, count);

        if (!Check(count >= chain.slots.size(), "sufficient images for frame ring"))
            return false;

        chain.views.resize(count);
        chain.release.resize(count);
        chain.presented.assign(count, false);
        nri::FenceDesc binary = {};
        binary.initialValue = nri::SWAPCHAIN_SEMAPHORE;

        for (uint32_t i = 0; i < count; i++) {
            const auto& texture = core.GetTextureDesc(*chain.textures[i]);

            if (!Check(texture.width == desc.width && texture.height == desc.height, "image extent"))
                return false;

            nri::TextureViewDesc view = {};
            view.texture = chain.textures[i];
            view.type = nri::TextureView::COLOR_ATTACHMENT;
            view.layout = nri::Layout::COLOR_ATTACHMENT;
            view.mipNum = view.layerNum = 1;
            view.planes = nri::PlaneBits::COLOR;

            if (!Check(core.CreateTextureView(view, chain.views[i]) == nri::Result::SUCCESS, "view") ||
                !Check(core.CreateFence(*device, binary, chain.release[i]) == nri::Result::SUCCESS, "per-image semaphore"))
                return false;
        }

        const auto& alignment = core.GetDeviceDesc(*device).memoryAlignment;
        uint32_t row = ((uint32_t(desc.width) * 4 + alignment.uploadBufferTextureRow - 1) / alignment.uploadBufferTextureRow) * alignment.uploadBufferTextureRow;
        nri::BufferDesc buffer = {};
        buffer.size = ((row * uint32_t(desc.height) + alignment.uploadBufferTextureSlice - 1) / alignment.uploadBufferTextureSlice) * alignment.uploadBufferTextureSlice;

        for (auto& slot : chain.slots) {
            if (!Check(core.CreateCommandAllocator(*queue, slot.allocator) == nri::Result::SUCCESS, "allocator") ||
                !Check(core.CreateCommandBuffer(*slot.allocator, slot.command) == nri::Result::SUCCESS, "command") ||
                !Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_READBACK, 0, buffer, slot.readback) == nri::Result::SUCCESS, "readback") ||
                !Check(core.CreateFence(*device, binary, slot.acquire) == nri::Result::SUCCESS, "acquire") ||
                !Check(core.CreateFence(*device, nri::FenceDesc{}, slot.done) == nri::Result::SUCCESS, "completion"))
                return false;
        }

        return true;
    }

    bool Complete(FrameSlot& slot) {
        if (!slot.submitted)
            return true;

        core.Wait(*slot.done, slot.value);

        if (!Check(core.GetFenceValue(*slot.done) >= slot.value, "slot GPU completion"))
            return false;

        const auto* bytes = (const uint8_t*)core.MapBuffer(*slot.readback, 0, slot.slicePitch);

        if (!Check(bytes != nullptr, "readback map"))
            return false;

        bool matches = true;

        for (uint32_t y = 0; y < slot.height && matches; y++) {
            for (uint32_t x = 0; x < slot.width; x++) {
                if (std::memcmp(bytes + y * slot.rowPitch + x * 4, slot.pixel.data(), 4) != 0) {
                    matches = false;
                    break;
                }
            }
        }

        core.UnmapBuffer(*slot.readback);
        slot.submitted = false;

        if (!Check(matches, "exact slot-specific image output"))
            return false;

        exact++;

        return true;
    }

    bool Frame(Chain& chain) {
        auto& slot = chain.slots[chain.frames % chain.slots.size()];

        if (!Complete(slot))
            return false;

        slotReuses += slot.value != 0;
        uint32_t index = UINT32_MAX;
        nri::Result result = nri::Result::NOT_READY;

        for (uint32_t attempt = 0; attempt < 5 && result == nri::Result::NOT_READY; attempt++) {
            if (!PumpMessages())
                return false;

            result = wsi.AcquireNextTexture(*chain.swapChain, *slot.acquire, index);
            noImage += result == nri::Result::NOT_READY;
        }

        // This fixture resizes only after retirement. Unexpected surface loss is
        // reported rather than discarding frames from its exact output accounting.
        if (!Check(result == nri::Result::SUCCESS && index < chain.views.size(), "acquire"))
            return false;

        const auto& desc = core.GetTextureDesc(*chain.textures[index]);
        bool bgra = desc.format == nri::Format::BGRA8_UNORM || desc.format == nri::Format::BGRA8_SRGB;

        if (!Check(bgra || desc.format == nri::Format::RGBA8_UNORM || desc.format == nri::Format::RGBA8_SRGB, "8-bit RGBA/BGRA image"))
            return false;

        core.ResetCommandAllocator(*slot.allocator);

        if (!Check(core.BeginCommandBuffer(*slot.command, nullptr) == nri::Result::SUCCESS, "begin frame"))
            return false;

        nri::TextureBarrierDesc transition = {};
        transition.texture = chain.textures[index];
        transition.mipNum = transition.layerNum = 1;
        transition.planes = nri::PlaneBits::COLOR;
        transition.before = {nri::AccessBits::NONE, chain.presented[index] ? nri::Layout::PRESENT : nri::Layout::UNDEFINED, nri::StageBits::NONE};
        transition.after = {nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT, nri::StageBits::COLOR_ATTACHMENT};
        nri::BarrierDesc barriers = {};
        barriers.textures = &transition;
        barriers.textureNum = 1;
        core.CmdBarrier(*slot.command, barriers);

        nri::AttachmentDesc color = {};
        color.descriptor = chain.views[index];
        color.loadOp = nri::LoadOp::CLEAR;
        color.storeOp = nri::StoreOp::STORE;
        color.clearValue.color.f = {float(chain.generation % 2), float(chain.frames % 2), float(chain.id), 1.0f};
        nri::RenderingDesc rendering = {};
        rendering.colors = &color;
        rendering.colorNum = 1;
        core.CmdBeginRendering(*slot.command, rendering);
        core.CmdEndRendering(*slot.command);
        transition.before = transition.after;
        transition.after = {nri::AccessBits::COPY_SOURCE, nri::Layout::COPY_SOURCE, nri::StageBits::COPY};
        core.CmdBarrier(*slot.command, barriers);

        uint32_t rowAlignment = core.GetDeviceDesc(*device).memoryAlignment.uploadBufferTextureRow;
        uint32_t sliceAlignment = core.GetDeviceDesc(*device).memoryAlignment.uploadBufferTextureSlice;
        nri::TextureDataLayoutDesc data = {};
        data.rowPitch = ((uint32_t(desc.width) * 4 + rowAlignment - 1) / rowAlignment) * rowAlignment;
        data.slicePitch = ((data.rowPitch * uint32_t(desc.height) + sliceAlignment - 1) / sliceAlignment) * sliceAlignment;
        nri::TextureRegionDesc region = {};
        region.planes = nri::PlaneBits::COLOR;
        core.CmdReadbackTextureToBuffer(*slot.command, *slot.readback, data, *chain.textures[index], region);
        transition.before = transition.after;
        transition.after = {nri::AccessBits::NONE, nri::Layout::PRESENT, nri::StageBits::NONE};
        core.CmdBarrier(*slot.command, barriers);
        nri::GlobalBarrierDesc host = {};
        host.before = {nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        host.after = {nri::AccessBits::HOST_READ, nri::StageBits::HOST};
        nri::BarrierDesc hostBarrier = {};
        hostBarrier.globals = &host;
        hostBarrier.globalNum = 1;
        core.CmdBarrier(*slot.command, hostBarrier);

        if (!Check(core.EndCommandBuffer(*slot.command) == nri::Result::SUCCESS, "end frame"))
            return false;

        slot.rowPitch = data.rowPitch;
        slot.slicePitch = data.slicePitch;
        slot.width = desc.width;
        slot.height = desc.height;
        uint8_t red = uint8_t((chain.generation % 2) * 255);
        uint8_t green = uint8_t((chain.frames % 2) * 255);
        uint8_t blue = uint8_t(chain.id * 255);
        slot.pixel = {bgra ? blue : red, green, bgra ? red : blue, 255};
        nri::FenceSubmitDesc wait = {slot.acquire, 0, nri::StageBits::ALL};
        nri::FenceSubmitDesc signals[] = {{chain.release[index], 0, nri::StageBits::ALL}, {slot.done, slot.value + 1, nri::StageBits::ALL}};
        nri::QueueSubmitDesc submit = {};
        submit.waitFences = &wait;
        submit.waitFenceNum = 1;
        submit.signalFences = signals;
        submit.signalFenceNum = 2;
        submit.commandBuffers = &slot.command;
        submit.commandBufferNum = 1;

        if (!Check(core.QueueSubmit(*queue, submit) == nri::Result::SUCCESS, "frame submit"))
            return false;

        slot.value++;
        slot.submitted = true;
        submitted++;
        chain.frames++;
        chain.presented[index] = true;
        maxOutstanding = std::max(maxOutstanding, submitted - exact);

        return Check(wsi.QueuePresent(*chain.swapChain, *chain.release[index], chain.frames) == nri::Result::SUCCESS, "frame present") && errors.load() == 0;
    }

    bool DestroyChain(Chain& chain) {
        if (!ready)
            return true;

        // Retirement of this chain must also account for all pending presents
        // on the shared queue, including the other still-live chain.
        bool idle = true;

        if (chain.swapChain) {
            if (chain.generation % 2) {
                idle = core.DeviceWaitIdle(device) == nri::Result::SUCCESS;
                deviceIdleChecks++;
            } else {
                idle = core.QueueWaitIdle(queue) == nri::Result::SUCCESS;
                queueIdleChecks++;
            }
        }

        if (!Check(idle, "retirement"))
            return false;

        bool passed = true;

        for (auto& slot : chain.slots) {
            passed = Complete(slot) && passed;

            if (slot.command)
                core.DestroyCommandBuffer(slot.command);

            if (slot.allocator)
                core.DestroyCommandAllocator(slot.allocator);

            if (slot.readback)
                core.DestroyBuffer(slot.readback);

            for (auto* fence : {slot.acquire, slot.done}) {
                if (fence)
                    core.DestroyFence(fence);
            }

            slot = {};
        }

        for (auto* view : chain.views) {
            if (view)
                core.DestroyDescriptor(view);
        }

        for (auto* fence : chain.release) {
            if (fence)
                core.DestroyFence(fence);
        }

        chain.views.clear();
        chain.release.clear();

        if (chain.swapChain)
            wsi.DestroySwapChain(chain.swapChain);

        chain.swapChain = nullptr;
        chain.textures = nullptr;

        return passed;
    }

    bool Run() {
        for (auto& chain : chains) {
            if (!CreateChain(chain))
                return false;
        }

        for (uint32_t cycle = 0; cycle < 100; cycle++) {
            for (uint32_t frame = 0; frame < 4; frame++) {
                for (auto& chain : chains) {
                    if (!Frame(chain))
                        return false;
                }
            }

            auto& retiring = chains[cycle % 2];
            auto& retained = chains[(cycle + 1) % 2];
            nri::SwapChain* retainedHandle = retained.swapChain;
            uint32_t retainedFrames = retained.frames;

            if (!DestroyChain(retiring))
                return false;

            retiring.generation++;

            if (!CreateChain(retiring) || !Check(retained.swapChain == retainedHandle && retained.frames == retainedFrames, "other swapchain retained"))
                return false;

            retainedOtherChain++;
            recreations++;

            if ((cycle + 1) % 10 == 0)
                std::fprintf(stderr, "Multiple WSI progress: %u recreations, %u submissions, %u verified frames\n", recreations, submitted, exact);
        }

        return true;
    }

    bool Cleanup() {
        bool passed = true;

        for (auto& chain : chains) {
            if (!DestroyChain(chain))
                passed = false;
        }

        if (!passed)
            return false; // Retain native state until owned process exit after failed retirement.

        if (device)
            nri::nriDestroyDevice(device);

        for (auto& chain : chains) {
            if (chain.window)
                DestroyWindow(chain.window);
        }

        UnregisterClassW(L"NRICompatibilityMultiPresentation", GetModuleHandleW(nullptr));

        return true;
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
    bool passed = fixture.Initialize(validation) && fixture.Run();
    passed = fixture.Cleanup() && passed && fixture.errors.load() == 0 && fixture.warnings.load() == 0;
    passed = passed && fixture.submitted == 800 && fixture.exact == 800 && fixture.recreations == 100 && fixture.retainedOtherChain == 100 && fixture.maxOutstanding == 6 && fixture.slotReuses > 0;
    std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_multi_presentation\",\"status\":\"%s\",\"source_commit\":\"%s\",\"compiler\":\"%s\",\"device_id\":%u,\"driver_version_raw\":%u,\"validation_errors\":%u,\"validation_warnings\":%u,\"submitted_frames\":%u,\"exact_frames\":%u,\"resize_cycles\":%u,\"retained_other_chain\":%u,\"max_outstanding_slots\":%u,\"slot_reuses\":%u,\"no_image_acquires\":%u,\"queue_idle_checks\":%u,\"device_idle_checks\":%u,\"legacy_sync_forced\":%s}\n",
        passed ? "passed" : "failed", NRI_COMPATIBILITY_SOURCE_COMMIT, NRI_COMPATIBILITY_COMPILER, fixture.deviceId, fixture.driver, fixture.errors.load(), fixture.warnings.load(), fixture.submitted, fixture.exact, fixture.recreations, fixture.retainedOtherChain, fixture.maxOutstanding, fixture.slotReuses, fixture.noImage, fixture.queueIdleChecks, fixture.deviceIdleChecks, NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false");

    return std::fflush(stdout) == 0 && passed ? 0 : 1;
}
