#pragma once

// On-screen PSP controls for touchscreens: a floating analog stick and a D-pad on the left, the face buttons on
// the right, L/R in the top corners and Select / Menu / Start at the top centre. Layout scales with the window
// height. Fingers keep the control they started on; the D-pad and face buttons follow a sliding finger.

#include <cstdint>
#include <map>

struct ImDrawList;

namespace ff7cc::platform {

class TouchControls {
public:
    void set_size(float width, float height);  // drawing and hit-test space (window points)

    // Finger events in window-normalised coordinates (0..1).
    void finger_down(std::uint64_t id, float x, float y);
    void finger_motion(std::uint64_t id, float x, float y);
    void finger_up(std::uint64_t id);
    void release_all();

    std::uint32_t buttons() const;           // PSP button bits (PspButton)
    bool stick(float& x, float& y) const;     // -1..1 while the stick is held
    bool take_menu_request();

    void draw(ImDrawList* list) const;

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
    std::map<std::uint64_t, Finger> fingers_;
    bool menu_request_ = false;
};

}  // namespace ff7cc::platform
