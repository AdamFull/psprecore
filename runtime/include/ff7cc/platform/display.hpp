#pragma once

// Window, presentation and input (SDL3 + Vulkan). Runs on the host main thread.

#include <cstdint>
#include <functional>
#include <memory>

#include "ff7cc/platform/vulkan_context.hpp"

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
    // True while the app is in the background (Android): nothing can be presented and the game should pause.
    bool in_background() const;
    // Shows a frame of 32-bit RGBA pixels (row-major, `width` pixels per row).
    void present(const std::uint32_t* rgba, int width, int height);
    // Shows the top-left width x height region of a GPU image (e.g. a GE render target). The image must be in
    // TRANSFER_SRC_OPTIMAL layout and is left in it; work submitted earlier on the queue is ordered before.
    void present_image(VkImage image, int width, int height);
    const VulkanContext& vulkan() const;
    PadState pad() const;

    // Dear ImGui overlay: `ui` runs once per presented frame, between ImGui::NewFrame() and ImGui::Render().
    void set_ui(std::function<void()> ui);
    // Presents the last frame again with the overlay (while the game is paused); false while minimised.
    bool present_again();
    // Drops the reference to the last image given to present_image (before its owner destroys it).
    void forget_last_image();
    // True once per press of Escape/F1 or the gamepad Guide button (or both stick clicks).
    bool take_menu_request();
    void set_fullscreen(bool on);  // borderless, at the desktop resolution
    bool fullscreen() const;
    // Off: frames are shown as soon as they are ready (mailbox, or immediate with tearing where the GPU offers
    // nothing else); with the frame limiter off the game then runs as fast as it can.
    void set_vsync(bool on);
    // Android: asks for this panel refresh rate (0: the device's default), e.g. 60 Hz instead of 120 Hz to save
    // power. Ignored elsewhere.
    void set_refresh_rate(float hz);
    // On-screen touch controls. They appear on touch input and hide when a key or gamepad button is used.
    void set_touch_controls(bool visible);
    bool touch_controls() const;

    // Testing: reads back the next presented window image (overlay included) as RGBA8.
    void capture_next_present(std::function<void(const std::uint32_t* rgba, int width, int height)> done);
    // Testing: queues a key press and release as if typed (SDL keycode), e.g. to drive the overlay.
    void inject_key(std::uint32_t keycode);
    // Testing: a finger touching window-normalised (x, y), lifted after `polls` calls to poll_events().
    void inject_finger(float x, float y, int polls);

    // Small RGBA8 textures for the overlay (e.g. save icons); the result is an ImTextureID.
    std::uint64_t create_texture(const std::uint32_t* rgba, int width, int height);
    void destroy_texture(std::uint64_t texture);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ff7cc::platform
