#include "psprecore/gpu/texture_replacement.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>

#include "psprecore/formats/ktx2.hpp"
#include "psprecore/io/file.hpp"
#include "psprecore/runtime/hle.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

namespace psprecore::rt::gpu {

namespace {

constexpr int kMaxSize = 2048;  // with mipmaps, fits the Vulkan renderer's staging buffer
constexpr std::size_t kMemoryBudget = 96u << 20;  // loaded pictures kept for uploading again

enum class State : std::uint8_t { Unloaded, Queued, Ready, Failed };

struct Entry {
    nx::string path;                 // PNG
    nx::string bc7_path, astc_path;  // KTX2 (baked textures)
    State state = State::Unloaded;
    std::uint64_t last_used = 0;
    ReplacementImage image;
};

// The folders' pictures, by hash. Each map is filled before the first lookup and keeps its keys afterwards.
struct Source {
    nx::hash_map<std::uint64_t, Entry> entries;
    bool mipmaps = true;
    bool compressed = false;  // has KTX2 files
};
Source g_mods, g_indexed{{}, false}, g_baked{{}, true, true};
std::atomic<bool> g_baked_enabled{true};
std::atomic<bool> g_bc7{false}, g_astc{false};

// Loading: entries go Unloaded -> Queued (renderer thread) -> Ready or Failed (worker), and Ready -> Unloaded when
// evicted (renderer thread). The lock guards states, queue and images.
nx::mutex g_lock;
nx::cond_var g_wake;
struct Job {
    Source* source;
    std::uint64_t hash;
    nx::string path;
};
nx::vector<Job> g_queue;
std::size_t g_loaded_bytes = 0;
std::uint64_t g_clock = 0;
bool g_stop = false;

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

void scan_folder(nx::string_view dir, Source& into) {
    auto files = nx::fs::directory_entries(dir);
    if (!files.has_value()) return;
    for (const auto& f : files.value()) {
        const nx::string_view name = file_name(f.path);
        std::uint64_t hash = 0;
        if (f.type != nx::fs::file_type::regular || !parse_hash(name, hash)) continue;
        if (name.ends_with(".png")) {
            if (into.compressed) into.entries[hash].path = f.path;
            else into.entries[hash] = Entry{f.path};  // a later mod's picture replaces an earlier one's
        } else if (into.compressed && name.ends_with(".bc7.ktx2")) {
            into.entries[hash].bc7_path = f.path;
        } else if (into.compressed && name.ends_with(".astc.ktx2")) {
            into.entries[hash].astc_path = f.path;
        }
    }
}

// A KTX2 file of baked blocks; false when it cannot be used.
bool read_blocks(const nx::string& path, ReplacementImage& img) {
    ktx2::Texture t;
    try {
        if (!ktx2::read(io::read_all(path), t)) return false;
    } catch (const std::exception&) {
        return false;
    }
    img.compression = t.format == ktx2::Format::BC7 ? Compression::BC7 : Compression::ASTC4x4;
    img.width = t.width, img.height = t.height, img.levels = t.levels;
    img.blocks = std::move(t.data);
    return true;
}

std::size_t memory_size(const ReplacementImage& img) { return img.rgba.size() * 4 + img.blocks.size(); }

// Appends the mipmaps of the level-0 picture in `img` (colours weighted by alpha, 2x2 boxes).
void add_mipmaps(ReplacementImage& img) {
    std::size_t total = img.rgba.size();
    for (int w = img.width, h = img.height; w > 1 || h > 1;) {
        w = std::max(1, w / 2), h = std::max(1, h / 2);
        total += static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    }
    img.rgba.reserve(total);
    std::size_t src = 0;
    for (int w = img.width, h = img.height; w > 1 || h > 1; ++img.levels) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const std::size_t dst = img.rgba.size();
        img.rgba.resize(dst + static_cast<std::size_t>(nw) * static_cast<std::size_t>(nh));
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                unsigned r = 0, g = 0, b = 0, a = 0, r0 = 0, g0 = 0, b0 = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const std::size_t sx = static_cast<std::size_t>(std::min(w - 1, 2 * x + dx));
                        const std::size_t sy = static_cast<std::size_t>(std::min(h - 1, 2 * y + dy));
                        const std::uint32_t c = img.rgba[src + sy * static_cast<std::size_t>(w) + sx];
                        const unsigned ca = c >> 24;
                        r += (c & 0xFF) * ca, g += (c >> 8 & 0xFF) * ca, b += (c >> 16 & 0xFF) * ca, a += ca;
                        r0 += c & 0xFF, g0 += c >> 8 & 0xFF, b0 += c >> 16 & 0xFF;
                    }
                img.rgba[dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(nw) + static_cast<std::size_t>(x)] =
                    a ? (r / a) | (g / a) << 8 | (b / a) << 16 | ((a + 2) / 4) << 24
                      : (r0 / 4) | (g0 / 4) << 8 | (b0 / 4) << 16;
            }
        src = dst;
        w = nw, h = nh;
    }
}

void worker() {
    while (true) {
        Job job;
        {
            nx::scoped_lock lock(g_lock);
            g_wake.wait(g_lock, [] { return !g_queue.empty() || g_stop; });
            if (g_stop) return;
            job = std::move(g_queue.back());  // the latest request first: what is on screen now
            g_queue.pop_back();
        }
        ReplacementImage img;
        const bool ktx2 = job.path.ends_with(".ktx2");
        bool ok = (ktx2 ? read_blocks(job.path, img) : read_png(job.path, img.rgba, img.width, img.height)) &&
                  img.width <= kMaxSize && img.height <= kMaxSize;
        if (!ok) std::fprintf(stderr, "textures: cannot use %s (unreadable or larger than 2048x2048)\n", job.path.c_str());
        else if (!ktx2 && job.source->mipmaps) add_mipmaps(img);
        nx::scoped_lock lock(g_lock);
        Entry& e = job.source->entries.find(job.hash)->second;
        e.state = ok ? State::Ready : State::Failed;
        if (ok) {
            g_loaded_bytes += memory_size(img);
            e.image = std::move(img);
        }
    }
}

// The worker thread, started with the first load and stopped at exit.
struct Loader {
    nx::thread thread{worker, nx::thread_options{.name = "texture loader"}};
    ~Loader() {
        {
            nx::scoped_lock lock(g_lock);
            g_stop = true;
        }
        g_wake.notify_all();
    }  // `thread` joins after this
};

// Least recently used pictures out until the loaded ones fit the budget (keeping `keep`). Lock held.
void evict(const Entry* keep) {
    while (g_loaded_bytes > kMemoryBudget) {
        Entry* oldest = nullptr;
        for (Source* s : {&g_mods, &g_indexed, &g_baked})
            for (auto& [_, e] : s->entries)
                if (e.state == State::Ready && &e != keep && (!oldest || e.last_used < oldest->last_used)) oldest = &e;
        if (!oldest) return;
        g_loaded_bytes -= memory_size(oldest->image);
        oldest->image = {};
        oldest->state = State::Unloaded;
    }
}

FoundImage find(Source& source, std::uint64_t hash) {
    if (source.entries.empty()) return {};
    const auto it = source.entries.find(hash);
    if (it == source.entries.end()) return {};
    Entry& e = it->second;
    nx::scoped_lock lock(g_lock);
    switch (e.state) {
        case State::Ready:
            e.last_used = ++g_clock;
            evict(&e);
            return {Lookup::Ready, &e.image};
        case State::Unloaded: {
            // Compressed blocks when the GPU takes them, else the picture.
            const nx::string& path = g_bc7 && !e.bc7_path.empty()    ? e.bc7_path
                                     : g_astc && !e.astc_path.empty() ? e.astc_path
                                                                      : e.path;
            if (path.empty()) {
                e.state = State::Failed;
                return {};
            }
            static Loader loader;
            e.state = State::Queued;
            g_queue.push_back(Job{&source, hash, path});
            g_wake.notify_one();
            return {Lookup::Loading};
        }
        case State::Queued: return {Lookup::Loading};
        case State::Failed: return {};
    }
    return {};
}

}  // namespace

void scan_texture_replacements(std::span<const nx::string> mods) {
    g_mods.entries.clear();
    g_indexed.entries.clear();
    for (const auto& folder : mods) {
        scan_folder(nx::format("{}/textures", folder), g_mods);
        scan_folder(nx::format("{}/textures/indexed", folder), g_indexed);
    }
    if (!g_mods.entries.empty() || !g_indexed.entries.empty())
        std::fprintf(stderr, "textures: %zu replacements, %zu index maps\n", g_mods.entries.size(),
                     g_indexed.entries.size());
}

void scan_baked_textures(nx::string_view dir) {
    g_baked.entries.clear();
    scan_folder(dir, g_baked);
    if (g_baked.entries.empty()) return;
    std::size_t bc7 = 0, astc = 0;
    for (const auto& [_, e] : g_baked.entries) bc7 += !e.bc7_path.empty(), astc += !e.astc_path.empty();
    std::fprintf(stderr, "textures: %zu baked HD textures, %zu as BC7, %zu as ASTC (%.*s)\n", g_baked.entries.size(), bc7,
                 astc, static_cast<int>(dir.size()), dir.data());
}

bool have_baked_textures() { return !g_baked.entries.empty(); }

void set_compressed_textures(bool bc7, bool astc) {
    g_bc7 = bc7;
    g_astc = astc;
    std::fprintf(stderr, "textures: the GPU samples%s%s\n", bc7 ? " BC7" : "", astc ? " ASTC" : bc7 ? "" : " no compressed formats");
}

bool enable_baked_textures(bool enabled) { return g_baked_enabled.exchange(enabled) != enabled && have_baked_textures(); }

FoundImage texture_replacement(std::uint64_t content_hash) {
    if (const FoundImage found = find(g_mods, content_hash); found.state != Lookup::None) return found;
    return g_baked_enabled ? find(g_baked, content_hash) : FoundImage{};
}

bool have_indexed_replacements() { return !g_indexed.entries.empty(); }

FoundImage indexed_replacement(std::uint64_t texels_hash) { return find(g_indexed, texels_hash); }

}  // namespace psprecore::rt::gpu
