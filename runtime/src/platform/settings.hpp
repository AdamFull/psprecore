#pragma once

// User settings, stored as key=value lines (ff7cc.ini). Unknown keys are ignored and missing keys keep their
// defaults, so the file survives version changes.

#include <filesystem>

namespace ff7cc::platform {

struct Settings {
    int scale = 2;            // Vulkan renderer internal resolution (multiples of 480x272)
    bool fullscreen = false;
    int volume = 100;         // master volume, percent
    bool frame_limit = true;  // run at the PSP's speed (off: as fast as possible)
    bool show_fps = false;

    static Settings load(const std::filesystem::path& path);  // defaults when the file is missing
    bool save(const std::filesystem::path& path) const;

    friend bool operator==(const Settings&, const Settings&) = default;
};

}  // namespace ff7cc::platform
