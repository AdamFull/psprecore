#include "platform/overlay.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ctime>

#include "psprecore/gpu/ui_font.hpp"
#include "psprecore/platform/display.hpp"
#include "psprecore/runtime/hle.hpp"
#include "platform/button_glyphs.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore::platform {

namespace {

constexpr ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings;

nx::string format_time(std::int64_t unix_seconds) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &local);
    return buf;
}

// The first `lines` lines of `text`.
nx::string_view first_lines(nx::string_view text, int lines) {
    std::size_t end = 0;
    for (int i = 0; i < lines && end != nx::string_view::npos; ++i) {
        end = text.find('\n', end ? end + 1 : 0);
    }
    return end == nx::string_view::npos ? text : text.substr(0, end);
}

bool cancel_pressed() {
    return ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
}

}  // namespace

Overlay::Overlay(Display& display, Settings& settings, Hooks hooks)
    : display_(display), settings_(settings), hooks_(std::move(hooks)) {}

Overlay::~Overlay() {
    release_icons();
    if (game_font_.texture) display_.destroy_texture(game_font_.texture);
    if (game_font_.icon) display_.destroy_texture(game_font_.icon);
}

void Overlay::open_menu() {
    menu_open_ = true;
    menu_appearing_ = true;
}

void Overlay::close_menu() { menu_open_ = false; }

void Overlay::handle_menu_request() {
    if (rt::pending_save_dialog() && !menu_open_) {
        rt::answer_save_dialog(-1);
        return;
    }
    menu_open_ ? close_menu() : open_menu();
}

bool Overlay::wants_input() const { return menu_open_ || rt::pending_save_dialog() != nullptr; }

void Overlay::draw() {
    if (settings_.show_fps) draw_fps();
    if ((skip_alpha_ > 0.0f || skipping_) && !menu_open_ && !draw_game_skip_prompt()) draw_skip_prompt();
    if (rt::pending_save_dialog()) {
        draw_save_dialog();
    } else {
        // Icons are released a frame after the dialog closes: the closing frame's draw list still uses them.
        release_icons();
        dialog_appearing_ = true;
        overwrite_slot_ = -1;
    }
    if (menu_open_) draw_menu();
}

bool Overlay::pad_glyphs() const {
    // Not for the keyboard: the overlay is driven with arrows, Enter and Escape, not the game's keys.
    if (display_.button_glyphs() == ButtonGlyphs::Keyboard) return false;
    return display_.gamepad_connected() || settings_.button_glyphs != ButtonGlyphs::Auto;
}

bool Overlay::button(const char* label, std::uint32_t pad_button) {
    return pad_glyphs() ? glyph_button(label, pad_button, display_.button_glyphs()) : ImGui::Button(label);
}

void Overlay::pad_hint(std::uint32_t pad_button, const char* text) {
    glyph(pad_button, display_.button_glyphs());
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextDisabled("%s", text);
}

bool Overlay::load_game_font() {
    const rt::gpu::UiFont* font = rt::gpu::ui_font();
    GameFont& f = game_font_;
    if (!font || !rt::gpu::ui_font_sheet(f.sheet, f.width, f.height)) return false;
    f.texture = display_.create_texture(f.sheet.data(), f.width, f.height);
    // Glyph ink: the columns of each character's cell that hold any texel (the font is proportional).
    for (int c = 0; c < 128; ++c) {
        const int g = font->ascii[static_cast<std::size_t>(c)];
        if (g < 0) continue;
        const int x0 = g % font->columns * font->cell_width, y0 = g / font->columns * font->cell_height;
        int first = font->cell_width, last = -1;
        for (int x = 0; x < font->cell_width; ++x)
            for (int y = 0; y < font->cell_height; ++y)
                if (x0 + x < f.width && y0 + y < f.height && (f.sheet[static_cast<std::size_t>((y0 + y) * f.width + x0 + x)] >> 24) != 0) {
                    first = std::min(first, x);
                    last = std::max(last, x);
                }
        f.left[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(last < 0 ? 0 : first);
        f.ink[static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(last < 0 ? 0 : last - first + 1);
    }
    return f.texture != 0;
}

// The prompt as the game would write it: its font at its own pixel size (scaled with the picture), white with the
// font's outline, in the picture's bottom-right corner; its button icon (a disc with the symbol of the button that
// skips) between "Hold" and "to skip", and under the text a thin gauge that fills while the button is held. While
// nothing is held the prompt breathes gently.
bool Overlay::draw_game_skip_prompt() {
    const rt::gpu::UiFont* font = rt::gpu::ui_font();
    GameFont& f = game_font_;
    if (!font || (!f.texture && !load_game_font())) return false;
    const bool touch = display_.touch_controls();
    const ButtonGlyphs glyphs = display_.button_glyphs();
    if (!touch && (!f.icon || f.icon_glyphs != static_cast<int>(glyphs))) {
        // The cell holding the font's button icon, with the Circle button drawn on its disc.
        const int cw = font->cell_width, ch = font->cell_height;
        const int x0 = font->button.x / cw * cw, y0 = font->button.y / ch * ch;
        nx::vector<std::uint32_t> cell(static_cast<std::size_t>(cw * ch));
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                cell[static_cast<std::size_t>(y * cw + x)] = f.sheet[static_cast<std::size_t>((y0 + y) * f.width + x0 + x)];
        const rt::gpu::IconStyle style = glyphs == ButtonGlyphs::Xbox       ? rt::gpu::IconStyle::Xbox
                                         : glyphs == ButtonGlyphs::Nintendo ? rt::gpu::IconStyle::Nintendo
                                         : glyphs == ButtonGlyphs::Keyboard ? rt::gpu::IconStyle::Keyboard
                                                                            : rt::gpu::IconStyle::PlayStation;
        rt::gpu::draw_icon(cell.data(), cw, font->button.x - x0, font->button.y - y0, rt::gpu::IconFace::Circle, style);
        if (f.icon) display_.destroy_texture(f.icon);
        f.icon = display_.create_texture(cell.data(), cw, ch);
        f.icon_glyphs = static_cast<int>(glyphs);
    }

    // Game pixels -> screen: the 480x272 picture fitted into the window, as the display draws it.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float s = std::min(vp->Size.x / 480.0f, vp->Size.y / 272.0f);
    const ImVec2 origin{vp->Pos.x + (vp->Size.x - 480.0f * s) * 0.5f, vp->Pos.y + (vp->Size.y - 272.0f * s) * 0.5f};
    const int cw = font->cell_width, ch = font->cell_height;
    constexpr int kSpace = 4, kGap = 1;  // advance of a space; between glyphs

    // The line: runs of text and (-1) the icon.
    nx::string text_before, text_after;
    bool icon = false;
    if (skipping_) {
        text_before = "Skipping";
        text_before.append(static_cast<std::size_t>(static_cast<int>(ImGui::GetTime() * 3.0) % 4), '.');
    } else if (touch) {
        text_before = "Touch and hold to skip";
    } else {
        text_before = "Hold";
        text_after = "to skip";
        icon = true;
    }
    const auto run_width = [&](const nx::string& t) {
        int w = 0;
        for (const char c : t) {
            const auto i = static_cast<std::size_t>(static_cast<unsigned char>(c) & 0x7F);
            w += c == ' ' ? kSpace : font->ascii[i] >= 0 ? f.ink[i] + kGap : 0;
        }
        return w;
    };
    const int icon_w = icon ? cw + 2 * kSpace : 0;
    // "Skipping" is laid out with all its dots, so the line stays put while they count up.
    const int width = run_width(skipping_ ? nx::string("Skipping...") : text_before) + icon_w + run_width(text_after);
    // Bottom right; centred with touch controls, whose face buttons cover the picture's right edge on phones.
    const int top = 272 - 12 - ch - 4;
    const int left = touch ? (480 - width) / 2 : 480 - 14 - width;

    const float pulse = skipping_ || skip_progress_ > 0.0f ? 1.0f
                                                           : 0.8f + 0.2f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
    const float a = (skipping_ ? 1.0f : std::clamp(skip_alpha_, 0.0f, 1.0f)) * pulse;
    const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(a * 255.0f));
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const auto quad = [&](std::uint64_t tex, float x, float y, float w, float h, ImVec2 uv0, ImVec2 uv1) {
        dl->AddImage(static_cast<ImTextureID>(tex), {origin.x + x * s, origin.y + y * s},
                     {origin.x + (x + w) * s, origin.y + (y + h) * s}, uv0, uv1, tint);
    };
    int x = left;
    const auto draw_run = [&](const nx::string& t) {
        for (const char c : t) {
            const auto i = static_cast<std::size_t>(static_cast<unsigned char>(c) & 0x7F);
            const int g = font->ascii[i];
            if (c == ' ' || g < 0) {
                x += c == ' ' ? kSpace : 0;
                continue;
            }
            const int gx = g % font->columns * cw + f.left[i], gy = g / font->columns * ch;
            quad(f.texture, static_cast<float>(x), static_cast<float>(top), f.ink[i], static_cast<float>(ch),
                 {static_cast<float>(gx) / f.width, static_cast<float>(gy) / f.height},
                 {static_cast<float>(gx + f.ink[i]) / f.width, static_cast<float>(gy + ch) / f.height});
            x += f.ink[i] + kGap;
        }
    };
    draw_run(text_before);
    if (icon) {
        x += kSpace;
        quad(f.icon, static_cast<float>(x), static_cast<float>(top), static_cast<float>(cw), static_cast<float>(ch), {0, 0}, {1, 1});
        x += cw + kSpace;
    }
    draw_run(text_after);

    if (skip_progress_ > 0.0f && !skipping_) {  // the gauge: 2 game pixels under the text
        const float gy = static_cast<float>(top + ch + 1);
        const ImVec2 p0{origin.x + left * s, origin.y + gy * s};
        const ImVec2 p1{origin.x + (left + width) * s, origin.y + (gy + 2.0f) * s};
        dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, static_cast<int>(a * 160.0f)));
        dl->AddRectFilled(p0, {p0.x + (p1.x - p0.x) * std::min(1.0f, skip_progress_), p1.y}, tint);
    }
    return true;
}

// The port's own version, until the game's font is available. Bottom right (bottom centre with touch controls, whose face buttons sit in that corner): a disc with the button
// (a fingertip for touch) and a ring that fills while it is held, then the label. While nothing is held the ring
// breathes slowly, so the prompt reads as "do something here".
void Overlay::draw_skip_prompt() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float h = ImGui::GetFontSize();
    const float r = h * 0.95f, margin = h * 1.4f;
    const char* label = skipping_ ? "Skipping..." : "Hold to skip";
    const float fade = skipping_ ? 1.0f : std::clamp(skip_alpha_, 0.0f, 1.0f);
    const ImVec2 text = ImGui::CalcTextSize(label);
    const float width = 2.0f * r + h * 0.6f + text.x;
    const float left = display_.touch_controls() ? vp->Pos.x + (vp->Size.x - width) * 0.5f
                                                 : vp->Pos.x + vp->Size.x - margin - width;
    const ImVec2 c{left + r, vp->Pos.y + vp->Size.y - margin - r};
    const auto alpha = [](float a) { return static_cast<int>(a * 255.0f); };
    const int first_vertex = dl->VtxBuffer.Size;  // the whole prompt fades below, the button glyph included

    dl->AddRectFilled({c.x - r - h * 0.5f, c.y - r - h * 0.4f}, {c.x + r + h * 0.6f + text.x + h * 0.5f, c.y + r + h * 0.4f},
                      IM_COL32(0, 0, 0, alpha(0.45f)), r);
    const float thick = std::max(2.0f, r * 0.16f);
    const float pulse = skip_progress_ > 0.0f ? 1.0f : 0.55f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, alpha(0.25f * pulse)), 48, thick);
    if (skipping_) {
        const float start = static_cast<float>(ImGui::GetTime()) * 6.0f;
        dl->PathArcTo(c, r, start, start + std::numbers::pi_v<float> / 2.0f, 16);
        dl->PathStroke(IM_COL32(255, 255, 255, alpha(1.0f)), 0, thick);
    } else if (skip_progress_ > 0.0f) {
        const float start = -std::numbers::pi_v<float> / 2.0f;
        dl->PathArcTo(c, r, start, start + skip_progress_ * 2.0f * std::numbers::pi_v<float>, 48);
        dl->PathStroke(IM_COL32(255, 255, 255, alpha(1.0f)), 0, thick);
    }
    if (!display_.touch_controls()) {
        const float inner = r - thick * 1.5f;
        draw_face_glyph(dl, c, inner, kCircle, display_.button_glyphs());
    } else {
        dl->AddCircleFilled(c, r * 0.3f, IM_COL32(255, 255, 255, alpha(0.8f)), 24);  // a fingertip
    }
    dl->AddText({c.x + r + h * 0.6f, c.y - text.y * 0.5f}, IM_COL32(255, 255, 255, alpha(0.95f)), label);
    for (int i = first_vertex; i < dl->VtxBuffer.Size; ++i) {
        ImU32& col = dl->VtxBuffer[i].col;
        const auto a = static_cast<ImU32>(static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) * fade);
        col = (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }
}

void Overlay::draw_fps() {
    ImGui::SetNextWindowPos({8, 8});
    ImGui::SetNextWindowBgAlpha(0.4f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##fps", nullptr, flags)) ImGui::Text("%.0f FPS", ImGui::GetIO().Framerate);
    ImGui::End();
}

void Overlay::draw_menu() {
    settings_.fullscreen = display_.fullscreen();  // F11 / Alt+Enter toggle it outside the menu
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowBgAlpha(0.94f);
    if (menu_appearing_) ImGui::SetNextWindowFocus();
    bool open = true;
    if (ImGui::Begin("Paused", &open, kDialogFlags | ImGuiWindowFlags_AlwaysAutoResize)) {
        const float width = ImGui::GetFontSize() * 18.0f;
        if (ImGui::Button("Resume", {width, 0})) open = false;
        if (menu_appearing_) ImGui::SetItemDefaultFocus();

        Settings s = settings_;
        ImGui::SeparatorText("Display");
        ImGui::PushItemWidth(width * 0.55f);
        if (hooks_.renderer_choice) {
            int choice = s.software_renderer ? 1 : 0;
            ImGui::Combo("Renderer", &choice, "Vulkan\0Software (reference)\0");
            s.software_renderer = choice == 1;
        }
        if (hooks_.renderer_choice ? !s.software_renderer : hooks_.gpu_renderer) {
            ImGui::SliderInt("Resolution", &s.scale, 1, 8, "%dx");
            ImGui::SameLine();
            ImGui::TextDisabled("%dx%d", 480 * s.scale, 272 * s.scale);
            int aa = s.msaa >= 8 ? 3 : s.msaa >= 4 ? 2 : s.msaa >= 2 ? 1 : 0;
            if (ImGui::Combo("Anti-aliasing", &aa, "Off\0MSAA 2x\0MSAA 4x\0MSAA 8x\0")) s.msaa = 1 << aa;
            ImGui::Checkbox("Upscale textures (2x)", &s.texture_upscale);
        }
#ifndef __ANDROID__
        ImGui::Checkbox("Fullscreen (F11)", &s.fullscreen);
#endif
        ImGui::Checkbox("Show FPS", &s.show_fps);
        ImGui::SeparatorText("Audio");
        ImGui::SliderInt("Volume", &s.volume, 0, 100, "%d%%");
        ImGui::SeparatorText("Speed");
        ImGui::Checkbox("Frame limiter", &s.frame_limit);
        ImGui::Checkbox("VSync", &s.vsync);
#ifdef __ANDROID__
        ImGui::Checkbox("60 Hz display (saves battery)", &s.refresh_60hz);
#endif
        ImGui::SeparatorText("Controls");
        int glyphs = static_cast<int>(s.button_glyphs);
        if (ImGui::Combo("Button glyphs", &glyphs, "Auto (last device used)\0PlayStation\0Xbox\0Nintendo\0Keyboard\0"))
            s.button_glyphs = static_cast<ButtonGlyphs>(glyphs);
        ImGui::PopItemWidth();
        if (s != settings_) {
            settings_ = s;
            if (hooks_.apply) hooks_.apply(settings_);
        }

        const char* guide = pad_glyphs() ? guide_name(display_.button_glyphs()) : "Guide button";
#ifdef __ANDROID__
        ImGui::TextDisabled("Touch controls hide while a gamepad is used");
        ImGui::TextDisabled("Menu: MENU button, Back, or the gamepad's %s", guide);
#else
        ImGui::TextDisabled("Keyboard: move with arrows / WASD     X O [] /\\: K L J I");
        ImGui::TextDisabled("L / R: Q / E     Start: Enter     Select: Backspace");
        ImGui::TextDisabled("Menu: Esc or F1 (gamepad: %s, or both sticks)", guide);
#endif
        ImGui::Separator();
        if (ImGui::Button("Quit game", {width, 0}) && hooks_.quit) hooks_.quit();
        if (pad_glyphs()) {
            pad_hint(kCross, "Select");
            ImGui::SameLine(0, ImGui::GetFontSize() * 1.5f);
            pad_hint(kCircle, "Back");
        }
        if (cancel_pressed()) open = false;
    }
    ImGui::End();
    menu_appearing_ = false;
    if (!open) close_menu();
}

std::uint64_t Overlay::icon(const nx::string& path) {
    if (const auto it = icons_.find(path); it != icons_.end()) return it->second;
    nx::vector<std::uint32_t> rgba;
    int w = 0, h = 0;
    const std::uint64_t texture = rt::read_png(path, rgba, w, h) ? display_.create_texture(rgba.data(), w, h) : 0;
    icons_.emplace(path, texture);
    return texture;
}

void Overlay::release_icons() {
    for (const auto& [_, texture] : icons_)
        if (texture) display_.destroy_texture(texture);
    icons_.clear();
}

void Overlay::draw_save_dialog() {
    const rt::SaveDialog& d = *rt::pending_save_dialog();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    if (!d.slots.empty()) ImGui::SetNextWindowSize({vp->Size.x * 0.8f, vp->Size.y * 0.85f}, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.94f);
    const bool appearing = dialog_appearing_;
    if (appearing && !menu_open_) ImGui::SetNextWindowFocus();
    dialog_appearing_ = false;

    int chosen = -2;  // -2: undecided, -1: cancel
    bool ask_overwrite = false;
    ImGui::Begin(d.save ? "Save###savedata" : "Load###savedata", nullptr,
                 kDialogFlags | ImGuiWindowFlags_NoResize | (d.slots.empty() ? ImGuiWindowFlags_AlwaysAutoResize : 0));
    if (d.slots.empty()) {
        ImGui::TextUnformatted("There is no save data.");
        if (button("OK", kCross)) chosen = -1;
    } else {
        const float line = ImGui::GetTextLineHeightWithSpacing();
        const float row_h = line * 4.0f;
        const float icon_w = row_h * 144.0f / 80.0f;  // ICON0 is 144x80
        ImGui::BeginChild("slots", {0, -ImGui::GetFrameHeightWithSpacing()});
        for (int i = 0; i < static_cast<int>(d.slots.size()); ++i) {
            const rt::SaveSlotInfo& slot = d.slots[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            const ImVec2 start = ImGui::GetCursorPos();
            if (ImGui::Selectable("##slot", false, ImGuiSelectableFlags_None, {0, row_h})) {
                if (d.save && slot.exists) {
                    overwrite_slot_ = i;
                    ask_overwrite = true;
                } else {
                    chosen = i;
                }
            }
            if (appearing && i == d.focus) {
                ImGui::SetItemDefaultFocus();
                ImGui::SetKeyboardFocusHere(-1);
                ImGui::SetScrollHereY(0.5f);
            }
            const ImVec2 after = ImGui::GetCursorPos();
            ImGui::SetCursorPos(start);
            if (const std::uint64_t tex = slot.icon.empty() ? 0 : icon(slot.icon))
                ImGui::Image(static_cast<ImTextureID>(tex), {icon_w, row_h});
            else
                ImGui::Dummy({icon_w, row_h});
            ImGui::SameLine();
            ImGui::BeginGroup();
            if (slot.exists) {
                ImGui::Text("%s", slot.title.empty() ? slot.name.c_str() : slot.title.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("  %s", format_time(slot.time).c_str());
                const nx::string_view detail = first_lines(slot.detail, 3);
                ImGui::TextUnformatted(detail.data(), detail.data() + detail.size());
            } else {
                ImGui::TextDisabled("New save data");
            }
            ImGui::EndGroup();
            ImGui::SetCursorPos(after);
            ImGui::Dummy({0, 0});
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (button("Cancel", kCircle)) chosen = -1;
    }
    if (ask_overwrite) ImGui::OpenPopup("Overwrite?");
    if (ImGui::BeginPopupModal("Overwrite?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Overwrite this save data?");
        if (button("Yes", kCross)) {
            chosen = overwrite_slot_;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (button("No", kCircle) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (chosen == -2 && cancel_pressed() && !menu_open_) chosen = -1;
    ImGui::End();
    if (chosen != -2) rt::answer_save_dialog(chosen);  // invalidates `d`
}

}  // namespace psprecore::platform
