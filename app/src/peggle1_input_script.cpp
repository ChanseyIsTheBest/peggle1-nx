// The scripted controller (see peggle1_input_script.h).

#include "peggle1_input_script.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

#include "peggle1_capture.h"

REXCVAR_DEFINE_STRING(peggle1_input_script, "", "Peggle1",
                      "Scripted controller for unattended test runs: \"<frame or Ns>:<buttons or axis>[:<hold>]\", "
                      "comma-separated (see app/src/peggle1_input_script.h). Empty = off");

REXCVAR_DECLARE(int32_t, peggle1_fps);

REXCVAR_DEFINE_STRING(peggle1_input_script_p2, "", "Peggle1",
                      "Scripted controller of a second player (same syntax as peggle1_input_script): a real pad for "
                      "player 2 behind an idle pad for player 1, for unattended runs of the multiplayer lobby");

namespace peggle1::input_script {
namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

constexpr DeviceId kScriptDevice = static_cast<DeviceId>(0x50324950);  // "P2IP"
constexpr DeviceId kIdleDevice = static_cast<DeviceId>(0x50324951);    // player 1's idle pad (second-player runs)
constexpr DeviceId kScriptDevice2 = static_cast<DeviceId>(0x50324952); // player 2's scripted pad
// Game frames a second: the script counts D3DDevice_Swap calls (30 a second, 60 with peggle1_fps = 60).
uint64_t Fps() { return REXCVAR_GET(peggle1_fps) >= 60 ? 60 : 30; }

struct Event {
  uint64_t frame = 0;
  uint64_t hold = 4;
  uint16_t buttons = 0;
  int axis = -1;  // 0..3 = LX LY RX RY, 4 = LT, 5 = RT
  int value = 0;
};

uint16_t ButtonBit(std::string_view name) {
  static const struct {
    const char* name;
    uint16_t bit;
  } kButtons[] = {
      {"UP", 0x0001},     {"DOWN", 0x0002}, {"LEFT", 0x0004}, {"RIGHT", 0x0008}, {"START", 0x0010},
      {"BACK", 0x0020},   {"LS", 0x0040},   {"RS", 0x0080},   {"LB", 0x0100},    {"RB", 0x0200},
      {"A", 0x1000},      {"B", 0x2000},    {"X", 0x4000},    {"Y", 0x8000},
  };
  for (const auto& b : kButtons) {
    if (name == b.name) {
      return b.bit;
    }
  }
  return 0;
}

std::vector<std::string> Split(std::string_view text, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t end = std::min(text.find(sep, start), text.size());
    std::string part(text.substr(start, end - start));
    part.erase(std::remove_if(part.begin(), part.end(), [](unsigned char c) { return std::isspace(c); }),
               part.end());
    if (!part.empty()) {
      out.push_back(part);
    }
    start = end + 1;
  }
  return out;
}

bool ParseEvent(const std::string& text, Event& ev) {
  const auto fields = Split(text, ':');
  if (fields.size() < 2 || fields.size() > 3) {
    return false;
  }
  std::string when = fields[0];
  const bool seconds = !when.empty() && (when.back() == 's' || when.back() == 'S');
  if (seconds) {
    when.pop_back();
  }
  char* end = nullptr;
  const double t = std::strtod(when.c_str(), &end);
  if (end == when.c_str() || *end || t < 0) {
    return false;
  }
  ev.frame = seconds ? uint64_t(t * Fps() + 0.5) : uint64_t(t);
  ev.hold = 4 * Fps() / 30;  // the default: the same time at either rate
  std::string input = fields[1];
  std::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  if (const size_t eq = input.find('='); eq != std::string::npos) {
    static const char* kAxes[] = {"LX", "LY", "RX", "RY", "LT", "RT"};
    for (int i = 0; i < 6; ++i) {
      if (input.compare(0, eq, kAxes[i]) == 0) {
        ev.axis = i;
      }
    }
    ev.value = std::atoi(input.c_str() + eq + 1);
    if (ev.axis < 0) {
      return false;
    }
    ev.value = ev.axis >= 4 ? std::clamp(ev.value, 0, 255) : std::clamp(ev.value, -32768, 32767);
  } else {
    for (const auto& name : Split(input, '+')) {
      const uint16_t bit = ButtonBit(name);
      if (!bit) {
        return false;
      }
      ev.buttons |= bit;
    }
  }
  if (fields.size() == 3) {
    ev.hold = std::max<uint64_t>(1, std::strtoull(fields[2].c_str(), nullptr, 10));
  }
  return true;
}

class ScriptedInputDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptedInputDriver(std::vector<Event> events, bool second = false)
      : InputDriver(nullptr, 0), events_(std::move(events)), id_(second ? kScriptDevice2 : kScriptDevice),
        second_(second) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<DeviceInfo>& out) override {
    if (second_) {
      // Real devices get players in the order they are listed: the idle one is player 1, the script player 2.
#if !defined(__SWITCH__)
      DeviceInfo idle;
      idle.id = kIdleDevice;
      idle.name = "Idle pad (player 1)";
      out.push_back(idle);
#endif
      // peggle1-nx (2026-10-21): not on the Switch, whose own pad (handheld, or Ryujinx's player 1) is listed first
      // and is player 1: there the idle pad made the script player 3 and player 2 never joined the duel.
      DeviceInfo info;
      info.id = id_;
      info.name = "Input script (player 2)";
      out.push_back(info);
      return;
    }
    DeviceInfo info;
    info.id = kScriptDevice;
    info.name = "Input script";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override {
    if (second_ && id == kIdleDevice) {
      if (out_state) {
        *out_state = {};
        out_state->packet_number = 1;
      }
      return X_ERROR_SUCCESS;
    }
    if (id != id_) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    const uint64_t frame = g_peggle1_game_frames.load(std::memory_order_relaxed);
    uint16_t buttons = 0;
    int axes[6] = {};
    size_t active = 0;
    for (size_t i = 0; i < events_.size(); ++i) {
      const Event& ev = events_[i];
      if (frame < ev.frame || frame >= ev.frame + ev.hold) {
        continue;
      }
      active = active * 31 + i + 1;
      buttons |= ev.buttons;
      if (ev.axis >= 0) {
        axes[ev.axis] = ev.value;
      }
    }
    // XInput's packet number changes whenever the state does; the merge keeps the largest, so start high.
    if (active != last_active_.exchange(active, std::memory_order_relaxed)) {
      packet_.fetch_add(1, std::memory_order_relaxed);
      if (active) {
        REXLOG_INFO("[input-script] frame {}: buttons {:04X} LX {} LY {} RX {} RY {} LT {} RT {}", frame, buttons,
                    axes[0], axes[1], axes[2], axes[3], axes[4], axes[5]);
      }
    }
    if (out_state) {
      *out_state = {};
      out_state->packet_number = packet_.load(std::memory_order_relaxed);
      out_state->gamepad.buttons = buttons;
      out_state->gamepad.thumb_lx = int16_t(axes[0]);
      out_state->gamepad.thumb_ly = int16_t(axes[1]);
      out_state->gamepad.thumb_rx = int16_t(axes[2]);
      out_state->gamepad.thumb_ry = int16_t(axes[3]);
      out_state->gamepad.left_trigger = uint8_t(axes[4]);
      out_state->gamepad.right_trigger = uint8_t(axes[5]);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t, X_INPUT_CAPABILITIES* out_caps) override {
    if (!Mine(id)) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_caps) {
      *out_caps = {};
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
      out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION*) override {
    return Mine(id) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, X_INPUT_KEYSTROKE*) override {
    return Mine(id) ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  bool Mine(DeviceId id) const { return id == id_ || (second_ && id == kIdleDevice); }

  std::vector<Event> events_;
  DeviceId id_;
  bool second_;
  std::atomic<size_t> last_active_{0};
  std::atomic<uint32_t> packet_{0x10000000};
};

}  // namespace

InputFactory Wrap(InputFactory base) {
  return [base = std::move(base)](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto system = base(tool_mode);
    const std::string script = REXCVAR_GET(peggle1_input_script);
    const std::string script2 = REXCVAR_GET(peggle1_input_script_p2);
    if (tool_mode || !system || (script.empty() && script2.empty())) {
      return system;
    }
    const auto parse = [](const std::string& text_all, const char* who) {
      std::vector<Event> events;
      for (const auto& text : Split(text_all, ',')) {
        Event ev;
        if (ParseEvent(text, ev)) {
          events.push_back(ev);
        } else {
          REXLOG_WARN("[input-script] cannot read event '{}', skipped", text);
        }
      }
      REXLOG_INFO("[input-script] {}: {} events", who, events.size());
      return events;
    };
    // The default input system is rex::input::InputSystem (CreateDefaultInputSystem).
    auto* input = static_cast<rex::input::InputSystem*>(system.get());
    if (!script.empty()) input->AddDriver(std::make_unique<ScriptedInputDriver>(parse(script, "player 1")));
    if (!script2.empty()) {
      input->AddDriver(std::make_unique<ScriptedInputDriver>(parse(script2, "player 2"), true));
    }
    return system;
  };
}

}  // namespace peggle1::input_script
