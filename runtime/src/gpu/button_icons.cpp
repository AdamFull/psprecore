#include "gpu/button_icons.hpp"

#include <iterator>

namespace ff7cc::rt::gpu {

namespace {

enum Face : std::uint8_t { Cross, Circle, Square, Triangle };

struct Icon {
    std::uint16_t x, y;  // centre of the 5x5 symbol
    Face face;
};

}  // namespace

struct IconAtlas {
    int format, width, height;
    std::uint64_t hash;  // icon_texture_hash of level 0
    const Icon* icons;
    std::size_t count;
};

namespace {

// Battle HUD atlas: Cross, Circle, Triangle, Square in a row (11x11 discs).
constexpr Icon kBattleHud[] = {{5, 101, Cross}, {16, 101, Circle}, {27, 101, Triangle}, {38, 101, Square}};
// Text font sheet: Triangle after the regular and the italic characters (13x13 discs).
constexpr Icon kFontSheet[] = {{6, 76, Triangle}, {6, 160, Triangle}};

constexpr IconAtlas kAtlases[] = {
    {5, 256, 256, 0x113A6D124D4EBA4Cull, kBattleHud, std::size(kBattleHud)},
    {4, 256, 256, 0x995C582AE01F4C31ull, kFontSheet, std::size(kFontSheet)},
};

// 5x5 letters, one byte per row, bit 4 the left column.
constexpr std::uint8_t kLetterA[5] = {0b01110, 0b10001, 0b11111, 0b10001, 0b10001};
constexpr std::uint8_t kLetterB[5] = {0b11110, 0b10001, 0b11110, 0b10001, 0b11110};
constexpr std::uint8_t kLetterX[5] = {0b10001, 0b01010, 0b00100, 0b01010, 0b10001};
constexpr std::uint8_t kLetterY[5] = {0b10001, 0b01010, 0b00100, 0b00100, 0b00100};
constexpr std::uint8_t kLetterK[5] = {0b10001, 0b10010, 0b11100, 0b10010, 0b10001};
constexpr std::uint8_t kLetterL[5] = {0b10000, 0b10000, 0b10000, 0b10000, 0b11111};
constexpr std::uint8_t kLetterJ[5] = {0b00111, 0b00010, 0b00010, 0b10010, 0b01100};
constexpr std::uint8_t kLetterI[5] = {0b01110, 0b00100, 0b00100, 0b00100, 0b01110};
// By face (Cross, Circle, Square, Triangle): the button at the same position.
constexpr const std::uint8_t* kXboxLetters[4] = {kLetterA, kLetterB, kLetterX, kLetterY};
constexpr const std::uint8_t* kNintendoLetters[4] = {kLetterB, kLetterA, kLetterY, kLetterX};
constexpr const std::uint8_t* kKeyboardLetters[4] = {kLetterK, kLetterL, kLetterJ, kLetterI};  // display_vulkan.cpp keys
// Xbox button colours (RGB, R in the low byte), light enough to read on the dark discs.
constexpr std::uint32_t kXboxColors[4] = {0x46D26E, 0x4650F0, 0xFFA05A, 0x28D2FA};

int luminance(std::uint32_t c) { return static_cast<int>((c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF)); }

}  // namespace

std::uint64_t icon_texture_hash(const std::uint8_t* data, std::size_t size) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h;
}

const IconAtlas* find_icon_atlas(int format, int width, int height, const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 0;
    bool hashed = false;
    for (const IconAtlas& a : kAtlases) {
        if (a.format != format || a.width != width || a.height != height) continue;
        if (!hashed) {
            hash = icon_texture_hash(data, size);
            hashed = true;
        }
        if (a.hash == hash) return &a;
    }
    return nullptr;
}

void draw_button_icons(const IconAtlas& atlas, std::uint32_t* rgba, int width, IconStyle style) {
    if (style == IconStyle::PlayStation) return;
    for (std::size_t n = 0; n < atlas.count; ++n) {
        const Icon& icon = atlas.icons[n];
        const auto at = [&](int x, int y) -> std::uint32_t& { return rgba[y * width + x]; };
        const int cx = icon.x, cy = icon.y;
        // The disc: the darkest texel just outside the symbol. The ink: the symbol's brightest texel.
        std::uint32_t disc = at(cx - 3, cy);
        for (const std::uint32_t c : {at(cx + 3, cy), at(cx, cy - 3), at(cx, cy + 3)})
            if (luminance(c) < luminance(disc)) disc = c;
        std::uint32_t ink = disc;
        for (int y = cy - 2; y <= cy + 2; ++y)
            for (int x = cx - 2; x <= cx + 2; ++x)
                if (luminance(at(x, y)) > luminance(ink)) ink = at(x, y);
        const std::uint8_t* letter = style == IconStyle::Xbox       ? kXboxLetters[icon.face]
                                     : style == IconStyle::Nintendo ? kNintendoLetters[icon.face]
                                                                    : kKeyboardLetters[icon.face];
        if (style == IconStyle::Xbox) ink = (ink & 0xFF000000u) | kXboxColors[icon.face];
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x) at(cx - 2 + x, cy - 2 + y) = (letter[y] >> (4 - x)) & 1 ? ink : disc;
    }
}

}  // namespace ff7cc::rt::gpu
