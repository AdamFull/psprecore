#pragma once

// User settings, stored as key=value lines (<game>.ini). Unknown keys are ignored and missing keys keep their
// defaults, so the file survives version changes.

#include "psprecore/platform/display.hpp"
#include "core/foundation/platform/filesystem.h"

namespace psprecore::platform {

struct Settings {
    bool software_renderer = false;  // the software (reference) renderer instead of Vulkan
    int scale = 2;            // Vulkan renderer internal resolution (multiples of 480x272)
    int msaa = 1;             // Vulkan renderer samples per pixel: 1 (off), 2, 4, 8
    bool fullscreen = false;
    int volume = 100;         // master volume, percent
    bool frame_limit = true;  // run at the PSP's speed (off: as fast as possible)
    bool vsync = true;        // wait for the display's refresh when presenting
    bool frame_skip = true;   // skip drawing frames when behind real time (the game keeps its speed)
    bool refresh_60hz = true; // Android: run the panel at 60 Hz (the game's rate) instead of the device's maximum
    bool show_fps = false;
    ButtonGlyphs button_glyphs = ButtonGlyphs::Auto;  // Auto: the last device used
    nx::string language;  // the language pack to use (psprecore/platform/mods.hpp); empty: the game's own

    static Settings load(nx::fs::path_view path);  // defaults when the file is missing
    bool save(nx::fs::path_view path) const;

    friend bool operator==(const Settings&, const Settings&) = default;
};

}  // namespace psprecore::platform
