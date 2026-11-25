// peggle1-nx (2026-11-24, the user: "adding the ingame menu ... make sure the ingame menu only has the items from their
// previous release toml"): the game menu's Settings tab (game_menu.h, SetGameMenuRows, the ultimate base's port 227)
// holds exactly the keys of Peggle 1.0.2's player file (app/peggle1_switch.toml), in its order:
//   - peggle1_language: "" (the console's language) or one of the game's own (en, ja, de, fr, es, it, ko, zh, pt); read at start (*).
//   - input_rumble (2026-11-25, the user: "put rumble as an option in the in game menu default to on"; on by
//     default, the ultimate base's port 59), input_xbox_layout and input_joycon_sideways (the base's rows,
//     at once).
//   - input_menu_combo, menu_color, menu_font and menu_background_opacity: the menu's own rows (the base's, at once),
//     last (2026-11-25, the user: "add the menu colour, menu font, menu buttons, and menu opacity back to the in
//     game menu").
// The base's other Settings rows (frame cap, resolutions, rumble, SMAA, MSAA) are not in that file and are left out.
// The Achievements tab and the P1 to P4 Controls tabs (input_p1_* ... input_p4_*) are the base's.
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/system/flags.h>

#include "game_menu.h"
#include "game_menu_text.h"

namespace {

using Setting = blur::GameMenuDialog::Setting;
using Choice = blur::GameMenuDialog::Choice;

Choice Quoted(std::string label, std::string raw) {
  std::string toml = "\"" + raw + "\"";
  return {std::move(label), std::move(raw), std::move(toml)};
}
Choice Plain(std::string label, std::string raw) {
  std::string toml = raw;
  return {std::move(label), std::move(raw), std::move(toml)};
}

void Rows(std::vector<Setting>& rows, int /*language_row*/, const std::string& /*file*/) {
  std::vector<Setting> out;
  {
    Setting s;
    s.label = blur::GameMenuText(blur::T_LANGUAGE);
    s.key = "peggle1_language";
    s.help = blur::GameMenuText(blur::H_LANGUAGE);
    s.restart = true;
    s.choices = {Quoted(blur::GameMenuText(blur::C_SYSTEM), ""),
                   Quoted("English", "en"),
                   Quoted("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "ja"),
                   Quoted("Deutsch", "de"),
                   Quoted("Fran\xC3\xA7" "ais", "fr"),
                   Quoted("Espa\xC3\xB1ol", "es"),
                   Quoted("Italiano", "it"),
                   Quoted("\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4", "ko"),
                   Quoted("\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87", "zh"),
                   Quoted("Portugu\xC3\xAAs", "pt")};
    s.default_choice = 0;
    out.push_back(std::move(s));
  }
  for (Setting& s : rows)
    if (s.key == "input_rumble" || s.key == "input_xbox_layout" || s.key == "input_joycon_sideways")
      out.push_back(std::move(s));
  // 2026-11-25: the menu's own rows (the buttons that open it, its colour, font and background), in the base's order.
  for (const char* key : {"input_menu_combo", "menu_color", "menu_font", "menu_background_opacity"})
    for (Setting& s : rows)
      if (s.key == key) out.push_back(std::move(s));
  rows = std::move(out);
}

}  // namespace

namespace peggle1 {
// Once, before the menu is first drawn (peggle1_app.h, OnCreateDialogs).
void RegisterMenuRows() { blur::SetGameMenuRows(&Rows); }
}  // namespace peggle1

// The language the game is told (game_menu.cpp asks it for the menu's own language): user_language as
// peggle1_app.h's OnPostInitLogging set it from peggle1_language (the base's own language key is not in this port).
namespace rex::system {
uint32_t EffectiveLanguage() { return REXCVAR_GET(user_language); }
}  // namespace rex::system

// game_menu.cpp's frame cap (the base's frame_cap row, from katamari_keys.cpp): this port has no frame_cap key, so the
// row is never made and these are never reached.
void BlurSetFrameCapLive(int /*fps*/) {}
int BlurFrameCapNow() { return 0; }
