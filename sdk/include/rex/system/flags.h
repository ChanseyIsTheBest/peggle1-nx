/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, headless);
REXCVAR_DECLARE(bool, log_high_frequency_kernel_calls);
REXCVAR_DECLARE(uint32_t, license_mask);
REXCVAR_DECLARE(uint32_t, user_country);
REXCVAR_DECLARE(uint32_t, user_language);
REXCVAR_DECLARE(std::string, language);  // blur-nx (2026-11-18): xam_user.cpp
namespace rex::system {
// blur-nx (2026-11-18): the language the game is told (XGetLanguage, XCONFIG_USER_LANGUAGE, the achievements' strings):
// the language key ("System" = the console's own, mapped to the Xbox 360's list), else user_language. Taken once.
uint32_t EffectiveLanguage();
}  // namespace rex::system
REXCVAR_DECLARE(bool, kernel_pix);
REXCVAR_DECLARE(std::string, cl);
REXCVAR_DECLARE(bool, kernel_debug_monitor);
REXCVAR_DECLARE(bool, kernel_cert_monitor);
REXCVAR_DECLARE(bool, ignore_thread_priorities);
REXCVAR_DECLARE(bool, ignore_thread_affinities);
REXCVAR_DECLARE(bool, writable_executable_memory);
REXCVAR_DECLARE(bool, protect_zero);
REXCVAR_DECLARE(bool, protect_on_release);
REXCVAR_DECLARE(bool, scribble_heap);
