#include "gpu/upscale.hpp"

#include <algorithm>
#include <cstdlib>

#include "core/foundation/core/foundation.h"

namespace psprecore::rt::gpu {

namespace {

struct Px {
    int r = 0, g = 0, b = 0, a = 0;  // premultiplied, 0..255
};

Px premultiply(std::uint32_t c) {
    const int a = static_cast<int>(c >> 24);
    return {static_cast<int>(c & 0xFF) * a / 255, static_cast<int>((c >> 8) & 0xFF) * a / 255,
            static_cast<int>((c >> 16) & 0xFF) * a / 255, a};
}

std::uint32_t unpremultiply(const Px& p) {
    if (p.a <= 0) return 0;
    const auto ch = [&](int v) { return static_cast<std::uint32_t>(std::clamp(v * 255 / p.a, 0, 255)); };
    return ch(p.r) | (ch(p.g) << 8) | (ch(p.b) << 16) | (static_cast<std::uint32_t>(std::min(p.a, 255)) << 24);
}

int difference(const Px& p, const Px& q) {
    const int luma = std::abs((p.r - q.r) * 2 + (p.g - q.g) * 5 + (p.b - q.b));
    return luma / 8 + std::abs(p.a - q.a);
}

Px average(const Px& p, const Px& q) { return {(p.r + q.r) / 2, (p.g + q.g) / 2, (p.b + q.b) / 2, (p.a + q.a) / 2}; }

// The texel between four neighbours: along whichever pair (p1-p2 or q1-q2) differs clearly less, else all four.
Px between(const Px& p1, const Px& p2, const Px& q1, const Px& q2) {
    const int dp = difference(p1, p2), dq = difference(q1, q2);
    if (dp * 2 < dq) return average(p1, p2);
    if (dq * 2 < dp) return average(q1, q2);
    return {(p1.r + p2.r + q1.r + q2.r) / 4, (p1.g + p2.g + q1.g + q2.g) / 4, (p1.b + p2.b + q1.b + q2.b) / 4,
            (p1.a + p2.a + q1.a + q2.a) / 4};
}

}  // namespace

void upscale_2x(const std::uint32_t* src, int w, int h, std::uint32_t* dst) {
    const auto at = [&](const nx::vector<Px>& v, int x, int y) -> const Px& {
        return v[static_cast<std::size_t>(std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1))];
    };
    nx::vector<Px> p(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (std::size_t i = 0; i < p.size(); ++i) p[i] = premultiply(src[i]);
    // The centre of each 2x2 block of source texels: along the smoother diagonal.
    nx::vector<Px> c(p.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            c[static_cast<std::size_t>(y * w + x)] =
                between(at(p, x, y), at(p, x + 1, y + 1), at(p, x + 1, y), at(p, x, y + 1));
    const int ow = w * 2;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Px& s = at(p, x, y);
            // Right of the texel: between it and its right neighbour, or between the centres above and below.
            const Px right = between(s, at(p, x + 1, y), at(c, x, y - 1), at(c, x, y));
            // Below the texel: between it and the texel below, or between the centres left and right.
            const Px below = between(s, at(p, x, y + 1), at(c, x - 1, y), at(c, x, y));
            std::uint32_t* row = dst + static_cast<std::size_t>(y * 2) * static_cast<std::size_t>(ow);
            row[x * 2] = src[static_cast<std::size_t>(y * w + x)];
            row[x * 2 + 1] = unpremultiply(right);
            row[ow + x * 2] = unpremultiply(below);
            row[ow + x * 2 + 1] = unpremultiply(at(c, x, y));
        }
}

}  // namespace psprecore::rt::gpu
