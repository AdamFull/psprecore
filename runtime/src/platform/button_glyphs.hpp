#pragma once

// Button glyphs for what the port draws itself (touch controls, pause menu, save dialog), in the style of the
// gamepad in use: PlayStation symbols, or Xbox / Nintendo letters for the buttons at the same positions (SDL maps
// pads by position, so Cross is the bottom face button: A on an Xbox pad, B on a Nintendo one), or the keyboard keys
// that press the buttons.

#include <imgui.h>

#include <cstdint>

#include "ff7cc/platform/display.hpp"

namespace ff7cc::platform {

// The symbol or letter of face button `button` (kCross, kCircle, kSquare or kTriangle) within `size` of `center`.
void draw_face_symbol(ImDrawList* dl, ImVec2 center, float size, std::uint32_t button, ButtonGlyphs glyphs,
                      float thickness);
// A face button as a dark disc of `radius` with its symbol.
void draw_face_glyph(ImDrawList* dl, ImVec2 center, float radius, std::uint32_t button, ButtonGlyphs glyphs);

// Names of the other buttons (kLTrigger, kRTrigger, kSelect, kStart), e.g. "LB", "BACK" or "+".
const char* button_name(std::uint32_t button, ButtonGlyphs glyphs);
// The gamepad's Guide button, e.g. "PS button".
const char* guide_name(ButtonGlyphs glyphs);

// ImGui layout: a face button glyph one text line high.
void glyph(std::uint32_t button, ButtonGlyphs glyphs);
// ImGui layout: a button showing the glyph of the face button that also presses it before `label` (also its ID).
bool glyph_button(const char* label, std::uint32_t button, ButtonGlyphs glyphs, float width = 0.0f);

}  // namespace ff7cc::platform
