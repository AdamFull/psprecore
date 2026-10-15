#include "gpu/texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gpu/hash.hpp"
#include "psprecore/runtime/memory.hpp"

namespace psprecore::rt::gpu {

namespace {

std::uint32_t expand5(std::uint32_t v) { return (v << 3) | (v >> 2); }
std::uint32_t expand6(std::uint32_t v) { return (v << 2) | (v >> 4); }

std::uint16_t load16(const std::uint8_t* p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
std::uint32_t load32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

std::uint32_t lerp_color(std::uint32_t a, std::uint32_t b, int wa, int wb, int total) {
    std::uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        const int ca = static_cast<int>((a >> s) & 0xFF), cb = static_cast<int>((b >> s) & 0xFF);
        out |= static_cast<std::uint32_t>((ca * wa + cb * wb) / total) << s;
    }
    return out;
}

// PSP DXT blocks keep the 2-bit color indices first, then the two 565 endpoints (R in the low bits).
std::uint32_t dxt_color(const std::uint8_t* block, int x, int y, bool one_bit_alpha) {
    const std::uint32_t lines = load32(block);
    const std::uint16_t c0 = load16(block + 4), c1 = load16(block + 6);
    const std::uint32_t a = decode_5650(c0), b = decode_5650(c1);
    switch ((lines >> ((y * 4 + x) * 2)) & 3) {
        case 0: return a;
        case 1: return b;
        case 2: return c0 > c1 || !one_bit_alpha ? lerp_color(a, b, 2, 1, 3) : lerp_color(a, b, 1, 1, 2);
        default:
            if (c0 > c1 || !one_bit_alpha) return lerp_color(a, b, 1, 2, 3);
            return 0;  // transparent black
    }
}

}  // namespace

std::uint32_t decode_5650(std::uint32_t p) {
    return rgba(expand5(p & 31), expand6((p >> 5) & 63), expand5((p >> 11) & 31), 255);
}
std::uint32_t decode_5551(std::uint32_t p) {
    return rgba(expand5(p & 31), expand5((p >> 5) & 31), expand5((p >> 10) & 31), (p >> 15) & 1 ? 255 : 0);
}
std::uint32_t decode_4444(std::uint32_t p) {
    return rgba((p & 15) * 17, ((p >> 4) & 15) * 17, ((p >> 8) & 15) * 17, ((p >> 12) & 15) * 17);
}

void TextureSampler::setup(const ge::State& s, const GuestMemory& memory) {
    format_ = static_cast<int>(s.param(ge::TEXFORMAT) & 15);
    const std::uint32_t mode = s.param(ge::TEXMODE);
    swizzled_ = mode & 1;
    levels_ = static_cast<int>((mode >> 16) & 7) + 1;
    const std::uint32_t wrap = s.param(ge::TEXWRAP);
    clamp_u_ = wrap & 1;
    clamp_v_ = (wrap >> 8) & 1;
    clut_format_ = s.param(ge::CLUTFORMAT);
    clut_ = s.clut.data();
    if (uses_clut()) build_palette();
    for (int i = 0; i < levels_; ++i) {
        const auto n = static_cast<std::uint8_t>(i);
        const std::uint32_t bufw = s.param(static_cast<std::uint8_t>(ge::TEXBUFWIDTH0 + n));
        const std::uint32_t addr = ge::vram_canonical(((bufw & 0x0F0000) << 8) |
                                                      (s.param(static_cast<std::uint8_t>(ge::TEXADDR0 + n)) & 0xFFFFF0));
        const std::uint32_t size = s.param(static_cast<std::uint8_t>(ge::TEXSIZE0 + n));
        Level& l = level_[i];
        l.data = memory.ptr(addr);
        l.width = 1 << std::min<std::uint32_t>(size & 15, 10);
        l.height = 1 << std::min<std::uint32_t>((size >> 8) & 15, 10);
        l.buffer_width = static_cast<int>(bufw & 0x7FF);
        static constexpr std::uint32_t kBits[8] = {16, 16, 16, 32, 4, 8, 16, 32};
        l.row_bytes = static_cast<std::uint32_t>(l.buffer_width) * kBits[format_ & 7] / 8;
        l.blocks_per_row = std::max<std::uint32_t>(l.row_bytes / 16, 1);
    }
    // Specialised sampling for the plain (non-DXT) formats.
    static constexpr SampleFn kFixed[8][2] = {
        {&sample_fixed<0, false>, &sample_fixed<0, true>}, {&sample_fixed<1, false>, &sample_fixed<1, true>},
        {&sample_fixed<2, false>, &sample_fixed<2, true>}, {&sample_fixed<3, false>, &sample_fixed<3, true>},
        {&sample_fixed<4, false>, &sample_fixed<4, true>}, {&sample_fixed<5, false>, &sample_fixed<5, true>},
        {&sample_fixed<6, false>, &sample_fixed<6, true>}, {&sample_fixed<7, false>, &sample_fixed<7, true>},
    };
    sample_fn_ = format_ < 8 ? kFixed[format_][swizzled_ ? 1 : 0] : &sample_generic;
}

// Entry k of the palette is CLUT entry (k | start); only the entries a texel can reach are decoded.
void TextureSampler::build_palette() {
    clut_shift_ = (clut_format_ >> 2) & 31;
    clut_mask_ = (clut_format_ >> 8) & 0xFF;
    static constexpr std::uint32_t kMaxIndex[4] = {15, 255, 65535, 0xFFFFFFFF};  // CLUT4/8/16/32
    const std::uint32_t count = std::min(clut_mask_, kMaxIndex[(format_ - 4) & 3] >> clut_shift_) + 1;
    if (count <= palette_count_ && clut_format_ == palette_format_ &&
        std::memcmp(palette_source_.data(), clut_, palette_source_.size()) == 0)
        return;
    std::memcpy(palette_source_.data(), clut_, palette_source_.size());
    palette_format_ = clut_format_;
    palette_count_ = count;
    const std::uint32_t start = ((clut_format_ >> 16) & 31) << 4;
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::uint32_t index = k | start;
        switch (clut_format_ & 3) {
            case 0: palette_[k] = decode_5650(load16(clut_ + ((index * 2) & 1023))); break;
            case 1: palette_[k] = decode_5551(load16(clut_ + ((index * 2) & 1023))); break;
            case 2: palette_[k] = decode_4444(load16(clut_ + ((index * 2) & 1023))); break;
            default: palette_[k] = load32(clut_ + ((index * 4) & 1023)); break;
        }
    }
}

// x and y are wrapped (non-negative), so the divisions are shifts.
template <int Format, bool Swizzled>
NX_FORCE_INLINE std::uint32_t TextureSampler::fetch(const TextureSampler& t, const Level& l, int x, int y) {
    static constexpr std::uint32_t kBits[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    constexpr std::uint32_t bits = kBits[Format];
    const auto ux = static_cast<std::uint32_t>(x), uy = static_cast<std::uint32_t>(y);
    const std::uint32_t xb = ux * bits / 8;
    std::size_t offset;
    if constexpr (Swizzled)
        offset = (static_cast<std::size_t>(uy / 8) * l.blocks_per_row + xb / 16) * 128 + (uy % 8) * 16 + xb % 16;
    else
        offset = static_cast<std::size_t>(uy) * l.row_bytes + xb;
    const std::uint8_t* p = l.data + offset;
    if constexpr (Format == 0) return decode_5650(load16(p));
    else if constexpr (Format == 1) return decode_5551(load16(p));
    else if constexpr (Format == 2) return decode_4444(load16(p));
    else if constexpr (Format == 3) return load32(p);
    else if constexpr (Format == 4) return t.clut((x & 1) ? *p >> 4 : *p & 15);
    else if constexpr (Format == 5) return t.clut(*p);
    else if constexpr (Format == 6) return t.clut(load16(p));
    else return t.clut(load32(p));
}

std::uint32_t TextureSampler::texel(const Level& l, int x, int y) const {
    if (format_ >= 8) {  // DXT: 4x4 blocks, buffer width in texels
        const int block_size = format_ == 8 ? 8 : 16;
        const int blocks_per_row = std::max(l.buffer_width / 4, 1);
        const std::uint8_t* block = l.data + (static_cast<std::ptrdiff_t>(y / 4) * blocks_per_row + x / 4) * block_size;
        const int bx = x & 3, by = y & 3;
        std::uint32_t c = dxt_color(block, bx, by, format_ == 8);
        if (format_ == 9) {  // DXT3: explicit 4-bit alpha after the color block
            const std::uint32_t a = (load16(block + 8 + by * 2) >> (bx * 4)) & 15;
            c = (c & 0x00FFFFFF) | ((a * 17) << 24);
        } else if (format_ == 10) {  // DXT5: 3-bit interpolated alpha after the color block
            const std::uint64_t bits = load32(block + 8) | (static_cast<std::uint64_t>(load16(block + 12)) << 32);
            const int a0 = block[14], a1 = block[15];
            const int i = static_cast<int>((bits >> ((by * 4 + bx) * 3)) & 7);
            int a;
            if (i == 0) a = a0;
            else if (i == 1) a = a1;
            else if (a0 > a1) a = ((8 - i) * a0 + (i - 1) * a1) / 7;
            else if (i < 6) a = ((6 - i) * a0 + (i - 1) * a1) / 5;
            else a = i == 6 ? 0 : 255;
            c = (c & 0x00FFFFFF) | (static_cast<std::uint32_t>(a) << 24);
        }
        return c;
    }

    // Bits per texel and byte offset of (x, y), honouring the 16-byte x 8-row swizzle.
    static constexpr int kBits[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    const int bits = kBits[format_ & 7];
    const int row_bytes = l.buffer_width * bits / 8;
    const int xb = x * bits / 8;
    std::ptrdiff_t offset;
    if (swizzled_) {
        const int blocks_per_row = std::max(row_bytes / 16, 1);
        offset = (static_cast<std::ptrdiff_t>(y / 8) * blocks_per_row + xb / 16) * 128 + (y % 8) * 16 + xb % 16;
    } else {
        offset = static_cast<std::ptrdiff_t>(y) * row_bytes + xb;
    }
    const std::uint8_t* p = l.data + offset;
    switch (format_) {
        case 0: return decode_5650(load16(p));
        case 1: return decode_5551(load16(p));
        case 2: return decode_4444(load16(p));
        case 3: return load32(p);
        case 4: return clut((x & 1) ? *p >> 4 : *p & 15);
        case 5: return clut(*p);
        case 6: return clut(load16(p));
        default: return clut(load32(p));
    }
}

template <int Format, bool Swizzled>
void TextureSampler::decode_fixed(const TextureSampler& t, const Level& l, std::uint32_t* out) {
    for (int y = 0; y < l.height; ++y)
        for (int x = 0; x < l.width; ++x) *out++ = fetch<Format, Swizzled>(t, l, x, y);
}

void TextureSampler::decode(int level, std::uint32_t* out) const {
    const Level& l = level_[std::clamp(level, 0, levels_ - 1)];
    if (format_ == 3 && !swizzled_) {  // 8888 rows as they are (e.g. movie frames, every frame)
        for (int y = 0; y < l.height; ++y)
            std::memcpy(out + static_cast<std::size_t>(y) * static_cast<std::size_t>(l.width),
                        l.data + static_cast<std::size_t>(y) * l.row_bytes, static_cast<std::size_t>(l.width) * 4);
        return;
    }
    using DecodeFn = void (*)(const TextureSampler&, const Level&, std::uint32_t*);
    static constexpr DecodeFn kFixed[8][2] = {
        {&decode_fixed<0, false>, &decode_fixed<0, true>}, {&decode_fixed<1, false>, &decode_fixed<1, true>},
        {&decode_fixed<2, false>, &decode_fixed<2, true>}, {&decode_fixed<3, false>, &decode_fixed<3, true>},
        {&decode_fixed<4, false>, &decode_fixed<4, true>}, {&decode_fixed<5, false>, &decode_fixed<5, true>},
        {&decode_fixed<6, false>, &decode_fixed<6, true>}, {&decode_fixed<7, false>, &decode_fixed<7, true>},
    };
    if (format_ < 8) {
        kFixed[format_][swizzled_ ? 1 : 0](*this, l, out);
        return;
    }
    for (int y = 0; y < l.height; ++y)  // DXT
        for (int x = 0; x < l.width; ++x) *out++ = texel(l, x, y);
}

bool TextureSampler::decode_indices(int level, std::uint8_t* out) const {
    if (format_ != 4 && format_ != 5) return false;
    const Level& l = level_[std::clamp(level, 0, levels_ - 1)];
    const int bits = format_ == 4 ? 4 : 8;
    const int row_bytes = l.buffer_width * bits / 8;
    for (int y = 0; y < l.height; ++y)
        for (int x = 0; x < l.width; ++x) {
            const int xb = x * bits / 8;
            std::ptrdiff_t offset;
            if (swizzled_) {
                const int blocks_per_row = std::max(row_bytes / 16, 1);
                offset = (static_cast<std::ptrdiff_t>(y / 8) * blocks_per_row + xb / 16) * 128 + (y % 8) * 16 + xb % 16;
            } else {
                offset = static_cast<std::ptrdiff_t>(y) * row_bytes + xb;
            }
            const std::uint8_t b = l.data[offset];
            *out++ = format_ == 5 ? b : static_cast<std::uint8_t>((x & 1) ? b >> 4 : b & 15);
        }
    return true;
}

std::size_t TextureSampler::level_size(int level) const {
    const Level& l = level_[std::clamp(level, 0, levels_ - 1)];
    if (format_ >= 8) {  // DXT blocks
        const std::size_t blocks = static_cast<std::size_t>(std::max(l.buffer_width / 4, 1)) * static_cast<std::size_t>((l.height + 3) / 4);
        return blocks * (format_ == 8 ? 8u : 16u);
    }
    static constexpr int kBits[8] = {16, 16, 16, 32, 4, 8, 16, 32};
    return static_cast<std::size_t>(l.buffer_width) * static_cast<std::size_t>(l.height) * static_cast<std::size_t>(kBits[format_ & 7]) / 8;
}

std::uint32_t TextureSampler::sample(float u, float v, int level, bool linear) const {
    return sample_fn_(*this, u, v, level, linear);
}

namespace {

// Nearest or bilinear (4-bit fractions, like the hardware's fixed-point filter) over `fetch(x, y)`.
template <class Fetch>
std::uint32_t filter(float u, float v, int width, int height, bool clamp_u, bool clamp_v, bool linear, Fetch&& fetch) {
    const auto wrap = [](int c, int size, bool clamp) { return clamp ? std::clamp(c, 0, size - 1) : c & (size - 1); };
    const float fu = u * static_cast<float>(width), fv = v * static_cast<float>(height);
    if (!linear) {
        const int x = wrap(static_cast<int>(std::floor(fu)), width, clamp_u);
        const int y = wrap(static_cast<int>(std::floor(fv)), height, clamp_v);
        return fetch(x, y);
    }
    const float su = fu - 0.5f, sv = fv - 0.5f;
    const int x0 = static_cast<int>(std::floor(su)), y0 = static_cast<int>(std::floor(sv));
    const int fx = static_cast<int>((su - static_cast<float>(x0)) * 16.0f), fy = static_cast<int>((sv - static_cast<float>(y0)) * 16.0f);
    const int xa = wrap(x0, width, clamp_u), xb = wrap(x0 + 1, width, clamp_u);
    const int ya = wrap(y0, height, clamp_v), yb = wrap(y0 + 1, height, clamp_v);
    const std::uint32_t t00 = fetch(xa, ya), t10 = fetch(xb, ya), t01 = fetch(xa, yb), t11 = fetch(xb, yb);
    std::uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        const int c00 = static_cast<int>((t00 >> s) & 0xFF), c10 = static_cast<int>((t10 >> s) & 0xFF);
        const int c01 = static_cast<int>((t01 >> s) & 0xFF), c11 = static_cast<int>((t11 >> s) & 0xFF);
        const int top = c00 * (16 - fx) + c10 * fx, bottom = c01 * (16 - fx) + c11 * fx;
        out |= static_cast<std::uint32_t>((top * (16 - fy) + bottom * fy) / 256) << s;
    }
    return out;
}

}  // namespace

template <int Format, bool Swizzled>
std::uint32_t TextureSampler::sample_fixed(const TextureSampler& t, float u, float v, int level, bool linear) {
    const Level& l = t.level_[std::clamp(level, 0, t.levels_ - 1)];
    return filter(u, v, l.width, l.height, t.clamp_u_, t.clamp_v_, linear,
                  [&](int x, int y) { return fetch<Format, Swizzled>(t, l, x, y); });
}

std::uint32_t TextureSampler::sample_generic(const TextureSampler& t, float u, float v, int level, bool linear) {
    const Level& l = t.level_[std::clamp(level, 0, t.levels_ - 1)];
    return filter(u, v, l.width, l.height, t.clamp_u_, t.clamp_v_, linear, [&](int x, int y) { return t.texel(l, x, y); });
}

std::uint64_t texture_texels_hash(const TextureSampler& ts) {
    std::uint64_t h = 0;
    for (int l = 0; l < ts.levels(); ++l) h = hash_bytes(ts.level_data(l), ts.level_size(l), h);
    return h;
}

std::uint64_t texture_content_hash(const ge::State& s, const TextureSampler& ts) {
    std::uint64_t content = 0;
    if (ts.uses_clut()) {
        content = hash_bytes(s.clut.data(), s.clut.size() * sizeof(s.clut[0]));
        const std::uint32_t clut_format = s.param(ge::CLUTFORMAT);
        content = hash_bytes(&clut_format, 4, content);
    }
    for (int l = 0; l < ts.levels(); ++l) content = hash_bytes(ts.level_data(l), ts.level_size(l), content);
    return content;
}

}  // namespace psprecore::rt::gpu
