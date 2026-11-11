// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "Extensions/NRIDrawCapabilitiesVK.h"

// Compile and invoke the real C facade, including its reference-to-pointer ABI.
const void* QueryDrawCapabilitiesFromC(const void* device) {
    NriDrawCapabilitiesVKInterface query = {0};

    if (nriGetInterface((const NriDevice*)device, NRI_INTERFACE(NriDrawCapabilitiesVKInterface), &query) != NriResult_SUCCESS)
        return NULL;

    return query.GetDrawCapabilitiesVK((const NriDevice*)device);
}
