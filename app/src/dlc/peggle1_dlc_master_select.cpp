// Peggle 2: Windy and Jimmy on the single-player "Select a Master" screen (peggle1_dlc_master_select;
// docs/peggle1-notes.md, "Select a Master with seven masters").
//
// The screen (SingleplayerMasterSelectScene; it opens when a level that was beaten is chosen again) makes one button
// per master of the game's list and one random button, and places them from its layout (MasterSelectLayout in
// PACKAGES/LAYOUTS.RTON): button i of the first `main row count` at mainRowOrigin + i * mainRowStep, the others at
// altRowOrigin + (i - count) * altRowStep. The Xbox 360 executable takes the count as "all buttons but the last"
// once, in a static (the placing function at 0x82 1F8AB0: `static int n = buttons - 1`), and its layout has
// altRowStep = (0, 0): five masters in a row, the random button alone under them. With seven masters the row runs
// off the screen.
//
// The PS4 gives each button a row and a column when it makes them (its constructor fn_2697F0: the row goes up and
// the column back to 0 at the master named "Fairy", the random button follows in that row), and its layout has
// altRowOrigin = (565, 1163), altRowStep = (465, 0) where the 360's has (1255, 1163), (0, 0): five masters above,
// Windy, Jimmy and the random button below, half a step in.
//
// Here, when the screen has more than six buttons: the static count is set to five before the placing function
// reads it, and the layout object gets the PS4's two values. (The buttons' pictures are the archive's:
// tools/ps4_dlc/build_mod.py --content ...,select. Without them the game draws Bjorn's for a master it has none
// for.)
//
// The side picture of a master is an animation with labels per costume (1_in, 1_idle, 1_out, 1_locked_in, ...),
// played through a state machine of 32 states: the 360 makes the states of four costumes, the PS4 (64 states) of
// six. tools/costume_states.py changes the three recompiled functions concerned so that the fifth and the sixth
// costume get their unlocked states in the room that is left; peggle1_costume_count below is the loop count it
// asks for: 6 when the archive has the PS4's animations (they come with Windy's, same build step), else the
// 360's 4, whose animations have no labels for more.
//
// (Game functions are named through their __imp__ symbols and addresses are written with a shift: tools/
// direct_calls.py reads every other 82xxxxxx word in the sources as a hooked function.)
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "dlc/peggle1_dlc_game.h"

REXCVAR_DECLARE(bool, peggle1_dlc_log);
REXCVAR_DEFINE_BOOL(peggle1_dlc_master_select, true, "Peggle1 DLC",
                    "The single-player Select a Master screen with more than five masters as on the PS4: five in "
                    "the row, the others and the random button in a second row. Off: the Xbox 360's one row");

extern "C" {
REX_FUNC(__imp__sub_821F8AB0);  // SingleplayerMasterSelectScene: place the buttons (r3 this)
REX_FUNC(__imp__sub_82163B08);  // is the layout reference set (r3 reference)
REX_FUNC(__imp__sub_821F96B0);  // reference -> MasterSelectLayout
}

namespace {

constexpr uint32_t kStatics = 0x416C8000u << 1;        // 0x82 D90000: the base its statics are addressed from
constexpr uint32_t kMainRowCount = kStatics + 10132;   // static int: buttons in the first row
constexpr uint32_t kMainRowGuard = kStatics + 10136;   // bit 0: the static is initialised
constexpr uint32_t kSceneLayout = 160;                 // the scene's reference to its layout
constexpr uint32_t kSceneButtons = 388;                // size of its map of buttons (masters + the random one)
constexpr uint32_t kLayoutAltRowOrigin = 36;           // Point (x, y)   [the layout's property list, 0x82 1F6AC0]
constexpr uint32_t kLayoutAltRowStep = 44;
constexpr uint32_t kFirstRow = 5;
constexpr int32_t kPs4AltRowOrigin[2] = {565, 1163};   // the PS4's LAYOUTS.RTON
constexpr int32_t kPs4AltRowStep[2] = {465, 0};

}  // namespace

// Called from the recompiled sub_821F8390 (0x82 1F8390: the states of a master's side picture) for its loop count.
// The registers the function has loaded at that point are kept.
extern "C" uint32_t peggle1_costume_count(PPCContext* context, uint8_t* base) {
  static const bool on = REXCVAR_GET(peggle1_dlc_master_select);
  if (!on) return 4;
  PPCContext& ctx = *context;
  const peggle1::dlc::Guest g{ctx, base};
  const PPCRegister saved[] = {ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r9, ctx.r10};
  const uint64_t lr = ctx.lr;
  const bool six = peggle1::dlc::HasResource(g, __imp__sub_82168D48, "POPANIM_MASTERS_WENDY_MASTER_SELECT_COSTUMES");
  PPCRegister* const back[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  for (int i = 0; i < 8; ++i) *back[i] = saved[i];
  ctx.lr = lr;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("[dlc] master select: costume pictures with states for {} costumes{}", six ? 6 : 4,
                six ? "" : " (the archive has no POPANIM_MASTERS_WENDY_MASTER_SELECT_COSTUMES: built without "
                           "--content select?)");
  }
  return six ? 6 : 4;
}

REX_HOOK_RAW(sub_821F8AB0) {
  static const bool on = REXCVAR_GET(peggle1_dlc_master_select);
  const uint32_t self = ctx.r3.u32;
  if (on && peggle1::dlc::Guest::Plausible(self)) {
    const peggle1::dlc::Guest g{ctx, base};
    const uint32_t buttons = g.u32(self + kSceneButtons);
    if (buttons > kFirstRow + 1 && buttons <= 16) {
      g.set32(kMainRowCount, kFirstRow);
      g.set32(kMainRowGuard, g.u32(kMainRowGuard) | 1);
      const uint64_t lr = ctx.lr;
      if (g.Call(__imp__sub_82163B08, {self + kSceneLayout}) & 0xFF) {
        const uint32_t layout = g.Call(__imp__sub_821F96B0, {self + kSceneLayout});
        if (peggle1::dlc::Guest::Plausible(layout)) {
          static bool logged = false;
          if (!logged) {
            logged = true;
            REXLOG_INFO("[dlc] master select: {} buttons; five in the first row, the layout's second row "
                        "({}, {}) step ({}, {}) set to the PS4's ({}, {}) step ({}, {})", buttons,
                        g.i32(layout + kLayoutAltRowOrigin), g.i32(layout + kLayoutAltRowOrigin + 4),
                        g.i32(layout + kLayoutAltRowStep), g.i32(layout + kLayoutAltRowStep + 4),
                        kPs4AltRowOrigin[0], kPs4AltRowOrigin[1], kPs4AltRowStep[0], kPs4AltRowStep[1]);
          }
          g.set32(layout + kLayoutAltRowOrigin, uint32_t(kPs4AltRowOrigin[0]));
          g.set32(layout + kLayoutAltRowOrigin + 4, uint32_t(kPs4AltRowOrigin[1]));
          g.set32(layout + kLayoutAltRowStep, uint32_t(kPs4AltRowStep[0]));
          g.set32(layout + kLayoutAltRowStep + 4, uint32_t(kPs4AltRowStep[1]));
        }
      }
      ctx.lr = lr;
      ctx.r3.u64 = self;
    }
  }
  __imp__sub_821F8AB0(ctx, base);
  if (on && REXCVAR_GET(peggle1_dlc_log)) {
    static int shown = 0;
    if (shown < 6) {
      ++shown;
      const peggle1::dlc::Guest g{ctx, base};
      REXLOG_INFO("[dlc] master select: placed; {} buttons, first row count {}, guard {:08X}",
                  peggle1::dlc::Guest::Plausible(self) ? g.u32(self + kSceneButtons) : 0, g.i32(kMainRowCount),
                  g.u32(kMainRowGuard));
    }
  }
}
