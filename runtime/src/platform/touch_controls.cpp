#include "platform/touch_controls.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>

#include "platform/button_glyphs.hpp"

namespace ff7cc::platform {

namespace {

constexpr ImU32 kBase = IM_COL32(255, 255, 255, 60);
constexpr ImU32 kOutline = IM_COL32(255, 255, 255, 130);
constexpr ImU32 kPressed = IM_COL32(255, 255, 255, 150);
constexpr ImU32 kText = IM_COL32(255, 255, 255, 200);

float distance(float ax, float ay, float bx, float by) { return std::hypot(ax - bx, ay - by); }

bool in_rect(float x, float y, float cx, float cy, float w, float h, float margin) {
    return std::abs(x - cx) <= w * 0.5f + margin && std::abs(y - cy) <= h * 0.5f + margin;
}

// Face buttons, clockwise from the top, with their offsets from the cluster centre.
struct FaceButton {
    std::uint32_t bit;
    float dx, dy;
};
constexpr FaceButton kFace[4] = {{kTriangle, 0, -1}, {kCircle, 1, 0}, {kCross, 0, 1}, {kSquare, -1, 0}};

}  // namespace

void TouchControls::set_size(float width, float height) {
    if (width > 0 && height > 0) {
        width_ = width;
        height_ = height;
    }
}

TouchControls::Layout TouchControls::layout() const {
    Layout l{};
    const float u = std::min(height_, width_ * 0.5625f) / 10.0f;  // 16:9 reference so wide screens keep sizes
    l.unit = u;
    l.stick_r = 1.5f * u;
    l.stick_x = 2.7f * u;
    l.stick_y = height_ - 2.6f * u;
    l.dpad_r = 1.25f * u;
    l.dpad_x = 2.7f * u;
    l.dpad_y = height_ - 6.5f * u;
    l.face_spread = 1.25f * u;
    l.face_r = 0.62f * u;
    l.face_x = width_ - 2.7f * u;
    l.face_y = height_ - 3.0f * u;
    l.shoulder_w = 3.0f * u;
    l.shoulder_h = 1.0f * u;
    l.top_y = 0.75f * u;
    l.top_w = 1.7f * u;
    l.top_h = 0.75f * u;
    l.top_gap = 0.4f * u;
    return l;
}

TouchControls::Control TouchControls::hit(float x, float y) const {
    const Layout l = layout();
    const float m = 0.3f * l.unit;
    const float step = l.top_w + l.top_gap;
    if (in_rect(x, y, width_ * 0.5f - step, l.top_y, l.top_w, l.top_h, m)) return Control::Select;
    if (in_rect(x, y, width_ * 0.5f, l.top_y, l.top_w, l.top_h, m)) return Control::Menu;
    if (in_rect(x, y, width_ * 0.5f + step, l.top_y, l.top_w, l.top_h, m)) return Control::Start;
    const float shoulder_y = 0.3f * l.unit + l.shoulder_h * 0.5f;
    if (in_rect(x, y, l.shoulder_w * 0.5f, shoulder_y, l.shoulder_w, l.shoulder_h, m)) return Control::L;
    if (in_rect(x, y, width_ - l.shoulder_w * 0.5f, shoulder_y, l.shoulder_w, l.shoulder_h, m)) return Control::R;
    if (distance(x, y, l.dpad_x, l.dpad_y) <= l.dpad_r * 1.3f) return Control::DPad;
    if (distance(x, y, l.face_x, l.face_y) <= l.face_spread + l.face_r * 1.5f) return Control::Face;
    if (x < width_ * 0.45f && y > height_ * 0.3f) return Control::Stick;  // floating stick: centred where touched
    return Control::None;
}

void TouchControls::finger_down(std::uint64_t id, float x, float y) {
    Finger f;
    f.x = f.origin_x = x * width_;
    f.y = f.origin_y = y * height_;
    f.control = hit(f.x, f.y);
    if (f.control == Control::Menu) menu_request_ = true;
    fingers_[id] = f;
    std::uint32_t bits = 0;
    switch (f.control) {
        case Control::DPad: bits = dpad_bits(f); break;
        case Control::Face: bits = face_bits(f); break;
        case Control::L: bits = kLTrigger; break;
        case Control::R: bits = kRTrigger; break;
        case Control::Select: bits = kSelect; break;
        case Control::Start: bits = kStart; break;
        default: break;
    }
    for (int b = 0; b < 32; ++b)
        if (bits & (1u << b)) hold_polls_[b] = kMinHoldPolls;
    held_bits_ |= bits;
}

void TouchControls::tick() {
    for (int b = 0; b < 32; ++b)
        if (hold_polls_[b] > 0 && --hold_polls_[b] == 0) held_bits_ &= ~(1u << b);
}

void TouchControls::finger_motion(std::uint64_t id, float x, float y) {
    const auto it = fingers_.find(id);
    if (it == fingers_.end()) return;
    it->second.x = x * width_;
    it->second.y = y * height_;
}

void TouchControls::finger_up(std::uint64_t id) { fingers_.erase(id); }

void TouchControls::release_all() {
    fingers_.clear();
    held_bits_ = 0;
    for (int& h : hold_polls_) h = 0;
}

bool TouchControls::take_menu_request() {
    const bool r = menu_request_;
    menu_request_ = false;
    return r;
}

std::uint32_t TouchControls::dpad_bits(const Finger& f) const {
    const Layout l = layout();
    const float dx = f.x - l.dpad_x, dy = f.y - l.dpad_y;
    if (std::hypot(dx, dy) < l.dpad_r * 0.2f) return 0;
    std::uint32_t bits = 0;  // diagonals when both components are large enough
    if (std::abs(dx) > 0.4f * std::abs(dy)) bits |= dx < 0 ? kLeft : kRight;
    if (std::abs(dy) > 0.4f * std::abs(dx)) bits |= dy < 0 ? kUp : kDown;
    return bits;
}

std::uint32_t TouchControls::face_bits(const Finger& f) const {
    const Layout l = layout();
    std::uint32_t best = 0;
    float best_d = l.face_r * 1.7f;
    for (const auto& b : kFace) {
        const float d = distance(f.x, f.y, l.face_x + b.dx * l.face_spread, l.face_y + b.dy * l.face_spread);
        if (d < best_d) {
            best_d = d;
            best = b.bit;
        }
    }
    return best;
}

std::uint32_t TouchControls::buttons() const {
    std::uint32_t bits = held_bits_;
    for (const auto& [_, f] : fingers_) {
        switch (f.control) {
            case Control::DPad: bits |= dpad_bits(f); break;
            case Control::Face: bits |= face_bits(f); break;
            case Control::L: bits |= kLTrigger; break;
            case Control::R: bits |= kRTrigger; break;
            case Control::Select: bits |= kSelect; break;
            case Control::Start: bits |= kStart; break;
            default: break;
        }
    }
    return bits;
}

bool TouchControls::stick(float& x, float& y) const {
    const Layout l = layout();
    for (const auto& [_, f] : fingers_) {
        if (f.control != Control::Stick) continue;
        float dx = (f.x - f.origin_x) / l.stick_r, dy = (f.y - f.origin_y) / l.stick_r;
        const float len = std::hypot(dx, dy);
        if (len > 1.0f) {
            dx /= len;
            dy /= len;
        }
        x = dx;
        y = dy;
        return true;
    }
    return false;
}

void TouchControls::draw(ImDrawList* dl, ButtonGlyphs glyphs) const {
    const Layout l = layout();
    const std::uint32_t pressed = buttons();
    const float thick = std::max(1.5f, l.unit * 0.05f);

    // Analog stick (drawn where the finger landed while held).
    float sx = l.stick_x, sy = l.stick_y, kx = 0, ky = 0;
    for (const auto& [_, f] : fingers_)
        if (f.control == Control::Stick) {
            sx = f.origin_x;
            sy = f.origin_y;
            stick(kx, ky);
        }
    dl->AddCircleFilled({sx, sy}, l.stick_r, kBase, 48);
    dl->AddCircle({sx, sy}, l.stick_r, kOutline, 48, thick);
    dl->AddCircleFilled({sx + kx * l.stick_r, sy + ky * l.stick_r}, l.stick_r * 0.45f, kPressed, 32);

    // D-pad arms.
    const float arm = l.dpad_r * 0.62f;
    const struct {
        std::uint32_t bit;
        float dx, dy;
    } arms[4] = {{kUp, 0, -1}, {kRight, 1, 0}, {kDown, 0, 1}, {kLeft, -1, 0}};
    for (const auto& a : arms) {
        const ImVec2 c{l.dpad_x + a.dx * arm, l.dpad_y + a.dy * arm};
        const ImVec2 p0{c.x - arm * 0.5f, c.y - arm * 0.5f}, p1{c.x + arm * 0.5f, c.y + arm * 0.5f};
        dl->AddRectFilled(p0, p1, (pressed & a.bit) ? kPressed : kBase, arm * 0.15f);
        dl->AddRect(p0, p1, kOutline, arm * 0.15f, 0, thick);
    }

    // Face buttons with their symbols.
    for (const auto& b : kFace) {
        const ImVec2 c{l.face_x + b.dx * l.face_spread, l.face_y + b.dy * l.face_spread};
        dl->AddCircleFilled(c, l.face_r, (pressed & b.bit) ? kPressed : kBase, 32);
        dl->AddCircle(c, l.face_r, kOutline, 32, thick);
        draw_face_symbol(dl, c, l.face_r * 0.45f, b.bit, glyphs, thick * 1.5f);
    }

    // Shoulders and the top row.
    const auto labelled = [&](ImVec2 center, float w, float h, const char* text, bool down) {
        const ImVec2 p0{center.x - w * 0.5f, center.y - h * 0.5f}, p1{center.x + w * 0.5f, center.y + h * 0.5f};
        dl->AddRectFilled(p0, p1, down ? kPressed : kBase, h * 0.4f);
        dl->AddRect(p0, p1, kOutline, h * 0.4f, 0, thick);
        const float font = h * 0.55f;
        const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(font, 1e9f, 0, text);
        dl->AddText(ImGui::GetFont(), font, {center.x - size.x * 0.5f, center.y - size.y * 0.5f}, kText, text);
    };
    const float shoulder_y = 0.3f * l.unit + l.shoulder_h * 0.5f;
    labelled({l.shoulder_w * 0.5f, shoulder_y}, l.shoulder_w, l.shoulder_h, button_name(kLTrigger, glyphs),
             pressed & kLTrigger);
    labelled({width_ - l.shoulder_w * 0.5f, shoulder_y}, l.shoulder_w, l.shoulder_h, button_name(kRTrigger, glyphs),
             pressed & kRTrigger);
    const float step = l.top_w + l.top_gap;
    labelled({width_ * 0.5f - step, l.top_y}, l.top_w, l.top_h, button_name(kSelect, glyphs), pressed & kSelect);
    labelled({width_ * 0.5f, l.top_y}, l.top_w, l.top_h, "MENU", false);
    labelled({width_ * 0.5f + step, l.top_y}, l.top_w, l.top_h, button_name(kStart, glyphs), pressed & kStart);
}

}  // namespace ff7cc::platform
