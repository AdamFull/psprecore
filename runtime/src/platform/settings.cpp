#include "platform/settings.hpp"

#include <algorithm>
#include <charconv>

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore::platform {

namespace {

int to_int(nx::string_view v, int fallback) {
    int out = fallback;
    std::from_chars(v.data(), v.data() + v.size(), out);
    return out;
}

constexpr const char* kGlyphNames[] = {"auto", "playstation", "xbox", "nintendo", "keyboard"};

ButtonGlyphs to_glyphs(nx::string_view v) {
    for (int i = 0; i < 5; ++i)
        if (v == kGlyphNames[i]) return static_cast<ButtonGlyphs>(i);
    return ButtonGlyphs::Auto;
}

}  // namespace

Settings Settings::load(nx::fs::path_view path) {
    Settings s;
    const auto text = nx::fs::file_read_text(path);
    if (!text.has_value()) return s;
    nx::string_view rest = text.value();
    while (!rest.empty()) {
        const std::size_t end = std::min(rest.find('\n'), rest.size());
        nx::string_view line = rest.substr(0, end);
        rest.remove_prefix(std::min(end + 1, rest.size()));
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == nx::string_view::npos) continue;
        const nx::string_view key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "renderer") s.software_renderer = value == "software";
        else if (key == "scale") s.scale = std::clamp(to_int(value, s.scale), 1, 8);
        else if (key == "msaa") s.msaa = std::clamp(to_int(value, s.msaa), 1, 8);
        else if (key == "fullscreen") s.fullscreen = to_int(value, 0) != 0;
        else if (key == "volume") s.volume = std::clamp(to_int(value, s.volume), 0, 100);
        else if (key == "frame_limit") s.frame_limit = to_int(value, 1) != 0;
        else if (key == "show_fps") s.show_fps = to_int(value, 0) != 0;
        else if (key == "vsync") s.vsync = to_int(value, 1) != 0;
        else if (key == "refresh_60hz") s.refresh_60hz = to_int(value, 1) != 0;
        else if (key == "button_glyphs") s.button_glyphs = to_glyphs(value);
    }
    return s;
}

bool Settings::save(nx::fs::path_view path) const {
    const nx::string text = nx::format("# Settings\nrenderer={}\nscale={}\nmsaa={}\nfullscreen={}\nvolume={}\n"
                                       "frame_limit={}\nvsync={}\nrefresh_60hz={}\nshow_fps={}\nbutton_glyphs={}\n",
                                       software_renderer ? "software" : "vulkan", scale, msaa, int{fullscreen}, volume,
                                       int{frame_limit}, int{vsync}, int{refresh_60hz}, int{show_fps},
                                       kGlyphNames[static_cast<int>(button_glyphs)]);
    return nx::fs::file_write_text(path, text).has_value();
}

}  // namespace psprecore::platform
