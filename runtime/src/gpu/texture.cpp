#include "gpu/texture.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ff7cc/runtime/memory.hpp"

namespace ff7cc::rt::gpu {

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
    }
}

std::uint32_t TextureSampler::clut(std::uint32_t index) const {
    const std::uint32_t shift = (clut_format_ >> 2) & 31, mask = (clut_format_ >> 8) & 0xFF;
    const std::uint32_t start = ((clut_format_ >> 16) & 31) << 4;
    index = ((index >> shift) & mask) | start;
    switch (clut_format_ & 3) {
        case 0: return decode_5650(load16(clut_ + ((index * 2) & 1023)));
        case 1: return decode_5551(load16(clut_ + ((index * 2) & 1023)));
        case 2: return decode_4444(load16(clut_ + ((index * 2) & 1023)));
        default: return load32(clut_ + ((index * 4) & 1023));
    }
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

void TextureSampler::decode(int level, std::uint32_t* out) const {
    const Level& l = level_[std::clamp(level, 0, levels_ - 1)];
    for (int y = 0; y < l.height; ++y)
        for (int x = 0; x < l.width; ++x) *out++ = texel(l, x, y);
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

int TextureSampler::wrap(int c, int size, bool clamp) const {
    return clamp ? std::clamp(c, 0, size - 1) : c & (size - 1);
}

std::uint32_t TextureSampler::sample(float u, float v, int level, bool linear) const {
    const Level& l = level_[std::clamp(level, 0, levels_ - 1)];
    const float fu = u * static_cast<float>(l.width), fv = v * static_cast<float>(l.height);
    if (!linear) {
        const int x = wrap(static_cast<int>(std::floor(fu)), l.width, clamp_u_);
        const int y = wrap(static_cast<int>(std::floor(fv)), l.height, clamp_v_);
        return texel(l, x, y);
    }
    // Bilinear with 4-bit fractions, like the hardware's fixed-point filter.
    const float su = fu - 0.5f, sv = fv - 0.5f;
    const int x0 = static_cast<int>(std::floor(su)), y0 = static_cast<int>(std::floor(sv));
    const int fx = static_cast<int>((su - static_cast<float>(x0)) * 16.0f), fy = static_cast<int>((sv - static_cast<float>(y0)) * 16.0f);
    const int xa = wrap(x0, l.width, clamp_u_), xb = wrap(x0 + 1, l.width, clamp_u_);
    const int ya = wrap(y0, l.height, clamp_v_), yb = wrap(y0 + 1, l.height, clamp_v_);
    const std::uint32_t t00 = texel(l, xa, ya), t10 = texel(l, xb, ya), t01 = texel(l, xa, yb), t11 = texel(l, xb, yb);
    std::uint32_t out = 0;
    for (int s = 0; s < 32; s += 8) {
        const int c00 = static_cast<int>((t00 >> s) & 0xFF), c10 = static_cast<int>((t10 >> s) & 0xFF);
        const int c01 = static_cast<int>((t01 >> s) & 0xFF), c11 = static_cast<int>((t11 >> s) & 0xFF);
        const int top = c00 * (16 - fx) + c10 * fx, bottom = c01 * (16 - fx) + c11 * fx;
        out |= static_cast<std::uint32_t>((top * (16 - fy) + bottom * fy) / 256) << s;
    }
    return out;
}

}  // namespace ff7cc::rt::gpu
