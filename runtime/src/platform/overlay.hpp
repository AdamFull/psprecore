#pragma once

// The frontend's Dear ImGui overlay: the pause menu (settings), the save/load slot picker that stands in for
// the PSP's savedata dialog, and an optional FPS counter. draw() is installed as the Display's UI callback.

#include <array>
#include <cstdint>

#include "platform/settings.hpp"
#include "core/foundation/core/callable.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore::platform {

class Display;

class Overlay {
public:
    struct Hooks {
        nx::unique_function<void(const Settings&)> apply;  // called whenever a setting changes
        nx::unique_function<void()> quit;
        bool renderer_choice = true;                 // false: fixed by the command line (--renderer)
        bool gpu_renderer = true;                    // the renderer in use when the choice is fixed
        nx::vector<nx::string> languages;            // the language packs installed (psprecore/platform/mods.hpp)
        nx::string active_language;                  // the one in use since the start (empty: the game's own)
    };
    Overlay(Display& display, Settings& settings, Hooks hooks);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    void draw();

    bool menu_open() const { return menu_open_; }
    // "Hold <Circle> to skip" in the corner, in the game's text style when it has one (rt::gpu::ui_font): `alpha` 0
    // hides it, `progress` 0..1 fills its gauge.
    void set_skip_prompt(float alpha, float progress) {
        skip_alpha_ = alpha;
        skip_progress_ = progress;
    }
    // While a skipped movie is fast-forwarded: "Skipping" in place of the prompt.
    void set_skipping(bool skipping) { skipping_ = skipping; }
    void open_menu();
    void close_menu();
    // Escape/F1/Guide: cancels an open save dialog, otherwise toggles the menu.
    void handle_menu_request();
    // True while the overlay owns the controller (menu or save dialog); the game sees no input then.
    bool wants_input() const;

private:
    void draw_menu();
    void draw_save_dialog();
    void draw_fps();
    void draw_skip_prompt();
    bool draw_game_skip_prompt();  // false until the game's font is available
    bool load_game_font();
    // Gamepad glyphs in the overlay: with a gamepad connected, or when the glyphs were chosen in the settings.
    bool pad_glyphs() const;
    // A button; with pad_glyphs(), showing the face button that presses it (the focused item takes Cross).
    bool button(const char* label, std::uint32_t pad_button);
    void pad_hint(std::uint32_t pad_button, const char* text);
    void release_icons();
    std::uint64_t icon(const nx::string& path);

    Display& display_;
    Settings& settings_;
    Hooks hooks_;
    bool menu_open_ = false;
    float skip_alpha_ = 0.0f, skip_progress_ = 0.0f;
    bool skipping_ = false;
    bool menu_appearing_ = false;
    bool dialog_appearing_ = true;
    int overwrite_slot_ = -1;
    nx::map<nx::string, std::uint64_t> icons_;  // ICON0.PNG path -> texture (0: failed to load)
    // The game's font (rt::gpu::ui_font) as a texture, its glyphs' ink (left column, width), and its button icon
    // redrawn as Circle for the current glyph style.
    struct GameFont {
        std::uint64_t texture = 0, icon = 0;
        int width = 0, height = 0, icon_glyphs = -1;
        nx::vector<std::uint32_t> sheet;
        std::array<std::uint8_t, 128> left{}, ink{};
    } game_font_;
};

}  // namespace psprecore::platform
