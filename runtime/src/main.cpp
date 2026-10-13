// ff7cc — runs the recompiled game (headless for now).
//
//   ff7cc <extracted disc folder> [--headless] [--trace] [--vblanks N] [--ge-dump FRAME] [--screenshot VBLANK out.png]

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <memory>
#include <vector>

#include "ff7cc/formats/elf.hpp"
#include "ff7cc/formats/prx.hpp"
#include "ff7cc/io/file.hpp"
#include "ff7cc/platform/display.hpp"
#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/io.hpp"
#include "ff7cc/runtime/kernel.hpp"
#include "ff7cc/runtime/memory.hpp"

namespace fs = std::filesystem;
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
        std::uint64_t screenshot_vblank = 0;
        std::string screenshot_path;
        std::uint64_t max_vblanks = 0;
        for (int i = 2; i < argc; ++i) {
            const std::string_view a = argv[i];
            if (a == "--trace") trace = true;
            else if (a == "--headless") headless = true;
            else if (a == "--screenshot" && i + 2 < argc) {
                screenshot_vblank = std::stoull(argv[++i]);
                screenshot_path = argv[++i];
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
        rt::hle_install();

        // module_start(argc = strlen(path) + 1, argv = path), as the PSP loader does.
        const std::string boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
        const std::uint32_t args = kernel.user_heap().alloc(static_cast<std::uint32_t>(boot_path.size() + 1));
        memory.write_string(args, boot_path);

        // Window + presentation + input, unless running headless.
        std::unique_ptr<platform::Display> display;
        std::vector<std::uint32_t> frame;
        if (headless && !screenshot_path.empty())
            kernel.on_vblank = [&] {
                int w = 0, h = 0;
                if (kernel.vblank_count() == screenshot_vblank && rt::read_framebuffer(frame, w, h))
                    rt::write_png(screenshot_path, frame.data(), w, h);
            };
        if (!headless) {
            display = std::make_unique<platform::Display>("Crisis Core -Final Fantasy VII-", 960, 544);
            kernel.realtime = true;
            kernel.on_vblank = [&] {
                if (!display->poll_events()) kernel.request_quit();
                const auto pad = display->pad();
                rt::set_pad(pad.buttons, pad.lx, pad.ly);
                int w = 0, h = 0;
                if (rt::read_framebuffer(frame, w, h)) {
                    display->present(frame.data(), w, h);
                    if (!screenshot_path.empty() && kernel.vblank_count() == screenshot_vblank)
                        rt::write_png(screenshot_path, frame.data(), w, h);
                }
            };
        }

        std::fprintf(stderr, "%s loaded at 0x%08X (%zu bytes), entry 0x%08X\n", module.name.c_str(), kLoadBase,
                     image.size(), kLoadBase + module.entry);
        const int code = kernel.run(kLoadBase + module.entry, static_cast<std::uint32_t>(boot_path.size() + 1), args);
        std::fprintf(stderr, "stopped after %.3f s of guest time (%llu vblanks)\n",
                     static_cast<double>(kernel.now()) / 1e6, static_cast<unsigned long long>(kernel.vblank_count()));
        std::fflush(stderr);
        std::_Exit(code);  // guest threads may still be parked on their batons
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ff7cc: %s\n", e.what());
        return 1;
    }
}
