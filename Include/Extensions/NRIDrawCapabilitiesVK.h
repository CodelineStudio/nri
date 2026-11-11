// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#pragma once
#include "../NRI.h"

NriNamespaceBegin

// Codeline fork extension. Actual enabled features, not physical-device support.
// Existing DeviceDesc and interface layouts are unchanged.
NriStruct(DrawCapabilitiesVKDesc) {
    bool multiDrawIndirect;
    bool drawIndirectFirstInstance;
    bool shaderDrawParameters;
    bool drawIndirectCount;
};

// Threadsafe: yes. Only available for NRI-created Vulkan devices.
// Wrapped devices do not provide their enabled feature chain: UNSUPPORTED.
// Returned immutable data remains valid until the device is destroyed.
NriStruct(DrawCapabilitiesVKInterface) {
    const NriRef(DrawCapabilitiesVKDesc) (NRI_CALL *GetDrawCapabilitiesVK)(const NriRef(Device) device);
};

NriNamespaceEnd
