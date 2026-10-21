/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#include <atomic>
#include <chrono>

#include <rex/cvar.h>

// peggle2-nx (2026-10-20): which users the game asks for its controllers' state (local multiplayer: player 2 had no
// control in a duel on the console). Off by default (a diagnostic).
REXCVAR_DEFINE_BOOL(input_diag_users, false, "Input",
                    "Diagnostic: every 10 s, XamInputGetState's calls by the user asked for (0-3, any) with how "
                    "many found a controller and how many had a button or trigger held");

#pragma GCC diagnostic ignored "-Wunused-parameter"

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;

using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr uint32_t XINPUT_FLAG_GAMEPAD = 0x01;
constexpr uint32_t XINPUT_FLAG_ANY_USER = 1 << 30;

rex::input::InputSystem* input_system() {
  return static_cast<rex::input::InputSystem*>(REX_KERNEL_STATE()->emulator()->input_system());
}

void XamResetInactivity_entry() {
  // Do we need to do anything?
}

u32 XamEnableInactivityProcessing_entry(u32 unk, u32 enable) {
  return X_ERROR_SUCCESS;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetcapabilities(v=vs.85).aspx
u32 XamInputGetCapabilities_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  REXKRNL_TRACE("[XAM] XamInputGetCapabilities called: user={}, flags=0x{:X}", (uint32_t)user_index,
                (uint32_t)flags);
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

u32 XamInputGetCapabilitiesEx_entry(u32 unk, u32 user_index, u32 flags,
                                    ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  if (!caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->GetCapabilities(actual_user_index, flags, caps);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetstate(v=vs.85).aspx
u32 XamInputGetState_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_STATE> input_state) {
  // Games call this with a NULL state ptr, probably as a query.
  static int call_count = 0;
  if (++call_count <= 5) {
    REXKRNL_TRACE("[XAM] XamInputGetState called: user={}, flags=0x{:X}", (uint32_t)user_index,
                  (uint32_t)flags);
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  if (!REXCVAR_GET(input_diag_users)) {
    return is->GetState(actual_user_index, input_state);
  }
  // peggle2-nx (input_diag_users): [user][0] calls, [1] connected, [2] something held; user 4 = any.
  static std::atomic<uint32_t> counts[5][3];
  static std::atomic<int64_t> window_start{0};
  const uint32_t slot = ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) ? 4 : (user_index & 3);
  const X_RESULT result = is->GetState(actual_user_index, input_state);
  counts[slot][0].fetch_add(1, std::memory_order_relaxed);
  if (result == X_ERROR_SUCCESS) {
    counts[slot][1].fetch_add(1, std::memory_order_relaxed);
    if (input_state && (uint16_t(input_state->gamepad.buttons) || uint8_t(input_state->gamepad.left_trigger) ||
                        uint8_t(input_state->gamepad.right_trigger)))
      counts[slot][2].fetch_add(1, std::memory_order_relaxed);
  }
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t start = window_start.load(std::memory_order_relaxed);
  if (start == 0) {
    window_start.compare_exchange_strong(start, now);
  } else if (now - start >= 10000 && window_start.compare_exchange_strong(start, now)) {
    std::string line;
    for (uint32_t u = 0; u < 5; ++u) {
      const uint32_t calls = counts[u][0].exchange(0), found = counts[u][1].exchange(0),
                     held = counts[u][2].exchange(0);
      if (calls)
        line += fmt::format("{}user {}: {} calls, {} connected, {} with input", line.empty() ? "" : "; ",
                            u == 4 ? std::string("any") : std::to_string(u), calls, found, held);
    }
    REXKRNL_INFO("[input] 10 s of XamInputGetState: {}", line.empty() ? std::string("no calls") : line);
  }
  return result;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputsetstate(v=vs.85).aspx
u32 XamInputSetState_entry(u32 user_index, u32 unk, ppc_ptr_t<X_INPUT_VIBRATION> vibration) {
  if (!vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  uint32_t actual_user_index = user_index;
  if ((user_index & 0xFF) == 0xFF) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  (void)unk;  // Unused in this implementation
  auto* is = input_system();
  return is->SetState(actual_user_index, vibration);
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinputgetkeystroke(v=vs.85).aspx
u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags, ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  // https://github.com/CodeAsm/ffplay360/blob/master/Common/AtgXime.cpp
  // user index = index or XUSER_INDEX_ANY
  // flags = XINPUT_FLAG_GAMEPAD (| _ANYUSER | _ANYDEVICE)

  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t actual_user_index = user_index;
  if ((actual_user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    actual_user_index = 0;
  }

  auto* is = input_system();
  return is->GetKeystroke(actual_user_index, flags, keystroke);
}

// Same as non-ex, just takes a pointer to user index.
u32 XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                 ppc_ptr_t<X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  if ((flags & 0xFF) && (flags & XINPUT_FLAG_GAMEPAD) == 0) {
    // Ignore any query for other types of devices.
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t user_index = *user_index_ptr;
  if ((user_index & 0xFF) == 0xFF || (flags & XINPUT_FLAG_ANY_USER)) {
    // Always pin user to 0.
    user_index = 0;
  }

  auto* is = input_system();
  auto result = is->GetKeystroke(user_index, flags, keystroke);
  if (XSUCCEEDED(result)) {
    *user_index_ptr = keystroke->user_index;
  }
  return result;
}

i32 XamUserGetDeviceContext_entry(u32 user_index, u32 unk, mapped_u32 out_ptr) {
  // Games check the result - usually with some masking.
  // If this function fails they assume zero, so let's fail AND
  // set zero just to be safe.
  *out_ptr = 0;
  const i32 result = (!user_index || (user_index & 0xFF) == 0xFF) ? X_E_SUCCESS : X_E_DEVICE_NOT_CONNECTED;
  if (REXCVAR_GET(input_diag_users)) {
    // peggle2-nx (input_diag_users): every call while the diagnostic is on (few: the game asks at sign-in changes).
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 400)
      REXKRNL_INFO("[input] XamUserGetDeviceContext(user {}, {}) -> 0x{:08X}, context 0x{:08X}", uint32_t(user_index),
                   uint32_t(unk), uint32_t(result), uint32_t(*out_ptr));
  }
  return result;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamResetInactivity, rex::kernel::xam::XamResetInactivity_entry)
REX_EXPORT(__imp__XamEnableInactivityProcessing,
           rex::kernel::xam::XamEnableInactivityProcessing_entry)
REX_EXPORT(__imp__XamInputGetCapabilities, rex::kernel::xam::XamInputGetCapabilities_entry)
REX_EXPORT(__imp__XamInputGetCapabilitiesEx, rex::kernel::xam::XamInputGetCapabilitiesEx_entry)
REX_EXPORT(__imp__XamInputGetState, rex::kernel::xam::XamInputGetState_entry)
REX_EXPORT(__imp__XamInputSetState, rex::kernel::xam::XamInputSetState_entry)
REX_EXPORT(__imp__XamInputGetKeystroke, rex::kernel::xam::XamInputGetKeystroke_entry)
REX_EXPORT(__imp__XamInputGetKeystrokeEx, rex::kernel::xam::XamInputGetKeystrokeEx_entry)
REX_EXPORT(__imp__XamUserGetDeviceContext, rex::kernel::xam::XamUserGetDeviceContext_entry)

REX_EXPORT_STUB(__imp__XamInputControl);
REX_EXPORT_STUB(__imp__XamInputEnableAutobind);
REX_EXPORT_STUB(__imp__XamInputGetDeviceStats);
REX_EXPORT_STUB(__imp__XamInputGetFailedConnectionOrBind);
REX_EXPORT_STUB(__imp__XamInputGetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHud);
REX_EXPORT_STUB(__imp__XamInputGetKeystrokeHudEx);
REX_EXPORT_STUB(__imp__XamInputGetUserVibrationLevel);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerGetRawEx);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRaw);
REX_EXPORT_STUB(__imp__XamInputNonControllerSetRawEx);
REX_EXPORT_STUB(__imp__XamInputRawState);
REX_EXPORT_STUB(__imp__XamInputResetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSendStayAliveRequest);
REX_EXPORT_STUB(__imp__XamInputSendXenonButtonPress);
REX_EXPORT_STUB(__imp__XamInputSetKeyLocks);
REX_EXPORT_STUB(__imp__XamInputSetKeyboardTranslationHud);
REX_EXPORT_STUB(__imp__XamInputSetLayoutKeyboard);
REX_EXPORT_STUB(__imp__XamInputSetMinMaxAuthDelay);
REX_EXPORT_STUB(__imp__XamInputSetTextMessengerIndicator);
REX_EXPORT_STUB(__imp__XamInputToggleKeyLocks);
