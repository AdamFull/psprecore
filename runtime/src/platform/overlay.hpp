#pragma once

// The frontend's Dear ImGui overlay: the pause menu (settings), the save/load slot picker that stands in for
// the PSP's savedata dialog, and an optional FPS counter. draw() is installed as the Display's UI callback.

#include <cstdint>
#include <functional>

#include "platform/settings.hpp"
#include "core/foundation/containers/map.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore::platform {

class Display;

class Overlay {
public:
    struct Hooks {
        std::function<void(const Settings&)> apply;  // called whenever a setting changes
        std::function<void()> quit;
        bool renderer_choice = true;                 // false: fixed by the command line (--renderer)
        bool gpu_renderer = true;                    // the renderer in use when the choice is fixed
    };
    Overlay(Display& display, Settings& settings, Hooks hooks);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    void draw();

    bool menu_open() const { return menu_open_; }
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
    bool menu_appearing_ = false;
    bool dialog_appearing_ = true;
    int overwrite_slot_ = -1;
    nx::map<nx::string, std::uint64_t> icons_;  // ICON0.PNG path -> texture (0: failed to load)
};

}  // namespace psprecore::platform
