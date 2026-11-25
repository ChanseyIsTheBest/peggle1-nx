// blur-nx (2026-11-18, the user: "make an imgui overlay opened up with the button combination of A+B+X+Y+L+R and
// closed with the start button the game doesn't accept input when the overlay is up ... make it look nice with green
// and black colours. any settings that can't be changed during the game running is marked with a *"): the game menu.
//
// Tabs: Settings (the player's file's nine keys and rumble), Achievements (the game's own list, names, gamerscores and
// images, read from its default.xex at run time: nothing of the game's is in the NRO), P1 to P4 Controls (the Switch
// buttons that press each Xbox 360 button, input_p1_* ... input_p4_*).
//
// - Opened by A+B+X+Y+L+R held together (switch_input_driver.cpp sends F8, this dialog's bind; F8 on the PC too).
//   The six buttons still held are ignored until they are released.
// - Closed by + (Start; Escape or F8 on the PC). The game sees untouched controllers from the opening until every
//   button is released after the close (rex::ui::GameInputBlocked), so the + that closes it does not pause the game.
// - Up / Down choose, Left / Right (or the confirm button) change, L / R change tabs; the touch screen works too.
// - Settings the running game can take apply at once; the others (marked *) are written for the next start. The
//   player's file (blur.toml, or the test builds' renut.toml) is written when the menu closes, key by key: the rest of
//   the file is kept as it is.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>

struct ImFont;
struct ImFontAtlas;
struct ImGuiIO;

namespace rex {
class Runtime;
namespace system {
class AchievementManager;
}  // namespace system
namespace ui {
class AchievementIconCache;
class ImmediateDrawer;
}  // namespace ui
}  // namespace rex

namespace blur {

// The menu's fonts (the console's own shared font at two sizes, on the Switch); from the app's OnConfigureFonts.
void AddGameMenuFonts(ImFontAtlas* atlas);

class GameMenuDialog : public rex::ui::ImGuiDialog {
 public:
  // The runtime and the immediate drawer are asked for when drawn: the dialogs are made before the runtime (the
  // console's first test showed "not loaded" for the achievements: the runtime given then was null).
  GameMenuDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                 std::function<rex::Runtime*()> runtime, std::function<rex::ui::ImmediateDrawer*()> immediate_drawer);
  ~GameMenuDialog() override;

  // The F8 bind (the UI thread): opens the menu, or closes it when open.
  void Toggle();
  bool WantsFrame() const override { return state_ != State::kClosed; }

  // One choice of a setting: its text, its value as the key takes it (SetFlagByName) and as the file writes it.
  struct Choice {
    std::string label, raw, toml;
  };
  struct Setting {
    std::string label, key, help;
    bool restart = false;          // * (read at start): written for the next start, not applied
    bool always_in_file = true;    // one of the player's file's keys; else written only when not the default
    bool apply_on_close = false;   // live, but taken when the menu closes (a heavy change: the render targets)
    std::vector<Choice> choices;
    int value = 0;                 // the choice shown (-1: a value of the file's that is none of them, `custom`)
    int running = 0;               // the choice the game runs with (a * setting's value at start)
    int default_choice = 0;
    std::string custom;            // the raw value when value is -1
    bool dirty = false;
  };

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum class State { kClosed, kOpening, kOpen, kClosing };
  enum Tab { kSettings, kAchievements, kP1, kP2, kP3, kP4, kTabCount };

  void Open();
  void BeginClose();
  void LoadValues();
  void Change(Setting& s, int step);
  void Apply(Setting& s);
  void Save();
  bool AnyButtonDown() const;
  int UiLanguage() const;  // the game's language's row in game_menu_text.h
  void Relabel();          // the rows' texts again in the menu's language
  rex::system::AchievementManager* Achievements() const;
  int RowCount() const;

  void DrawTabs(float width);
  void DrawSettingsRows(std::vector<Setting>& rows, bool controls, int player);
  void DrawAchievements();
  void DrawFooter(float height);
  float FooterHeight(float width) const;
  float FooterKeysHeight() const;  // the footer with the buttons' line alone (the Achievements tab)

  std::filesystem::path config_path_;
  std::function<rex::Runtime*()> runtime_;
  std::function<rex::ui::ImmediateDrawer*()> immediate_drawer_;
  std::unique_ptr<rex::ui::AchievementIconCache> icons_;

  State state_ = State::kClosed;
  int tab_ = kSettings;
  int row_[kTabCount] = {};
  bool scroll_to_row_ = false;
  std::vector<Setting> settings_;
  std::vector<Setting> controls_[4];
  bool loaded_ = false;
  float row_height_ = 0.0f;
};

// banjo-nx (2026-11-19, the user: "the menu ... to have all the current items in the toml exposed to the user from
// 1.0.2"; a handoff for the base): a game's own rows on the Settings tab. The hook gets the rows as the base makes them
// (each time they are made: the menu's language may have changed), the menu's language row (game_menu_text.h:
// 0 English ... 11 Chinese (Traditional)) and the player's file's name, and may change, add, remove or reorder rows.
// Set once, before the menu is first drawn (the app's OnCreateDialogs). None set: the base's rows.
using GameMenuRowsHook = void (*)(std::vector<GameMenuDialog::Setting>& settings, int language_row,
                                  const std::string& file);
void SetGameMenuRows(GameMenuRowsHook hook);
// The menu's own text (game_menu_text.h's MenuText ids: On, Off, System ...) in the menu's language.
const char* GameMenuText(int id);
// The Settings tab's rows as the menu makes them in a language row (game hook included), for a check without the UI.
std::vector<GameMenuDialog::Setting> GameMenuSettingsRows(int language_row, const std::string& file);
// The value the menu shows for a row's key now (as the row's choices' raw values are written).
std::string GameMenuCurrentRaw(const GameMenuDialog::Setting& s);

}  // namespace blur
