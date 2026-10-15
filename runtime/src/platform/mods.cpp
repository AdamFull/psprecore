#include "psprecore/platform/mods.hpp"

#include <algorithm>

#include "psprecore/io/path.hpp"
#include "core/foundation/platform/filesystem.h"

namespace psprecore::platform {

namespace {

nx::string_view trim(nx::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

nx::vector<ModInfo> find_mods(nx::string_view mods_dir) {
    nx::vector<ModInfo> mods;
    auto list = nx::fs::directory_entries(nx::string(mods_dir));
    if (!list.has_value()) return mods;
    for (const auto& e : list.value()) {
        if (e.type != nx::fs::file_type::directory) continue;
        ModInfo m;
        m.path = e.path;
        m.name = file_name(e.path);
        if (const auto ini = nx::fs::file_read_text(path_join(e.path, "mod.ini")); ini.has_value()) {
            nx::string_view rest = ini.value();
            if (rest.starts_with("\xEF\xBB\xBF")) rest.remove_prefix(3);  // a UTF-8 BOM
            while (!rest.empty()) {
                const std::size_t end = rest.find('\n');
                const nx::string_view line = trim(rest.substr(0, end));
                rest.remove_prefix(end == nx::string_view::npos ? rest.size() : end + 1);
                const std::size_t eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == nx::string_view::npos) continue;
                const nx::string_view key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
                if (key == "name" && !value.empty()) m.name = nx::string(value);
                else if (key == "language") m.language = nx::string(value);
            }
        }
        mods.push_back(std::move(m));
    }
    std::sort(mods.begin(), mods.end(), [](const ModInfo& a, const ModInfo& b) { return a.path < b.path; });
    return mods;
}

nx::vector<nx::string> active_mods(const nx::vector<ModInfo>& mods, nx::string_view language) {
    nx::vector<nx::string> active;
    for (const auto& m : mods)
        if (m.language.empty() || m.language == language) active.push_back(m.path);
    return active;
}

nx::vector<nx::string> mod_languages(const nx::vector<ModInfo>& mods) {
    nx::vector<nx::string> languages;
    for (const auto& m : mods)
        if (!m.language.empty() && std::find(languages.begin(), languages.end(), m.language) == languages.end())
            languages.push_back(m.language);
    return languages;
}

}  // namespace psprecore::platform
