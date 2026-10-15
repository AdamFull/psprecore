#include "psprecore/gpu/ui_font.hpp"

#include <atomic>
#include <bit>
#include <cstring>
#include <optional>

#include "gpu/texture.hpp"
#include "psprecore/runtime/kernel.hpp"
#include "core/foundation/threading/sync.h"

namespace psprecore::rt::gpu {

namespace {

std::optional<UiFont> g_font;
std::atomic<bool> g_kept{false};
nx::mutex g_mutex;  // the renderer keeps the sheet, the overlay reads it
nx::vector<std::uint32_t> g_sheet;
int g_width = 0, g_height = 0;

// The sheet decoded from where the game keeps it, if the texels there are the font's: the texture registers the
// game would set, through the software renderer's sampler.
bool decode_from_memory(const UiFont& font, nx::vector<std::uint32_t>& out) {
    if (!font.texels || font.width <= 0 || font.height <= 0) return false;
    ge::State s;
    s.regs[ge::TEXFORMAT] = static_cast<std::uint32_t>(font.format);
    s.regs[ge::TEXMODE] = font.swizzled ? 1 : 0;
    s.regs[ge::TEXSIZE0] = static_cast<std::uint32_t>(std::countr_zero(static_cast<unsigned>(font.width)) |
                                                      (std::countr_zero(static_cast<unsigned>(font.height)) << 8));
    s.regs[ge::TEXBUFWIDTH0] = static_cast<std::uint32_t>(font.buffer_width) | ((font.texels >> 8) & 0x0F0000);
    s.regs[ge::TEXADDR0] = font.texels & 0xFFFFF0;
    const bool indexed = font.format >= 4 && font.format <= 7;
    if (indexed) {
        static constexpr int kIndexBits[4] = {4, 8, 16, 32};
        const std::size_t entries = std::min<std::size_t>(std::size_t{1} << std::min(kIndexBits[font.format - 4], 8), 256);
        const std::size_t bytes = std::min(entries * (font.palette_format == 3 ? 4u : 2u), s.clut.size());
        const std::uint8_t* palette = kernel().memory().ptr(font.palette);
        if (!palette) return false;
        std::memcpy(s.clut.data(), palette, bytes);
        s.regs[ge::CLUTFORMAT] = static_cast<std::uint32_t>(font.palette_format) | (0xFFu << 8);  // mask 0xFF, no shift
    }
    TextureSampler ts;
    ts.setup(s, kernel().memory());
    if (!ts.level_data(0)) return false;
    const bool partial = font.check_bytes && font.check_bytes <= ts.level_size(0);
    if (partial ? icon_texture_hash(ts.level_data(0), font.check_bytes) != font.check_hash
                : icon_texture_hash(ts.level_data(0), ts.level_size(0)) != font.atlas_hash)
        return false;
    out.resize(static_cast<std::size_t>(font.width) * static_cast<std::size_t>(font.height));
    ts.decode(0, out.data());
    return true;
}

}  // namespace

void set_ui_font(const UiFont& font) { g_font = font; }

const UiFont* ui_font() { return g_font ? &*g_font : nullptr; }

bool ui_font_wanted() { return g_font && !g_kept.load(std::memory_order_acquire); }

void keep_ui_font_sheet(const std::uint32_t* rgba, int width, int height) {
    nx::scoped_lock lock(g_mutex);
    g_sheet.assign(rgba, rgba + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    g_width = width;
    g_height = height;
    g_kept.store(true, std::memory_order_release);
}

bool ui_font_sheet(nx::vector<std::uint32_t>& rgba, int& width, int& height) {
    if (!g_kept.load(std::memory_order_acquire)) {
        nx::vector<std::uint32_t> decoded;
        if (!g_font || !decode_from_memory(*g_font, decoded)) return false;
        keep_ui_font_sheet(decoded.data(), g_font->width, g_font->height);
    }
    nx::scoped_lock lock(g_mutex);
    rgba = g_sheet;
    width = g_width;
    height = g_height;
    return true;
}

}  // namespace psprecore::rt::gpu
