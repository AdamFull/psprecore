// psptex — HD textures baked offline. `bake` upscales a texture collection (--tex-collect,
// psprecore/gpu/texture_collection.hpp) with an external upscaler into <cooked>/textures/<content hash>.png, which the
// runtime draws in place of the game's textures (psprecore/gpu/texture_replacement.hpp).
//
// The upscaler is any command that upscales a folder of PNG files 4x into another folder, keeping the names: a
// Real-ESRGAN ncnn build (--realesrgan) or a template (--upscaler) with {in}, {out} and {model}. Interface textures
// (drawn in 2D) and scene textures get separate models. 8x runs two passes and halves the result; 2x halves one.
// Before upscaling, every texture gets a border of its own edges, from the opposite side for repeating ones, so tiles
// still meet. Colours under alpha 0 are kept as they are: games draw some textures ignoring their alpha (the GE's
// RGB texture function, fixed blend factors). Afterwards, what the upscaler smoothed away is put back (--detail):
// Real-ESRGAN models remove noise, and take faint grime, cracks and dithering for it. The result is shrunk to the
// original size, and the difference from the original, upscaled smoothly, added to it (back-projection), so the
// result shrinks back to the original while keeping the upscaler's edges. Baked textures are listed in <cooked>/textures/baked.tsv, so a later
// run only bakes new ones (or ones baked with other settings).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_DEPRECATION
#include <stb_image_write.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include "psprecore/cli.hpp"
#include "psprecore/io/path.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

using namespace psprecore;

namespace {

struct Options {
    nx::string realesrgan, upscaler;
    nx::string model_2d = "realesrgan-x4plus-anime", model_3d = "realesrgan-x4plus";
    int scale = 4, max_size = 2048, pad = 8;  // max_size: the runtime's limit
    int detail = 100;                         // percent of the lost detail put back
    nx::string only;
    std::size_t limit = static_cast<std::size_t>(-1);
    bool force = false, keep_temp = false;
};

struct Image {
    nx::vector<std::uint32_t> px;  // RGBA8, R in the low byte
    int w = 0, h = 0;
    std::uint32_t& at(int x, int y) { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; }
    std::uint32_t at(int x, int y) const { return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; }
};

bool read_png(const nx::string& path, Image& img) {
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &img.w, &img.h, &channels, 4);
    if (!pixels) return false;
    img.px.resize(static_cast<std::size_t>(img.w) * static_cast<std::size_t>(img.h));
    std::memcpy(img.px.data(), pixels, img.px.size() * 4);
    stbi_image_free(pixels);
    return true;
}

bool write_png(const nx::string& path, const Image& img) {
    return stbi_write_png(path.c_str(), img.w, img.h, 4, img.px.data(), img.w * 4) != 0;
}

std::uint32_t alpha(std::uint32_t c) { return c >> 24; }

// The image with a border of `pad` texels: the opposite edge along repeating axes, the nearest edge otherwise.
Image padded(const Image& img, int pad, bool repeat_u, bool repeat_v) {
    Image out;
    out.w = img.w + 2 * pad, out.h = img.h + 2 * pad;
    out.px.resize(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h));
    const auto source = [](int i, int n, bool repeat) { return repeat ? ((i % n) + n) % n : std::clamp(i, 0, n - 1); };
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            out.at(x, y) = img.at(source(x - pad, img.w, repeat_u), source(y - pad, img.h, repeat_v));
    return out;
}

Image cropped(const Image& img, int x0, int y0, int w, int h) {
    Image out;
    out.w = w, out.h = h;
    out.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        std::memcpy(&out.at(0, y), img.px.data() + static_cast<std::size_t>(y0 + y) * static_cast<std::size_t>(img.w) + x0,
                    static_cast<std::size_t>(w) * 4);
    return out;
}

// Half the size; colours weighted by alpha.
Image halved(const Image& img) {
    Image out;
    out.w = std::max(1, img.w / 2), out.h = std::max(1, img.h / 2);
    out.px.resize(static_cast<std::size_t>(out.w) * static_cast<std::size_t>(out.h));
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x) {
            unsigned r = 0, g = 0, b = 0, a = 0, r0 = 0, g0 = 0, b0 = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const std::uint32_t c = img.at(std::min(img.w - 1, 2 * x + dx), std::min(img.h - 1, 2 * y + dy));
                    const unsigned ca = alpha(c);
                    r += (c & 0xFF) * ca, g += (c >> 8 & 0xFF) * ca, b += (c >> 16 & 0xFF) * ca, a += ca;
                    r0 += c & 0xFF, g0 += c >> 8 & 0xFF, b0 += c >> 16 & 0xFF;
                }
            out.at(x, y) = a ? (r / a) | (g / a) << 8 | (b / a) << 16 | ((a + 2) / 4) << 24
                             : (r0 / 4) | (g0 / 4) << 8 | (b0 / 4) << 16;
        }
    return out;
}

// Back-projection: `img` (`orig` upscaled `scale` times) shrunk by box averages, the difference from `orig`
// upscaled bilinearly (wrapping along repeating axes) and `percent` of it added.
void restore_detail(Image& img, const Image& orig, int scale, int percent, bool repeat_u, bool repeat_v) {
    const std::size_t n = orig.px.size();
    nx::vector<float> diff(n * 4);
    const float area = static_cast<float>(scale * scale);
    for (int y = 0; y < orig.h; ++y)
        for (int x = 0; x < orig.w; ++x) {
            unsigned sum[4] = {};
            for (int dy = 0; dy < scale; ++dy)
                for (int dx = 0; dx < scale; ++dx) {
                    const std::uint32_t c = img.at(x * scale + dx, y * scale + dy);
                    for (int k = 0; k < 4; ++k) sum[k] += c >> (8 * k) & 0xFF;
                }
            const std::uint32_t o = orig.at(x, y);
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(orig.w) + static_cast<std::size_t>(x)) * 4;
            for (int k = 0; k < 4; ++k)
                diff[i + static_cast<std::size_t>(k)] = static_cast<float>(o >> (8 * k) & 0xFF) - static_cast<float>(sum[k]) / area;
        }
    const auto index = [](int i, int size, bool repeat) { return repeat ? ((i % size) + size) % size : std::clamp(i, 0, size - 1); };
    const float strength = static_cast<float>(percent) / 100.0f;
    for (int y = 0; y < img.h; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(scale) - 0.5f;
        const int y0 = static_cast<int>(std::floor(v));
        const float fy = v - static_cast<float>(y0);
        const int ya = index(y0, orig.h, repeat_v), yb = index(y0 + 1, orig.h, repeat_v);
        for (int x = 0; x < img.w; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(scale) - 0.5f;
            const int x0 = static_cast<int>(std::floor(u));
            const float fx = u - static_cast<float>(x0);
            const int xa = index(x0, orig.w, repeat_u), xb = index(x0 + 1, orig.w, repeat_u);
            const auto at = [&](int xx, int yy, int k) {
                return diff[(static_cast<std::size_t>(yy) * static_cast<std::size_t>(orig.w) + static_cast<std::size_t>(xx)) * 4 +
                            static_cast<std::size_t>(k)];
            };
            std::uint32_t& c = img.at(x, y);
            std::uint32_t out = 0;
            for (int k = 0; k < 4; ++k) {
                const float d = (at(xa, ya, k) * (1 - fx) + at(xb, ya, k) * fx) * (1 - fy) +
                                (at(xa, yb, k) * (1 - fx) + at(xb, yb, k) * fx) * fy;
                const float value = static_cast<float>(c >> (8 * k) & 0xFF) + strength * d;
                out |= static_cast<std::uint32_t>(std::clamp(static_cast<int>(std::lround(value)), 0, 255)) << (8 * k);
            }
            c = out;
        }
    }
}

nx::string quote(nx::string_view s) { return nx::string("\"") + nx::string(s) + "\""; }

// Runs a command line through the shell; returns the exit status.
int run(const nx::string& command) {
#ifdef _WIN32
    // cmd.exe strips the outer quotes of a command that starts with one; wrap the whole line once more.
    return std::system(("\"" + command + "\"").c_str());
#else
    return std::system(command.c_str());
#endif
}

nx::string replaced(nx::string_view text, nx::string_view key, nx::string_view value) {
    nx::string out;
    for (std::size_t at; (at = text.find(key)) != nx::string_view::npos; text.remove_prefix(at + key.size()))
        out += nx::string(text.substr(0, at)) + nx::string(value);
    return out + nx::string(text);
}

nx::vector<nx::string_view> split(nx::string_view line, char sep) {
    nx::vector<nx::string_view> out;
    while (true) {
        const std::size_t end = line.find(sep);
        out.push_back(line.substr(0, end));
        if (end == nx::string_view::npos) return out;
        line.remove_prefix(end + 1);
    }
}

nx::vector<nx::string> lines(const nx::string& path) {
    nx::vector<nx::string> out;
    const auto text = nx::fs::file_read_text(path);
    if (!text.has_value()) return out;
    for (nx::string_view line : split(text.value(), '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) out.emplace_back(line);
    }
    return out;
}

int to_int(nx::string_view s) { return std::atoi(nx::string(s).c_str()); }

struct Texture {
    nx::string hash;
    int w = 0, h = 0;
    bool two_d = false, repeat_u = false, repeat_v = false;
    int scale = 1;  // for this texture (lower than asked when the result would exceed --max-size)
};

// A set of textures upscaled together: same model, same number of passes.
struct Batch {
    nx::string model;
    int passes = 1;
    nx::vector<Texture> textures;
};

int bake(const nx::string& collection, const nx::string& cooked, const Options& opt) {
    const nx::string out_dir = path_join(cooked, "textures"), tmp = path_join(cooked, "textures-tmp");
    if (!nx::fs::create_directories(out_dir).has_value()) {
        std::fprintf(stderr, "psptex: cannot create %s\n", out_dir.c_str());
        return 1;
    }
    nx::string upscaler = opt.upscaler;
    if (upscaler.empty())
        upscaler = quote(opt.realesrgan.empty() ? nx::string("realesrgan-ncnn-vulkan") : opt.realesrgan) +
                   " -i \"{in}\" -o \"{out}\" -n {model} -s 4 -f png";

    // Already baked: hash -> settings.
    const nx::string manifest = path_join(out_dir, "baked.tsv");
    nx::hash_map<nx::string, nx::string> baked;
    for (const nx::string& line : lines(manifest)) {
        const auto cols = split(line, '\t');
        if (cols.size() >= 2) baked[nx::string(cols[0])] = nx::string(line);
    }
    const auto settings = [&](const Texture& t) {
        return nx::format("{}\t{}\t{}\t{}\t{}\t{}", t.hash, t.w, t.h, t.scale, t.two_d ? opt.model_2d : opt.model_3d,
                          opt.detail);
    };

    nx::vector<Batch> batches;
    std::size_t queued = 0, up_to_date = 0, too_big = 0, flat = 0;
    for (const nx::string& line : lines(path_join(collection, "index.tsv"))) {
        const auto cols = split(line, '\t');
        if (cols.size() < 5 || cols[0].size() != 16) continue;
        Texture t{nx::string(cols[0]), to_int(cols[1]), to_int(cols[2]), cols[4] == "2d"};
        if ((opt.only == "2d" && !t.two_d) || (opt.only == "3d" && t.two_d)) continue;
        if (cols.size() >= 6) t.repeat_u = cols[5] == "repeat" || cols[5] == "repeat-u",
                              t.repeat_v = cols[5] == "repeat" || cols[5] == "repeat-v";
        t.scale = opt.scale;
        while (t.scale > 1 && std::max(t.w, t.h) * t.scale > opt.max_size) t.scale /= 2;
        if (t.scale < 2) {
            ++too_big;
            continue;
        }
        if (!opt.force && nx::fs::exists(path_join(out_dir, t.hash + ".png"))) {
            const auto it = baked.find(t.hash);
            if (it != baked.end() && it->second == settings(t)) {
                ++up_to_date;
                continue;
            }
        }
        if (queued >= opt.limit) continue;
        const nx::string& model = t.two_d ? opt.model_2d : opt.model_3d;
        const int passes = t.scale > 4 ? 2 : 1;
        auto it = std::find_if(batches.begin(), batches.end(),
                               [&](const Batch& b) { return b.model == model && b.passes == passes; });
        if (it == batches.end()) it = batches.insert(batches.end(), Batch{model, passes, {}});
        it->textures.push_back(std::move(t));
        ++queued;
    }
    std::fprintf(stderr, "baking %zu textures at %dx (%zu up to date, %zu too large for --max-size %d)\n", queued,
                 opt.scale, up_to_date, too_big, opt.max_size);

    std::size_t done = 0, failed = 0;
    for (std::size_t bi = 0; bi < batches.size(); ++bi) {
        Batch& batch = batches[bi];
        const nx::string dir = path_join(tmp, nx::format("{}", bi));
        const nx::string in = path_join(dir, "in");
        (void)nx::fs::remove_all(dir);
        if (!nx::fs::create_directories(in).has_value()) return 1;

        // Prepared inputs. Single-colour textures need no upscaling.
        nx::vector<Texture> work;
        nx::hash_map<nx::string, Image> originals;
        for (Texture& t : batch.textures) {
            Image img;
            if (!read_png(path_join(collection, t.hash + ".png"), img)) {
                std::fprintf(stderr, "%s: cannot read the collected picture\n", t.hash.c_str());
                ++failed;
                continue;
            }
            if (std::all_of(img.px.begin(), img.px.end(), [&](std::uint32_t c) { return c == img.px[0]; })) {
                ++flat;
                continue;
            }
            if (!write_png(path_join(in, t.hash + ".png"), padded(img, opt.pad, t.repeat_u, t.repeat_v))) return 1;
            originals[t.hash] = std::move(img);
            work.push_back(t);
        }
        if (work.empty()) continue;

        nx::string pass_in = in;
        for (int p = 1; p <= batch.passes; ++p) {
            const nx::string pass_out = path_join(dir, nx::format("pass{}", p));
            (void)nx::fs::create_directories(pass_out);
            const nx::string log = path_join(dir, nx::format("pass{}.log", p));
            nx::string command = replaced(replaced(replaced(upscaler, "{in}", pass_in), "{out}", pass_out), "{model}",
                                          batch.model);
            std::fprintf(stderr, "%s, pass %d/%d: %zu textures\n", batch.model.c_str(), p, batch.passes, work.size());
            const auto start = std::chrono::steady_clock::now();
            if (run(command + " > " + quote(log) + " 2>&1") != 0)
                std::fprintf(stderr, "  the upscaler failed (see %s)\n", log.c_str());
            std::fprintf(stderr, "  %.1f s\n",
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            pass_in = pass_out;
        }

        for (const Texture& t : work) {
            Image img;
            if (!read_png(path_join(pass_in, t.hash + ".png"), img)) {
                std::fprintf(stderr, "%s: no upscaled picture\n", t.hash.c_str());
                ++failed;
                continue;
            }
            const int factor = batch.passes == 2 ? 16 : 4;
            if (img.w != (t.w + 2 * opt.pad) * factor || img.h != (t.h + 2 * opt.pad) * factor) {
                std::fprintf(stderr, "%s: the upscaler made %dx%d, not %dx\n", t.hash.c_str(), img.w, img.h, factor);
                ++failed;
                continue;
            }
            img = cropped(img, opt.pad * factor, opt.pad * factor, t.w * factor, t.h * factor);
            for (int f = factor; f > t.scale; f /= 2) img = halved(img);
            const Image& orig = originals[t.hash];
            if (opt.detail) restore_detail(img, orig, t.scale, opt.detail, t.repeat_u, t.repeat_v);
            if (std::all_of(orig.px.begin(), orig.px.end(), [](std::uint32_t c) { return alpha(c) == 255; }))
                for (auto& c : img.px) c |= 0xFF000000u;
            if (!write_png(path_join(out_dir, t.hash + ".png"), img)) {
                ++failed;
                continue;
            }
            baked[t.hash] = settings(t);
            ++done;
        }
        if (!opt.keep_temp) (void)nx::fs::remove_all(dir);

        nx::vector<nx::string> sorted;
        for (const auto& [hash, line] : baked) sorted.push_back(line);
        std::sort(sorted.begin(), sorted.end());
        nx::string text;
        for (const nx::string& line : sorted) text += line + "\n";
        (void)nx::fs::file_write_text(manifest, text);
    }
    if (!opt.keep_temp) (void)nx::fs::remove_all(tmp);
    std::fprintf(stderr, "done: %zu baked, %zu single-colour skipped, %zu failed\n", done, flat, failed);
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    nx::string collection, cooked;
    nx::cli::Parser cli("psptex", "HD textures baked offline for the runtime (cooked/textures).");
    cli.command("bake", "upscale a texture collection (--tex-collect) into <cooked>/textures")
        .positional("collection", collection, "texture collection folder (with index.tsv)")
        .positional("cooked", cooked, "cooked media folder")
        .option("scale", opt.scale, "2, 4 or 8 (default: 4)", {.value_name = "N", .choices = {"2", "4", "8"}})
        .option("max-size", opt.max_size,
                "largest result; bigger textures get a lower scale (default, and the runtime's limit: 2048)",
                {.value_name = "PIXELS"})
        .option("realesrgan", opt.realesrgan, "realesrgan-ncnn-vulkan executable (default: on PATH)",
                {.value_name = "PATH"})
        .option("upscaler", opt.upscaler,
                "another 4x upscaler: a command line with {in} and {out} (folders of PNG files) and {model}",
                {.value_name = "COMMAND"})
        .option("model-2d", opt.model_2d, "model for interface textures (default: realesrgan-x4plus-anime)",
                {.value_name = "NAME"})
        .option("model-3d", opt.model_3d, "model for scene textures (default: realesrgan-x4plus)",
                {.value_name = "NAME"})
        .option("detail", opt.detail,
                "percent of the detail the upscaler smoothed away to put back (default: 100; 0: the upscaler's result)",
                {.value_name = "PERCENT"})
        .option("pad", opt.pad, "border added around each texture while upscaling (default: 8)", {.value_name = "N"})
        .option("only", opt.only, "bake one kind only", {.value_name = "KIND", .choices = {"2d", "3d"}})
        .option("limit", opt.limit, "bake at most N textures (testing)", {.value_name = "N"})
        .flag("force", opt.force, "bake again textures already baked")
        .flag("keep-temp", opt.keep_temp, "keep the upscaler's inputs and outputs");
    nx::string command;
    if (const int code = parse_command_line(cli, argc, argv, &command); code >= 0) return code;
    try {
        if (command == "bake") return bake(collection, cooked, opt);
        std::fputs(cli.help().c_str(), stderr);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "psptex: %s\n", e.what());
        return 1;
    }
}
