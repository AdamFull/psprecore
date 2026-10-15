#include "psprecore/gpu/texture_replacement.hpp"

#include <algorithm>
#include <cstdio>

#include "psprecore/runtime/hle.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"

namespace psprecore::rt::gpu {

namespace {

struct Entry {
    nx::string path;
    bool loaded = false;
    ReplacementImage image;  // empty if it could not be read
};

nx::hash_map<std::uint64_t, Entry> g_replacements;

nx::string_view file_name(nx::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return slash == nx::string_view::npos ? path : path.substr(slash + 1);
}

bool parse_hash(nx::string_view name, std::uint64_t& hash) {
    if (name.size() < 16) return false;
    hash = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        const char c = name[i];
        const int d = c >= '0' && c <= '9'   ? c - '0'
                      : c >= 'a' && c <= 'f' ? c - 'a' + 10
                      : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                             : -1;
        if (d < 0) return false;
        hash = hash << 4 | static_cast<std::uint64_t>(d);
    }
    return true;
}

}  // namespace

void scan_texture_replacements(nx::string_view mods_dir) {
    g_replacements.clear();
    auto mods = nx::fs::directory_entries(nx::string(mods_dir));
    if (!mods.has_value()) return;
    nx::vector<nx::string> folders;
    for (const auto& e : mods.value())
        if (e.type == nx::fs::file_type::directory) folders.push_back(e.path);
    std::sort(folders.begin(), folders.end());
    for (const auto& folder : folders) {
        auto files = nx::fs::directory_entries(nx::format("{}/textures", folder));
        if (!files.has_value()) continue;
        for (const auto& f : files.value()) {
            const nx::string_view name = file_name(f.path);
            std::uint64_t hash = 0;
            if (f.type != nx::fs::file_type::regular || !name.ends_with(".png") || !parse_hash(name, hash)) continue;
            g_replacements[hash] = Entry{f.path};
        }
    }
    if (!g_replacements.empty())
        std::fprintf(stderr, "textures: %zu replacements in %s\n", g_replacements.size(), nx::string(mods_dir).c_str());
}

bool have_texture_replacements() { return !g_replacements.empty(); }

const ReplacementImage* texture_replacement(std::uint64_t content_hash) {
    if (g_replacements.empty()) return nullptr;
    const auto it = g_replacements.find(content_hash);
    if (it == g_replacements.end()) return nullptr;
    Entry& e = it->second;
    if (!e.loaded) {
        e.loaded = true;
        if (!read_png(e.path, e.image.rgba, e.image.width, e.image.height) || e.image.width > 2048 ||
            e.image.height > 2048) {
            std::fprintf(stderr, "textures: cannot use %s (unreadable or larger than 2048x2048)\n", e.path.c_str());
            e.image = {};
        }
    }
    return e.image.rgba.empty() ? nullptr : &e.image;
}

}  // namespace psprecore::rt::gpu
