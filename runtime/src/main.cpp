// ff7cc — runs the recompiled game in a window (or headless, for testing).
//
//   ff7cc <extracted disc folder> [--headless] [--trace] [--vblanks N] [--ge-dump FRAME] [--screenshot VBLANK out.png]
//       [--screenshot-every N prefix]   (writes prefix_<vblank>.png every N vblanks)
//       [--renderer vulkan|soft|compare] [--scale N]   (window only, default: the setting, which the pause menu
//                                        changes; headless always uses the software renderer;
//                                        compare runs both and screenshots show reference | Vulkan | difference)
//       [--ge-dump-at VBLANK]           (writes ge_dump.txt for the first full frame after VBLANK)
//       [--soft-threads N]              (software renderer threads; default: one per hardware thread, at most 8)
//       [--profile VBLANK]              (Windows: samples where the host spends its time from VBLANK on and prints
//                                        the hottest functions at exit)
//       [--ge-bench VBLANK]             (captures the draws of the vblanks after VBLANK, times vertex decoding and
//                                        transform on them in a loop, prints the result and quits)
//       [--record FILE] [--replay FILE]  (controller input and save picker answers per vblank; a replay
//                                        reproduces the run, then live input takes over)
//       [--ram-dump VBLANK FILE]        (writes user RAM, 0x08800000-0x0A000000, at VBLANK)
//       [--state-hash N]                (prints a hash of user RAM every N vblanks: determinism checks)
//       [--clock UNIX]                  (guest wall clock at boot; default: now, or the recording's)
//       [--saves DIR]                   (save data folder; default: saves/ next to the settings; recordings
//                                        snapshot it to <file>.saves and replays run on a copy of that)
//       [--mods DIR]                    (mod folders, each with pkg/NNNNN.* entry replacements; default ./mods)
//       [--settings FILE]               (default ./ff7cc.ini; written when changed in the pause menu)
//       [--touch]                       (show the on-screen touch controls from the start; default on Android)
//       [--perf]                        (prints host time per 300 vblanks and display frames)
//       [--no-audio] [--unthrottled]       (testing: no sound device; run as fast as possible)
//       [--mute]                        (testing: open the sound device but play silence)
//       [--cooked DIR]                  (cooked media from cccook; default ./cooked)
//       [--audio-dump prefix]           (writes prefix_<source>.wav per audio source at exit)
//       [--press VBLANK BUTTON]...      (scripted input: holds start/select/cross/circle/square/triangle/
//                                        up/down/left/right/l/r for 6 vblanks; for automated testing)
//       [--ui VBLANK ACTION]...         (scripted overlay input: menu, enter, escape, up, down, space,
//                                        tap:X,Y for a touch at window-normalised coordinates,
//                                        renderer:vulkan / renderer:software as if chosen in the menu)
//       [--window-shot VBLANK out.png]  (captures the window, overlay included; a pause menu opened by --ui
//                                        closes itself after this capture so the run can continue)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <SDL3/SDL_keycode.h>
#ifdef __ANDROID__
#include <SDL3/SDL_main.h>  // main() becomes SDL_main, called by SDLActivity
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_system.h>
#include <android/log.h>
#include <unistd.h>
#endif

#include "ff7cc/formats/elf.hpp"
#include "ff7cc/formats/prx.hpp"
#include "ff7cc/io/file.hpp"
#include "ff7cc/io/path.hpp"
#include "ff7cc/text.hpp"
#include "ff7cc/platform/audio.hpp"
#include "ff7cc/platform/display.hpp"
#include "platform/gpu_vulkan.hpp"
#include "platform/overlay.hpp"
#include "platform/settings.hpp"
#include "gpu/soft_renderer.hpp"
#include "gpu/vertex_pipeline.hpp"
#include "platform/sampling_profiler.hpp"
#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/io.hpp"
#include "ff7cc/runtime/kernel.hpp"
#include "ff7cc/runtime/memory.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/strings/format.h"


namespace {

// Records every audio source to <prefix>_<source>.wav (and forwards to the device, if any). For testing.
class WavRecorder final : public ff7cc::rt::AudioSink {
public:
    WavRecorder(nx::string prefix, ff7cc::rt::AudioSink* next) : prefix_(std::move(prefix)), next_(next) {}
    ~WavRecorder() override { flush(); }

    void submit(int source, const std::int16_t* frames, std::size_t count) override {
        if (source >= 0 && source < kSources) samples_[source].insert(samples_[source].end(), frames, frames + count * 2);
        if (next_) next_->submit(source, frames, count);
    }
    void flush() {
        for (int s = 0; s < kSources; ++s) {
            const auto& pcm = samples_[s];
            if (pcm.empty()) continue;
            const nx::string path = prefix_ + "_" + nx::format("{}", s) + ".wav";
            std::FILE* f = ff7cc::io::open_c_file(path, "wb");
            if (!f) continue;
            const auto put32 = [f](std::uint32_t v) { std::fwrite(&v, 4, 1, f); };
            const auto put16 = [f](std::uint16_t v) { std::fwrite(&v, 2, 1, f); };
            const auto bytes = static_cast<std::uint32_t>(pcm.size() * 2);
            std::fwrite("RIFF", 1, 4, f);
            put32(36 + bytes);
            std::fwrite("WAVEfmt ", 1, 8, f);
            put32(16);
            put16(1);  // PCM
            put16(2);
            put32(44100);
            put32(44100 * 4);
            put16(4);
            put16(16);
            std::fwrite("data", 1, 4, f);
            put32(bytes);
            std::fwrite(pcm.data(), 2, pcm.size(), f);
            std::fclose(f);
        }
    }

private:
    nx::string prefix_;
    ff7cc::rt::AudioSink* next_;
    nx::vector<std::int16_t> samples_[kSources];
};

// Sends every display list to two renderers (--renderer compare).
class TeeRenderer final : public ff7cc::rt::ge::Renderer {
public:
    TeeRenderer(std::unique_ptr<Renderer> a, std::unique_ptr<Renderer> b) : a_(std::move(a)), b_(std::move(b)) {}
    void draw(const ff7cc::rt::ge::State& s, ff7cc::rt::ge::Prim prim, const nx::vector<ff7cc::rt::ge::Vertex>& v) override {
        a_->draw(s, prim, v);
        b_->draw(s, prim, v);
    }
    void transfer(const ff7cc::rt::ge::State& s) override {
        a_->transfer(s);
        b_->transfer(s);
    }
    void flush() override {
        a_->flush();
        b_->flush();
    }

private:
    std::unique_ptr<Renderer> a_, b_;
};

// Reference frame (w x h) next to the GPU frame (box-filtered down from `scale` times the size) and their
// difference (x4). Returns the composite; `mean` is the mean per-pixel max-channel difference and `bad` the
// share of pixels differing by more than 32.
nx::vector<std::uint32_t> compare_frames(const nx::vector<std::uint32_t>& ref, const nx::vector<std::uint32_t>& gpu, int w,
                                          int h, int scale, double& mean, double& bad) {
    nx::vector<std::uint32_t> out(static_cast<std::size_t>(w) * 3 * h, 0xFF000000u);
    double total = 0;
    std::size_t over = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int acc[3] = {0, 0, 0};
            for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale; ++sx) {
                    const std::uint32_t px = gpu[static_cast<std::size_t>(y * scale + sy) * (w * scale) + (x * scale + sx)];
                    for (int c = 0; c < 3; ++c) acc[c] += static_cast<int>((px >> (c * 8)) & 0xFF);
                }
            const std::uint32_t r = ref[static_cast<std::size_t>(y) * w + x];
            std::uint32_t g = 0xFF000000u;
            int diff = 0;
            for (int c = 0; c < 3; ++c) {
                const int gv = acc[c] / (scale * scale);
                g |= static_cast<std::uint32_t>(gv) << (c * 8);
                diff = std::max(diff, std::abs(gv - static_cast<int>((r >> (c * 8)) & 0xFF)));
            }
            total += diff;
            if (diff > 32) ++over;
            const std::size_t row = static_cast<std::size_t>(y) * w * 3;
            out[row + x] = r | 0xFF000000u;
            out[row + w + x] = g;
            const std::uint32_t d = static_cast<std::uint32_t>(std::min(255, diff * 4));
            out[row + 2 * w + x] = 0xFF000000u | d | (d << 8) | (d << 16);
        }
    mean = total / (static_cast<double>(w) * h);
    bad = 100.0 * static_cast<double>(over) / (static_cast<double>(w) * h);
    return out;
}

// Input recordings (--record / --replay), a text file with one line per change:
//     ff7cc-input 1 <wall clock base>
//     <vblank> pad <buttons, hex> <lx> <ly>
//     <vblank> dialog <slot>            (save picker answer; -1 = cancel)
// With the same game data, saves and clock base the run is reproduced exactly (virtual time is deterministic).
class InputRecorder {
public:
    InputRecorder(nx::fs::path_view path, std::int64_t clock_base) : f_(ff7cc::io::open_c_file(path, "w")) {
        if (!f_) throw ff7cc::Error(nx::string("cannot write ") + path.native());
        std::fprintf(f_, "ff7cc-input 1 %lld\n", static_cast<long long>(clock_base));
    }
    ~InputRecorder() {
        if (f_) std::fclose(f_);
    }
    InputRecorder(const InputRecorder&) = delete;
    InputRecorder& operator=(const InputRecorder&) = delete;

    void pad(std::uint64_t vblank, std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly) {
        if (buttons == buttons_ && lx == lx_ && ly == ly_) return;
        buttons_ = buttons;
        lx_ = lx;
        ly_ = ly;
        std::fprintf(f_, "%llu pad %X %u %u\n", static_cast<unsigned long long>(vblank), buttons, lx, ly);
    }
    void dialog(std::uint64_t vblank, int slot) {
        std::fprintf(f_, "%llu dialog %d\n", static_cast<unsigned long long>(vblank), slot);
        std::fflush(f_);
    }
    void flush() { std::fflush(f_); }

private:
    std::FILE* f_;
    std::uint32_t buttons_ = 0;
    std::uint8_t lx_ = 128, ly_ = 128;
};

class InputReplay {
public:
    explicit InputReplay(nx::fs::path_view path) {
        std::FILE* f = ff7cc::io::open_c_file(path, "r");
        if (!f) throw ff7cc::Error(nx::string("cannot read ") + path.native());
        long long base = 0;
        if (std::fscanf(f, "ff7cc-input 1 %lld", &base) != 1) {
            std::fclose(f);
            throw ff7cc::Error(nx::string(path.native()) + " is not an ff7cc input recording");
        }
        clock_base_ = base;
        char kind[16];
        unsigned long long vblank = 0;
        while (std::fscanf(f, "%llu %15s", &vblank, kind) == 2) {
            Event e{vblank};
            if (nx::string_view(kind) == "pad") {
                unsigned b = 0, lx = 128, ly = 128;
                if (std::fscanf(f, "%x %u %u", &b, &lx, &ly) != 3) break;
                e.buttons = b;
                e.lx = static_cast<std::uint8_t>(lx);
                e.ly = static_cast<std::uint8_t>(ly);
            } else if (nx::string_view(kind) == "dialog") {
                if (std::fscanf(f, "%d", &e.dialog) != 1) break;
                e.is_dialog = true;
            } else {
                break;
            }
            events_.push_back(e);
        }
        std::fclose(f);
    }

    std::int64_t clock_base() const { return clock_base_; }
    bool finished() const { return next_ >= events_.size(); }
    std::uint64_t last_vblank() const { return events_.empty() ? 0 : events_.back().vblank; }

    // Advances to `vblank`: updates the pad state and returns save picker answers due now (-2: none).
    int advance(std::uint64_t vblank) {
        int dialog = -2;
        for (; next_ < events_.size() && events_[next_].vblank <= vblank; ++next_) {
            const Event& e = events_[next_];
            if (e.is_dialog) {
                dialog = e.dialog;
            } else {
                buttons_ = e.buttons;
                lx_ = e.lx;
                ly_ = e.ly;
            }
        }
        return dialog;
    }
    std::uint32_t buttons() const { return buttons_; }
    std::uint8_t lx() const { return lx_; }
    std::uint8_t ly() const { return ly_; }

private:
    struct Event {
        std::uint64_t vblank;
        std::uint32_t buttons = 0;
        std::uint8_t lx = 128, ly = 128;
        bool is_dialog = false;
        int dialog = -1;
    };
    nx::vector<Event> events_;
    std::size_t next_ = 0;
    std::int64_t clock_base_ = 0;
    std::uint32_t buttons_ = 0;
    std::uint8_t lx_ = 128, ly_ = 128;
};

struct ScriptedPress {
    std::uint64_t vblank;
    std::uint32_t buttons;
};

struct ScriptedUi {
    std::uint64_t vblank;
    nx::string action;
};

std::uint32_t button_from_name(nx::string_view name) {
    using namespace ff7cc::platform;
    static constexpr std::pair<nx::string_view, std::uint32_t> kNames[] = {
        {"select", kSelect}, {"start", kStart},   {"up", kUp},       {"right", kRight},
        {"down", kDown},     {"left", kLeft},     {"l", kLTrigger},  {"r", kRTrigger},
        {"triangle", kTriangle}, {"circle", kCircle}, {"cross", kCross}, {"square", kSquare},
    };
    for (const auto& [n, b] : kNames)
        if (n == name) return b;
    throw ff7cc::Error("unknown button: " + nx::string(name));
}

std::uint32_t scripted_buttons(const nx::vector<ScriptedPress>& presses, std::uint64_t vblank) {
    constexpr std::uint64_t kHold = 6;
    std::uint32_t buttons = 0;
    for (const auto& p : presses)
        if (vblank >= p.vblank && vblank < p.vblank + kHold) buttons |= p.buttons;
    return buttons;
}

}  // namespace
using namespace ff7cc;

namespace {

constexpr std::uint32_t kLoadBase = 0x08804000;  // must match the base ccrecomp translated for

#ifdef __ANDROID__
// The runtime logs to stderr; on Android forward it line by line to logcat (tag "ff7cc") and to `log_path`
// (logcat's ring buffer is small and shared with the whole system).
void redirect_stderr_to_logcat(nx::fs::path_view log_path) {
    static int fds[2];
    static std::FILE* log = nullptr;
    if (pipe(fds) != 0) return;
    log = io::open_c_file(log_path, "w");
    dup2(fds[1], STDERR_FILENO);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    std::thread([] {
        char buf[1024];
        nx::string line;
        for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;)
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] != '\n') {
                    line += buf[i];
                    continue;
                }
                __android_log_write(ANDROID_LOG_INFO, "ff7cc", line.c_str());
                if (log) {
                    std::fputs(line.c_str(), log);
                    std::fputc('\n', log);
                    std::fflush(log);
                }
                line.clear();
            }
    }).detach();
}
#endif

}  // namespace

int main(int argc, char** argv) {
    // Saves, settings and cooked media live in the working directory on desktop and in the app's external
    // files directory (Android/data/<package>/files) on Android, next to the extracted game folder.
    nx::string data_dir = ".";
#ifdef __ANDROID__
    if (const char* ext = SDL_GetAndroidExternalStoragePath()) data_dir = ext;
    redirect_stderr_to_logcat(path_join(data_dir, "ff7cc.log"));
    if (argc > 1) {  // launch-intent options (testing)
        std::fprintf(stderr, "arguments:");
        for (int i = 1; i < argc; ++i) std::fprintf(stderr, " [%s]", argv[i]);
        std::fprintf(stderr, "\n");
    }
    // Options may come from the launch intent (GameActivity); a leading non-option argument is the game folder.
    const bool explicit_dir = argc >= 2 && nx::string_view(argv[1]).substr(0, 2) != "--";
    const int first_option = explicit_dir ? 2 : 1;
    const nx::string game_dir = explicit_dir ? nx::string(argv[1]) : path_join(data_dir, "Crisis Core - Final Fantasy VII EU");
    // A folder created by `adb push` before the app's first start belongs to the shell user, and the app is then
    // denied access to it.
    const auto probe = nx::fs::open_file(path_join(game_dir, "PSP_GAME", "SYSDIR", "BOOT.BIN"));
    if (!probe.has_value()) {
        const bool denied = probe.error().kind == nx::fs::io_error::PermissionDenied;
        const nx::string message =
            denied ? "The app is not allowed to read:\n\n" + game_dir +
                         "\n\nThe folder was probably created before the app's first start (for example by adb push), so it "
                         "belongs to another user. Uninstall the app (this deletes its files folder), install it again, start "
                         "it once, then copy the game files. scripts/build-android.ps1 -InstallData does this in the right order."
                   : "Copy your extracted game folder (with PSP_GAME inside) and the cooked folder to:\n\n" + data_dir +
                         "\n\nExpected: " + game_dir +
                         "\n\nscripts/build-android.ps1 -InstallData can copy them from your PC.";
        std::fprintf(stderr, "%s\n", message.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, denied ? "Game files not readable" : "Game files not found",
                                 message.c_str(), nullptr);
        return 1;
    }
#else
    if (argc < 2) {
        std::fputs("usage: ff7cc <extracted disc folder> [--trace] [--vblanks N]\n", stderr);
        return 2;
    }
    const nx::string game_dir = argv[1];
    const int first_option = 2;
#endif
    try {
        bool trace = false, headless = false;
        std::uint64_t screenshot_vblank = 0, screenshot_every = 0;
        nx::string screenshot_path;
        nx::string audio_dump;
        nx::string cooked_dir;
        nx::string renderer;  // empty: the setting (Vulkan or software, switchable in the pause menu)
        int scale = 0;  // 0: from the settings file
        nx::string settings_path = path_join(data_dir, "ff7cc.ini");
        bool no_audio = false, unthrottled = false, perf = false, mute = false;
#ifdef __ANDROID__
        bool touch = true;  // shown until a gamepad is used
#else
        bool touch = false;
#endif
        std::uint64_t max_vblanks = 0;
        std::uint64_t ge_dump_vblank = 0, ge_bench_vblank = 0, profile_vblank = 0;
        std::uint64_t state_hash_every = 0;
        std::uint64_t ram_dump_vblank = 0;
        nx::string ram_dump_path;
        nx::string record_path, replay_path, saves_dir, mods_dir;
        std::int64_t clock_base = static_cast<std::int64_t>(std::time(nullptr));
        bool clock_given = false;
        nx::vector<ScriptedPress> presses;
        nx::vector<ScriptedUi> ui_script;
        std::uint64_t window_shot_vblank = 0;
        nx::string window_shot_path;
        for (int i = first_option; i < argc; ++i) {
            const nx::string_view a = argv[i];
            if (a == "--trace") trace = true;
            else if (a == "--headless") headless = true;
            else if (a == "--screenshot" && i + 2 < argc) {
                screenshot_vblank = parse_u64(argv[++i]);
                screenshot_path = argv[++i];
            } else if (a == "--screenshot-every" && i + 2 < argc) {
                screenshot_every = parse_u64(argv[++i]);
                screenshot_path = argv[++i];
            } else if (a == "--unthrottled") {
                unthrottled = true;
            } else if (a == "--touch") {
                touch = true;
            } else if (a == "--perf") {
                perf = true;
            } else if (a == "--no-audio") {
                no_audio = true;
            } else if (a == "--mute") {
                mute = true;
            } else if (a == "--renderer" && i + 1 < argc) {
                renderer = argv[++i];
            } else if (a == "--scale" && i + 1 < argc) {
                scale = std::max(1, static_cast<int>(parse_i64(argv[++i])));
            } else if (a == "--settings" && i + 1 < argc) {
                settings_path = argv[++i];
            } else if (a == "--cooked" && i + 1 < argc) {
                cooked_dir = argv[++i];
            } else if (a == "--audio-dump" && i + 1 < argc) {
                audio_dump = argv[++i];
            }
            else if (a == "--ui" && i + 2 < argc) {
                const std::uint64_t at = parse_u64(argv[++i]);
                ui_script.push_back({at, argv[++i]});
            } else if (a == "--window-shot" && i + 2 < argc) {
                window_shot_vblank = parse_u64(argv[++i]);
                window_shot_path = argv[++i];
            }
            else if (a == "--press" && i + 2 < argc) {
                const std::uint64_t at = parse_u64(argv[++i]);
                presses.push_back({at, button_from_name(argv[++i])});
            }
            else if (a == "--vblanks" && i + 1 < argc) max_vblanks = parse_u64(argv[++i]);
            else if (a == "--ge-dump" && i + 1 < argc) rt::set_ge_dump(parse_u64(argv[++i]), "ge_dump.txt");
            else if (a == "--ge-dump-at" && i + 1 < argc) ge_dump_vblank = parse_u64(argv[++i]);
            else if (a == "--ge-bench" && i + 1 < argc) ge_bench_vblank = parse_u64(argv[++i]);
            else if (a == "--profile" && i + 1 < argc) profile_vblank = parse_u64(argv[++i]);
            else if (a == "--soft-threads" && i + 1 < argc) rt::gpu::set_soft_renderer_threads(static_cast<int>(parse_u64(argv[++i])));
            else if (a == "--state-hash" && i + 1 < argc) state_hash_every = parse_u64(argv[++i]);
            else if (a == "--ram-dump" && i + 2 < argc) {
                ram_dump_vblank = parse_u64(argv[++i]);
                ram_dump_path = argv[++i];
            }
            else if (a == "--record" && i + 1 < argc) record_path = argv[++i];
            else if (a == "--replay" && i + 1 < argc) replay_path = argv[++i];
            else if (a == "--saves" && i + 1 < argc) saves_dir = argv[++i];
            else if (a == "--mods" && i + 1 < argc) mods_dir = argv[++i];
            else if (a == "--clock" && i + 1 < argc) {
                clock_base = parse_i64(argv[++i]);
                clock_given = true;
            }
        }

        rt::install_crash_handler();
        rt::GuestMemory memory;
        const auto boot = io::read_all(path_join(game_dir, "PSP_GAME", "SYSDIR", "BOOT.BIN"));
        const elf::File file(boot);
        const auto module = prx::parse(file);
        const auto image = prx::load_image(file, module, kLoadBase);
        memory.write(kLoadBase, image);

        rt::Kernel kernel(memory);
        rt::set_kernel(&kernel);
        kernel.trace_hle = trace;
        kernel.max_vblanks = max_vblanks;
        kernel.user_heap().alloc_at(kLoadBase, static_cast<std::uint32_t>(image.size()));
        // Recordings are self-contained: --record snapshots the save folder next to the recording (<file>.saves),
        // and --replay runs on a fresh copy of that snapshot (<file>.saves-run), so the game can save freely.
        nx::string saves = saves_dir.empty() ? path_join(data_dir, "saves") : saves_dir;
        if (!replay_path.empty() && saves_dir.empty()) {
            const nx::string snapshot = replay_path + ".saves", run = replay_path + ".saves-run";
            (void)nx::fs::remove_all(run);
            (void)nx::fs::create_directories(run);
            if (nx::fs::exists(snapshot)) (void)nx::fs::copy(snapshot, run, true);
            saves = run;
        }
        if (!record_path.empty()) {
            const nx::string snapshot = record_path + ".saves";
            (void)nx::fs::remove_all(snapshot);
            (void)nx::fs::create_directories(snapshot);
            if (nx::fs::exists(saves)) (void)nx::fs::copy(saves, snapshot, true);
        }
        rt::set_storage(game_dir, saves);
        rt::set_mods(mods_dir.empty() ? path_join(data_dir, "mods") : mods_dir);

        // Input recording / replay. A replay also fixes the guest clock to the recording's.
        std::unique_ptr<InputReplay> replay;
        std::unique_ptr<InputRecorder> recorder_in;
        if (!replay_path.empty()) {
            replay = std::make_unique<InputReplay>(replay_path);
            if (!clock_given) clock_base = replay->clock_base();
            std::fprintf(stderr, "replaying %s (until vblank %llu)\n", replay_path.c_str(),
                         static_cast<unsigned long long>(replay->last_vblank()));
        }
        rt::set_wall_clock_base(clock_base);
        if (!record_path.empty()) recorder_in = std::make_unique<InputRecorder>(record_path, clock_base);
        const bool deterministic_dialogs = replay || recorder_in;
        if (deterministic_dialogs) {
            rt::set_savedata_ui(true);
            rt::set_save_dialog_deferred(true);  // answers apply at the next vblank (same as in recordings)  // list dialogs wait for recorded / picked answers
            rt::set_save_dialog_deferred(true);
        }
        // Per vblank: applies due save picker answers and replayed input; records what the game sees.
        const auto input_step = [&](std::uint32_t& buttons, std::uint8_t& lx, std::uint8_t& ly) {
            const std::uint64_t vblank = kernel.vblank_count();
            if (!ram_dump_path.empty() && vblank == ram_dump_vblank) {  // debugging: user RAM to a file
                std::FILE* f = io::open_c_file(ram_dump_path, "wb");
                if (f) {
                    std::fwrite(memory.ptr(0x08800000), 1, 0x01800000, f);
                    std::fclose(f);
                }
            }
            if (state_hash_every && vblank % state_hash_every == 0) {  // determinism checks: user RAM (24 MiB)
                std::uint64_t h = 0xCBF29CE484222325ull;
                const auto* p = reinterpret_cast<const std::uint64_t*>(memory.ptr(0x08800000));
                for (std::size_t i = 0; i < 0x01800000 / 8; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
                std::fprintf(stderr, "state vblank %llu: %016llx\n", static_cast<unsigned long long>(vblank),
                             static_cast<unsigned long long>(h));
            }
            if (replay && !replay->finished()) {
                const int dialog = replay->advance(vblank);
                if (dialog != -2) rt::answer_save_dialog(dialog);
                buttons = replay->buttons();
                lx = replay->lx();
                ly = replay->ly();
                if (replay->finished()) std::fprintf(stderr, "replay finished at vblank %llu\n", static_cast<unsigned long long>(vblank));
            }
            // Headless runs have no picker: answer with the slot the PSP dialog would focus (recorded like a pick).
            if (headless && !(replay && !replay->finished()))
                if (const rt::SaveDialog* d = rt::pending_save_dialog()) rt::answer_save_dialog(d->slots.empty() ? -1 : d->focus);
            int slot = 0;
            if (rt::apply_save_dialog_answer(slot) && recorder_in) recorder_in->dialog(vblank, slot);
            if (recorder_in) recorder_in->pad(vblank, buttons, lx, ly);
        };
        if (cooked_dir.empty()) {
            cooked_dir = path_join(data_dir, "cooked");
            if (!nx::fs::exists(cooked_dir)) cooked_dir = path_join(parent_path(argv[0]), "cooked");
        }
        rt::set_cooked_root(cooked_dir);
        rt::hle_install();

        // module_start(argc = strlen(path) + 1, argv = path), as the PSP loader does.
        const nx::string boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
        const std::uint32_t args = kernel.user_heap().alloc(static_cast<std::uint32_t>(boot_path.size() + 1));
        memory.write_string(args, boot_path);

        // Window + presentation + input, unless running headless.
        std::unique_ptr<platform::Display> display;
        nx::vector<std::uint32_t> frame;
        bool screenshot_pending = false;  // GPU renderer: capture at the next flip
        std::uint64_t screenshot_at = 0;
        const auto screenshot = [&](const nx::vector<std::uint32_t>& rgba, int w, int h) {
            const std::uint64_t n = screenshot_pending ? screenshot_at : kernel.vblank_count();
            if (screenshot_every && n % screenshot_every == 0) {
                char name[32];
                std::snprintf(name, sizeof name, "_%06llu.png", static_cast<unsigned long long>(n));
                rt::write_png(screenshot_path + name, rgba.data(), w, h);
            } else if (!screenshot_every && n == screenshot_vblank) {
                rt::write_png(screenshot_path, rgba.data(), w, h);
            }
        };
        // --ge-bench: the non-indexed draws of a few vblanks, copied out, then decoded and transformed repeatedly.
        struct BenchDraw {
            rt::ge::State state;
            float morph[8];
            std::uint32_t count = 0;
            nx::vector<std::uint8_t> bytes;
        };
        nx::vector<BenchDraw> bench_draws;
        std::unique_ptr<platform::SamplingProfiler> profiler;
        const auto bench_tick = [&] {
            if (profile_vblank && kernel.vblank_count() == profile_vblank) profiler = std::make_unique<platform::SamplingProfiler>();
            if (!ge_bench_vblank) return;
            const std::uint64_t vblank = kernel.vblank_count();
            if (vblank == ge_bench_vblank) {
                rt::set_ge_draw_listener([&](const rt::ge::Draw& d) {
                    if (d.indices || !d.count) return;
                    BenchDraw b;
                    b.state = *d.state;
                    for (int i = 0; i < 8; ++i) b.morph[i] = d.state->flt(static_cast<std::uint8_t>(rt::ge::MORPHWEIGHT0 + i));
                    b.count = d.count;
                    const auto size = static_cast<std::size_t>(d.count) * static_cast<std::size_t>(d.state->format.stride());
                    const std::uint8_t* p = memory.ptr(rt::ge::vram_canonical(d.vertices));
                    b.bytes.assign(p, p + size);
                    bench_draws.push_back(std::move(b));
                });
                return;
            }
            if (vblank != ge_bench_vblank + 4) return;
            rt::set_ge_draw_listener({});
            std::size_t vertices = 0;
            for (const auto& b : bench_draws) vertices += b.count;
            nx::vector<nx::vector<rt::ge::Vertex>> decoded(bench_draws.size());
            rt::gpu::VertexPipeline vp;
            double checksum = 0;
            constexpr int kRounds = 200;
            const auto t0 = std::chrono::steady_clock::now();
            for (int round = 0; round < kRounds; ++round)
                for (std::size_t i = 0; i < bench_draws.size(); ++i) {
                    const auto& b = bench_draws[i];
                    rt::ge::decode_vertices(b.state.format, b.morph, b.bytes.data(), b.count, decoded[i]);
                    checksum += decoded[i][0].pos[0];
                }
            const auto t1 = std::chrono::steady_clock::now();
            for (int round = 0; round < kRounds; ++round)
                for (std::size_t i = 0; i < bench_draws.size(); ++i) {
                    const auto& b = bench_draws[i];
                    vp.configure(b.state, b.state.on(rt::ge::TEXTUREMAPENABLE), 256, 256, b.state.on(rt::ge::FOGENABLE));
                    for (const auto& v : decoded[i]) checksum += vp.transform(b.state, v).clip[3];
                }
            const auto t2 = std::chrono::steady_clock::now();
            const double n = static_cast<double>(vertices) * kRounds;
            {  // per format and lighting: where the transform time goes
                nx::map<std::pair<std::uint32_t, bool>, std::pair<double, std::size_t>> by_format;
                for (std::size_t i = 0; i < bench_draws.size(); ++i) {
                    const auto& b = bench_draws[i];
                    const auto s0 = std::chrono::steady_clock::now();
                    for (int round = 0; round < 20; ++round) {
                        vp.configure(b.state, b.state.on(rt::ge::TEXTUREMAPENABLE), 256, 256, b.state.on(rt::ge::FOGENABLE));
                        for (const auto& v : decoded[i]) checksum += vp.transform(b.state, v).clip[3];
                    }
                    auto& e = by_format[{b.state.format.raw & 0xFFFFFF, b.state.on(rt::ge::LIGHTINGENABLE)}];
                    e.first += std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - s0).count() / 20;
                    e.second += b.count;
                }
                for (const auto& [k, e] : by_format)
                    std::fprintf(stderr, "  format %06X lighting %d: %zu vertices, %.1f ns/vertex, %.0f us per capture\n", k.first,
                                 k.second ? 1 : 0, e.second, e.first / static_cast<double>(e.second), e.first / 1000.0);
            }
            std::fprintf(stderr, "ge bench: %zu draws, %zu vertices; decode %.2f ns/vertex, transform %.2f ns/vertex (%g)\n",
                         bench_draws.size(), vertices, std::chrono::duration<double, std::nano>(t1 - t0).count() / n,
                         std::chrono::duration<double, std::nano>(t2 - t1).count() / n, checksum);
            kernel.request_quit();
        };
        platform::VulkanGeRenderer* vulkan_renderer = nullptr;
        platform::AudioDevice* audio_device = nullptr;
        auto perf_start = std::chrono::steady_clock::now(), perf_last = perf_start;
        std::uint64_t perf_frames = 0;
        double perf_worst_ms = 0.0;  // longest host time between two vblanks
        const auto perf_tick = [&] {
            if (!perf) return;
            const auto now = std::chrono::steady_clock::now();
            perf_worst_ms = std::max(perf_worst_ms, std::chrono::duration<double, std::milli>(now - perf_last).count());
            perf_last = now;
            if (kernel.vblank_count() % 300 != 0) return;
            const double ms = std::chrono::duration<double, std::milli>(now - perf_start).count();
            const std::uint64_t frames = rt::display_frame_count();
            std::fprintf(stderr, "perf vblank %llu: %.1f ms per vblank (%.2fx real time, worst %.1f ms), %llu frames\n",
                         static_cast<unsigned long long>(kernel.vblank_count()), ms / 300.0, 300.0 * 16.683 / ms,
                         perf_worst_ms, static_cast<unsigned long long>(frames - perf_frames));
            perf_worst_ms = 0.0;
            {
                static rt::ge::Stats last;
                const rt::ge::Stats g = rt::ge_stats();
                const auto per_vblank = [&](std::uint64_t now_ns, std::uint64_t then_ns) {
                    return static_cast<double>(now_ns - then_ns) / 1e6 / 300.0;
                };
                std::fprintf(stderr, "    ge: %.2f ms/vblank in lists (decode %.2f, renderer %.2f), %.0f draws, %.0f vertices per vblank\n",
                             per_vblank(g.run_ns, last.run_ns), per_vblank(g.decode_ns, last.decode_ns),
                             per_vblank(g.render_ns, last.render_ns), static_cast<double>(g.draws - last.draws) / 300.0,
                             static_cast<double>(g.vertices - last.vertices) / 300.0);
                last = g;
            }
            if (vulkan_renderer) {
                const auto st = vulkan_renderer->take_stats();
                std::fprintf(stderr, "    vulkan draw: transform %.2f ms/vblank, textures %.2f ms/vblank\n",
                             static_cast<double>(st.transform_ns) / 1e6 / 300.0, static_cast<double>(st.texture_ns) / 1e6 / 300.0);
                std::fprintf(stderr, "    vulkan: %.1f MB texture hashing, %llu uploads (%.2f Mtexels), %llu pipelines (%.1f ms, worst %.1f)\n",
                             static_cast<double>(st.hashed_bytes) / 1e6,
                             static_cast<unsigned long long>(st.uploads), static_cast<double>(st.uploaded_texels) / 1e6,
                             static_cast<unsigned long long>(st.pipelines), st.pipeline_ms, st.worst_pipeline_ms);
            }
            if (audio_device) {
                const auto st = audio_device->take_stats();
                std::fprintf(stderr, "    audio: %llu underruns, %.1f ms skipped, %.1f ms dropped, queue %.1f..%.1f ms, %llu fell behind\n",
                             static_cast<unsigned long long>(st.underruns), st.skipped_frames / 44.1,
                             st.dropped_frames / 44.1, st.min_queued / 44.1, st.max_queued / 44.1,
                             static_cast<unsigned long long>(kernel.fell_behind_count()));
            }
            perf_start = now;
            perf_frames = frames;
        };
        if (headless)
            kernel.on_vblank = [&] {
                perf_tick();
                bench_tick();
                if (ge_dump_vblank && kernel.vblank_count() == ge_dump_vblank)
                    rt::set_ge_dump(rt::display_frame_count() + 1, "ge_dump.txt");
                std::uint32_t buttons = scripted_buttons(presses, kernel.vblank_count());
                std::uint8_t lx = 128, ly = 128;
                input_step(buttons, lx, ly);
                rt::set_pad(buttons, lx, ly);
                int w = 0, h = 0;
                if (!screenshot_path.empty() && rt::read_framebuffer(frame, w, h)) screenshot(frame, w, h);
            };
        platform::Settings settings = platform::Settings::load(settings_path);
        if (scale) settings.scale = scale;
        std::unique_ptr<platform::Overlay> overlay;
        std::unique_ptr<platform::AudioDevice> audio;
        std::unique_ptr<WavRecorder> recorder;
        if (!headless && !no_audio) audio = std::make_unique<platform::AudioDevice>();
        audio_device = audio.get();
        if (!audio_dump.empty()) recorder = std::make_unique<WavRecorder>(audio_dump, audio.get());
        rt::set_audio_sink(recorder ? static_cast<rt::AudioSink*>(recorder.get()) : audio.get());
        if (!headless) {
            display = std::make_unique<platform::Display>("Crisis Core -Final Fantasy VII-", 960, 544);
            display->set_touch_controls(touch);
            // The renderer can change at run time (pause menu) unless the command line fixed it.
            const bool renderer_choice = renderer.empty();
            if (renderer_choice) renderer = settings.software_renderer ? "soft" : "vulkan";
            bool gpu = renderer != "soft";
            bool switch_renderer = false;
            const auto apply = [&](const platform::Settings& s) {
                if (vulkan_renderer) {
                    vulkan_renderer->set_scale(s.scale);
                    vulkan_renderer->set_msaa(s.msaa);
                }
                if (display->fullscreen() != s.fullscreen) display->set_fullscreen(s.fullscreen);
                if (audio) audio->set_volume(mute ? 0.0f : static_cast<float>(s.volume) / 100.0f);
                kernel.realtime = s.frame_limit && !unthrottled;
                if (renderer_choice) switch_renderer = s.software_renderer == gpu;  // applied when the menu closes
            };
            apply(settings);
            platform::Overlay::Hooks hooks;
            hooks.apply = apply;
            hooks.quit = [&] { kernel.request_quit(); };
            hooks.renderer_choice = renderer_choice;
            hooks.gpu_renderer = gpu;
            overlay = std::make_unique<platform::Overlay>(*display, settings, std::move(hooks));
            display->set_ui([&] { overlay->draw(); });
            rt::set_savedata_ui(true);
            // The pause menu stops the game inside on_vblank (the host main thread) until it is closed.
            bool window_shot_armed = false, window_shot_taken = false;
            const auto arm_window_shot = [&] {
                window_shot_armed = false;
                display->capture_next_present([&](const std::uint32_t* rgba, int w, int h) {
                    rt::write_png(window_shot_path, rgba, w, h);
                    window_shot_taken = true;
                });
            };
            const auto pause = [&] {
                const platform::Settings before = settings;
                for (int frame = 0; overlay->menu_open() && !kernel.quit_requested(); ++frame) {
                    if (window_shot_taken && !ui_script.empty()) overlay->close_menu();  // scripted test run
                    if (window_shot_armed && frame == 5) arm_window_shot();  // auto-sized windows show after a frame
                    if (!display->poll_events()) kernel.request_quit();
                    if (display->take_menu_request()) overlay->handle_menu_request();
                    if (!display->present_again()) std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                if (settings != before && !settings.save(settings_path))
                    std::fprintf(stderr, "could not write %s\n", settings_path.c_str());
            };
            {
                rt::set_ge_renderer_factory([&](rt::GuestMemory& mem) {
                    vulkan_renderer = nullptr;
                    if (!gpu) return rt::gpu::make_soft_renderer(mem);
                    auto r = std::make_unique<platform::VulkanGeRenderer>(display->vulkan(), mem, settings.scale,
                                                                          path_join(data_dir, "vulkan_pipelines.bin"));
                    r->set_msaa(settings.msaa);
                    vulkan_renderer = r.get();
                    if (renderer != "compare") return std::unique_ptr<rt::ge::Renderer>(std::move(r));
                    r->set_guest_copies(false);  // the software renderer already copies in guest memory
                    return std::unique_ptr<rt::ge::Renderer>(
                        std::make_unique<TeeRenderer>(rt::gpu::make_soft_renderer(mem), std::move(r)));
                });
                rt::set_flip_handler([&](std::uint32_t address, std::uint32_t stride, std::uint32_t format) {
                    if (!vulkan_renderer) return;
                    const bool capture = screenshot_pending;
                    const auto out = vulkan_renderer->finish_frame(address, stride, static_cast<int>(format), capture ? &frame : nullptr);
                    if (out.image) display->present_image(out.image, out.width, out.height);
                    if (capture && renderer == "compare") {
                        nx::vector<std::uint32_t> ref;
                        int w = 0, h = 0;
                        if (rt::read_framebuffer(ref, w, h) && w * vulkan_renderer->scale() == out.width) {
                            double mean = 0, bad = 0;
                            const auto composite = compare_frames(ref, frame, w, h, vulkan_renderer->scale(), mean, bad);
                            std::fprintf(stderr, "compare vblank %llu: mean difference %.2f, %.2f%% of pixels over 32\n",
                                         static_cast<unsigned long long>(screenshot_at), mean, bad);
                            screenshot(composite, w * 3, h);
                        }
                        screenshot_pending = false;
                    } else if (capture) {
                        screenshot(frame, out.width, out.height);
                        screenshot_pending = false;
                    }
                });
            }
            kernel.on_vblank = [&, pause, arm_window_shot] {
                perf_tick();
                bench_tick();
                const std::uint64_t vblank = kernel.vblank_count();
                if (ge_dump_vblank && vblank == ge_dump_vblank) rt::set_ge_dump(rt::display_frame_count() + 1, "ge_dump.txt");
                for (const auto& u : ui_script) {
                    if (u.vblank != vblank) continue;
                    if (u.action == "menu") overlay->handle_menu_request();
                    else if (u.action == "enter") display->inject_key(SDLK_RETURN);
                    else if (u.action == "escape") display->inject_key(SDLK_ESCAPE);
                    else if (u.action == "up") display->inject_key(SDLK_UP);
                    else if (u.action == "down") display->inject_key(SDLK_DOWN);
                    else if (u.action == "space") display->inject_key(SDLK_SPACE);
                    else if (u.action == "renderer:vulkan" || u.action == "renderer:software") {
                        settings.software_renderer = u.action == "renderer:software";
                        apply(settings);
                    }
                    else if (u.action.starts_with("tap:")) {  // tap:X,Y in window-normalised coordinates
                        const auto comma = u.action.find(',');
                        if (comma != nx::string::npos)
                            display->inject_finger(static_cast<float>(parse_double(u.action.substr(4, comma - 4))), static_cast<float>(parse_double(u.action.substr(comma + 1))), 8);
                    }
                }
                if (!window_shot_path.empty() && vblank == window_shot_vblank) window_shot_armed = true;
                if (!display->poll_events()) kernel.request_quit();
                if (display->in_background()) {  // Android: hold the game until the app returns
                    if (audio) audio->set_paused(true);
                    while (display->in_background() && !kernel.quit_requested()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        if (!display->poll_events()) kernel.request_quit();
                    }
                    if (audio) audio->set_paused(false);
                }
                if (display->take_menu_request()) overlay->handle_menu_request();
                if (overlay->menu_open()) pause();
                if (switch_renderer) {
                    switch_renderer = false;
                    gpu = !gpu;
                    renderer = gpu ? "vulkan" : "soft";
                    display->forget_last_image();
                    rt::reset_ge_renderer();
                    std::fprintf(stderr, "renderer: %s\n", gpu ? "Vulkan" : "software");
                }
                if (window_shot_armed) arm_window_shot();
                const auto pad = overlay->wants_input() ? platform::PadState{} : display->pad();
                std::uint32_t buttons = pad.buttons | scripted_buttons(presses, kernel.vblank_count());
                std::uint8_t lx = pad.lx, ly = pad.ly;
                input_step(buttons, lx, ly);
                rt::set_pad(buttons, lx, ly);
                if (gpu && !screenshot_path.empty()) {  // captured at the next flip
                    const std::uint64_t n = kernel.vblank_count();
                    if ((screenshot_every && n % screenshot_every == 0) || (!screenshot_every && n == screenshot_vblank)) {
                        screenshot_pending = true;
                        screenshot_at = n;
                    }
                }
                int w = 0, h = 0;
                if (!gpu && rt::read_framebuffer(frame, w, h)) {
                    display->present(frame.data(), w, h);
                    if (!screenshot_path.empty()) screenshot(frame, w, h);
                }
            };
        }

        std::fprintf(stderr, "%s loaded at 0x%08X (%zu bytes), entry 0x%08X\n", module.name.c_str(), kLoadBase,
                     image.size(), kLoadBase + module.entry);
        const int code = kernel.run(kLoadBase + module.entry, static_cast<std::uint32_t>(boot_path.size() + 1), args);
        std::fprintf(stderr, "stopped after %.3f s of guest time (%llu vblanks)\n",
                     static_cast<double>(kernel.now()) / 1e6, static_cast<unsigned long long>(kernel.vblank_count()));
        if (profiler) profiler->report(stderr);
        if (recorder) recorder->flush();
        if (recorder_in) recorder_in->flush();
        if (trace || headless) kernel.dump_threads(stderr);
        std::fflush(stderr);
        std::_Exit(code);  // guest threads may still be parked on their batons
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ff7cc: %s\n", e.what());
        return 1;
    }
}
