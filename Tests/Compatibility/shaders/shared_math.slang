// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
public uint mixValue(uint value) {
    value ^= value >> 16;
    return value * 0x85ebca6bu;
}
