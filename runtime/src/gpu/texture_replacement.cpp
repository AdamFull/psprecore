#include "psprecore/gpu/texture_replacement.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>

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
    nx::string path;
    State state = State::Unloaded;
    std::uint64_t last_used = 0;
    ReplacementImage image;
};

// The folders' pictures, by hash. Each map is filled before the first lookup and keeps its keys afterwards.
struct Source {
    nx::hash_map<std::uint64_t, Entry> entries;
    bool mipmaps = true;
};
Source g_mods, g_indexed{{}, false}, g_baked;
std::atomic<bool> g_baked_enabled{true};

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
        if (f.type != nx::fs::file_type::regular || !name.ends_with(".png") || !parse_hash(name, hash)) continue;
        into.entries[hash] = Entry{f.path};
    }
}

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
        bool ok = read_png(job.path, img.rgba, img.width, img.height) && img.width <= kMaxSize && img.height <= kMaxSize;
        if (!ok) std::fprintf(stderr, "textures: cannot use %s (unreadable or larger than 2048x2048)\n", job.path.c_str());
        else if (job.source->mipmaps) add_mipmaps(img);
        nx::scoped_lock lock(g_lock);
        Entry& e = job.source->entries.find(job.hash)->second;
        e.state = ok ? State::Ready : State::Failed;
        if (ok) {
            g_loaded_bytes += img.rgba.size() * 4;
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
        g_loaded_bytes -= oldest->image.rgba.size() * 4;
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
            static Loader loader;
            e.state = State::Queued;
            g_queue.push_back(Job{&source, hash, e.path});
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
    if (!g_baked.entries.empty())
        std::fprintf(stderr, "textures: %zu baked HD textures (%.*s)\n", g_baked.entries.size(),
                     static_cast<int>(dir.size()), dir.data());
}

bool have_baked_textures() { return !g_baked.entries.empty(); }

bool enable_baked_textures(bool enabled) { return g_baked_enabled.exchange(enabled) != enabled && have_baked_textures(); }

FoundImage texture_replacement(std::uint64_t content_hash) {
    if (const FoundImage found = find(g_mods, content_hash); found.state != Lookup::None) return found;
    return g_baked_enabled ? find(g_baked, content_hash) : FoundImage{};
}

bool have_indexed_replacements() { return !g_indexed.entries.empty(); }

FoundImage indexed_replacement(std::uint64_t texels_hash) { return find(g_indexed, texels_hash); }

}  // namespace psprecore::rt::gpu
