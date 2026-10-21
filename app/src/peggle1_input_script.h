// A scripted controller for unattended test runs (the window stays minimized, so nobody can press a button): with
// peggle1_input_script set, a synthetic pad is added to the input system and replays the script. Synthetic devices
// belong to player 1 and are merged into its state (buttons OR-ed, the larger stick), so a real pad still works.
//
// Script: comma-separated events "<when>:<input>[:<hold>]".
//   <when>   game frames since start (D3DDevice_Swap count, 30 per second), or seconds with an "s" suffix ("40s")
//   <input>  buttons joined by '+': A B X Y START BACK UP DOWN LEFT RIGHT LB RB LS RS, or an axis LX= LY= RX= RY=
//            (-32768..32767) or LT= RT= (0..255)
//   <hold>   how long, in game frames (default 4); an axis keeps its value for the hold, then returns to 0
// Example: peggle1_input_script = "40s:A,44s:A,50s:DOWN,52s:A,60s:LX=-20000:45,64s:A"

#pragma once

#include <functional>
#include <memory>

namespace rex::system {
class IInputSystem;
}

namespace peggle1::input_script {

using InputFactory = std::function<std::unique_ptr<rex::system::IInputSystem>(bool tool_mode)>;

// Returns a factory that builds what `base` builds and, when peggle1_input_script is set, adds the scripted pad.
InputFactory Wrap(InputFactory base);

}  // namespace peggle1::input_script
