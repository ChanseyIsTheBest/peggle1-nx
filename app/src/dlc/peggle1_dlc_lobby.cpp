// Peggle 2: Windy and Jimmy in the multiplayer lobby (peggle1_dlc_mp_masters; docs/peggle1-notes.md, "Windy and
// Jimmy in the multiplayer lobby").
//
// The PS4 release offers its seven masters in multiplayer; the Xbox 360 executable builds the lobby's row of master
// buttons in a loop that stops at five (MultiplayerLobbyLayout, the function at 0x82 206AC0: `cmpwi cr6,r11,5` at
// its +0x40C), although the list it walks is the game's master list (a vector of 256-byte records, its first and
// last pointers at 0x82 D91F1C and +4, filled from MASTERS.JSON: seven with the PS4 content). tools/mp_masters.py
// puts a call of peggle1_mp_more_masters in front of that compare: while it answers true the loop goes on.
//
// The lobby then places each button on a placeholder of its layout, by button number. The layout is an object of
// PACKAGES/LAYOUTS.RTON (class MultiplayerLobbyLayout); the 360's has six placeholders for the row, master1 to
// master5 and master6 for the random button (16 bytes each: x, y in the game's design units, two delays), the PS4's
// has eight (master8 = the random button) and a masterScale the 360's class does not know:
//
//                 360 (x, y)                                      PS4 (x, y)
//   master1..8    (797,125) (1201,87) (1604,63) (2038,65)         (910,200) (1194,160) (1470,120) (1748,80)
//                 (2450,86) (2842,114)                            (2028,80) (2301,120) (2580,160) (2860,200)
//   pip1..4       (-335,-66) (-54,-66) (-335,240) (-54,240)       (-380,-100) (-125,-100) (-380,150) (-125,150)
//   optsButton    (430,-15)                                       (556,14)
//   masterScale   -                                               0.8
//
// (out/ps4_dlc/scripts/layouts_diff.py prints the two files side by side.) When the archive has more than five
// masters with lobby art, the port gives the 360's layout object the PS4's values as the lobby places its buttons
// (the second call tools/mp_masters.py adds, in the placing function at 0x82 207658), with two more placeholders
// from the game's heap for buttons 6 and 7, and makes each button of the row smaller the way the PS4's placing
// function does (its fn_288D90: after placing a button, GameAnim scale = masterScale * art lines / 2304; the 360's
// GameAnim keeps that factor in its PopAnim, see ScaleButton).
//
// The art comes with tools/ps4_dlc/build_mod.py --content ...,multiplayer. With an archive that lacks it (the
// Xbox 360 game's own, or a pack built without that option) the lobby is the 360's: five masters, its own layout.
//
// (Game functions are named through their __imp__ symbols and addresses are written with a space: tools/
// direct_calls.py reads every other 82xxxxxx word in the sources as a hooked function.)
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "dlc/peggle1_dlc_game.h"

REXCVAR_DEFINE_BOOL(peggle1_dlc_mp_masters, true, "Peggle1 DLC",
                    "Multiplayer lobby with a button for every master of MASTERS.JSON that has lobby art (Windy and "
                    "Jimmy with the PS4 content built with --content multiplayer) and the PS4's lobby layout: eight "
                    "smaller buttons in the row. Without such an archive, or off: the Xbox 360's lobby of five");

namespace {

constexpr uint32_t kMastersBegin = (0x416C8000u << 1) + 0x1F1Cu;
constexpr uint32_t kMastersEnd = kMastersBegin + 4;
constexpr uint32_t kLobbyMasters = 5;   // the executable's own count
constexpr uint32_t kMostMasters = 7;    // the PS4's

// The 360's layout object [its property list, the function at 0x82 202E08, and the placing function].
constexpr uint32_t kLayoutMaster1 = 20;      // master1 ... master6, 16 bytes each
constexpr uint32_t kLayoutPip1 = 116;        // pip1 ... pip4
constexpr uint32_t kLayoutOptions = 244;     // optsButton
// The button: MultiplayerMasterButton, a value of the lobby's map of buttons [the placing function, the button's
// constructor at 0x82 203FD8, the animation player's setter at 0x82 22B2C8, GameAnim's constructor].
constexpr uint32_t kButtonAnchor = 468;      // what the placing call gets
constexpr uint32_t kButtonPlayer = 480;      // its animation player
constexpr uint32_t kPlayerAnim = 104;        // the player's GameAnim
constexpr uint32_t kAnimPopAnim = 152;       // the GameAnim's PopAnim
constexpr uint32_t kPopAnimScale = 224;      // art lines / 2304 (and again at +228); the PS4's +0x198 of +0x110

// The PS4's layout (the table above).
constexpr int32_t kPs4Masters[8][2] = {{910, 200},  {1194, 160}, {1470, 120}, {1748, 80},
                                       {2028, 80},  {2301, 120}, {2580, 160}, {2860, 200}};
constexpr int32_t kPs4Pips[4][2] = {{-380, -100}, {-125, -100}, {-380, 150}, {-125, 150}};
constexpr int32_t kPs4Options[2] = {556, 14};
constexpr float kPs4MasterScale = 0.8f;

// How many masters get a button (5 = the 360's lobby, nothing changed), the layout the placing function was given
// last and the two placeholders that are not part of it.
uint32_t g_buttons = kLobbyMasters;
uint32_t g_layout = 0;
uint32_t g_extra = 0;

bool On() {
  static const bool on = REXCVAR_GET(peggle1_dlc_mp_masters);
  return on;
}

// The placeholder of the row's button (0 to g_buttons - 1 the masters, g_buttons the random button): the layout's
// own for the first five masters and the random button, the port's for masters 6 and 7.
uint32_t Placeholder(uint32_t button) {
  if (button < kLobbyMasters) return g_layout + kLayoutMaster1 + 16 * button;
  if (button < g_buttons) return g_extra + 16 * (button - kLobbyMasters);
  return g_layout + kLayoutMaster1 + 16 * kLobbyMasters;
}

bool IsRowPlaceholder(uint32_t placeholder) {
  if (g_buttons <= kLobbyMasters || !g_layout || !g_extra) return false;
  for (uint32_t button = 0; button <= g_buttons; ++button) {
    if (Placeholder(button) == placeholder) return true;
  }
  return false;
}

// Where the PS4's row has the button: its slot when the row is full (seven masters and the random button); with
// six masters the seven buttons sit half a slot in, between the PS4's positions.
void RowPosition(uint32_t button, int32_t& x, int32_t& y) {
  const double slot = button + (kMostMasters - g_buttons) * 0.5;
  const uint32_t a = uint32_t(slot), b = a + 1 < 8 ? a + 1 : a;
  const double t = slot - a;
  const auto mix = [t](int32_t p, int32_t q) {
    const double v = p + (q - p) * t;
    return int32_t(v < 0 ? v - 0.5 : v + 0.5);
  };
  x = mix(kPs4Masters[a][0], kPs4Masters[b][0]);
  y = mix(kPs4Masters[a][1], kPs4Masters[b][1]);
}

// The masters of the list that get a button: the first five, and those after them (to seven) whose lobby button
// the archive has (the id the button's constructor asks for: POPANIM_<the master's group>_MULTIPLAYER_LOBBY_BUTTON).
uint32_t CountButtons(const peggle1::dlc::Guest& g) {
  const uint32_t begin = g.u32(kMastersBegin), end = g.u32(kMastersEnd);
  const uint32_t count = end > begin ? (end - begin) >> 8 : 0;
  uint32_t buttons = kLobbyMasters;
  while (buttons < count && buttons < kMostMasters) {
    std::string group = g.String(begin + 256 * buttons + 128);
    for (char& c : group) c = char(std::toupper(static_cast<unsigned char>(c)));
    const std::string id = "POPANIM_" + group + "_MULTIPLAYER_LOBBY_BUTTON";
    if (group.empty() || !peggle1::dlc::HasResource(g, __imp__sub_82168D48, id)) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        REXLOG_WARN("[dlc] multiplayer lobby: the archive has no {} (built without --content multiplayer?): no "
                    "button for master {} and those after it", id, buttons + 1);
      }
      break;
    }
    ++buttons;
  }
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[dlc] multiplayer lobby: the master list has {} masters; buttons for {}", count, buttons);
  }
  return buttons;
}

// The PS4's masterScale on a button of the row.
void ScaleButton(const peggle1::dlc::Guest& g, uint32_t button) {
  using peggle1::dlc::Guest;
  if (!Guest::Plausible(button)) return;
  const uint32_t anim = g.u32(button + kButtonPlayer + kPlayerAnim);
  if (!Guest::Plausible(anim)) return;
  const uint32_t popanim = g.u32(anim + kAnimPopAnim);
  if (!Guest::Plausible(popanim)) return;
  g.setf(popanim + kPopAnimScale, kPs4MasterScale * peggle1::dlc::ArtScale(g));
}

}  // namespace

// The function that builds the lobby (the one with the loop). Before it runs: how many masters get a button, and
// the two placeholders the layout lacks (made once, from the game's heap). Its arguments are kept over the port's
// calls of the game.
extern "C" REX_FUNC(__imp__sub_82206AC0);
REX_HOOK_RAW(sub_82206AC0) {
  if (On()) {
    const peggle1::dlc::Guest g{ctx, base};
    const PPCRegister saved[] = {ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r9, ctx.r10};
    const uint64_t lr = ctx.lr;
    uint32_t buttons = CountButtons(g);
    if (buttons > kLobbyMasters && !g_extra) {
      const uint32_t made = g.Call(__imp__sub_82369B48, {32});
      if (peggle1::dlc::Guest::Plausible(made)) {
        std::memset(base + made, 0, 32);
        g_extra = made;
      } else {
        buttons = kLobbyMasters;
      }
    }
    g_buttons = buttons;
    PPCRegister* const back[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    for (int i = 0; i < 8; ++i) *back[i] = saved[i];
    ctx.lr = lr;
  }
  __imp__sub_82206AC0(ctx, base);
}

// Called from the lobby's loop with the index of the button it would make next (tools/mp_masters.py). C linkage:
// the recompiled functions are declared inside extern "C", and so is the declaration the step puts there.
extern "C" bool peggle1_mp_more_masters(uint8_t*, uint32_t next_index) {
  // below five the game's own compare continues the loop
  return On() && next_index >= kLobbyMasters && next_index < g_buttons;
}

// Called from the lobby's placing function (tools/mp_masters.py) for button numbers 5 and 6, after it has entered
// its own six placeholders and before it places anything on them: a placeholder for that button, or 0 for none.
// With more than five buttons the layout gets the PS4's values here.
extern "C" uint32_t peggle1_mp_placeholder(PPCContext* context, uint8_t* base, uint32_t layout, uint32_t number) {
  if (!On() || number < kLobbyMasters || number >= g_buttons || !g_extra) return 0;
  const peggle1::dlc::Guest g{*context, base};
  if (number == kLobbyMasters) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      const uint32_t m = layout + kLayoutMaster1, p = layout + kLayoutPip1, o = layout + kLayoutOptions;
      REXLOG_INFO("[dlc] multiplayer lobby: the layout had master1..6 ({}, {}) ({}, {}) ({}, {}) ({}, {}) ({}, {}) "
                  "({}, {}), pip1..4 ({}, {}) ({}, {}) ({}, {}) ({}, {}), optsButton ({}, {}); now the PS4's",
                  g.i32(m), g.i32(m + 4), g.i32(m + 16), g.i32(m + 20), g.i32(m + 32), g.i32(m + 36), g.i32(m + 48),
                  g.i32(m + 52), g.i32(m + 64), g.i32(m + 68), g.i32(m + 80), g.i32(m + 84), g.i32(p), g.i32(p + 4),
                  g.i32(p + 16), g.i32(p + 20), g.i32(p + 32), g.i32(p + 36), g.i32(p + 48), g.i32(p + 52), g.i32(o),
                  g.i32(o + 4));
    }
    g_layout = layout;
    for (uint32_t button = 0; button <= g_buttons; ++button) {
      int32_t x, y;
      RowPosition(button, x, y);
      g.set32(Placeholder(button), uint32_t(x));
      g.set32(Placeholder(button) + 4, uint32_t(y));
    }
    for (uint32_t i = 0; i < 4; ++i) {
      g.set32(layout + kLayoutPip1 + 16 * i, uint32_t(kPs4Pips[i][0]));
      g.set32(layout + kLayoutPip1 + 16 * i + 4, uint32_t(kPs4Pips[i][1]));
    }
    g.set32(layout + kLayoutOptions, uint32_t(kPs4Options[0]));
    g.set32(layout + kLayoutOptions + 4, uint32_t(kPs4Options[1]));
  }
  return g_extra + (number - kLobbyMasters) * 16;
}

// The lobby's placing call: (panel, the widget's anchor, placeholder). With the key on a button of the row gets the
// PS4's scale as it is placed, and a null placeholder is not passed on (the game reads through it).
extern "C" REX_FUNC(__imp__sub_822346A8);
REX_HOOK_RAW(sub_822346A8) {
  if (On()) {
    const uint32_t placeholder = ctx.r5.u32;
    if (!placeholder) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_WARN("[dlc] multiplayer lobby: a widget without a placeholder was left unplaced");
      }
      return;
    }
    if (IsRowPlaceholder(placeholder)) {
      ScaleButton(peggle1::dlc::Guest{ctx, base}, ctx.r4.u32 - kButtonAnchor);
    }
  }
  __imp__sub_822346A8(ctx, base);
}
