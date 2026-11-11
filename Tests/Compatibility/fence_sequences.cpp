// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT

#include "NRI.h"
#include "Extensions/NRIDeviceCreation.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Diagnostics {
    std::atomic<uint32_t> errors{0};
    std::atomic<uint32_t> warnings{0};
};

static void NRI_CALL Message(nri::Message severity, const char* file, uint32_t line, const char* message, void* user) {
    auto& diagnostics = *static_cast<Diagnostics*>(user);

    if (severity == nri::Message::ERROR)
        ++diagnostics.errors;
    else if (severity == nri::Message::WARNING)
        ++diagnostics.warnings;

    std::fprintf(stderr, "[NRI %u] %s:%u: %s\n", uint32_t(severity), file ? file : "", line, message ? message : "");
}

static void NRI_CALL DoNotAbort(void*) {
    // Report failures through the fixture result instead of an interactive debugger.
}

static std::string JsonString(const char* input) {
    std::string result = "\"";

    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; ++p) {
        if (*p == '\\' || *p == '"') {
            result += '\\';
            result += char(*p);
        } else if (*p < 32) {
            char escaped[7] = {};
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", unsigned(*p));
            result += escaped;
        } else
            result += char(*p);
    }
    result += '"';

    return result;
}

struct Fixture {
    static constexpr uint32_t BYTE_COUNT = 4096;
    Diagnostics diagnostics;
    nri::Device* device = nullptr;
    nri::CoreInterface core = {};
    nri::AdapterDesc adapter = {};
    nri::Queue* queues[2] = {};
    nri::CommandAllocator* allocators[2] = {};
    nri::CommandBuffer* producer = nullptr;
    nri::CommandBuffer* consumers[2] = {};
    nri::Buffer* upload = nullptr;
    nri::Buffer* storage = nullptr;
    nri::Buffer* readbacks[2] = {};
    nri::Buffer* timestamps = nullptr;
    nri::QueryPool* queries = nullptr;
    nri::Fence* gate = nullptr;
    nri::Fence* done[2] = {};
    bool coreReady = false;
    bool futureAccepted = false;
    bool gateSignaled = false;
    bool exactBytes = false;
    bool queriesExact = false;
    bool partialResetExact = false;
    uint32_t retirementCycles = 0;
    bool repeatedWaits = false;
    double hostResetSeconds = 0.0;
    std::string failure;

    bool Check(nri::Result result, const char* operation) {
        if (result == nri::Result::SUCCESS)
            return true;

        failure = std::string(operation) + " returned " + std::to_string(int(result));

        return false;
    }

    void Barrier(nri::CommandBuffer& command, nri::Buffer& buffer, nri::AccessBits before, nri::AccessBits after) {
        nri::BufferBarrierDesc transition = {};
        transition.buffer = &buffer;
        transition.before = {before, before == nri::AccessBits::NONE ? nri::StageBits::NONE : nri::StageBits::COPY};
        transition.after = {after, nri::StageBits::COPY};
        nri::BarrierDesc barriers = {};
        barriers.buffers = &transition;
        barriers.bufferNum = 1;
        core.CmdBarrier(command, barriers);
    }

    void HostBarrier(nri::CommandBuffer& command, bool uploadBarrier) {
        nri::GlobalBarrierDesc global = {};
        global.before = uploadBarrier ? nri::AccessStage{nri::AccessBits::HOST_WRITE, nri::StageBits::HOST} : nri::AccessStage{nri::AccessBits::COPY_DESTINATION, nri::StageBits::COPY};
        global.after = uploadBarrier ? nri::AccessStage{nri::AccessBits::COPY_SOURCE, nri::StageBits::COPY} : nri::AccessStage{nri::AccessBits::HOST_READ, nri::StageBits::HOST};
        nri::BarrierDesc barriers = {};
        barriers.globals = &global;
        barriers.globalNum = 1;
        core.CmdBarrier(command, barriers);
    }

    bool Run(bool validation, uint32_t adapterIndex) {
        uint32_t count = 0;

        if (!Check(nri::nriEnumerateAdapters(nullptr, count, nri::GraphicsAPI::VK), "EnumerateAdapters") || adapterIndex >= count)
            return false;

        std::vector<nri::AdapterDesc> adapters(count);

        if (!Check(nri::nriEnumerateAdapters(adapters.data(), count, nri::GraphicsAPI::VK), "EnumerateAdapters(list)") || adapterIndex >= count)
            return false;

        adapter = adapters[adapterIndex];

        if (adapter.queueNum[size_t(nri::QueueType::GRAPHICS)] < 2) {
            failure = "This fixture requires two distinct graphics queues";

            return false;
        }

        nri::QueueFamilyDesc family = {};
        family.queueType = nri::QueueType::GRAPHICS;
        family.queueNum = 2;
        nri::DeviceCreationDesc create = {};
        create.graphicsAPI = nri::GraphicsAPI::VK;
        create.adapterDesc = &adapter;
        create.queueFamilies = &family;
        create.queueFamilyNum = 1;
        create.enableNRIValidation = validation;
        create.enableGraphicsAPIValidation = validation;
        create.disableVKRayTracing = true;
        create.callbackInterface.MessageCallback = Message;
        create.callbackInterface.AbortExecution = DoNotAbort;
        create.callbackInterface.userArg = &diagnostics;

        if (!Check(nri::nriCreateDevice(create, device), "CreateDevice") || !Check(nri::nriGetInterface(*device, NRI_INTERFACE(nri::CoreInterface), &core), "CoreInterface"))
            return false;

        coreReady = true;
        adapter = core.GetDeviceDesc(*device).adapterDesc;

        for (uint32_t i = 0; i < 2; i++) {
            if (!Check(core.GetQueue(*device, nri::QueueType::GRAPHICS, i, queues[i]), "GetQueue") ||
                !Check(core.CreateCommandAllocator(*queues[i], allocators[i]), "CreateCommandAllocator") ||
                !Check(core.CreateCommandBuffer(*allocators[i], consumers[i]), "CreateConsumer") ||
                !Check(core.CreateFence(*device, nri::FenceDesc{}, done[i]), "CreateCompletionFence"))
                return false;
        }

        nri::FenceDesc gateDesc = {};
        gateDesc.initialValue = 5;
        nri::BufferDesc bufferDesc = {};
        bufferDesc.size = BYTE_COUNT;

        if (!Check(core.CreateFence(*device, gateDesc, gate), "CreateGate") ||
            !Check(core.CreateCommandBuffer(*allocators[1], producer), "CreateProducer") ||
            !Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_UPLOAD, 0.0f, bufferDesc, upload), "CreateUpload") ||
            !Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::DEVICE, 0.0f, bufferDesc, storage), "CreateStorage"))
            return false;

        for (nri::Buffer*& readback : readbacks) {
            if (!Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_READBACK, 0.0f, bufferDesc, readback), "CreateReadback"))
                return false;
        }

        nri::QueryPoolDesc queryDesc = {};
        queryDesc.queryType = nri::QueryType::TIMESTAMP;
        queryDesc.capacity = 2;
        bufferDesc.size = 2 * sizeof(uint64_t);

        if (!Check(core.CreateQueryPool(*device, queryDesc, queries), "CreateQueryPool") ||
            !Check(core.CreateCommittedBuffer(*device, nri::MemoryLocation::HOST_READBACK, 0.0f, bufferDesc, timestamps), "CreateTimestampReadback"))
            return false;

        std::array<uint8_t, BYTE_COUNT> expected = {};

        for (uint32_t i = 0; i < BYTE_COUNT; i++)
            expected[i] = uint8_t((i * 41u) ^ (i >> 5u) ^ 0x6du);

        void* mapped = core.MapBuffer(*upload, 0, BYTE_COUNT);

        if (!mapped)
            return false;

        std::memcpy(mapped, expected.data(), BYTE_COUNT);
        core.UnmapBuffer(*upload);

        // Record before the host reset. Reset must affect already-recorded,
        // not-yet-submitted query commands as well as newly recorded commands.
        if (!Check(core.BeginCommandBuffer(*producer, nullptr), "BeginProducer"))
            return false;

        core.CmdEndQuery(*producer, *queries, 0);
        HostBarrier(*producer, true);
        Barrier(*producer, *storage, nri::AccessBits::NONE, nri::AccessBits::COPY_DESTINATION);
        core.CmdCopyBuffer(*producer, *storage, 0, *upload, 0, BYTE_COUNT);
        Barrier(*producer, *storage, nri::AccessBits::COPY_DESTINATION, nri::AccessBits::COPY_SOURCE);
        core.CmdEndQuery(*producer, *queries, 1);
        Barrier(*producer, *timestamps, nri::AccessBits::NONE, nri::AccessBits::COPY_DESTINATION);
        core.CmdCopyQueries(*producer, *queries, 0, 2, *timestamps, 0);
        HostBarrier(*producer, false);

        if (!Check(core.EndCommandBuffer(*producer), "EndProducer"))
            return false;

        for (uint32_t i = 0; i < 2; i++) {
            if (!Check(core.BeginCommandBuffer(*consumers[i], nullptr), "BeginConsumer"))
                return false;

            Barrier(*consumers[i], *readbacks[i], nri::AccessBits::NONE, nri::AccessBits::COPY_DESTINATION);
            core.CmdCopyBuffer(*consumers[i], *readbacks[i], 0, *storage, 0, BYTE_COUNT);
            HostBarrier(*consumers[i], false);

            if (!Check(core.EndCommandBuffer(*consumers[i]), "EndConsumer"))
                return false;
        }

        nri::FenceSubmitDesc wait = {};
        wait.fence = gate;
        wait.value = 7; // The later signal jumps from 5 to 9 and must satisfy 7.
        wait.stages = nri::StageBits::ALL;
        nri::FenceSubmitDesc signal = {};
        signal.fence = done[0];
        signal.value = 1;
        signal.stages = nri::StageBits::ALL;
        const nri::CommandBuffer* command = consumers[0];
        nri::QueueSubmitDesc submit = {};
        submit.commandBuffers = &command;
        submit.commandBufferNum = 1;
        submit.waitFences = &wait;
        submit.waitFenceNum = 1;
        submit.signalFences = &signal;
        submit.signalFenceNum = 1;

        if (!Check(core.QueueSubmit(*queues[0], submit), "SubmitFutureWait"))
            return false;

        futureAccepted = true;

        if (core.GetFenceValue(*done[0]) != 0 || core.GetFenceValue(*gate) != 5) {
            failure = "An unresolved queued wait was reported complete";

            return false;
        }

        // Queue0 is blocked on a future signal. No submitted command uses the
        // queries. Native host reset is legal here and cannot depend on queue0.
        const auto resetStarted = std::chrono::steady_clock::now();
        core.ResetQueries(*queries, 0, 1);
        core.ResetQueries(*queries, 1, 1);
        hostResetSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - resetStarted).count();

        command = producer;
        signal.fence = gate;
        signal.value = 9;
        submit.waitFences = nullptr;
        submit.waitFenceNum = 0;

        if (!Check(core.QueueSubmit(*queues[1], submit), "SubmitFutureProducer"))
            return false;

        gateSignaled = true;
        command = consumers[1];
        signal.fence = done[1];
        signal.value = 1;
        submit.waitFences = &wait;
        submit.waitFenceNum = 1;

        if (!Check(core.QueueSubmit(*queues[1], submit), "SubmitFanoutConsumer"))
            return false;

        // Concurrent and repeated host waits must not consume a binary payload.
        std::thread first([&] { core.Wait(*done[0], 1); });
        std::thread second([&] { core.Wait(*done[0], 1); });
        core.Wait(*done[1], 1);
        core.Wait(*gate, 7);
        first.join();
        second.join();
        core.Wait(*done[0], 1);
        core.Wait(*gate, 5);
        repeatedWaits = core.GetFenceValue(*done[0]) == 1 && core.GetFenceValue(*done[1]) == 1 && core.GetFenceValue(*gate) == 9;
        exactBytes = true;

        for (nri::Buffer* readback : readbacks) {
            mapped = core.MapBuffer(*readback, 0, BYTE_COUNT);

            if (!mapped)
                return false;

            exactBytes &= std::memcmp(mapped, expected.data(), BYTE_COUNT) == 0;
            core.UnmapBuffer(*readback);
        }

        mapped = core.MapBuffer(*timestamps, 0, 2 * sizeof(uint64_t));

        if (!mapped)
            return false;

        uint64_t values[2] = {};
        std::memcpy(values, mapped, sizeof(values));
        core.UnmapBuffer(*timestamps);
        queriesExact = values[0] != 0 && values[1] >= values[0];

        // Reset only query0 after both consumers complete. Query1 must retain
        // its previous result; replacing/resetting the whole pool is incorrect.
        core.ResetCommandAllocator(*allocators[1]);

        if (!Check(core.BeginCommandBuffer(*producer, nullptr), "BeginPartialReset"))
            return false;

        core.CmdEndQuery(*producer, *queries, 0);
        Barrier(*producer, *timestamps, nri::AccessBits::COPY_DESTINATION, nri::AccessBits::COPY_DESTINATION);
        core.CmdCopyQueries(*producer, *queries, 0, 2, *timestamps, 0);
        HostBarrier(*producer, false);

        if (!Check(core.EndCommandBuffer(*producer), "EndPartialReset"))
            return false;

        core.ResetQueries(*queries, 0, 1);
        command = producer;
        signal.value = 2;
        submit.waitFenceNum = 0;
        submit.waitFences = nullptr;

        if (!Check(core.QueueSubmit(*queues[1], submit), "SubmitPartialReset"))
            return false;

        core.Wait(*done[1], 2);
        mapped = core.MapBuffer(*timestamps, 0, sizeof(values));

        if (!mapped)
            return false;

        uint64_t afterReset[2] = {};
        std::memcpy(afterReset, mapped, sizeof(afterReset));
        core.UnmapBuffer(*timestamps);
        partialResetExact = afterReset[0] >= values[1] && afterReset[1] == values[1];

        // Move one logical fence between physical queues repeatedly. Each wait
        // for an older value must remain a reusable dependency after compaction.
        submit.commandBuffers = nullptr;
        submit.commandBufferNum = 0;
        wait.fence = done[1];
        signal.fence = done[1];
        submit.waitFences = &wait;
        submit.waitFenceNum = 1;

        for (uint32_t cycle = 0; cycle < 128; cycle++) {
            wait.value = 2 + cycle;
            signal.value = 3 + cycle;

            if (!Check(core.QueueSubmit(*queues[cycle % 2], submit), "SubmitRetirementCycle"))
                return false;

            core.Wait(*done[1], signal.value);
            core.Wait(*done[1], 1);

            if (core.GetFenceValue(*done[1]) != signal.value) {
                failure = "Retirement cycle did not complete its signal";

                return false;
            }

            retirementCycles++;

            if ((retirementCycles % 16) == 0 && !Check(core.DeviceWaitIdle(device), "IdleRetirementSample"))
                return false;
        }

        if (!exactBytes || !queriesExact || !repeatedWaits || !partialResetExact)
            failure = "Future wait, fanout, query reset or repeated completion mismatch";

        return exactBytes && queriesExact && repeatedWaits && partialResetExact;
    }

    bool Cleanup() {
        bool idle = true;

        if (coreReady) {
            // Release only this fixture's unresolved queue on an early failure.
            if (futureAccepted && !gateSignaled) {
                nri::FenceSubmitDesc signal = {};
                signal.fence = gate;
                signal.value = 9;
                signal.stages = nri::StageBits::ALL;
                nri::QueueSubmitDesc submit = {};
                submit.signalFences = &signal;
                submit.signalFenceNum = 1;
                core.QueueSubmit(*queues[1], submit);
            }

            idle = core.DeviceWaitIdle(device) == nri::Result::SUCCESS;

            if (producer)
                core.DestroyCommandBuffer(producer);

            for (uint32_t i = 0; i < 2; i++) {
                if (consumers[i])
                    core.DestroyCommandBuffer(consumers[i]);

                if (allocators[i])
                    core.DestroyCommandAllocator(allocators[i]);

                if (readbacks[i])
                    core.DestroyBuffer(readbacks[i]);

                if (done[i])
                    core.DestroyFence(done[i]);
            }

            if (gate)
                core.DestroyFence(gate);

            if (queries)
                core.DestroyQueryPool(queries);

            if (timestamps)
                core.DestroyBuffer(timestamps);

            if (storage)
                core.DestroyBuffer(storage);

            if (upload)
                core.DestroyBuffer(upload);
        }

        if (device)
            nri::nriDestroyDevice(device);

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
            const unsigned long long parsed = std::strtoull(value, &end, 10);

            if (!*value || *end || *value == '-' || errno == ERANGE || parsed > UINT32_MAX)
                return 2;

            adapterIndex = uint32_t(parsed);
        } else
            return 2;
    }

    Fixture fixture;
    bool passed = fixture.Run(validation, adapterIndex);
    passed = fixture.Cleanup() && passed;
    passed = passed && fixture.diagnostics.errors.load() == 0;
    const int written = std::printf("{\"schema_version\":1,\"fixture\":\"nri_vulkan_fence_sequences\",\"status\":\"%s\",\"source_commit\":%s,\"compiler\":%s,\"adapter_name\":%s,\"device_id\":%u,\"driver_version_raw\":%u,\"validation_errors\":%u,\"validation_warnings\":%u,\"future_wait_accepted\":%s,\"fanout_exact_bytes\":%s,\"host_reset_queries_exact\":%s,\"repeated_concurrent_waits\":%s,\"partial_reset_exact\":%s,\"retirement_cycles\":%u,\"host_reset_seconds\":%.6f,\"legacy_sync_forced\":%s,\"failure\":%s}\n",
        passed ? "passed" : "failed", JsonString(NRI_COMPATIBILITY_SOURCE_COMMIT).c_str(), JsonString(NRI_COMPATIBILITY_COMPILER).c_str(), JsonString(fixture.adapter.name).c_str(), fixture.adapter.deviceId, fixture.adapter.driverVersion,
        fixture.diagnostics.errors.load(), fixture.diagnostics.warnings.load(), fixture.futureAccepted ? "true" : "false", fixture.exactBytes ? "true" : "false", fixture.queriesExact ? "true" : "false", fixture.repeatedWaits ? "true" : "false", fixture.partialResetExact ? "true" : "false", fixture.retirementCycles, fixture.hostResetSeconds,
        NRI_COMPATIBILITY_FORCE_LEGACY_SYNC ? "true" : "false", JsonString(fixture.failure.c_str()).c_str());

    if (written < 0 || std::fflush(stdout) != 0)
        return 3;

    return passed ? 0 : 1;
}
