// ff7cc — runs the recompiled game (headless for now).
//
//   ff7cc <extracted disc folder> [--headless] [--trace] [--vblanks N] [--ge-dump FRAME] [--screenshot VBLANK out.png]
//       [--screenshot-every N prefix]   (writes prefix_<vblank>.png every N vblanks)
//       [--renderer vulkan|soft] [--scale N]   (window only; headless always uses the software renderer)
//       [--no-audio] [--unthrottled]       (testing: no sound device; run as fast as possible)
//       [--cooked DIR]                  (cooked media from cccook; default ./cooked)
//       [--audio-dump prefix]           (writes prefix_<source>.wav per audio source at exit)
//       [--press VBLANK BUTTON]...      (scripted input: holds start/select/cross/circle/square/triangle/
//                                        up/down/left/right/l/r for 6 vblanks; for automated testing)

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ff7cc/formats/elf.hpp"
#include "ff7cc/formats/prx.hpp"
#include "ff7cc/io/file.hpp"
#include "ff7cc/platform/audio.hpp"
#include "ff7cc/platform/display.hpp"
#include "platform/gpu_vulkan.hpp"
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

struct ScriptedPress {
    std::uint64_t vblank;
    std::uint32_t buttons;
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: ff7cc <extracted disc folder> [--trace] [--vblanks N]\n", stderr);
        return 2;
    }
    try {
        const fs::path game_dir = argv[1];
        bool trace = false, headless = false;
        std::uint64_t screenshot_vblank = 0, screenshot_every = 0;
        std::string screenshot_path;
        std::string audio_dump;
        fs::path cooked_dir;
        std::string renderer = "vulkan";
        int scale = 2;
        bool no_audio = false, unthrottled = false;
        std::uint64_t max_vblanks = 0;
        std::vector<ScriptedPress> presses;
        for (int i = 2; i < argc; ++i) {
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
            } else if (a == "--no-audio") {
                no_audio = true;
            } else if (a == "--renderer" && i + 1 < argc) {
                renderer = argv[++i];
            } else if (a == "--scale" && i + 1 < argc) {
                scale = std::max(1, std::stoi(argv[++i]));
            } else if (a == "--cooked" && i + 1 < argc) {
                cooked_dir = argv[++i];
            } else if (a == "--audio-dump" && i + 1 < argc) {
                audio_dump = argv[++i];
            }
            else if (a == "--press" && i + 2 < argc) {
                const std::uint64_t at = std::stoull(argv[++i]);
                presses.push_back({at, button_from_name(argv[++i])});
            }
            else if (a == "--vblanks" && i + 1 < argc) max_vblanks = std::stoull(argv[++i]);
            else if (a == "--ge-dump" && i + 1 < argc) rt::set_ge_dump(std::stoull(argv[++i]), "ge_dump.txt");
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
        rt::set_storage(game_dir, fs::current_path() / "saves");
        if (cooked_dir.empty()) {
            cooked_dir = fs::current_path() / "cooked";
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
        if (headless)
            kernel.on_vblank = [&] {
                rt::set_pad(scripted_buttons(presses, kernel.vblank_count()), 128, 128);
                int w = 0, h = 0;
                if (!screenshot_path.empty() && rt::read_framebuffer(frame, w, h)) screenshot(frame, w, h);
            };
        platform::VulkanGeRenderer* vulkan_renderer = nullptr;
        std::unique_ptr<platform::AudioDevice> audio;
        std::unique_ptr<WavRecorder> recorder;
        if (!headless && !no_audio) audio = std::make_unique<platform::AudioDevice>();
        if (!audio_dump.empty()) recorder = std::make_unique<WavRecorder>(audio_dump, audio.get());
        rt::set_audio_sink(recorder ? static_cast<rt::AudioSink*>(recorder.get()) : audio.get());
        if (!headless) {
            display = std::make_unique<platform::Display>("Crisis Core -Final Fantasy VII-", 960, 544);
            kernel.realtime = !unthrottled;
            const bool gpu = renderer != "soft";
            if (gpu) {
                rt::set_ge_renderer_factory([&](rt::GuestMemory& mem) {
                    auto r = std::make_unique<platform::VulkanGeRenderer>(display->vulkan(), mem, scale);
                    vulkan_renderer = r.get();
                    return std::unique_ptr<rt::ge::Renderer>(std::move(r));
                });
                rt::set_flip_handler([&](std::uint32_t address, std::uint32_t stride, std::uint32_t format) {
                    if (!vulkan_renderer) return;
                    const bool capture = screenshot_pending;
                    const auto out = vulkan_renderer->finish_frame(address, stride, static_cast<int>(format), capture ? &frame : nullptr);
                    if (out.image) display->present_image(out.image, out.width, out.height);
                    if (capture) {
                        screenshot(frame, out.width, out.height);
                        screenshot_pending = false;
                    }
                });
            }
            kernel.on_vblank = [&, gpu] {
                if (!display->poll_events()) kernel.request_quit();
                const auto pad = display->pad();
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
