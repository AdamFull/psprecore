#pragma once

// On-screen PSP controls for touchscreens: a floating analog stick and a D-pad on the left, the face buttons on
// the right, L/R in the top corners and Select / Menu / Start at the top centre. Layout scales with the window
// height. Fingers keep the control they started on; the D-pad and face buttons follow a sliding finger.

#include <cstdint>

#include "psprecore/platform/display.hpp"
#include "core/foundation/containers/map.h"

struct ImDrawList;

namespace psprecore::platform {

class TouchControls {
public:
    void set_size(float width, float height);  // drawing and hit-test space (window points)

    // Finger events in window-normalised coordinates (0..1).
    void finger_down(std::uint64_t id, float x, float y);
    void finger_motion(std::uint64_t id, float x, float y);
    void finger_up(std::uint64_t id);
    void release_all();
    // Once per input poll: ages the minimum hold of quick taps.
    void tick();

    std::uint32_t buttons() const;           // PSP button bits (PspButton)
    bool stick(float& x, float& y) const;     // -1..1 while the stick is held
    bool take_menu_request();
    bool free_touch() const;  // a finger is down outside every control (on the picture)

    void draw(ImDrawList* list, ButtonGlyphs glyphs) const;

private:
    enum class Control { None, Stick, DPad, Face, L, R, Select, Start, Menu };
    struct Finger {
        Control control = Control::None;
        float x = 0, y = 0;            // current position (points)
        float origin_x = 0, origin_y = 0;  // stick centre (where the finger landed)
    };
    struct Layout {
        float unit;
        float stick_x, stick_y, stick_r;
        float dpad_x, dpad_y, dpad_r;
        float face_x, face_y, face_r, face_spread;
        float shoulder_w, shoulder_h;
        float top_y, top_w, top_h, top_gap;
    };
    Layout layout() const;
    Control hit(float x, float y) const;
    std::uint32_t dpad_bits(const Finger& f) const;
    std::uint32_t face_bits(const Finger& f) const;

    float width_ = 1280, height_ = 720;
    nx::map<std::uint64_t, Finger> fingers_;
    // A tap shorter than one input poll would never reach the game, which samples the pad once per frame:
    // buttons touched recently stay pressed for a few polls.
    static constexpr int kMinHoldPolls = 3;
    std::uint32_t held_bits_ = 0;
    int hold_polls_[32] = {};
    int free_hold_polls_ = 0;  // a finger touched the picture lately
    bool menu_request_ = false;
};

}  // namespace psprecore::platform
