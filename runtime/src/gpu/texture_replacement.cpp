#include "psprecore/gpu/texture_replacement.hpp"

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

nx::hash_map<std::uint64_t, Entry> g_replacements;  // by content hash
nx::hash_map<std::uint64_t, Entry> g_indexed;       // index maps, by texels hash

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

void scan_folder(const nx::string& dir, nx::hash_map<std::uint64_t, Entry>& into) {
    auto files = nx::fs::directory_entries(dir);
    if (!files.has_value()) return;
    for (const auto& f : files.value()) {
        const nx::string_view name = file_name(f.path);
        std::uint64_t hash = 0;
        if (f.type != nx::fs::file_type::regular || !name.ends_with(".png") || !parse_hash(name, hash)) continue;
        into[hash] = Entry{f.path};
    }
}

const ReplacementImage* load(nx::hash_map<std::uint64_t, Entry>& map, std::uint64_t hash) {
    if (map.empty()) return nullptr;
    const auto it = map.find(hash);
    if (it == map.end()) return nullptr;
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

}  // namespace

void scan_texture_replacements(std::span<const nx::string> mods) {
    g_replacements.clear();
    g_indexed.clear();
    for (const auto& folder : mods) {
        scan_folder(nx::format("{}/textures", folder), g_replacements);
        scan_folder(nx::format("{}/textures/indexed", folder), g_indexed);
    }
    if (!g_replacements.empty() || !g_indexed.empty())
        std::fprintf(stderr, "textures: %zu replacements, %zu index maps\n", g_replacements.size(), g_indexed.size());
}

bool have_texture_replacements() { return !g_replacements.empty(); }
bool have_indexed_replacements() { return !g_indexed.empty(); }

const ReplacementImage* texture_replacement(std::uint64_t content_hash) { return load(g_replacements, content_hash); }
const ReplacementImage* indexed_replacement(std::uint64_t texels_hash) { return load(g_indexed, texels_hash); }

}  // namespace psprecore::rt::gpu
