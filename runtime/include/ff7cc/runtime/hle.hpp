#pragma once

// High-level emulation of the PSP system libraries the game imports.
//
// hle_call(ctx, nid) (recomp.h) dispatches through a table built from hle_imports(); implementations are bound
// by function name. Arguments follow the PSP EABI: a0-a3, t0-t3; results go to v0 (and v1 for 64-bit values).
// Calls without an implementation are logged once and return 0.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ff7cc/recomp/recomp.h"

namespace ff7cc::rt {

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

// Display output and controller input for the host frontend.
bool read_framebuffer(std::vector<std::uint32_t>& rgba, int& width, int& height);  // current frame as RGBA8
void set_pad(std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly);
bool write_png(const std::string& path, const std::uint32_t* rgba, int width, int height);

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

}  // namespace ff7cc::rt
