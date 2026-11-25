// blur-nx (2026-11-18): the game menu (game_menu.h).
#include "game_menu.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/achievement_icon_cache.h>

#include "game_menu_fonts.h"
#include "game_menu_text.h"  // blur-nx (2026-11-18): the menu's text in the Xbox 360's twelve languages

#include <rex/system/flags.h>              // rex::system::EffectiveLanguage
#include <rex/system/util/xdbf_utils.h>    // the game's own languages (its XDBF)  // blur-nx (2026-11-18): Dear ImGui's misc/fonts, embedded by CMake (app/CMakeLists.txt)

#if REX_PLATFORM_SWITCH
#include <rex/input/switch/switch_input_driver.h>  // rex::input::nx::RebuildButtonMaps

// libnx's shared font service (switch.h is not included: its types clash with the SDK's). pl.h's PlFontData.
extern "C" {
struct BlurPlFontData {
  uint32_t type, offset, size;
  void* address;
};
uint32_t plInitialize(int service_type);
uint32_t plGetSharedFontByType(BlurPlFontData* font, int shared_font_type);
int RexSwitchApplyFourCoreMode(int on);  // threading_switch.cpp: the forwarder's NPDM written or removed
}
#endif

// blur-nx (2026-11-18): the menu's colour and font, both at once (the font: the atlas made again between frames).
REXCVAR_DEFINE_STRING(menu_color, "Green", "UI",
                      "The game menu's colour: Green, Yellow, Red, Blue, Purple, Pink, Orange, White or Rainbow");
REXCVAR_DEFINE_STRING(menu_font, "System", "UI",
                      "The game menu's font: System (the console's own), Pixel (the debug overlay's ProggyTiny), "
                      "Roboto, Karla or Cousine. The menu changes it at once (the ImGui drawer's RequestFontRebuild)");
// Port 180 (the user, 2026-11-18: the black background's opacity as an option, in the toml and in the menu under the
// font and the colour): the window's black, read at every frame.
REXCVAR_DEFINE_INT32(menu_background_opacity, 80, "UI",
                     "The game menu's black background, in percent opaque: 0 (the game shows through) to 100; 80 by "
                     "default (the user's choice; the menu's first look was 97). The menu changes it at once");

// blur_keys.cpp: the frame cap from the next frame, and the one in force.
void BlurSetFrameCapLive(int fps);
int BlurFrameCapNow();

namespace blur {

// banjo-nx (2026-11-19): a game's own Settings rows (game_menu.h, SetGameMenuRows).
static GameMenuRowsHook g_game_rows = nullptr;
void SetGameMenuRows(GameMenuRowsHook hook) { g_game_rows = hook; }

namespace {

// ---- Fonts ----------------------------------------------------------------------------------------------------------

ImFont* g_font_small = nullptr;       // for a 720-line display
ImFont* g_font_large = nullptr;       // for a 1080-line one
ImFont* g_font_small_text = nullptr;  // their smaller text (help, descriptions, the footer): baked at its own size, as
ImFont* g_font_large_text = nullptr;  // the Switch's drawer has a fixed atlas (another size would be scaled, blurred)

// Only a font of this context's atlas (the PC draws the app's dialogs with Renut's own ImGui context).
ImFont* InAtlas(const ImGuiIO& io, ImFont* font) {
  return font && io.Fonts && io.Fonts->Fonts.contains(font) ? font : nullptr;
}
ImFont* MenuFont(const ImGuiIO& io) { return InAtlas(io, io.DisplaySize.y > 900.0f ? g_font_large : g_font_small); }
// The sizes the fonts are drawn at (the bakes' own, but for Japanese and Chinese, baked at the 720-line sizes only).
float g_size_main[2] = {22.0f, 33.0f};
float g_size_text[2] = {18.0f, 27.0f};
bool g_cjk_sizes_once = false;
float MenuFontSize(const ImGuiIO& io) { return g_size_main[io.DisplaySize.y > 900.0f ? 1 : 0]; }

// The smaller text's font and size this frame (OnDraw sets them): its own font, else the current one at 0.8.
ImFont* g_text_font = nullptr;
float g_text_size = 0.0f;
void PickTextFont(const ImGuiIO& io) {
  ImFont* own = InAtlas(io, io.DisplaySize.y > 900.0f ? g_font_large_text : g_font_small_text);
  g_text_font = own ? own : ImGui::GetFont();
  g_text_size = own ? g_size_text[io.DisplaySize.y > 900.0f ? 1 : 0] : ImGui::GetFontSize() * 0.8f;
}
ImVec2 TextSize(const char* text, float wrap = 0.0f) {
  return g_text_font->CalcTextSizeA(g_text_size, FLT_MAX, wrap, text);
}

// ---- The menu's language ----

int g_lang = 0;  // kMenuTextTable's row in use

const char* Tr(int id) {
  const char* s = kMenuTextTable[g_lang][id];
  return s ? s : kMenuTextTable[0][id];
}
std::string TrS(int id, const char* a) {
  char b[640];
  std::snprintf(b, sizeof(b), Tr(id), a);
  return b;
}
std::string TrD(int id, int a) {
  char b[640];
  std::snprintf(b, sizeof(b), Tr(id), a);
  return b;
}
std::string TrSD(int id, const char* a, int d) {
  char b[640];
  std::snprintf(b, sizeof(b), Tr(id), a, d);
  return b;
}
std::string TrDD(int id, int a, int d) {
  char b[640];
  std::snprintf(b, sizeof(b), Tr(id), a, d);
  return b;
}

// XLanguage (1 English ... 12 Russian) to the table's row.
int RowOfXLanguage(uint32_t x) {
  static const int kRows[] = {0, 0, 8, 2, 1, 4, 3, 9, 11, 7, 10, 5, 6};
  return x < std::size(kRows) ? kRows[x] : 0;
}

// The Switch button names the file writes, as the menu shows them.
std::string ButtonName(const std::string& raw) {
  if (raw == "PLUS") return "+";
  if (raw == "MINUS") return "-";
  if (raw == "LSTICK") return Tr(C_LSTICK);
  if (raw == "RSTICK") return Tr(C_RSTICK);
  if (raw == "UP") return Tr(C_DPAD_UP);
  if (raw == "DOWN") return Tr(C_DPAD_DOWN);
  if (raw == "LEFT") return Tr(C_DPAD_LEFT);
  if (raw == "RIGHT") return Tr(C_DPAD_RIGHT);
  if (raw == "NONE") return Tr(C_NONE);
  return raw;
}

// ---- Colours: green and black ---------------------------------------------------------------------------------------

const ImVec4 kBlack(0.012f, 0.020f, 0.014f, 0.97f);
const ImVec4 kText(0.880f, 1.000f, 0.900f, 1.00f);
const ImVec4 kTextDim(0.500f, 0.660f, 0.540f, 1.00f);
const ImVec4 kWarn(1.000f, 0.720f, 0.180f, 1.00f);  // the four-core option's warning, and its Off

// blur-nx (2026-11-18, the user: "add a colour change option for the menu UI yellow, red, green, blue, purple, pink,
// orange, white, and rainbow ... the current green should be default"): menu_color. The accent and its shades, set at
// every frame (Rainbow turns through the hues every 12 s); Green is the first menu's colours exactly.
ImVec4 kAccent(0.200f, 0.930f, 0.420f, 1.00f);
ImVec4 kAccentDim(0.090f, 0.420f, 0.190f, 1.00f);
ImVec4 kAccentDark(0.050f, 0.230f, 0.105f, 1.00f);
ImVec4 kPanel(0.030f, 0.060f, 0.038f, 1.00f);
ImVec4 kPending(0.780f, 0.960f, 0.350f, 1.00f);  // a * setting's value for the next start
bool g_rainbow = false;
float g_rainbow_hue = 0.0f;

ImVec4 Hue(float h, float s = 0.72f, float v = 0.97f) {
  ImVec4 c(0, 0, 0, 1);
  ImGui::ColorConvertHSVtoRGB(h - std::floor(h), s, v, c.x, c.y, c.z);
  return c;
}

void SetTheme(const std::string& name, double time) {
  struct Named {
    const char* name;
    float r, g, b;
  };
  static const Named kNamed[] = {
      {"Yellow", 0.98f, 0.86f, 0.20f}, {"Red", 0.96f, 0.27f, 0.25f},    {"Blue", 0.27f, 0.56f, 1.00f},
      {"Purple", 0.67f, 0.42f, 1.00f}, {"Pink", 1.00f, 0.44f, 0.76f},   {"Orange", 1.00f, 0.58f, 0.17f},
      {"White", 0.93f, 0.93f, 0.93f},
  };
  g_rainbow = rex::string::compare_case(name.c_str(), "Rainbow") == 0;
  ImVec4 a(0.200f, 0.930f, 0.420f, 1.0f);
  bool green = !g_rainbow;
  if (g_rainbow) {
    g_rainbow_hue = float(std::fmod(time / 12.0, 1.0));
    a = Hue(g_rainbow_hue);
  } else {
    for (const Named& n : kNamed)
      if (rex::string::compare_case(name.c_str(), n.name) == 0) {
        a = ImVec4(n.r, n.g, n.b, 1.0f);
        green = false;
      }
  }
  if (green) {  // the default: the first menu's own colours
    kAccent = ImVec4(0.200f, 0.930f, 0.420f, 1.00f);
    kAccentDim = ImVec4(0.090f, 0.420f, 0.190f, 1.00f);
    kAccentDark = ImVec4(0.050f, 0.230f, 0.105f, 1.00f);
    kPanel = ImVec4(0.030f, 0.060f, 0.038f, 1.00f);
    kPending = ImVec4(0.780f, 0.960f, 0.350f, 1.00f);
    return;
  }
  auto scale = [&](float f, float base) { return ImVec4(a.x * f + base, a.y * f + base, a.z * f + base, 1.0f); };
  kAccent = a;
  kAccentDim = scale(0.45f, 0.0f);
  kAccentDark = scale(0.24f, 0.0f);
  kPanel = scale(0.06f, 0.015f);
  kPending = ImVec4(a.x * 0.5f + 0.5f, a.y * 0.5f + 0.5f, a.z * 0.5f + 0.38f, 1.0f);  // lighter
}

ImU32 U32(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

// Port 180: the window's black at menu_background_opacity (0-100 %; 80 by default, kBlack's own alpha was 0.97).
ImVec4 BackgroundBlack() {
  const int percent = std::clamp(int(REXCVAR_GET(menu_background_opacity)), 0, 100);
  return ImVec4(kBlack.x, kBlack.y, kBlack.z, float(percent) / 100.0f);
}

// ---- The player's file, key by key ----------------------------------------------------------------------------------

struct FileEdit {
  std::string key;
  bool remove = false;
  std::string toml;
};

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t");
  size_t b = s.find_last_not_of(" \t\r");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool LineIsKey(const std::string& line, const std::string& key) {
  const std::string t = Trim(line);
  if (t.size() <= key.size() || t.compare(0, key.size(), key) != 0)
    return false;
  size_t i = key.size();
  while (i < t.size() && (t[i] == ' ' || t[i] == '\t'))
    ++i;
  return i < t.size() && t[i] == '=';
}

bool IsTableLine(const std::string& line, std::string* name) {
  const std::string t = Trim(line);
  if (t.size() < 3 || t.front() != '[' || t.back() != ']')
    return false;
  if (name)
    *name = t.substr(1, t.size() - 2);
  return true;
}

// Each key set (its line replaced, or added at the end of its part) or removed; the rest of the file as it was. A map key
// (input_p1_a) goes in its table ([input_p1] a = ...) when the file has that table, as the shipped files did.
bool UpdateConfigFile(const std::filesystem::path& path, const std::vector<FileEdit>& edits) {
  std::string text;
  {
    std::ifstream in(path, std::ios::binary);
    if (in)
      text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const bool crlf = text.find("\r\n") != std::string::npos;
  std::vector<std::string> lines;
  {
    size_t start = 0;
    while (start < text.size()) {
      size_t end = text.find('\n', start);
      if (end == std::string::npos)
        end = text.size();
      std::string line = text.substr(start, end - start);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      lines.push_back(std::move(line));
      start = end + 1;
    }
  }
  for (const FileEdit& e : edits) {
    std::string table, sub = e.key;
    if (e.key.rfind("input_p", 0) == 0 && e.key.size() > 9 && e.key[8] == '_') {
      const std::string want = e.key.substr(0, 8);
      for (const std::string& l : lines) {
        std::string name;
        if (IsTableLine(l, &name) && name == want) {
          table = want;
          sub = e.key.substr(9);
          break;
        }
      }
    }
    // The part: the top (before the first table), or the table's lines.
    size_t begin = 0, end = lines.size();
    if (table.empty()) {
      for (size_t i = 0; i < lines.size(); ++i)
        if (IsTableLine(lines[i], nullptr)) {
          end = i;
          break;
        }
    } else {
      for (size_t i = 0; i < lines.size(); ++i) {
        std::string name;
        if (IsTableLine(lines[i], &name) && name == table) {
          begin = i + 1;
          end = lines.size();
          for (size_t j = begin; j < lines.size(); ++j)
            if (IsTableLine(lines[j], nullptr)) {
              end = j;
              break;
            }
          break;
        }
      }
    }
    bool found = false;
    for (size_t i = begin; i < end; ++i) {
      if (!LineIsKey(lines[i], sub))
        continue;
      found = true;
      if (e.remove)
        lines.erase(lines.begin() + long(i));
      else
        lines[i] = sub + " = " + e.toml;
      break;
    }
    if (!found && !e.remove) {
      size_t at = end;
      while (at > begin && Trim(lines[at - 1]).empty())
        --at;
      lines.insert(lines.begin() + long(at), sub + " = " + e.toml);
    }
  }
  std::string out;
  for (const std::string& l : lines) {
    out += l;
    out += crlf ? "\r\n" : "\n";
  }
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream o(temporary, std::ios::binary | std::ios::trunc);
    if (!o)
      return false;
    o.write(out.data(), std::streamsize(out.size()));
    if (!o.good())
      return false;
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);  // the Switch's file system renames onto no existing file
  std::filesystem::rename(temporary, path, ec);
  return !ec;
}

// ---- The settings ---------------------------------------------------------------------------------------------------

using Setting = GameMenuDialog::Setting;
using Choice = GameMenuDialog::Choice;

Choice C(std::string label, std::string raw, std::string toml) { return {std::move(label), std::move(raw), std::move(toml)}; }
Choice Q(std::string label, std::string raw) {  // a string key: quoted in the file
  std::string toml = "\"" + raw + "\"";
  return {std::move(label), std::move(raw), std::move(toml)};
}
std::vector<Choice> OnOff() { return {C("On", "true", "true"), C("Off", "false", "false")}; }

std::vector<Setting> MakeSettings(const std::string& file) {
  std::vector<Setting> s;
  auto add = [&](int label, std::string key, bool restart, std::vector<Choice> choices, int def, std::string help,
                 bool always_in_file = true) {
    // renut-ultimate (2026-11-19, port 189; sasrt-nx's): no row for a key this build does not define (Katamari's
    // frame_cap in a port that took app/src/katamari_keys.cpp out; four_core_mode and the Joy-Con keys off the Switch),
    // so a port needs no edit here. The file keeps any such line as it is.
    if (!rex::cvar::GetFlagInfo(key))
      return;
    Setting x;
    x.label = Tr(label);
    x.key = std::move(key);
    x.restart = restart;
    x.choices = std::move(choices);
    x.default_choice = def;
    x.help = std::move(help);
    x.always_in_file = always_in_file;
    s.push_back(std::move(x));
  };
  auto on_off = [] { return std::vector<Choice>{C(Tr(C_ON), "true", "true"), C(Tr(C_OFF), "false", "false")}; };
  // The languages by their own names (the file's value in English).
  std::vector<Choice> languages = {Q(Tr(C_SYSTEM), "System")};
  for (int i = 0; i < kMenuLanguages; ++i)
    languages.push_back(Q(kMenuLanguageNames[i], kMenuLanguageKeys[i]));
  add(T_LANGUAGE, "language", true, languages, 0, Tr(H_LANGUAGE));
  add(T_FRAMERATE, "frame_cap", false, {Q("30 fps", "30"), Q("60 fps", "60"), Q(Tr(C_OFF), "Off")}, 0,
      Tr(H_FRAMERATE));
  add(T_HANDHELD, "banjo_renut_render_height_handheld", false,
      {C("960x540", "540", "540"), C("1024x576", "576", "576"), C("1088x612", "612", "612"),
       C("1152x648", "648", "648"), C("1216x684", "684", "684"), C("1280x720", "720", "720")},
      5, Tr(H_HANDHELD));
  // rr6-nx (2026-11-22, the user: "a separate toggle for handheld and docked dynamic resolution"): each mode's switch
  // under its fixed resolution; the help says its range (banjo_renut_dynamic_*_low / _high, as set).
  const auto lines = [](const char *key) { return std::atoi(rex::cvar::GetFlagByName(key).c_str()); };
  add(T_DYN_HANDHELD, "banjo_renut_dynamic_resolution_handheld", false, on_off(), 1,
      TrDD(H_DYN_HANDHELD, lines("banjo_renut_dynamic_handheld_low"), lines("banjo_renut_dynamic_handheld_high")));
  add(T_DOCKED, "banjo_renut_render_height_docked", false,
      {C("1280x720", "720", "720"), C("1408x792", "792", "792"), C("1536x864", "864", "864"),
       C("1600x900", "900", "900"), C("1664x936", "936", "936"), C("1728x972", "972", "972"),
       C("1792x1008", "1008", "1008"), C("1920x1080", "1080", "1080")},
      5, Tr(H_DOCKED));
  add(T_DYN_DOCKED, "banjo_renut_dynamic_resolution_docked", false, on_off(), 0,
      TrDD(H_DYN_DOCKED, lines("banjo_renut_dynamic_docked_low"), lines("banjo_renut_dynamic_docked_high")));
  add(T_RUMBLE, "input_rumble", false, on_off(), 0, Tr(H_RUMBLE), /*always_in_file=*/false);
  add(T_SMAA, "banjo_renut_smaa", false, on_off(), 0, Tr(H_SMAA));
  add(T_MSAA, "msaa_level", false, {C(Tr(C_OFF), "1", "1"), C("2x", "2", "2"), C("4x", "4", "4")}, 0, Tr(H_MSAA));
  // blur-nx (2026-11-18, the user: "make 4 core mode an option ... on by default and a warning that it is highly
  // reccomended to be left on").
  add(T_FOURCORE, "four_core_mode", true, on_off(), 0, Tr(H_FOURCORE));
  add(T_XBOXLAYOUT, "input_xbox_layout", false, on_off(), 0, Tr(H_XBOXLAYOUT));
  add(T_SIDEWAYS, "input_joycon_sideways", false, on_off(), 0, Tr(H_SIDEWAYS));
  add(T_SIDEWAYS_STICK, "input_joycon_sideways_stick", false, on_off(), 0, Tr(H_SIDEWAYS_STICK));
  // blur-nx (2026-11-18, the user: "add an option to the config please exposed to the user to change the button config
  // combo to open the menu"): input_menu_combo; any other set in the file shows as it is written.
  add(T_MENUBUTTONS, "input_menu_combo", false,
      {Q("A+B+X+Y+L+R", "A+B+X+Y+L+R"), Q("L+R+(-)", "L+R+MINUS"), Q("L+R+(+)", "L+R+PLUS"),
       Q("ZL+ZR+(-)", "ZL+ZR+MINUS"), Q(Tr(C_STICKS), "LSTICK+RSTICK")},
      0, TrS(H_MENUBUTTONS, file.c_str()));
  // blur-nx (2026-11-18, the user's requests): the menu's colour and font, both at once.
  add(T_MENUCOLOR, "menu_color", false,
      {Q(Tr(C_GREEN), "Green"), Q(Tr(C_YELLOW), "Yellow"), Q(Tr(C_RED), "Red"), Q(Tr(C_BLUE), "Blue"),
       Q(Tr(C_PURPLE), "Purple"), Q(Tr(C_PINK), "Pink"), Q(Tr(C_ORANGE), "Orange"), Q(Tr(C_WHITE), "White"),
       Q(Tr(C_RAINBOW), "Rainbow")},
      0, Tr(H_MENUCOLOR));
  for (Setting& x : s)  // the render targets are made anew for these: once, when the menu closes
    if (x.key == "banjo_renut_render_height_handheld" || x.key == "banjo_renut_render_height_docked" ||
        x.key == "banjo_renut_dynamic_resolution_handheld" || x.key == "banjo_renut_dynamic_resolution_docked" ||
        x.key == "msaa_level")
      x.apply_on_close = true;
  add(T_MENUFONT, "menu_font", false,
      {Q(Tr(C_SYSTEM), "System"), Q(Tr(C_PIXEL), "Pixel"), Q("Roboto", "Roboto"), Q("Karla", "Karla"),
       Q("Cousine", "Cousine")},
      0, Tr(H_MENUFONT));
  // Port 180 (the user, 2026-11-18): menu_background_opacity, under the colour and the font; Right: more solid.
  {
    std::vector<Choice> opacity;
    for (int p = 0; p <= 100; p += 10)
      opacity.push_back(C(std::to_string(p) + " %", std::to_string(p), std::to_string(p)));
    add(T_MENUOPACITY, "menu_background_opacity", false, std::move(opacity), 8, Tr(H_MENUOPACITY));  // 80 %
  }
  if (g_game_rows)  // banjo-nx (2026-11-19): the game's own rows (game_menu.h)
    g_game_rows(s, g_lang, file);
  return s;
}

struct XboxButton {
  const char* label;      // as it is (A, LB, ...), or
  int label_id;           // the table's (-1: none)
  const char* suffix;     // input_pN_<suffix>
  const char* by_letter;  // the default Switch button (as the file names it), by letter / by position
  const char* by_position;
};
const XboxButton kXboxButtons[] = {
    {"A", -1, "a", "A", "B"},
    {"B", -1, "b", "B", "A"},
    {"X", -1, "x", "X", "Y"},
    {"Y", -1, "y", "Y", "X"},
    {"LB", -1, "lb", "L", "L"},
    {"RB", -1, "rb", "R", "R"},
    {"LT", -1, "lt", "ZL", "ZL"},
    {"RT", -1, "rt", "ZR", "ZR"},
    {nullptr, X_START, "start", "PLUS", "PLUS"},
    {nullptr, X_BACK, "back", "MINUS", "MINUS"},
    {nullptr, X_LS, "ls", "LSTICK", "LSTICK"},
    {nullptr, X_RS, "rs", "RSTICK", "RSTICK"},
    {nullptr, C_DPAD_UP, "up", "UP", "UP"},
    {nullptr, C_DPAD_DOWN, "down", "DOWN", "DOWN"},
    {nullptr, C_DPAD_LEFT, "left", "LEFT", "LEFT"},
    {nullptr, C_DPAD_RIGHT, "right", "RIGHT", "RIGHT"},
};
constexpr const char* kResetKey = "#reset";

const char* XboxButtonName(const XboxButton& b) { return b.label ? b.label : Tr(b.label_id); }

std::vector<Setting> MakeControls(int player) {
  std::vector<Choice> buttons = {Q(Tr(C_DEFAULT), "")};
  for (const char* raw : {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LSTICK", "RSTICK", "UP", "DOWN",
                          "LEFT", "RIGHT", "SL", "SR", "NONE"})
    buttons.push_back(Q(ButtonName(raw), raw));
  std::vector<Setting> s;
  for (const XboxButton& b : kXboxButtons) {
    Setting x;
    x.label = std::string("Xbox ") + XboxButtonName(b);
    x.key = "input_p" + std::to_string(player + 1) + "_" + b.suffix;
    x.restart = false;
    x.always_in_file = false;
    x.choices = buttons;
    x.default_choice = 0;
    x.help = TrSD(H_CONTROL, XboxButtonName(b), player + 1);
    s.push_back(std::move(x));
  }
  Setting reset;
  reset.label = Tr(R_RESET);
  reset.key = kResetKey;
  reset.choices = {C(Tr(R_PRESS), "", "")};
  reset.help = TrD(H_RESET, player + 1);
  s.push_back(std::move(reset));
  return s;
}

int FindChoice(const Setting& s, const std::string& raw) {
  for (size_t i = 0; i < s.choices.size(); ++i)
    if (rex::string::compare_case(s.choices[i].raw.c_str(), raw.c_str()) == 0)
      return int(i);
  return -1;
}

std::string CurrentRaw(const Setting& s) {
  if (s.key == "frame_cap") {
    const int fps = BlurFrameCapNow();
    return fps <= 0 ? "Off" : std::to_string(fps);
  }
  return rex::cvar::GetFlagByName(s.key);
}

const ImGuiKey kGamepadKeys[] = {
    ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceUp,   ImGuiKey_GamepadFaceLeft,
    ImGuiKey_GamepadDpadUp,   ImGuiKey_GamepadDpadDown,  ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
    ImGuiKey_GamepadL1,       ImGuiKey_GamepadR1,        ImGuiKey_GamepadL2,       ImGuiKey_GamepadR2,
    ImGuiKey_GamepadStart,    ImGuiKey_GamepadBack,      ImGuiKey_GamepadL3,       ImGuiKey_GamepadR3,
    ImGuiKey_Escape,          ImGuiKey_F8,               ImGuiKey_Enter,
};

bool Pressed(ImGuiKey a, ImGuiKey b = ImGuiKey_None, ImGuiKey c = ImGuiKey_None, bool repeat = true) {
  return ImGui::IsKeyPressed(a, repeat) || (b != ImGuiKey_None && ImGui::IsKeyPressed(b, repeat)) ||
         (c != ImGuiKey_None && ImGui::IsKeyPressed(c, repeat));
}

// The tabs' names in the menu's language (DrawTabs).
std::string TabName(int i) {
  if (i == 0) return Tr(T_TAB_SETTINGS);
  if (i == 1) return Tr(T_TAB_ACHIEVEMENTS);
  return TrD(T_TAB_CONTROLS, i - 1);
}

}  // namespace

const char* GameMenuText(int id) { return Tr(id); }  // banjo-nx (2026-11-19): game_menu.h
std::vector<GameMenuDialog::Setting> GameMenuSettingsRows(int language_row, const std::string& file) {
  const int saved = g_lang;
  g_lang = language_row >= 0 && language_row < kMenuLanguages ? language_row : 0;
  std::vector<GameMenuDialog::Setting> rows = MakeSettings(file);
  g_lang = saved;
  return rows;
}
std::string GameMenuCurrentRaw(const GameMenuDialog::Setting& s) { return CurrentRaw(s); }

// ---- Fonts ----------------------------------------------------------------------------------------------------------

void AddGameMenuFonts(ImFontAtlas* atlas) {
  if (!atlas)
    return;
  // blur-nx (2026-11-18, the user: "add a couple font options as well for the UI including the debug pixelly font but
  // keep the current as default"): menu_font, baked at start (the Switch's drawer has a fixed atlas). The others are
  // Dear ImGui's own (misc/fonts: game_menu_fonts.h, made by CMake), kept in the NRO.
  // The glyphs: Latin (with Latin Extended-A), Cyrillic, punctuation and the CJK forms the texts use; every character
  // of the menu's texts in its language at start (the language key, "System" the console's) and the languages' own
  // names; Japanese and Chinese: their common characters too (the game's achievements in them).
  static ImVector<ImWchar> s_ranges;
  {
    static const ImWchar kBase[] = {0x0020, 0x017F, 0x0400, 0x045F, 0x2010, 0x2027, 0x2190, 0x2193, 0x25B2, 0x25C0,
                                    0x3000, 0x303F, 0xFF01, 0xFF5E, 0};
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(kBase);
    const int lang = RowOfXLanguage(rex::system::EffectiveLanguage());
    for (int i = 0; i < kMenuTextCount; ++i)
      if (kMenuTextTable[lang][i])
        builder.AddText(kMenuTextTable[lang][i]);
    for (const char* n : kMenuLanguageNames)
      builder.AddText(n);
    if (lang == 8)
      builder.AddRanges(atlas->GetGlyphRangesJapanese());
    if (lang == 10 || lang == 11)
      builder.AddRanges(atlas->GetGlyphRangesChineseSimplifiedCommon());
    s_ranges.clear();
    builder.BuildRanges(&s_ranges);
    g_cjk_sizes_once = lang >= 8;
  }
  const ImWchar* kRanges = s_ranges.Data;
  const std::string& name = REXCVAR_GET(menu_font);
  auto is = [&](const char* n) { return rex::string::compare_case(name.c_str(), n) == 0; };
  ImFontConfig config;
  config.FontDataOwnedByAtlas = false;  // static data, or the system's shared memory
  config.OversampleH = 2;
  config.OversampleV = 1;
  const void* data = nullptr;
  int size = 0;
  float sizes[4] = {22.0f, 33.0f, 18.0f, 27.0f};  // the rows' and the smaller text's, for 720 and 1080 lines
  if (is("Pixel")) {
    // ProggyTiny (10 px) at 2x and 3x, whole pixels (the debug overlay's font).
    data = kMenuFontProggyTiny;
    size = int(sizeof(kMenuFontProggyTiny));
    config.OversampleH = 1;
    config.PixelSnapH = true;
    sizes[0] = 20.0f, sizes[1] = 30.0f, sizes[2] = 20.0f, sizes[3] = 30.0f;
  } else if (is("Roboto")) {
    data = kMenuFontRoboto, size = int(sizeof(kMenuFontRoboto));
  } else if (is("Karla")) {
    data = kMenuFontKarla, size = int(sizeof(kMenuFontKarla));
  } else if (is("Cousine")) {
    data = kMenuFontCousine, size = int(sizeof(kMenuFontCousine));
    sizes[0] = 20.0f, sizes[1] = 30.0f, sizes[2] = 17.0f, sizes[3] = 25.0f;
  }
#if REX_PLATFORM_SWITCH
  plInitialize(0 /* PlServiceType_User */);  // for the shared fonts (System, and the merged ones)
  if (!data) {  // System (the default): the console's shared font
    BlurPlFontData font{};
    if (plGetSharedFontByType(&font, 0 /* PlSharedFontType_Standard */) == 0 && font.address && font.size) {
      data = font.address;
      size = int(font.size);
    }
  }
#endif
  if (!data) {  // no system font (the PC): Roboto
    data = kMenuFontRoboto;
    size = int(sizeof(kMenuFontRoboto));
  }
  void* bytes = const_cast<void*>(data);
  g_size_main[0] = sizes[0], g_size_main[1] = sizes[1], g_size_text[0] = sizes[2], g_size_text[1] = sizes[3];
  // The console's fonts merged in for the glyphs the chosen one lacks (Cyrillic, CJK, the languages' own names): the
  // standard (Latin, Cyrillic, Japanese), Korean, and the two Chinese ones.
  auto add = [&](float px) {
    ImFont* f = atlas->AddFontFromMemoryTTF(bytes, size, px, &config, kRanges);
#if REX_PLATFORM_SWITCH
    for (int type : {0 /* Standard */, 4 /* KO */, 1 /* ChineseSimplified */, 3 /* ChineseTraditional */}) {
      BlurPlFontData shared{};
      if (plGetSharedFontByType(&shared, type) != 0 || !shared.address || !shared.size || shared.address == data)
        continue;
      ImFontConfig merge = config;
      merge.MergeMode = true;
      merge.FontDataOwnedByAtlas = false;
      merge.PixelSnapH = false;
      merge.OversampleH = 2;
      atlas->AddFontFromMemoryTTF(shared.address, int(shared.size), px, &merge, kRanges);
    }
#endif
    return f;
  };
  // Japanese and Chinese (thousands of characters) at the 720-line sizes only; a 1080-line display scales them.
  g_font_small = add(sizes[0]);
  g_font_small_text = add(sizes[2]);
  g_font_large = g_cjk_sizes_once ? g_font_small : add(sizes[1]);
  g_font_large_text = g_cjk_sizes_once ? g_font_small_text : add(sizes[3]);
}

// ---- The dialog -----------------------------------------------------------------------------------------------------

GameMenuDialog::GameMenuDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                               std::function<rex::Runtime*()> runtime,
                               std::function<rex::ui::ImmediateDrawer*()> immediate_drawer)
    : rex::ui::ImGuiDialog(drawer),
      config_path_(std::move(config_path)),
      runtime_(std::move(runtime)),
      immediate_drawer_(std::move(immediate_drawer)) {
  rex::ui::RegisterBind("bind_game_menu", "F8", "Open or close the game menu (A+B+X+Y+L+R on the Switch)",
                        [this] { Toggle(); });
}

GameMenuDialog::~GameMenuDialog() {
  rex::ui::UnregisterBind("bind_game_menu");
  if (state_ != State::kClosed)
    rex::ui::SetGameInputBlocked(false);
}

void GameMenuDialog::Toggle() {
  if (state_ == State::kClosed)
    Open();
  else if (state_ == State::kOpening || state_ == State::kOpen)
    BeginClose();
}

void GameMenuDialog::Open() {
  // The menu's language: the game's (the language key's choice, "System" the console's), as the game shows it: its own
  // data's language for that one (English when the game lacks it). The rows take it at their first open, and again
  // when it differs (the game's data may load after a first open).
  const int lang = UiLanguage();
  if (!loaded_ || lang != g_lang) {
    g_lang = lang;
    if (loaded_)
      Relabel();
  }
  LoadValues();
  rex::ui::SetGameInputBlocked(true);
  state_ = State::kOpening;
  scroll_to_row_ = true;
  REXLOG_INFO("game menu: open");
}

void GameMenuDialog::BeginClose() {
  // The heavy live settings (the resolutions, MSAA) take their last choice now, once.
  for (Setting& s : settings_)
    if (s.apply_on_close && s.dirty)
      Apply(s);
  Save();
  state_ = State::kClosing;
  REXLOG_INFO("game menu: closed");
}

int GameMenuDialog::UiLanguage() const {
  uint32_t lang = rex::system::EffectiveLanguage();
  rex::Runtime* runtime = runtime_ ? runtime_() : nullptr;
  if (runtime && runtime->kernel_state()) {
    const auto db = runtime->kernel_state()->title_xdbf();
    if (db.is_valid())
      lang = uint32_t(db.GetExistingLanguage(static_cast<rex::system::XLanguage>(lang)));
  }
  return RowOfXLanguage(lang);
}

void GameMenuDialog::Relabel() {
  auto copy = [](std::vector<Setting>& to, const std::vector<Setting>& from) {
    for (size_t i = 0; i < to.size() && i < from.size(); ++i) {
      to[i].label = from[i].label;
      to[i].help = from[i].help;
      for (size_t j = 0; j < to[i].choices.size() && j < from[i].choices.size(); ++j)
        to[i].choices[j].label = from[i].choices[j].label;
    }
  };
  copy(settings_, MakeSettings(config_path_.filename().string()));
  for (int p = 0; p < 4; ++p)
    copy(controls_[p], MakeControls(p));
}

void GameMenuDialog::LoadValues() {
  if (!loaded_) {
    settings_ = MakeSettings(config_path_.filename().string());
    for (int p = 0; p < 4; ++p)
      controls_[p] = MakeControls(p);
  }
  auto load = [&](Setting& s) {
    if (s.key == kResetKey)
      return;
    // A * setting keeps what was chosen for the next start; the others show what the game runs with now.
    if (loaded_ && (s.restart || s.dirty))
      return;
    const std::string raw = CurrentRaw(s);
    s.value = FindChoice(s, raw);
    if (s.value < 0)
      s.custom = raw;
    if (!loaded_)
      s.running = s.value;
  };
  for (Setting& s : settings_)
    load(s);
  for (auto& player : controls_)
    for (Setting& s : player)
      load(s);
  loaded_ = true;
}

void GameMenuDialog::Change(Setting& s, int step) {
  if (s.key == kResetKey) {
    for (auto& player : controls_)
      if (&player.back() == &s)
        for (Setting& b : player)
          if (b.key != kResetKey && b.value != b.default_choice) {
            b.value = b.default_choice;
            b.dirty = true;
            Apply(b);
          }
    return;
  }
  const int n = int(s.choices.size());
  if (n == 0)
    return;
  s.value = s.value < 0 ? (step > 0 ? 0 : n - 1) : (s.value + step % n + n) % n;
  s.dirty = true;
  if (!s.apply_on_close)
    Apply(s);
}

void GameMenuDialog::Apply(Setting& s) {
  if (s.restart || s.value < 0)
    return;  // read at start: written for the next one
  const Choice& c = s.choices[size_t(s.value)];
  if (s.key == "frame_cap") {
    BlurSetFrameCapLive(c.raw == "Off" ? 0 : std::atoi(c.raw.c_str()));
    return;
  }
  if (s.key == "menu_font") {  // the atlas made again before the next frame, with this font
    rex::cvar::SetFlagByName(s.key, c.raw);
    imgui_drawer()->RequestFontRebuild();
    return;
  }
  if (s.key == "input_menu_combo") {  // the driver's own copy (the key's string is read at start only), and the key
    rex::cvar::SetFlagByName(s.key, c.raw);  // (the row reads it back at the next opening: the first test showed the old)
#if REX_PLATFORM_SWITCH
    rex::input::nx::SetGameMenuCombo(c.raw);
#endif
    return;
  }
  if (!rex::cvar::SetFlagByName(s.key, c.raw))
    REXLOG_WARN("game menu: {} = {} not taken", s.key, c.raw);
#if REX_PLATFORM_SWITCH
  if (s.key.rfind("input_p", 0) == 0)
    rex::input::nx::RebuildButtonMaps();
#endif
}

void GameMenuDialog::Save() {
  std::vector<FileEdit> edits;
  auto collect = [&](Setting& s) {
    if (!s.dirty || s.key == kResetKey || s.value < 0)
      return;
    s.dirty = false;
    FileEdit e;
    e.key = s.key;
    if (!s.always_in_file && s.value == s.default_choice)
      e.remove = true;
    else
      e.toml = s.choices[size_t(s.value)].toml;
    edits.push_back(std::move(e));
  };
  for (Setting& s : settings_)
    collect(s);
  for (auto& player : controls_)
    for (Setting& s : player)
      collect(s);
  if (edits.empty())
    return;
  const bool ok = UpdateConfigFile(config_path_, edits);
  REXLOG_INFO("game menu: {} setting(s) written to {}{}", edits.size(), config_path_.string(), ok ? "" : ": FAILED");
  // rr6-nx (2026-11-21): no four_core_mode here (the base's threading_switch.cpp part is not in rr6-nx), so no row
  // for it and nothing to write at once.
}

rex::system::AchievementManager* GameMenuDialog::Achievements() const {
  rex::Runtime* runtime = runtime_ ? runtime_() : nullptr;
  return runtime && runtime->kernel_state() ? &runtime->kernel_state()->achievements() : nullptr;
}

bool GameMenuDialog::AnyButtonDown() const {
  for (ImGuiKey k : kGamepadKeys)
    if (ImGui::IsKeyDown(k))
      return true;
  return false;
}

int GameMenuDialog::RowCount() const {
  if (tab_ == kSettings)
    return int(settings_.size());
  if (tab_ == kAchievements) {
    rex::system::AchievementManager* manager = Achievements();
    return manager ? int(manager->ListAchievements().size()) : 0;
  }
  return int(controls_[tab_ - kP1].size());
}

void GameMenuDialog::OnDraw(ImGuiIO& io) {
  if (state_ == State::kClosed)
    return;
  if (state_ == State::kClosing) {
    // Nothing drawn; the game gets its controllers back once every button is up (the + that closed the menu too).
    if (!AnyButtonDown()) {
      rex::ui::SetGameInputBlocked(false);
      state_ = State::kClosed;
    }
    return;
  }
  if (state_ == State::kOpening && !AnyButtonDown())
    state_ = State::kOpen;  // the six buttons that opened it are up: from now on the presses are the menu's

  ImFont* font = MenuFont(io);
  if (font)
    ImGui::PushFont(font, MenuFontSize(io));
  const float scale = font ? 1.0f : std::max(1.0f, io.DisplaySize.y / 720.0f) * 1.55f;

  // ---- Input (the menu's own: the window takes no ImGui navigation) ----
  const int before_tab = tab_;
  if (state_ == State::kOpen) {
    if (Pressed(ImGuiKey_GamepadStart, ImGuiKey_Escape, ImGuiKey_None, false)) {
      if (font)
        ImGui::PopFont();
      BeginClose();
      return;
    }
    if (Pressed(ImGuiKey_GamepadL1, ImGuiKey_PageUp, ImGuiKey_None, false))
      tab_ = (tab_ + kTabCount - 1) % kTabCount;
    if (Pressed(ImGuiKey_GamepadR1, ImGuiKey_PageDown, ImGuiKey_None, false))
      tab_ = (tab_ + 1) % kTabCount;
    const int rows = RowCount();
    int& row = row_[tab_];
    if (rows > 0) {
      if (Pressed(ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp, ImGuiKey_UpArrow)) {
        row = (row + rows - 1) % rows;
        scroll_to_row_ = true;
      }
      if (Pressed(ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown, ImGuiKey_DownArrow)) {
        row = (row + 1) % rows;
        scroll_to_row_ = true;
      }
      row = std::clamp(row, 0, rows - 1);
      if (tab_ != kAchievements) {
        Setting& s = tab_ == kSettings ? settings_[size_t(row)] : controls_[tab_ - kP1][size_t(row)];
        // Confirm by the layout, as ImGui's own (ApplyEntry): the bottom button with the Xbox layout, else A.
        const ImGuiKey confirm = io.ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceRight : ImGuiKey_GamepadFaceDown;
        if (Pressed(ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft, ImGuiKey_LeftArrow))
          Change(s, -1);
        if (Pressed(ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight, ImGuiKey_RightArrow) ||
            Pressed(confirm, ImGuiKey_Enter, ImGuiKey_None, false))
          Change(s, +1);
      }
    }
  }
  if (tab_ != before_tab)
    scroll_to_row_ = true;

  // ---- The window ----
  // Sized to its contents (the user: "get rid of some white space at the bottom"): the tabs, the list in whole rows, the
  // footer; centred. The same height on every tab.
  SetTheme(REXCVAR_GET(menu_color), ImGui::GetTime());
  const float font_px = font ? MenuFontSize(io) : ImGui::GetFontSize() * scale;
  row_height_ = font_px * 1.75f;
  PickTextFont(io);
  const ImVec2 pad(22.0f, 12.0f);
  const float win_w = std::floor(io.DisplaySize.x * 0.88f);
  const float tabs_h = font_px * 2.4f + 6.0f;  // DrawTabs: the bar, the line under it, a gap
  const float footer = FooterHeight(win_w - 2.0f * pad.x);
  const float room = io.DisplaySize.y * 0.92f - 2.0f * pad.y - tabs_h - footer;
  const float list = std::max(row_height_, std::floor(room / row_height_) * row_height_);
  const float win_h = 2.0f * pad.y + tabs_h + list + footer + 2.0f;
  ImGui::SetNextWindowPos(ImVec2(std::floor((io.DisplaySize.x - win_w) * 0.5f), std::floor((io.DisplaySize.y - win_h) * 0.5f)),
                          ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_Always);
  ImGui::SetNextWindowFocus();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, BackgroundBlack());  // port 180: menu_background_opacity
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Border, kAccent);
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, kPanel);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, kAccentDim);
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kAccentDark);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoScrollWithMouse;
  if (ImGui::Begin("##blur_game_menu", nullptr, flags)) {
    if (!font)
      ImGui::SetWindowFontScale(scale);
    const float width = ImGui::GetContentRegionAvail().x;
    DrawTabs(width);
    // The Achievements tab has no help: its list takes the footer's help space (the window's height stays).
    const float footer_here = tab_ == kAchievements ? FooterKeysHeight() : footer;
    ImGui::BeginChild("##list", ImVec2(0.0f, list + footer - footer_here), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
    if (!font)
      ImGui::SetWindowFontScale(scale);
    if (tab_ == kSettings)
      DrawSettingsRows(settings_, false, 0);
    else if (tab_ == kAchievements)
      DrawAchievements();
    else
      DrawSettingsRows(controls_[tab_ - kP1], true, tab_ - kP1);
    ImGui::EndChild();
    DrawFooter(footer_here);
  }
  ImGui::End();
  ImGui::PopStyleVar(4);
  ImGui::PopStyleColor(8);
  if (font)
    ImGui::PopFont();
}

void GameMenuDialog::DrawTabs(float width) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float font = ImGui::GetFontSize();
  const float height = font * 1.9f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  // L and R at the ends, the tabs between them, as the mockup: names split by bars. The names in the rows' font when
  // they fit between L and R, else in the smaller text's (the console's first test: P3 and P4 ran off the right).
  const float hint_w = font * 1.6f;
  auto hint = [&](float x, const char* t) {
    const ImVec2 a(x, origin.y + height * 0.14f), b(x + hint_w, origin.y + height * 0.86f);
    dl->AddRect(a, b, U32(kAccentDim), 5.0f, 0, 2.0f);
    const ImVec2 ts = ImGui::CalcTextSize(t);
    dl->AddText(ImVec2(a.x + (hint_w - ts.x) * 0.5f, origin.y + (height - ts.y) * 0.5f), U32(kTextDim), t);
  };
  hint(origin.x, "L");
  hint(origin.x + width - hint_w, "R");
  const float room = width - 2.0f * (hint_w + font * 0.6f);
  ImFont* tab_font = ImGui::GetFont();
  float tab_size = font;
  float sep = font * 1.1f, pad = font * 0.45f;
  auto total_of = [&](ImFont* f, float size, float s) {
    float total = 0.0f;
    for (int i = 0; i < kTabCount; ++i)
      total += f->CalcTextSizeA(size, FLT_MAX, 0.0f, TabName(i).c_str()).x + (i ? s : 0.0f);
    return total;
  };
  if (total_of(tab_font, tab_size, sep) > room) {
    tab_font = g_text_font;
    tab_size = g_text_size;
    sep = tab_size * 1.1f;
    pad = tab_size * 0.45f;
  }
  const float total = total_of(tab_font, tab_size, sep);
  float x = origin.x + hint_w + font * 0.6f + std::max(0.0f, (room - total) * 0.5f);
  for (int i = 0; i < kTabCount; ++i) {
    if (i) {
      const ImVec2 bar(x + sep * 0.5f, origin.y + height * 0.22f);
      dl->AddLine(bar, ImVec2(bar.x, origin.y + height * 0.78f), U32(kAccentDim), 2.0f);
      x += sep;
    }
    const std::string name = TabName(i);
    const ImVec2 ts = tab_font->CalcTextSizeA(tab_size, FLT_MAX, 0.0f, name.c_str());
    const ImVec2 a(x - pad, origin.y + height * 0.1f), b(x + ts.x + pad, origin.y + height * 0.9f);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(i);
    ImGui::InvisibleButton("##tab", ImVec2(b.x - a.x, b.y - a.y));
    if (ImGui::IsItemClicked() && state_ == State::kOpen && tab_ != i) {
      tab_ = i;
      scroll_to_row_ = true;
    }
    ImGui::PopID();
    if (i == tab_)
      dl->AddRectFilled(a, b, U32(kAccent), 6.0f);
    dl->AddText(tab_font, tab_size, ImVec2(x, origin.y + (height - ts.y) * 0.5f),
                i == tab_ ? U32(kBlack, 1.0f / kBlack.w) : U32(kText), name.c_str());
    x += ts.x;
  }
  // The thick line under the tabs (with Rainbow, the hues along it, turning).
  if (g_rainbow) {
    constexpr int kParts = 12;
    const float x0 = origin.x - 4.0f, w = width + 8.0f;
    for (int i = 0; i < kParts; ++i) {
      const ImU32 c0 = U32(Hue(g_rainbow_hue + float(i) / kParts)), c1 = U32(Hue(g_rainbow_hue + float(i + 1) / kParts));
      dl->AddRectFilledMultiColor(ImVec2(x0 + w * i / kParts, origin.y + height + 2.0f),
                                  ImVec2(x0 + w * (i + 1) / kParts, origin.y + height + 6.0f), c0, c1, c1, c0);
    }
  } else {
    dl->AddRectFilled(ImVec2(origin.x - 4.0f, origin.y + height + 2.0f),
                      ImVec2(origin.x + width + 4.0f, origin.y + height + 6.0f), U32(kAccent));
  }
  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height + 6.0f + font * 0.5f));
  ImGui::Dummy(ImVec2(width, 0.0f));
}

void GameMenuDialog::DrawSettingsRows(std::vector<Setting>& rows, bool controls, int player) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float font = ImGui::GetFontSize();
  const float pad = font * 0.7f;
  const float value_x = width * 0.52f;
  const float value_w = width - value_x - pad;
  const float arrow_w = font * 1.6f;
  const bool by_position = rex::cvar::GetFlagByName("input_xbox_layout") == "true";
  int& selected = row_[tab_];
  for (size_t i = 0; i < rows.size(); ++i) {
    Setting& s = rows[i];
    const bool sel = int(i) == selected;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(int(i));
    ImGui::InvisibleButton("##row", ImVec2(width, row_height_));
    if (ImGui::IsItemClicked() && state_ == State::kOpen) {
      const float mx = ImGui::GetIO().MousePos.x - p.x;
      if (s.key != kResetKey && mx >= value_x && mx < value_x + arrow_w)
        Change(s, -1);
      else if (mx >= value_x + value_w - arrow_w || (sel && mx >= value_x) || s.key == kResetKey)
        Change(s, +1);
      selected = int(i);
    }
    if (sel && scroll_to_row_) {
      // Whole rows: the chosen one in the middle where the list allows.
      const int visible = std::max(1, int(ImGui::GetWindowHeight() / row_height_ + 0.01f));
      const int first = std::clamp(int(i) - visible / 2, 0, std::max(0, int(rows.size()) - visible));
      ImGui::SetScrollY(float(first) * row_height_);
      scroll_to_row_ = false;
    }
    ImGui::PopID();
    // The row: a band and a bar for the chosen one, a thin line under the others.
    const ImVec2 a(p.x, p.y + 2.0f), b(p.x + width, p.y + row_height_ - 2.0f);
    if (sel) {
      dl->AddRectFilled(a, b, U32(kAccentDark), 6.0f);
      dl->AddRectFilled(a, ImVec2(a.x + 5.0f, b.y), U32(kAccent), 2.0f);
    } else {
      dl->AddLine(ImVec2(a.x + pad, b.y + 1.0f), ImVec2(b.x - pad, b.y + 1.0f), U32(kAccentDim, 0.35f), 1.0f);
    }
    const float ty = p.y + (row_height_ - font) * 0.5f;
    std::string label = s.label;
    dl->AddText(ImVec2(p.x + pad * 1.6f, ty), U32(sel ? kText : kTextDim), label.c_str());
    if (s.restart) {
      const float lw = ImGui::CalcTextSize(label.c_str()).x;
      dl->AddText(ImVec2(p.x + pad * 1.6f + lw + font * 0.15f, ty), U32(kAccent), "*");
    }
    // The value, between its arrows.
    std::string value;
    if (s.key == kResetKey) {
      value = s.choices[0].label;
    } else if (s.value < 0) {
      value = s.custom;
    } else if (controls && s.value == 0) {
      // "Default (B)": the Switch button the layout gives this Xbox button.
      const XboxButton& xb = kXboxButtons[i < std::size(kXboxButtons) ? i : 0];
      value = TrS(C_DEFAULT_OF, ButtonName(by_position ? xb.by_position : xb.by_letter).c_str());
    } else {
      value = s.choices[size_t(s.value)].label;
    }
    const bool pending = s.restart && s.value != s.running;
    const bool warn = s.key == "four_core_mode" && s.value != s.default_choice;  // Off: the warning's colour
    const ImVec2 vs = ImGui::CalcTextSize(value.c_str());
    const float vx = p.x + value_x + (value_w - vs.x) * 0.5f;
    dl->AddText(ImVec2(vx, ty), U32(warn ? kWarn : pending ? kPending : (sel ? kAccent : kText)), value.c_str());
    if (sel && s.key != kResetKey) {
      const float cy = p.y + row_height_ * 0.5f, h = font * 0.32f;
      const float lx = p.x + value_x + arrow_w * 0.35f, rx = p.x + value_x + value_w - arrow_w * 0.35f;
      dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + h * 1.3f, cy - h), ImVec2(lx + h * 1.3f, cy + h), U32(kAccent));
      dl->AddTriangleFilled(ImVec2(rx, cy), ImVec2(rx - h * 1.3f, cy - h), ImVec2(rx - h * 1.3f, cy + h), U32(kAccent));
    }
    if (pending) {
      const char* tag = Tr(F_NEXT);
      const ImVec2 tsz = TextSize(tag);
      const float tx = std::min(vx + vs.x + font * 0.6f, p.x + width - tsz.x - pad);
      dl->AddText(g_text_font, g_text_size, ImVec2(tx, ty + (font - g_text_size) * 0.7f), U32(kPending, 0.85f), tag);
    }
  }
  (void)player;
}

void GameMenuDialog::DrawAchievements() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float font = ImGui::GetFontSize();
  const float pad = font * 0.7f;
  rex::system::AchievementManager* manager = Achievements();
  if (!manager) {
    ImGui::TextColored(kTextDim, "%s", Tr(A_NOT_LOADED));
    return;
  }
  if (!icons_) {
    rex::ui::ImmediateDrawer* immediate = immediate_drawer_ ? immediate_drawer_() : nullptr;
    rex::Runtime* runtime = runtime_ ? runtime_() : nullptr;
    if (immediate && runtime)
      icons_ = std::make_unique<rex::ui::AchievementIconCache>(immediate, runtime);
  }
  const auto list = manager->ListAchievements();
  int unlocked = 0, earned = 0, total = 0;
  for (const auto& a : list) {
    total += int(a.gamerscore);
    if (manager->IsUnlocked(a.id)) {
      ++unlocked;
      earned += int(a.gamerscore);
    }
  }
  if (list.empty()) {
    ImGui::TextColored(kTextDim, "%s", Tr(A_NONE));
    return;
  }
  // The summary and its bar.
  {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    char line[96];
    std::snprintf(line, sizeof(line), "%s", TrDD(A_UNLOCKED_OF, unlocked, int(list.size())).c_str());
    dl->AddText(ImVec2(p.x + pad, p.y), U32(kText), line);
    std::snprintf(line, sizeof(line), "%dG / %dG", earned, total);
    const float gw = ImGui::CalcTextSize(line).x;
    dl->AddText(ImVec2(p.x + width - pad - gw, p.y), U32(kAccent), line);
    const float by = p.y + font * 1.35f, bh = font * 0.32f;
    dl->AddRectFilled(ImVec2(p.x + pad, by), ImVec2(p.x + width - pad, by + bh), U32(kAccentDark), bh * 0.5f);
    const float frac = float(unlocked) / float(list.size());
    if (frac > 0.0f)
      dl->AddRectFilled(ImVec2(p.x + pad, by), ImVec2(p.x + pad + (width - 2.0f * pad) * frac, by + bh), U32(kAccent),
                        bh * 0.5f);
    ImGui::Dummy(ImVec2(width, font * 2.1f));
  }
  const float row_h = font * 2.9f;
  const float icon = row_h - font * 0.6f;
  int& selected = row_[tab_];
  ImGui::BeginChild("##ach", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar);
  ImDrawList* cl = ImGui::GetWindowDrawList();
  const float cw = ImGui::GetContentRegionAvail().x;
  for (size_t i = 0; i < list.size(); ++i) {
    const auto& a = list[i];
    const bool got = manager->IsUnlocked(a.id);
    const bool sel = int(i) == selected;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(int(i));
    ImGui::InvisibleButton("##a", ImVec2(cw, row_h));
    if (ImGui::IsItemClicked() && state_ == State::kOpen)
      selected = int(i);
    if (sel && scroll_to_row_) {
      const int visible = std::max(1, int(ImGui::GetWindowHeight() / row_h + 0.01f));
      const int first = std::clamp(int(i) - visible / 2, 0, std::max(0, int(list.size()) - visible));
      ImGui::SetScrollY(float(first) * row_h);
      scroll_to_row_ = false;
    }
    ImGui::PopID();
    const ImVec2 ra(p.x, p.y + 2.0f), rb(p.x + cw, p.y + row_h - 2.0f);
    if (sel) {
      cl->AddRectFilled(ra, rb, U32(kAccentDark), 6.0f);
      cl->AddRectFilled(ra, ImVec2(ra.x + 5.0f, rb.y), U32(kAccent), 2.0f);
    } else if (got) {
      cl->AddRectFilled(ra, rb, U32(kPanel), 6.0f);
    }
    // The game's own image (its default.xex's), dimmed while locked.
    const ImVec2 ia(p.x + pad * 1.4f, p.y + (row_h - icon) * 0.5f), ib(ia.x + icon, ia.y + icon);
    rex::ui::ImmediateTexture* tex = icons_ ? icons_->GetIcon(a) : nullptr;
    if (tex)
      cl->AddImage(reinterpret_cast<ImTextureID>(tex), ia, ib, ImVec2(0, 0), ImVec2(1, 1),
                   got ? IM_COL32_WHITE : IM_COL32(90, 110, 95, 255));
    else
      cl->AddRect(ia, ib, U32(kAccentDim), 4.0f, 0, 2.0f);
    cl->AddRect(ia, ib, U32(got ? kAccent : kAccentDim), 4.0f, 0, got ? 2.0f : 1.0f);
    const float tx = ib.x + pad;
    const float text_w = p.x + cw - pad - tx;
    char score[16];
    std::snprintf(score, sizeof(score), "%uG", a.gamerscore);
    const float sw = ImGui::CalcTextSize(score).x;
    cl->AddText(ImVec2(p.x + cw - pad - sw, p.y + font * 0.35f), U32(got ? kAccent : kTextDim), score);
    cl->AddText(ImVec2(tx, p.y + font * 0.35f), U32(got ? kText : kTextDim), a.label.c_str());
    const std::string& desc = got ? a.description : a.unachieved_description;
    const char* tag = got ? Tr(A_UNLOCKED) : nullptr;
    const float tw = tag ? TextSize(tag).x + pad : 0.0f;
    cl->AddText(g_text_font, g_text_size, ImVec2(tx, p.y + font * 1.55f), U32(kTextDim, got ? 1.0f : 0.8f), desc.c_str(),
                nullptr, text_w - tw);
    if (tag)
      cl->AddText(g_text_font, g_text_size, ImVec2(p.x + cw - pad - TextSize(tag).x, p.y + font * 1.55f), U32(kAccent), tag);
  }
  ImGui::EndChild();
}

namespace {
// The footer's texts, in the menu's language.
#define kNote Tr(F_NOTE)
#define kKeysList Tr(F_KEYS)
#define kKeysScroll Tr(F_KEYS_SCROLL)
}  // namespace

// The chosen row's help (and whether it is the four-core warning).
static const std::string* HelpOf(int tab, const int* rows, const std::vector<GameMenuDialog::Setting>& settings,
                                 const std::vector<GameMenuDialog::Setting>* controls, bool* warning) {
  if (warning)
    *warning = false;
  if (tab == 0 && rows[0] < int(settings.size())) {
    if (warning)
      *warning = settings[size_t(rows[0])].key == "four_core_mode";
    return &settings[size_t(rows[0])].help;
  }
  if (tab >= 2 && tab < 6 && rows[tab] < int(controls[tab - 2].size()))
    return &controls[tab - 2][size_t(rows[tab])].help;
  return nullptr;
}

// The footer: the chosen row's help, then the * note (the Settings tab) and the buttons, on one line when they fit
// side by side, else on two. Its height is the same on every tab (the window's too): the tallest help of any row, and
// the note's line whether shown or not. Little padding (the user: "get rid of some white space at the bottom").
float GameMenuDialog::FooterHeight(float width) const {
  const float font = ImGui::GetFontSize();
  const float line = g_text_size * 1.3f;
  float help_h = line;
  auto taller = [&](const std::vector<Setting>& rows) {
    for (const Setting& s : rows)
      if (!s.help.empty())
        help_h = std::max(help_h, TextSize(s.help.c_str(), width - font * 0.6f).y + g_text_size * 0.3f);
  };
  taller(settings_);
  taller(controls_[0]);
  const bool one_line = TextSize(kNote).x + TextSize(kKeysList).x + font * 1.5f <= width;
  return font * 0.55f + help_h + line * (one_line ? 1.0f : 2.0f);
}

float GameMenuDialog::FooterKeysHeight() const {
  return ImGui::GetFontSize() * 0.55f + g_text_size * 1.3f;
}

void GameMenuDialog::DrawFooter(float height) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float font = ImGui::GetFontSize();
  const float line = g_text_size * 1.3f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddLine(ImVec2(p.x - 4.0f, p.y + font * 0.2f), ImVec2(p.x + width + 4.0f, p.y + font * 0.2f), U32(kAccentDim), 2.0f);
  bool warning = false;
  const std::string* help = HelpOf(tab_, row_, settings_, controls_, &warning);
  if (help && !help->empty())
    dl->AddText(g_text_font, g_text_size, ImVec2(p.x + font * 0.3f, p.y + font * 0.45f),
                U32(warning ? kWarn : kTextDim), help->c_str(), nullptr, width - font * 0.6f);
  // The * note and the buttons, at the bottom.
  const char* keys = tab_ == kAchievements ? kKeysScroll : kKeysList;
  const bool note = tab_ == kSettings;
  const bool one_line = TextSize(kNote).x + TextSize(kKeysList).x + font * 1.5f <= width;
  const float keys_y = p.y + height - line + (line - g_text_size) * 0.5f;
  dl->AddText(g_text_font, g_text_size, ImVec2(p.x + width - TextSize(keys).x - font * 0.3f, keys_y), U32(kTextDim),
              keys);
  if (note)
    dl->AddText(g_text_font, g_text_size, ImVec2(p.x + font * 0.3f, one_line ? keys_y : keys_y - line), U32(kAccent),
                kNote);
  ImGui::Dummy(ImVec2(width, height));
}

}  // namespace blur
