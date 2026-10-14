#pragma once

// High-level emulation of the PSP system libraries the game imports.
//
// hle_call(ctx, nid) (recomp.h) dispatches through a table built from hle_imports(); implementations are bound
// by function name. Arguments follow the PSP EABI: a0-a3, t0-t3; results go to v0 (and v1 for 64-bit values).
// Calls without an implementation are logged once and return 0.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ff7cc/recomp/recomp.h"

namespace ff7cc::rt {

class GuestMemory;
namespace ge {
class Renderer;
}

struct HleImport {
    const char* library;
    std::uint32_t nid;
    const char* name;
};
const std::vector<HleImport>& hle_imports();

using HleFunction = void (*)(Context& ctx);
struct HleBinding {
    const char* name;
    HleFunction fn;
};

void hle_bind(const HleBinding* bindings, std::size_t count);
template <std::size_t N>
void hle_bind(const HleBinding (&bindings)[N]) { hle_bind(bindings, N); }

// Prints the fault and a symbolised host stack on crashes (Windows).
void install_crash_handler();

// Writes every GE command executed during display frame `frame` to `path` (debugging).
void set_ge_dump(std::uint64_t frame, const char* path);
std::uint64_t display_frame_count();  // framebuffer flips so far (the frame numbering set_ge_dump uses)

// Write tracking for renderers that cache guest memory (textures). The PSP's GE only sees CPU writes after a data
// cache writeback, so writebacks, DMA-style writes by HLE functions (file reads, movie frames, sceDmacMemcpy) and
// GE block transfers mark what may have changed; frame flips mark everything (writes through uncached addresses
// cannot be seen). A cache entry checked at write_counter() is still valid while written_since(range) is not
// newer. Tracking is per 64 KiB page.
void mark_guest_written(std::uint32_t address, std::uint32_t size);
void mark_guest_written_all();
std::uint64_t guest_write_counter();
std::uint64_t guest_written_since(std::uint32_t address, std::uint32_t size);  // newest mark covering the range

// GE renderer used for display lists (call before the game starts; default: the software renderer).
void set_ge_renderer_factory(std::function<std::unique_ptr<ge::Renderer>(GuestMemory&)> factory);
// Called on every framebuffer flip (sceDisplaySetFrameBuf) with the address, stride and pixel format.
void set_flip_handler(std::function<void(std::uint32_t address, std::uint32_t stride, std::uint32_t format)> handler);

// Display output and controller input for the host frontend.
bool read_framebuffer(std::vector<std::uint32_t>& rgba, int& width, int& height);  // current frame as RGBA8
void set_pad(std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly);
bool write_png(const std::string& path, const std::uint32_t* rgba, int width, int height);
bool read_png(const std::string& path, std::vector<std::uint32_t>& rgba, int& width, int& height);  // RGBA8

// Save/load list dialogs (sceUtilitySavedata list modes). With a frontend UI enabled, a dialog stays open
// (status RUNNING) until the frontend answers it; otherwise a slot is chosen automatically. Both calls are made
// between guest time slices (e.g. from Kernel::on_vblank), never concurrently with guest code.
struct SaveSlotInfo {
    std::string name;    // save name suffix (e.g. "0001")
    bool exists = false;
    std::string title;   // SFO savedata title and detail text written by the game (UTF-8)
    std::string detail;
    std::filesystem::path icon;  // ICON0.PNG, if any
    std::int64_t time = 0;       // last write, seconds since the Unix epoch
};
struct SaveDialog {
    bool save = false;  // save (any slot) or load (existing slots only)
    std::vector<SaveSlotInfo> slots;
    int focus = 0;      // slot the PSP dialog would highlight first
};
void set_savedata_ui(bool enabled);
const SaveDialog* pending_save_dialog();  // nullptr when no dialog is open
void answer_save_dialog(int slot);        // index into SaveDialog::slots, or -1 to cancel

// Directory with cooked media (cccook output); sceAtrac/sceMpeg look up replacements there.
void set_cooked_root(const std::filesystem::path& root);
const std::filesystem::path& cooked_root();

// Registers every HLE module (call once at startup).
void hle_install();

inline std::uint32_t arg(const Context& c, int i) {
    static constexpr int kRegs[8] = {4, 5, 6, 7, 8, 9, 10, 11};
    return c.r[kRegs[i]];
}
inline void ret(Context& c, std::uint32_t v) { c.r[2] = v; }
inline void ret64(Context& c, std::uint64_t v) {
    c.r[2] = static_cast<std::uint32_t>(v);
    c.r[3] = static_cast<std::uint32_t>(v >> 32);
}

// Per-module installers (one per hle_*.cpp).
void hle_install_threadman();
void hle_install_sysmem();
void hle_install_io();
void hle_install_display();
void hle_install_audio();
void hle_install_mpeg();
void hle_install_atrac();
void hle_install_savedata();

}  // namespace ff7cc::rt
