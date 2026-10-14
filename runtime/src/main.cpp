// ff7cc — runs the recompiled game in a window (or headless, for testing).
//
//   ff7cc <extracted disc folder> [--headless] [--trace] [--vblanks N] [--ge-dump FRAME] [--screenshot VBLANK out.png]
//       [--screenshot-every N prefix]   (writes prefix_<vblank>.png every N vblanks)
//       [--renderer vulkan|soft|compare] [--scale N]   (window only; headless always uses the software renderer;
//                                        compare runs both and screenshots show reference | Vulkan | difference)
//       [--ge-dump-at VBLANK]           (writes ge_dump.txt for the first full frame after VBLANK)
//       [--settings FILE]               (default ./ff7cc.ini; written when changed in the pause menu)
//       [--touch]                       (show the on-screen touch controls from the start; default on Android)
//       [--perf]                        (prints host time per 300 vblanks and display frames)
//       [--no-audio] [--unthrottled]       (testing: no sound device; run as fast as possible)
//       [--cooked DIR]                  (cooked media from cccook; default ./cooked)
//       [--audio-dump prefix]           (writes prefix_<source>.wav per audio source at exit)
//       [--press VBLANK BUTTON]...      (scripted input: holds start/select/cross/circle/square/triangle/
//                                        up/down/left/right/l/r for 6 vblanks; for automated testing)
//       [--ui VBLANK ACTION]...         (scripted overlay input: menu, enter, escape, up, down, space,
//                                        tap:X,Y for a touch at window-normalised coordinates)
//       [--window-shot VBLANK out.png]  (captures the window, overlay included; a pause menu opened by --ui
//                                        closes itself after this capture so the run can continue)

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
#include "ff7cc/platform/audio.hpp"
#include "ff7cc/platform/display.hpp"
#include "platform/gpu_vulkan.hpp"
#include "platform/overlay.hpp"
#include "platform/settings.hpp"
#include "gpu/soft_renderer.hpp"
#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/io.hpp"
#include "ff7cc/runtime/kernel.hpp"
#include "ff7cc/runtime/memory.hpp"

namespace fs = std::filesystem;

namespace {

// Records every audio source to <prefix>_<source>.wav (and forwards to the device, if any). For testing.
class WavRecorder final : public ff7cc::rt::AudioSink {
public:
    WavRecorder(std::string prefix, ff7cc::rt::AudioSink* next) : prefix_(std::move(prefix)), next_(next) {}
    ~WavRecorder() override { flush(); }

    void submit(int source, const std::int16_t* frames, std::size_t count) override {
        if (source >= 0 && source < kSources) samples_[source].insert(samples_[source].end(), frames, frames + count * 2);
        if (next_) next_->submit(source, frames, count);
    }
    void flush() {
        for (int s = 0; s < kSources; ++s) {
            const auto& pcm = samples_[s];
            if (pcm.empty()) continue;
            const std::string path = prefix_ + "_" + std::to_string(s) + ".wav";
            std::FILE* f = std::fopen(path.c_str(), "wb");
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
    std::string prefix_;
    ff7cc::rt::AudioSink* next_;
    std::vector<std::int16_t> samples_[kSources];
};

// Sends every display list to two renderers (--renderer compare).
class TeeRenderer final : public ff7cc::rt::ge::Renderer {
public:
    TeeRenderer(std::unique_ptr<Renderer> a, std::unique_ptr<Renderer> b) : a_(std::move(a)), b_(std::move(b)) {}
    void draw(const ff7cc::rt::ge::State& s, ff7cc::rt::ge::Prim prim, const std::vector<ff7cc::rt::ge::Vertex>& v) override {
        a_->draw(s, prim, v);
        b_->draw(s, prim, v);
    }
    void transfer(const ff7cc::rt::ge::State& s) override {
        a_->transfer(s);
        b_->transfer(s);
    }

private:
    std::unique_ptr<Renderer> a_, b_;
};

// Reference frame (w x h) next to the GPU frame (box-filtered down from `scale` times the size) and their
// difference (x4). Returns the composite; `mean` is the mean per-pixel max-channel difference and `bad` the
// share of pixels differing by more than 32.
std::vector<std::uint32_t> compare_frames(const std::vector<std::uint32_t>& ref, const std::vector<std::uint32_t>& gpu, int w,
                                          int h, int scale, double& mean, double& bad) {
    std::vector<std::uint32_t> out(static_cast<std::size_t>(w) * 3 * h, 0xFF000000u);
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

struct ScriptedPress {
    std::uint64_t vblank;
    std::uint32_t buttons;
};

struct ScriptedUi {
    std::uint64_t vblank;
    std::string action;
};

std::uint32_t button_from_name(std::string_view name) {
    using namespace ff7cc::platform;
    static constexpr std::pair<std::string_view, std::uint32_t> kNames[] = {
        {"select", kSelect}, {"start", kStart},   {"up", kUp},       {"right", kRight},
        {"down", kDown},     {"left", kLeft},     {"l", kLTrigger},  {"r", kRTrigger},
        {"triangle", kTriangle}, {"circle", kCircle}, {"cross", kCross}, {"square", kSquare},
    };
    for (const auto& [n, b] : kNames)
        if (n == name) return b;
    throw std::runtime_error("unknown button: " + std::string(name));
}

std::uint32_t scripted_buttons(const std::vector<ScriptedPress>& presses, std::uint64_t vblank) {
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
void redirect_stderr_to_logcat(const fs::path& log_path) {
    static int fds[2];
    static std::FILE* log = nullptr;
    if (pipe(fds) != 0) return;
    log = std::fopen(log_path.string().c_str(), "w");
    dup2(fds[1], STDERR_FILENO);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    std::thread([] {
        char buf[1024];
        std::string line;
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
    fs::path data_dir = fs::current_path();
#ifdef __ANDROID__
    if (const char* ext = SDL_GetAndroidExternalStoragePath()) data_dir = ext;
    redirect_stderr_to_logcat(data_dir / "ff7cc.log");
    // Options may come from the launch intent (GameActivity); a leading non-option argument is the game folder.
    const bool explicit_dir = argc >= 2 && std::string_view(argv[1]).substr(0, 2) != "--";
    const int first_option = explicit_dir ? 2 : 1;
    const fs::path game_dir = explicit_dir ? fs::path(argv[1]) : data_dir / "Crisis Core - Final Fantasy VII EU";
    if (!fs::exists(game_dir / "PSP_GAME" / "SYSDIR" / "BOOT.BIN")) {
        const std::string message = "Copy your extracted game folder (with PSP_GAME inside) and the cooked folder to:\n\n" +
                                    data_dir.string() + "\n\nExpected: " + game_dir.string();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Game files not found", message.c_str(), nullptr);
        return 1;
    }
#else
    if (argc < 2) {
        std::fputs("usage: ff7cc <extracted disc folder> [--trace] [--vblanks N]\n", stderr);
        return 2;
    }
    const fs::path game_dir = argv[1];
    const int first_option = 2;
#endif
    try {
        bool trace = false, headless = false;
        std::uint64_t screenshot_vblank = 0, screenshot_every = 0;
        std::string screenshot_path;
        std::string audio_dump;
        fs::path cooked_dir;
        std::string renderer = "vulkan";
        int scale = 0;  // 0: from the settings file
        fs::path settings_path = data_dir / "ff7cc.ini";
        bool no_audio = false, unthrottled = false, perf = false;
#ifdef __ANDROID__
        bool touch = true;  // shown until a gamepad is used
#else
        bool touch = false;
#endif
        std::uint64_t max_vblanks = 0;
        std::uint64_t ge_dump_vblank = 0;
        std::vector<ScriptedPress> presses;
        std::vector<ScriptedUi> ui_script;
        std::uint64_t window_shot_vblank = 0;
        std::string window_shot_path;
        for (int i = first_option; i < argc; ++i) {
            const std::string_view a = argv[i];
            if (a == "--trace") trace = true;
            else if (a == "--headless") headless = true;
            else if (a == "--screenshot" && i + 2 < argc) {
                screenshot_vblank = std::stoull(argv[++i]);
                screenshot_path = argv[++i];
            } else if (a == "--screenshot-every" && i + 2 < argc) {
                screenshot_every = std::stoull(argv[++i]);
                screenshot_path = argv[++i];
            } else if (a == "--unthrottled") {
                unthrottled = true;
            } else if (a == "--touch") {
                touch = true;
            } else if (a == "--perf") {
                perf = true;
            } else if (a == "--no-audio") {
                no_audio = true;
            } else if (a == "--renderer" && i + 1 < argc) {
                renderer = argv[++i];
            } else if (a == "--scale" && i + 1 < argc) {
                scale = std::max(1, std::stoi(argv[++i]));
            } else if (a == "--settings" && i + 1 < argc) {
                settings_path = argv[++i];
            } else if (a == "--cooked" && i + 1 < argc) {
                cooked_dir = argv[++i];
            } else if (a == "--audio-dump" && i + 1 < argc) {
                audio_dump = argv[++i];
            }
            else if (a == "--ui" && i + 2 < argc) {
                const std::uint64_t at = std::stoull(argv[++i]);
                ui_script.push_back({at, argv[++i]});
            } else if (a == "--window-shot" && i + 2 < argc) {
                window_shot_vblank = std::stoull(argv[++i]);
                window_shot_path = argv[++i];
            }
            else if (a == "--press" && i + 2 < argc) {
                const std::uint64_t at = std::stoull(argv[++i]);
                presses.push_back({at, button_from_name(argv[++i])});
            }
            else if (a == "--vblanks" && i + 1 < argc) max_vblanks = std::stoull(argv[++i]);
            else if (a == "--ge-dump" && i + 1 < argc) rt::set_ge_dump(std::stoull(argv[++i]), "ge_dump.txt");
            else if (a == "--ge-dump-at" && i + 1 < argc) ge_dump_vblank = std::stoull(argv[++i]);
        }

        rt::install_crash_handler();
        rt::GuestMemory memory;
        const auto boot = io::read_all(game_dir / "PSP_GAME" / "SYSDIR" / "BOOT.BIN");
        const elf::File file(boot);
        const auto module = prx::parse(file);
        const auto image = prx::load_image(file, module, kLoadBase);
        memory.write(kLoadBase, image);

        rt::Kernel kernel(memory);
        rt::set_kernel(&kernel);
        kernel.trace_hle = trace;
        kernel.max_vblanks = max_vblanks;
        kernel.user_heap().alloc_at(kLoadBase, static_cast<std::uint32_t>(image.size()));
        rt::set_storage(game_dir, data_dir / "saves");
        if (cooked_dir.empty()) {
            cooked_dir = data_dir / "cooked";
            if (!fs::exists(cooked_dir)) cooked_dir = fs::path(argv[0]).parent_path() / "cooked";
        }
        rt::set_cooked_root(cooked_dir);
        rt::hle_install();

        // module_start(argc = strlen(path) + 1, argv = path), as the PSP loader does.
        const std::string boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
        const std::uint32_t args = kernel.user_heap().alloc(static_cast<std::uint32_t>(boot_path.size() + 1));
        memory.write_string(args, boot_path);

        // Window + presentation + input, unless running headless.
        std::unique_ptr<platform::Display> display;
        std::vector<std::uint32_t> frame;
        bool screenshot_pending = false;  // GPU renderer: capture at the next flip
        std::uint64_t screenshot_at = 0;
        const auto screenshot = [&](const std::vector<std::uint32_t>& rgba, int w, int h) {
            const std::uint64_t n = screenshot_pending ? screenshot_at : kernel.vblank_count();
            if (screenshot_every && n % screenshot_every == 0) {
                char name[32];
                std::snprintf(name, sizeof name, "_%06llu.png", static_cast<unsigned long long>(n));
                rt::write_png(screenshot_path + name, rgba.data(), w, h);
            } else if (!screenshot_every && n == screenshot_vblank) {
                rt::write_png(screenshot_path, rgba.data(), w, h);
            }
        };
        platform::VulkanGeRenderer* vulkan_renderer = nullptr;
        auto perf_start = std::chrono::steady_clock::now();
        std::uint64_t perf_frames = 0;
        const auto perf_tick = [&] {
            if (!perf || kernel.vblank_count() % 300 != 0) return;
            const auto now = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now - perf_start).count();
            const std::uint64_t frames = rt::display_frame_count();
            std::fprintf(stderr, "perf vblank %llu: %.1f ms per vblank (%.2fx real time), %llu frames\n",
                         static_cast<unsigned long long>(kernel.vblank_count()), ms / 300.0, 300.0 * 16.683 / ms,
                         static_cast<unsigned long long>(frames - perf_frames));
            if (vulkan_renderer) {
                const auto st = vulkan_renderer->take_stats();
                std::fprintf(stderr, "    vulkan: %.1f MB texture hashing, %llu uploads (%.2f Mtexels)\n",
                             static_cast<double>(st.hashed_bytes) / 1e6,
                             static_cast<unsigned long long>(st.uploads), static_cast<double>(st.uploaded_texels) / 1e6);
            }
            perf_start = now;
            perf_frames = frames;
        };
        if (headless)
            kernel.on_vblank = [&] {
                perf_tick();
                if (ge_dump_vblank && kernel.vblank_count() == ge_dump_vblank)
                    rt::set_ge_dump(rt::display_frame_count() + 1, "ge_dump.txt");
                rt::set_pad(scripted_buttons(presses, kernel.vblank_count()), 128, 128);
                int w = 0, h = 0;
                if (!screenshot_path.empty() && rt::read_framebuffer(frame, w, h)) screenshot(frame, w, h);
            };
        platform::Settings settings = platform::Settings::load(settings_path);
        if (scale) settings.scale = scale;
        std::unique_ptr<platform::Overlay> overlay;
        std::unique_ptr<platform::AudioDevice> audio;
        std::unique_ptr<WavRecorder> recorder;
        if (!headless && !no_audio) audio = std::make_unique<platform::AudioDevice>();
        if (!audio_dump.empty()) recorder = std::make_unique<WavRecorder>(audio_dump, audio.get());
        rt::set_audio_sink(recorder ? static_cast<rt::AudioSink*>(recorder.get()) : audio.get());
        if (!headless) {
            display = std::make_unique<platform::Display>("Crisis Core -Final Fantasy VII-", 960, 544);
            display->set_touch_controls(touch);
            const bool gpu = renderer != "soft";
            const auto apply = [&](const platform::Settings& s) {
                if (vulkan_renderer) vulkan_renderer->set_scale(s.scale);
                if (display->fullscreen() != s.fullscreen) display->set_fullscreen(s.fullscreen);
                if (audio) audio->set_volume(static_cast<float>(s.volume) / 100.0f);
                kernel.realtime = s.frame_limit && !unthrottled;
            };
            apply(settings);
            platform::Overlay::Hooks hooks;
            hooks.apply = apply;
            hooks.quit = [&] { kernel.request_quit(); };
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
                    std::fprintf(stderr, "could not write %s\n", settings_path.string().c_str());
            };
            if (gpu) {
                rt::set_ge_renderer_factory([&](rt::GuestMemory& mem) {
                    auto r = std::make_unique<platform::VulkanGeRenderer>(display->vulkan(), mem, settings.scale);
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
                        std::vector<std::uint32_t> ref;
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
            kernel.on_vblank = [&, gpu, pause, arm_window_shot] {
                perf_tick();
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
                    else if (u.action.starts_with("tap:")) {  // tap:X,Y in window-normalised coordinates
                        const auto comma = u.action.find(',');
                        if (comma != std::string::npos)
                            display->inject_finger(std::stof(u.action.substr(4, comma - 4)), std::stof(u.action.substr(comma + 1)), 8);
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
                if (window_shot_armed) arm_window_shot();
                const auto pad = overlay->wants_input() ? platform::PadState{} : display->pad();
                rt::set_pad(pad.buttons | scripted_buttons(presses, kernel.vblank_count()), pad.lx, pad.ly);
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
        if (recorder) recorder->flush();
        if (trace || headless) kernel.dump_threads(stderr);
        std::fflush(stderr);
        std::_Exit(code);  // guest threads may still be parked on their batons
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ff7cc: %s\n", e.what());
        return 1;
    }
}
