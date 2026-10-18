// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
#pragma once

#include <cstdint>
#include <cstddef>

#include "ultramodern/ultra64.h"

// High-level native audio DSP interpreter for DKR (ABI 1 / aspMain microcode).
// Replaces instruction-by-instruction RSP scalar emulation with native C DSP routines,
// preserving 100% original audio fidelity, all 40 voices, and full BIGROOM reverb.
//
// Returns true if the audio command list was fully executed.
// Returns false if an unknown/unsupported command occurred, allowing seamless fallback to dkrAspMain.
bool dkr_native_audio_process(std::uint8_t* rdram, const OSTask* task);
