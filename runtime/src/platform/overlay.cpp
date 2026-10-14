#include "platform/overlay.hpp"

#include <imgui.h>

#include <algorithm>
#include <ctime>
#include <string_view>
#include <vector>

#include "ff7cc/platform/display.hpp"
#include "ff7cc/runtime/hle.hpp"

namespace ff7cc::platform {

namespace {

constexpr ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings;

std::string format_time(std::int64_t unix_seconds) {
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
std::string_view first_lines(std::string_view text, int lines) {
    std::size_t end = 0;
    for (int i = 0; i < lines && end != std::string_view::npos; ++i) {
        end = text.find('\n', end ? end + 1 : 0);
    }
    return end == std::string_view::npos ? text : text.substr(0, end);
}

bool cancel_pressed() {
    return ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
}

}  // namespace

Overlay::Overlay(Display& display, Settings& settings, Hooks hooks)
    : display_(display), settings_(settings), hooks_(std::move(hooks)) {}

Overlay::~Overlay() { release_icons(); }

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
        if (hooks_.gpu_renderer) {
            ImGui::SliderInt("Resolution", &s.scale, 1, 8, "%dx");
            ImGui::SameLine();
            ImGui::TextDisabled("%dx%d", 480 * s.scale, 272 * s.scale);
        }
        ImGui::Checkbox("Fullscreen (F11)", &s.fullscreen);
        ImGui::Checkbox("Show FPS", &s.show_fps);
        ImGui::SeparatorText("Audio");
        ImGui::SliderInt("Volume", &s.volume, 0, 100, "%d%%");
        ImGui::SeparatorText("Speed");
        ImGui::Checkbox("Frame limiter", &s.frame_limit);
        ImGui::PopItemWidth();
        if (s != settings_) {
            settings_ = s;
            if (hooks_.apply) hooks_.apply(settings_);
        }

        ImGui::SeparatorText("Keyboard");
        ImGui::TextDisabled("Move: arrows / WASD     X O [] /\\: K L J I");
        ImGui::TextDisabled("L / R: Q / E     Start: Enter     Select: Backspace");
        ImGui::TextDisabled("Menu: Esc or F1 (gamepad: Guide, or both sticks)");
        ImGui::Separator();
        if (ImGui::Button("Quit game", {width, 0}) && hooks_.quit) hooks_.quit();
        if (cancel_pressed()) open = false;
    }
    ImGui::End();
    menu_appearing_ = false;
    if (!open) close_menu();
}

std::uint64_t Overlay::icon(const std::string& path) {
    if (const auto it = icons_.find(path); it != icons_.end()) return it->second;
    std::vector<std::uint32_t> rgba;
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
        if (ImGui::Button("OK")) chosen = -1;
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
            if (const std::uint64_t tex = slot.icon.empty() ? 0 : icon(slot.icon.string()))
                ImGui::Image(static_cast<ImTextureID>(tex), {icon_w, row_h});
            else
                ImGui::Dummy({icon_w, row_h});
            ImGui::SameLine();
            ImGui::BeginGroup();
            if (slot.exists) {
                ImGui::Text("%s", slot.title.empty() ? slot.name.c_str() : slot.title.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("  %s", format_time(slot.time).c_str());
                const std::string_view detail = first_lines(slot.detail, 3);
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
        if (ImGui::Button("Cancel")) chosen = -1;
    }
    if (ask_overwrite) ImGui::OpenPopup("Overwrite?");
    if (ImGui::BeginPopupModal("Overwrite?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Overwrite this save data?");
        if (ImGui::Button("Yes")) {
            chosen = overwrite_slot_;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("No") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (chosen == -2 && cancel_pressed() && !menu_open_) chosen = -1;
    ImGui::End();
    if (chosen != -2) rt::answer_save_dialog(chosen);  // invalidates `d`
}

}  // namespace ff7cc::platform
