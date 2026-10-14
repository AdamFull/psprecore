#include "platform/settings.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string>
#include <string_view>

namespace ff7cc::platform {

namespace {

int to_int(std::string_view v, int fallback) {
    int out = fallback;
    std::from_chars(v.data(), v.data() + v.size(), out);
    return out;
}

}  // namespace

Settings Settings::load(const std::filesystem::path& path) {
    Settings s;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string_view key = std::string_view(line).substr(0, eq), value = std::string_view(line).substr(eq + 1);
        if (key == "renderer") s.software_renderer = value == "software";
        else if (key == "scale") s.scale = std::clamp(to_int(value, s.scale), 1, 8);
        else if (key == "fullscreen") s.fullscreen = to_int(value, 0) != 0;
        else if (key == "volume") s.volume = std::clamp(to_int(value, s.volume), 0, 100);
        else if (key == "frame_limit") s.frame_limit = to_int(value, 1) != 0;
        else if (key == "show_fps") s.show_fps = to_int(value, 0) != 0;
    }
    return s;
}

bool Settings::save(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::trunc);
    out << "# FF7CCRECOMP settings\n"
        << "renderer=" << (software_renderer ? "software" : "vulkan") << '\n'
        << "scale=" << scale << '\n'
        << "fullscreen=" << int{fullscreen} << '\n'
        << "volume=" << volume << '\n'
        << "frame_limit=" << int{frame_limit} << '\n'
        << "show_fps=" << int{show_fps} << '\n';
    return static_cast<bool>(out);
}

}  // namespace ff7cc::platform
