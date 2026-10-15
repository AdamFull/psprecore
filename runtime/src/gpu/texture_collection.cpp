#include "psprecore/gpu/texture_collection.hpp"

#include <cinttypes>
#include <cstdio>

#include "psprecore/io/file.hpp"
#include "psprecore/io/path.hpp"
#include "psprecore/runtime/hle.hpp"
#include "core/foundation/containers/hash_set.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"

namespace psprecore::rt::gpu {

namespace {

nx::string g_dir;
std::FILE* g_index = nullptr;
nx::hash_set<std::uint64_t> g_seen;
std::size_t g_added = 0;

}  // namespace

void start_texture_collection(nx::string_view dir) {
    g_dir = nx::string(dir);
    (void)nx::fs::create_directories(g_dir);
    const nx::string index = path_join(g_dir, "index.tsv");
    if (const auto text = nx::fs::file_read_text(index); text.has_value()) {
        nx::string_view rest = text.value();
        while (!rest.empty()) {
            const std::size_t end = rest.find('\n');
            const nx::string_view line = rest.substr(0, end);
            rest.remove_prefix(end == nx::string_view::npos ? rest.size() : end + 1);
            std::uint64_t hash = 0;
            if (std::sscanf(nx::string(line).c_str(), "%" SCNx64, &hash) == 1) g_seen.insert(hash);
        }
    }
    g_index = io::open_c_file(index, "a");
    if (!g_index) {
        std::fprintf(stderr, "tex-collect: cannot write %s\n", index.c_str());
        return;
    }
    std::fprintf(stderr, "tex-collect: %s (%zu textures so far)\n", g_dir.c_str(), g_seen.size());
}

bool collecting_textures() { return g_index != nullptr; }

void collect_texture(std::uint64_t content_hash, const std::uint32_t* rgba, int width, int height, int format,
                     bool two_d, bool repeat_u, bool repeat_v) {
    if (!g_index || !g_seen.insert(content_hash).second) return;
    const nx::string name = nx::format("{:016X}", content_hash);
    if (!write_png(path_join(g_dir, name + ".png"), rgba, width, height)) return;
    const char* wrap = repeat_u ? (repeat_v ? "repeat" : "repeat-u") : (repeat_v ? "repeat-v" : "clamp");
    std::fprintf(g_index, "%s\t%d\t%d\t%d\t%s\t%s\n", name.c_str(), width, height, format, two_d ? "2d" : "3d", wrap);
    std::fflush(g_index);
    if (++g_added % 100 == 0) std::fprintf(stderr, "tex-collect: %zu new textures\n", g_added);
}

}  // namespace psprecore::rt::gpu
