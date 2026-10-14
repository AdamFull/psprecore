#include "platform/button_glyphs.hpp"

#include <algorithm>

namespace psprecore::platform {

namespace {

// Face buttons by position: Cross bottom, Circle right, Square left, Triangle top.
int face_index(std::uint32_t button) {
    switch (button) {
        case kCross: return 0;
        case kCircle: return 1;
        case kSquare: return 2;
        default: return 3;
    }
}

constexpr ImU32 kPlayStationColors[4] = {IM_COL32(130, 170, 255, 230), IM_COL32(255, 100, 100, 230),
                                         IM_COL32(240, 140, 220, 230), IM_COL32(64, 224, 160, 230)};
constexpr ImU32 kXboxColors[4] = {IM_COL32(110, 200, 60, 240), IM_COL32(240, 75, 60, 240), IM_COL32(60, 140, 245, 240),
                                  IM_COL32(250, 200, 40, 240)};
constexpr const char* kXboxLetters[4] = {"A", "B", "X", "Y"};
constexpr const char* kNintendoLetters[4] = {"B", "A", "Y", "X"};
constexpr const char* kKeyboardLetters[4] = {"K", "L", "J", "I"};  // the keys mapped in display_vulkan.cpp
constexpr ImU32 kNintendoColor = IM_COL32(240, 240, 240, 240);

void draw_letter(ImDrawList* dl, ImVec2 center, float size, const char* letter, ImU32 color) {
    ImFont* font = ImGui::GetFont();
    const float font_size = size * 2.1f;
    const ImVec2 extent = font->CalcTextSizeA(font_size, 1e9f, 0, letter);
    dl->AddText(font, font_size, {center.x - extent.x * 0.5f, center.y - extent.y * 0.5f}, color, letter);
}

}  // namespace

void draw_face_symbol(ImDrawList* dl, ImVec2 c, float s, std::uint32_t button, ButtonGlyphs glyphs, float thickness) {
    const int i = face_index(button);
    switch (glyphs) {
        case ButtonGlyphs::Xbox: draw_letter(dl, c, s, kXboxLetters[i], kXboxColors[i]); return;
        case ButtonGlyphs::Nintendo: draw_letter(dl, c, s, kNintendoLetters[i], kNintendoColor); return;
        case ButtonGlyphs::Keyboard: draw_letter(dl, c, s, kKeyboardLetters[i], kNintendoColor); return;
        default: break;
    }
    const ImU32 color = kPlayStationColors[i];
    switch (button) {
        case kTriangle:
            dl->AddTriangle({c.x, c.y - s}, {c.x + s * 0.95f, c.y + s * 0.65f}, {c.x - s * 0.95f, c.y + s * 0.65f}, color,
                            thickness);
            break;
        case kCircle: dl->AddCircle(c, s, color, 24, thickness); break;
        case kCross:
            dl->AddLine({c.x - s, c.y - s}, {c.x + s, c.y + s}, color, thickness);
            dl->AddLine({c.x - s, c.y + s}, {c.x + s, c.y - s}, color, thickness);
            break;
        default: dl->AddRect({c.x - s * 0.85f, c.y - s * 0.85f}, {c.x + s * 0.85f, c.y + s * 0.85f}, color, 0, 0, thickness); break;
    }
}

void draw_face_glyph(ImDrawList* dl, ImVec2 center, float radius, std::uint32_t button, ButtonGlyphs glyphs) {
    dl->AddCircleFilled(center, radius, IM_COL32(24, 24, 28, 235), 24);
    dl->AddCircle(center, radius, IM_COL32(255, 255, 255, 90), 24, std::max(1.0f, radius * 0.08f));
    const float size = glyphs == ButtonGlyphs::PlayStation ? radius * 0.5f : radius * 0.8f;  // letters fill more
    draw_face_symbol(dl, center, size, button, glyphs, std::max(1.0f, radius * 0.16f));
}

const char* button_name(std::uint32_t button, ButtonGlyphs glyphs) {
    const bool xbox = glyphs == ButtonGlyphs::Xbox, nintendo = glyphs == ButtonGlyphs::Nintendo;
    const bool keyboard = glyphs == ButtonGlyphs::Keyboard;
    switch (button) {
        case kLTrigger: return xbox ? "LB" : keyboard ? "Q" : "L";
        case kRTrigger: return xbox ? "RB" : keyboard ? "E" : "R";
        case kSelect: return xbox ? "BACK" : nintendo ? "-" : keyboard ? "BKSP" : "SELECT";
        case kStart: return nintendo ? "+" : keyboard ? "ENTER" : "START";
        default: return "?";
    }
}

const char* guide_name(ButtonGlyphs glyphs) {
    switch (glyphs) {
        case ButtonGlyphs::Xbox: return "Xbox button";
        case ButtonGlyphs::Nintendo: return "HOME button";
        case ButtonGlyphs::Keyboard: return "Guide button";
        default: return "PS button";
    }
}

void glyph(std::uint32_t button, ButtonGlyphs glyphs) {
    const float h = ImGui::GetFontSize();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy({h, h});
    draw_face_glyph(ImGui::GetWindowDrawList(), {at.x + h * 0.5f, at.y + h * 0.5f}, h * 0.5f, button, glyphs);
}

bool glyph_button(const char* label, std::uint32_t button, ButtonGlyphs glyphs, float width) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float h = ImGui::GetFontSize();
    const ImVec2 text = ImGui::CalcTextSize(label);
    const float content = h + style.ItemInnerSpacing.x + text.x;
    ImGui::PushID(label);
    const bool pressed = ImGui::Button("##glyph", {std::max(width, content + style.FramePadding.x * 2.0f), 0.0f});
    ImGui::PopID();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float x = (min.x + max.x - content) * 0.5f, y = (min.y + max.y) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    draw_face_glyph(dl, {x + h * 0.5f, y}, h * 0.5f, button, glyphs);
    dl->AddText({x + h + style.ItemInnerSpacing.x, y - text.y * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), label);
    return pressed;
}

}  // namespace psprecore::platform
