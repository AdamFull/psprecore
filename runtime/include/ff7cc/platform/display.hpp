#pragma once

// Window, presentation and input (SDL3 + Vulkan). Runs on the host main thread.

#include <cstdint>
#include <memory>

namespace ff7cc::platform {

// PSP controller state (sceCtrl button bits).
struct PadState {
    std::uint32_t buttons = 0;
    std::uint8_t lx = 128, ly = 128;  // analog stick, 128 = centre
};

enum PspButton : std::uint32_t {
    kSelect = 0x0001, kStart = 0x0008, kUp = 0x0010, kRight = 0x0020, kDown = 0x0040, kLeft = 0x0080,
    kLTrigger = 0x0100, kRTrigger = 0x0200, kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000,
};

class Display {
public:
    Display(const char* title, int width, int height);
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    // Processes window/input events; returns false once the user closes the window.
    bool poll_events();
    // Shows a frame of 32-bit RGBA pixels (row-major, `width` pixels per row).
    void present(const std::uint32_t* rgba, int width, int height);
    PadState pad() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ff7cc::platform
