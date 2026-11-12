// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include "Extensions/NRIDrawCapabilitiesVK.h"

typedef char FormatSupportAbiWidth[(sizeof(NriFormatSupportBits) == sizeof(uint32_t)) ? 1 : -1];
typedef char FormatSupportLinearBit[(NriFormatSupportBits_TEXTURE_FILTER_LINEAR == (1u << 17)) ? 1 : -1];

// Compile and invoke the real C facade, including its reference-to-pointer ABI.
const void* QueryDrawCapabilitiesFromC(const void* device) {
    NriDrawCapabilitiesVKInterface query = {0};

    if (nriGetInterface((const NriDevice*)device, NRI_INTERFACE(NriDrawCapabilitiesVKInterface), &query) != NriResult_SUCCESS)
        return NULL;

    return query.GetDrawCapabilitiesVK((const NriDevice*)device);
}
