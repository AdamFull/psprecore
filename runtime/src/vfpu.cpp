// VFPU (vector unit) semantics for recompiled code: vfpu_op / vfpu_load / vfpu_store (recomp.h).
//
// Register file: 8 matrices of 4x4 floats, Context::v[matrix * 16 + column * 4 + row]. A 7-bit register
// number encodes the matrix (bits 2-4), a column (bits 0-1), a transpose flag (bit 5) and a start offset
// (bits 5-6, depending on the vector size). Column vectors run down a column, row vectors along a row.
//
// Prefixes: vpfxs/vpfxt rewrite source lanes (swizzle, abs, constants, negate), vpfxd saturates or masks
// destination lanes. They apply to the next arithmetic instruction and are then reset.
//
// Transcendentals use IEEE functions; the PSP's own approximations differ in the last bits (TODO: verify
// against hardware).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numbers>

#include "ff7cc/mips/allegrex.hpp"
#include "ff7cc/recomp/recomp.h"

namespace {

using ff7cc::mips::Op;
using Vec = std::array<float, 4>;

constexpr std::uint32_t kPfxIdentity = 0xE4;  // x,y,z,w

// ---- register addressing -----------------------------------------------------------------------

std::array<int, 4> lanes(std::uint32_t reg, int n) {
    const int mtx = (reg >> 2) & 7, col = reg & 3;
    int row = 0;
    bool transpose = (reg >> 5) & 1;
    switch (n) {
        case 1: row = (reg >> 5) & 3; transpose = false; break;
        case 2: row = (reg >> 5) & 2; break;
        case 3: row = (reg >> 6) & 1; break;
        default: row = 0; break;
    }
    std::array<int, 4> idx{};
    for (int i = 0; i < n; ++i)
        idx[i] = transpose ? mtx * 16 + ((row + i) & 3) * 4 + col : mtx * 16 + col * 4 + ((row + i) & 3);
    return idx;
}

Vec read_raw(const Context* c, std::uint32_t reg, int n) {
    Vec v{};
    const auto idx = lanes(reg, n);
    for (int i = 0; i < n; ++i) v[i] = c->v[idx[i]];
    return v;
}

void write_raw(Context* c, std::uint32_t reg, int n, const Vec& v) {
    const auto idx = lanes(reg, n);
    for (int i = 0; i < n; ++i) c->v[idx[i]] = v[i];
}

// Matrix register as n vectors: vector k uses column (col + k) for M, row (col + k) for E.
std::uint32_t matrix_vector(std::uint32_t reg, int k) { return (reg & ~3u) | ((reg + k) & 3); }

// ---- prefixes ----------------------------------------------------------------------------------

Vec apply_source_prefix(std::uint32_t pfx, const Vec& raw, int n) {
    static constexpr float kConst[8] = {0.0f, 1.0f, 2.0f, 0.5f, 3.0f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f};
    Vec out{};
    for (int i = 0; i < n; ++i) {
        const int sw = (pfx >> (i * 2)) & 3;
        const bool abs = (pfx >> (8 + i)) & 1, cst = (pfx >> (12 + i)) & 1, neg = (pfx >> (16 + i)) & 1;
        float v = cst ? kConst[sw + (abs ? 4 : 0)] : (abs ? std::fabs(raw[sw]) : raw[sw]);
        out[i] = neg ? -v : v;
    }
    return out;
}

Vec read_s(Context* c, std::uint32_t reg, int n) {
    Vec full = read_raw(c, reg, n);
    for (int i = n; i < 4; ++i) full[i] = 0.0f;
    return apply_source_prefix(c->vfpu_ctrl[VFPU_PFXS], full, n);
}
Vec read_t(Context* c, std::uint32_t reg, int n) {
    Vec full = read_raw(c, reg, n);
    for (int i = n; i < 4; ++i) full[i] = 0.0f;
    return apply_source_prefix(c->vfpu_ctrl[VFPU_PFXT], full, n);
}

void write_d(Context* c, std::uint32_t reg, int n, Vec v) {
    const std::uint32_t pfx = c->vfpu_ctrl[VFPU_PFXD];
    const auto idx = lanes(reg, n);
    for (int i = 0; i < n; ++i) {
        if ((pfx >> (8 + i)) & 1) continue;  // write mask
        switch ((pfx >> (i * 2)) & 3) {
            case 1: v[i] = std::clamp(v[i], 0.0f, 1.0f); break;
            case 3: v[i] = std::clamp(v[i], -1.0f, 1.0f); break;
            default: break;
        }
        c->v[idx[i]] = v[i];
    }
}

void reset_prefixes(Context* c) {
    c->vfpu_ctrl[VFPU_PFXS] = kPfxIdentity;
    c->vfpu_ctrl[VFPU_PFXT] = kPfxIdentity;
    c->vfpu_ctrl[VFPU_PFXD] = 0;
}

// ---- helpers -----------------------------------------------------------------------------------

float quarter_sin(float x) { return static_cast<float>(std::sin(static_cast<double>(x) * std::numbers::pi / 2)); }
float quarter_cos(float x) { return static_cast<float>(std::cos(static_cast<double>(x) * std::numbers::pi / 2)); }

float half_to_float(std::uint16_t h) {
    const int sign = h >> 15, exp = (h >> 10) & 31, frac = h & 0x3FF;
    float f;
    if (exp == 0) f = std::ldexp(static_cast<float>(frac), -24);
    else if (exp == 31) f = frac ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else f = std::ldexp(static_cast<float>(frac | 0x400), exp - 25);
    return sign ? -f : f;
}

std::uint16_t float_to_half(float f) {
    const std::uint32_t b = std::bit_cast<std::uint32_t>(f);
    const std::uint32_t sign = (b >> 16) & 0x8000;
    const int exp = static_cast<int>((b >> 23) & 0xFF) - 127 + 15;
    std::uint32_t frac = b & 0x7FFFFF;
    if (((b >> 23) & 0xFF) == 0xFF) return static_cast<std::uint16_t>(sign | 0x7C00 | (frac ? 0x200 : 0));
    if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7C00);
    if (exp <= 0) {
        if (exp < -10) return static_cast<std::uint16_t>(sign);
        frac |= 0x800000;
        return static_cast<std::uint16_t>(sign | (frac >> (14 - exp)));
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exp) << 10) | (frac >> 13));
}

std::uint32_t float_to_int(float f, int mode) {  // 0 nearest, 1 truncate, 2 ceil, 3 floor
    if (std::isnan(f)) return 0x7FFFFFFF;
    const double d = mode == 0 ? std::nearbyint(static_cast<double>(f)) : mode == 1 ? std::trunc(static_cast<double>(f))
                   : mode == 2 ? std::ceil(static_cast<double>(f)) : std::floor(static_cast<double>(f));
    if (d >= 2147483647.0) return 0x7FFFFFFF;
    if (d <= -2147483648.0) return 0x80000000;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(d));
}

float vcst(int index) {
    static constexpr float kTable[32] = {
        0.0f, std::numeric_limits<float>::max(), std::numbers::sqrt2_v<float>, 1.0f / std::numbers::sqrt2_v<float>,
        1.12837916709551f /* 2/sqrt(pi) */, 2.0f / std::numbers::pi_v<float>, 1.0f / std::numbers::pi_v<float>,
        std::numbers::pi_v<float> / 4, std::numbers::pi_v<float> / 2, std::numbers::pi_v<float>, std::numbers::e_v<float>,
        std::numbers::log2e_v<float>, std::numbers::log10e_v<float>, std::numbers::ln2_v<float>, std::numbers::ln10_v<float>,
        2 * std::numbers::pi_v<float>, std::numbers::pi_v<float> / 6, 0.30102999566f, 3.32192809489f,
        0.86602540378f,
    };
    return kTable[index & 31];
}

std::uint32_t random_next(Context* c) {  // xorshift32 kept in the random control registers
    std::uint32_t x = c->vfpu_ctrl[VFPU_RCX0] ? c->vfpu_ctrl[VFPU_RCX0] : 0x3F800001u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    c->vfpu_ctrl[VFPU_RCX0] = x;
    return x;
}

[[noreturn]] void unsupported(std::uint32_t word) {
    std::fprintf(stderr, "VFPU: unsupported instruction %08X (%s)\n", word,
                 ff7cc::mips::format(ff7cc::mips::decode(word, 0)).c_str());
    std::abort();
}

bool compare(int cond, float a, float b) {
    switch (cond) {
        case 0: return false;           // FL
        case 1: return a == b;          // EQ
        case 2: return a < b;           // LT
        case 3: return a <= b;          // LE
        case 4: return true;            // TR
        case 5: return a != b;          // NE
        case 6: return a >= b;          // GE
        case 7: return a > b;           // GT
        case 8: return a == 0.0f;       // EZ
        case 9: return std::isnan(a);   // EN
        case 10: return std::isinf(a);  // EI
        case 11: return std::isnan(a) || std::isinf(a);  // ES
        case 12: return a != 0.0f;      // NZ
        case 13: return !std::isnan(a); // NN
        case 14: return !std::isinf(a); // NI
        default: return !(std::isnan(a) || std::isinf(a));  // NS
    }
}

}  // namespace

extern "C" void vfpu_op(Context* c, uint32_t w) {
    const auto in = ff7cc::mips::decode(w, 0);
    const int n = in.vsize();
    const std::uint32_t vd = in.vd(), vs = in.vs(), vt = in.vt();
    Vec d{};

    switch (in.op) {
        // ---- prefixes, immediates and transfers (do not consume prefixes) ----
        case Op::Vpfxs: c->vfpu_ctrl[VFPU_PFXS] = w & 0x000FFFFF; return;
        case Op::Vpfxt: c->vfpu_ctrl[VFPU_PFXT] = w & 0x000FFFFF; return;
        case Op::Vpfxd: c->vfpu_ctrl[VFPU_PFXD] = w & 0x00000FFF; return;
        case Op::Mfv: if (in.rt()) c->r[in.rt()] = std::bit_cast<std::uint32_t>(read_raw(c, vd, 1)[0]); return;
        case Op::Mtv: write_raw(c, vd, 1, {std::bit_cast<float>(c->r[in.rt()])}); return;
        case Op::Mfvc: if (in.rt()) c->r[in.rt()] = c->vfpu_ctrl[(w & 0x7F) & 15]; return;
        case Op::Mtvc: c->vfpu_ctrl[(w & 0x7F) & 15] = c->r[in.rt()]; return;
        case Op::Vnop: case Op::Vsync: case Op::Vflush: return;
        default: break;
    }

    switch (in.op) {
        // ---- binary arithmetic ----
        case Op::Vadd: case Op::Vsub: case Op::Vdiv: case Op::Vmul: case Op::Vmin: case Op::Vmax:
        case Op::Vscmp: case Op::Vsge: case Op::Vslt: {
            const Vec s = read_s(c, vs, n), t = read_t(c, vt, n);
            for (int i = 0; i < n; ++i) {
                switch (in.op) {
                    case Op::Vadd: d[i] = s[i] + t[i]; break;
                    case Op::Vsub: d[i] = s[i] - t[i]; break;
                    case Op::Vdiv: d[i] = s[i] / t[i]; break;
                    case Op::Vmul: d[i] = s[i] * t[i]; break;
                    case Op::Vmin: d[i] = std::min(s[i], t[i]); break;
                    case Op::Vmax: d[i] = std::max(s[i], t[i]); break;
                    case Op::Vscmp: d[i] = s[i] < t[i] ? -1.0f : s[i] > t[i] ? 1.0f : 0.0f; break;
                    case Op::Vsge: d[i] = s[i] >= t[i] ? 1.0f : 0.0f; break;
                    default: d[i] = s[i] < t[i] ? 1.0f : 0.0f; break;
                }
            }
            write_d(c, vd, n, d);
            break;
        }
        case Op::Vsbn: {
            const Vec s = read_s(c, vs, n), t = read_t(c, vt, n);
            for (int i = 0; i < n; ++i) d[i] = std::ldexp(s[i], static_cast<int>(std::bit_cast<std::int32_t>(t[i])));
            write_d(c, vd, n, d);
            break;
        }
        case Op::Vdot: case Op::Vhdp: {
            const Vec s = read_s(c, vs, n), t = read_t(c, vt, n);
            float sum = 0.0f;
            for (int i = 0; i < n; ++i) sum += (in.op == Op::Vhdp && i == n - 1) ? t[i] : s[i] * t[i];
            write_d(c, vd, 1, {sum});
            break;
        }
        case Op::Vscl: {
            const Vec s = read_s(c, vs, n);
            const float t = read_raw(c, vt, 1)[0];
            for (int i = 0; i < n; ++i) d[i] = s[i] * t;
            write_d(c, vd, n, d);
            break;
        }
        case Op::Vcrs: {  // half cross product
            const Vec s = read_s(c, vs, 3), t = read_t(c, vt, 3);
            write_d(c, vd, 3, {s[1] * t[2], s[2] * t[0], s[0] * t[1]});
            break;
        }
        case Op::Vcrsp: {
            const Vec s = read_s(c, vs, 3), t = read_t(c, vt, 3);
            write_d(c, vd, 3, {s[1] * t[2] - s[2] * t[1], s[2] * t[0] - s[0] * t[2], s[0] * t[1] - s[1] * t[0]});
            break;
        }
        case Op::Vqmul: {
            const Vec s = read_s(c, vs, 4), t = read_t(c, vt, 4);
            write_d(c, vd, 4,
                    {s[0] * t[3] + s[1] * t[2] - s[2] * t[1] + s[3] * t[0],
                     -s[0] * t[2] + s[1] * t[3] + s[2] * t[0] + s[3] * t[1],
                     s[0] * t[1] - s[1] * t[0] + s[2] * t[3] + s[3] * t[2],
                     -s[0] * t[0] - s[1] * t[1] - s[2] * t[2] + s[3] * t[3]});
            break;
        }
        case Op::Vdet: {
            const Vec s = read_s(c, vs, 2), t = read_t(c, vt, 2);
            write_d(c, vd, 1, {s[0] * t[1] - s[1] * t[0]});
            break;
        }
        case Op::Vcmp: {
            const Vec s = read_s(c, vs, n), t = read_t(c, vt, n);
            const int cond = static_cast<int>(w & 15);
            std::uint32_t cc = 0, any = 0, all = 1;
            for (int i = 0; i < n; ++i) {
                const bool r = compare(cond, s[i], t[i]);
                cc |= static_cast<std::uint32_t>(r) << i;
                any |= r;
                all &= r;
            }
            c->vfpu_ctrl[VFPU_CC] = (c->vfpu_ctrl[VFPU_CC] & ~0x3Fu) | cc | (any << 4) | (all << 5);
            break;
        }

        // ---- unary ----
        case Op::Vmov: case Op::Vabs: case Op::Vneg: case Op::Vsat0: case Op::Vsat1: case Op::Vrcp: case Op::Vrsq:
        case Op::Vsin: case Op::Vcos: case Op::Vexp2: case Op::Vlog2: case Op::Vsqrt: case Op::Vasin: case Op::Vnrcp:
        case Op::Vnsin: case Op::Vrexp2: case Op::Vocp: case Op::Vsgn: {
            const Vec s = read_s(c, vs, n);
            for (int i = 0; i < n; ++i) {
                const float x = s[i];
                switch (in.op) {
                    case Op::Vmov: d[i] = x; break;
                    case Op::Vabs: d[i] = std::fabs(x); break;
                    case Op::Vneg: d[i] = -x; break;
                    case Op::Vsat0: d[i] = std::clamp(x, 0.0f, 1.0f); break;
                    case Op::Vsat1: d[i] = std::clamp(x, -1.0f, 1.0f); break;
                    case Op::Vrcp: d[i] = 1.0f / x; break;
                    case Op::Vrsq: d[i] = 1.0f / std::sqrt(x); break;
                    case Op::Vsin: d[i] = quarter_sin(x); break;
                    case Op::Vcos: d[i] = quarter_cos(x); break;
                    case Op::Vexp2: d[i] = std::exp2(x); break;
                    case Op::Vlog2: d[i] = std::log2(x); break;
                    case Op::Vsqrt: d[i] = std::sqrt(x); break;
                    case Op::Vasin: d[i] = static_cast<float>(std::asin(static_cast<double>(x)) * 2 / std::numbers::pi); break;
                    case Op::Vnrcp: d[i] = -1.0f / x; break;
                    case Op::Vnsin: d[i] = -quarter_sin(x); break;
                    case Op::Vrexp2: d[i] = 1.0f / std::exp2(x); break;
                    case Op::Vocp: d[i] = 1.0f - x; break;
                    default: d[i] = x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f; break;  // vsgn
                }
            }
            write_d(c, vd, n, d);
            break;
        }
        case Op::Vidt: {
            const int lane = (vd & 3);
            for (int i = 0; i < n; ++i) d[i] = (n == 4 ? i == lane : i == (lane & (n - 1))) ? 1.0f : 0.0f;
            write_d(c, vd, n, d);
            break;
        }
        case Op::Vzero: write_d(c, vd, n, {}); break;
        case Op::Vone: write_d(c, vd, n, {1.0f, 1.0f, 1.0f, 1.0f}); break;
        case Op::Vcst: {
            const float k = vcst(static_cast<int>((w >> 16) & 31));
            write_d(c, vd, n, {k, k, k, k});
            break;
        }
        case Op::Vfad: case Op::Vavg: {
            const Vec s = read_s(c, vs, n);
            float sum = 0.0f;
            for (int i = 0; i < n; ++i) sum += s[i];
            write_d(c, vd, 1, {in.op == Op::Vavg ? sum / static_cast<float>(n) : sum});
            break;
        }
        case Op::Vsrt1: case Op::Vsrt2: case Op::Vsrt3: case Op::Vsrt4: case Op::Vbfy1: case Op::Vbfy2: {
            const Vec s = read_s(c, vs, n);
            const float x = s[0], y = s[1], z = s[2], ww = s[3];
            switch (in.op) {
                case Op::Vsrt1: d = {std::min(x, y), std::max(x, y), std::min(z, ww), std::max(z, ww)}; break;
                case Op::Vsrt2: d = {std::min(x, ww), std::min(y, z), std::max(y, z), std::max(x, ww)}; break;
                case Op::Vsrt3: d = {std::max(x, y), std::min(x, y), std::max(z, ww), std::min(z, ww)}; break;
                case Op::Vsrt4: d = {std::max(x, ww), std::max(y, z), std::min(y, z), std::min(x, ww)}; break;
                case Op::Vbfy1: d = {x + y, x - y, z + ww, z - ww}; break;
                default: d = {x + z, y + ww, x - z, y - ww}; break;
            }
            write_d(c, vd, n, d);
            break;
        }
        case Op::Vsocp: {
            const Vec s = read_s(c, vs, n);
            const int out = std::min(n * 2, 4);
            for (int i = 0; i < out / 2; ++i) {
                d[2 * i] = std::clamp(1.0f - s[i], 0.0f, 1.0f);
                d[2 * i + 1] = std::clamp(s[i], 0.0f, 1.0f);
            }
            write_d(c, vd, out, d);
            break;
        }

        // ---- conversions ----
        case Op::Vf2in: case Op::Vf2iz: case Op::Vf2iu: case Op::Vf2id: {
            const Vec s = read_s(c, vs, n);
            const int mode = in.op == Op::Vf2in ? 0 : in.op == Op::Vf2iz ? 1 : in.op == Op::Vf2iu ? 2 : 3;
            const int scale = static_cast<int>((w >> 16) & 31);
            for (int i = 0; i < n; ++i) d[i] = std::bit_cast<float>(float_to_int(std::ldexp(s[i], scale), mode));
            reset_prefixes(c);  // integer results: write without saturation
            write_raw(c, vd, n, d);
            return;
        }
        case Op::Vi2f: {
            const Vec s = read_raw(c, vs, n);
            const int scale = static_cast<int>((w >> 16) & 31);
            for (int i = 0; i < n; ++i) d[i] = std::ldexp(static_cast<float>(std::bit_cast<std::int32_t>(s[i])), -scale);
            write_d(c, vd, n, d);
            break;
        }
        case Op::Vi2uc: case Op::Vi2c: {  // 4 ints -> 4 packed bytes
            const Vec s = read_raw(c, vs, 4);
            std::uint32_t packed = 0;
            for (int i = 0; i < 4; ++i) {
                const std::int32_t v = std::bit_cast<std::int32_t>(s[i]);
                const std::uint32_t byte = in.op == Op::Vi2uc ? (v < 0 ? 0u : static_cast<std::uint32_t>(v) >> 23)
                                                               : static_cast<std::uint32_t>(v) >> 24;
                packed |= (byte & 0xFF) << (i * 8);
            }
            write_raw(c, vd, 1, {std::bit_cast<float>(packed)});
            break;
        }
        case Op::Vi2us: case Op::Vi2s: {  // pairs of ints -> packed 16-bit
            const Vec s = read_raw(c, vs, n);
            for (int i = 0; i < n / 2; ++i) {
                std::uint32_t packed = 0;
                for (int k = 0; k < 2; ++k) {
                    const std::int32_t v = std::bit_cast<std::int32_t>(s[i * 2 + k]);
                    const std::uint32_t half = in.op == Op::Vi2us ? (v < 0 ? 0u : static_cast<std::uint32_t>(v) >> 15)
                                                                   : static_cast<std::uint32_t>(v) >> 16;
                    packed |= (half & 0xFFFF) << (k * 16);
                }
                d[i] = std::bit_cast<float>(packed);
            }
            write_raw(c, vd, std::max(n / 2, 1), d);
            break;
        }
        case Op::Vuc2i: case Op::Vc2i: {  // packed bytes -> 4 ints
            const std::uint32_t packed = std::bit_cast<std::uint32_t>(read_raw(c, vs, 1)[0]);
            for (int i = 0; i < 4; ++i) {
                const std::uint32_t b = (packed >> (i * 8)) & 0xFF;
                const std::uint32_t v = in.op == Op::Vuc2i ? ((b << 24) | (b << 16) | (b << 8) | b) >> 1 : b << 24;
                d[i] = std::bit_cast<float>(v);
            }
            write_raw(c, vd, 4, d);
            break;
        }
        case Op::Vus2i: case Op::Vs2i: {
            const Vec s = read_raw(c, vs, n);
            for (int i = 0; i < n; ++i) {
                const std::uint32_t packed = std::bit_cast<std::uint32_t>(s[i]);
                for (int k = 0; k < 2; ++k) {
                    const std::uint32_t h = (packed >> (k * 16)) & 0xFFFF;
                    d[i * 2 + k] = std::bit_cast<float>(in.op == Op::Vus2i ? h << 15 : h << 16);
                }
            }
            write_raw(c, vd, std::min(n * 2, 4), d);
            break;
        }
        case Op::Vf2h: {
            const Vec s = read_s(c, vs, n);
            for (int i = 0; i < n / 2; ++i)
                d[i] = std::bit_cast<float>(static_cast<std::uint32_t>(float_to_half(s[2 * i])) |
                                            (static_cast<std::uint32_t>(float_to_half(s[2 * i + 1])) << 16));
            write_raw(c, vd, std::max(n / 2, 1), d);
            break;
        }
        case Op::Vh2f: {
            const Vec s = read_raw(c, vs, n);
            for (int i = 0; i < n; ++i) {
                const std::uint32_t packed = std::bit_cast<std::uint32_t>(s[i]);
                d[2 * i] = half_to_float(static_cast<std::uint16_t>(packed));
                d[2 * i + 1] = half_to_float(static_cast<std::uint16_t>(packed >> 16));
            }
            write_d(c, vd, std::min(n * 2, 4), d);
            break;
        }
        case Op::Vt4444: case Op::Vt5551: case Op::Vt5650: {  // 8888 colours -> 16-bit formats, packed in pairs
            const Vec s = read_raw(c, vs, n);
            for (int i = 0; i < n / 2; ++i) {
                std::uint32_t packed = 0;
                for (int k = 0; k < 2; ++k) {
                    const std::uint32_t col = std::bit_cast<std::uint32_t>(s[2 * i + k]);
                    const std::uint32_t r = col & 0xFF, g = (col >> 8) & 0xFF, b = (col >> 16) & 0xFF, a = col >> 24;
                    std::uint32_t v;
                    if (in.op == Op::Vt4444) v = (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
                    else if (in.op == Op::Vt5551) v = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
                    else v = (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
                    packed |= v << (k * 16);
                }
                d[i] = std::bit_cast<float>(packed);
            }
            write_raw(c, vd, std::max(n / 2, 1), d);
            break;
        }

        // ---- conditional moves, immediates, random ----
        case Op::Vcmovt: case Op::Vcmovf: {
            const Vec s = read_s(c, vs, n);
            const Vec old = read_raw(c, vd, n);
            const std::uint32_t imm = (w >> 16) & 7, cc = c->vfpu_ctrl[VFPU_CC];
            const bool want = in.op == Op::Vcmovt;
            for (int i = 0; i < n; ++i) {
                bool take;
                if (imm < 6) take = (((cc >> imm) & 1) != 0) == want;
                else if (imm == 6) take = (((cc >> i) & 1) != 0) == want;
                else take = false;
                d[i] = take ? s[i] : old[i];
            }
            write_d(c, vd, n, d);
            break;
        }
        case Op::Viim: write_raw(c, vt, 1, {static_cast<float>(in.simm())}); break;
        case Op::Vfim: write_raw(c, vt, 1, {half_to_float(static_cast<std::uint16_t>(w & 0xFFFF))}); break;
        case Op::Vrnds: c->vfpu_ctrl[VFPU_RCX0] = std::bit_cast<std::uint32_t>(read_raw(c, vs, 1)[0]) | 1; break;
        case Op::Vrndi: case Op::Vrndf1: case Op::Vrndf2: {
            for (int i = 0; i < n; ++i) {
                const std::uint32_t r = random_next(c);
                if (in.op == Op::Vrndi) d[i] = std::bit_cast<float>(r);
                else d[i] = std::bit_cast<float>(0x3F800000u | (r >> 9)) * (in.op == Op::Vrndf2 ? 2.0f : 1.0f);
            }
            write_d(c, vd, n, d);
            break;
        }
        case Op::Vmfvc: write_raw(c, vd, 1, {std::bit_cast<float>(c->vfpu_ctrl[vs & 15])}); break;
        case Op::Vmtvc: c->vfpu_ctrl[vd & 15] = std::bit_cast<std::uint32_t>(read_raw(c, vs, 1)[0]); break;

        // ---- matrices ----
        case Op::Vmmov: case Op::Vmidt: case Op::Vmzero: case Op::Vmone: {
            for (int k = 0; k < n; ++k) {
                Vec row{};
                if (in.op == Op::Vmmov) row = read_raw(c, matrix_vector(vs, k), n);
                for (int i = 0; i < n; ++i) {
                    if (in.op == Op::Vmidt) row[i] = i == k ? 1.0f : 0.0f;
                    else if (in.op == Op::Vmone) row[i] = 1.0f;
                }
                write_d(c, matrix_vector(vd, k), n, row);
            }
            break;
        }
        case Op::Vmmul: {  // d = transpose(s) * t, as vectors: d[a][b] = sum_c s[b][c] * t[a][c]
            std::array<Vec, 4> s{}, t{}, r{};
            for (int k = 0; k < n; ++k) {
                s[k] = read_raw(c, matrix_vector(vs, k), n);
                t[k] = read_raw(c, matrix_vector(vt, k), n);
            }
            for (int a = 0; a < n; ++a)
                for (int b = 0; b < n; ++b) {
                    float sum = 0.0f;
                    for (int k = 0; k < n; ++k) sum += s[b][k] * t[a][k];
                    r[a][b] = sum;
                }
            for (int a = 0; a < n; ++a) write_d(c, matrix_vector(vd, a), n, r[a]);
            break;
        }
        case Op::Vmscl: {
            const float scale = read_raw(c, vt, 1)[0];
            for (int k = 0; k < n; ++k) {
                Vec row = read_raw(c, matrix_vector(vs, k), n);
                for (int i = 0; i < n; ++i) row[i] *= scale;
                write_d(c, matrix_vector(vd, k), n, row);
            }
            break;
        }
        case Op::Vtfm2: case Op::Vtfm3: case Op::Vtfm4: case Op::Vhtfm2: case Op::Vhtfm3: case Op::Vhtfm4: {
            const bool homogeneous = in.op == Op::Vhtfm2 || in.op == Op::Vhtfm3 || in.op == Op::Vhtfm4;
            const int m = homogeneous ? n + 1 : n;  // matrix/result size; vt has n lanes (plus implicit 1)
            Vec t = read_raw(c, vt, n);
            if (homogeneous) t[n] = 1.0f;
            for (int i = 0; i < m; ++i) {
                const Vec row = read_raw(c, matrix_vector(vs, i), m);
                float sum = 0.0f;
                for (int k = 0; k < m; ++k) sum += row[k] * t[k];
                d[i] = sum;
            }
            write_d(c, vd, m, d);
            break;
        }
        case Op::Vrot: {
            const float angle = read_raw(c, vs, 1)[0];
            const std::uint32_t imm = (w >> 16) & 31;
            const int sin_pos = static_cast<int>((imm >> 2) & 3), cos_pos = static_cast<int>(imm & 3);
            const float sine = (imm & 16) ? -quarter_sin(angle) : quarter_sin(angle), cosine = quarter_cos(angle);
            for (int i = 0; i < n; ++i)
                d[i] = i == cos_pos ? cosine : (i == sin_pos || sin_pos == cos_pos) ? sine : 0.0f;
            write_d(c, vd, n, d);
            break;
        }
        default:
            unsupported(w);
    }
    reset_prefixes(c);
}

// lv.s / lv.q / lvl.q / lvr.q
extern "C" void vfpu_load(Context* c, uint32_t w, uint32_t address) {
    const auto in = ff7cc::mips::decode(w, 0);
    switch (in.op) {
        case Op::LvS: {
            const std::uint32_t reg = ((w >> 16) & 31) | ((w & 3) << 5);
            write_raw(c, reg, 1, {std::bit_cast<float>(mem_lw(address))});
            return;
        }
        case Op::LvQ: {
            const std::uint32_t reg = ((w >> 16) & 31) | ((w & 1) << 5);
            Vec v{};
            for (int i = 0; i < 4; ++i) v[i] = std::bit_cast<float>(mem_lw(address + 4 * i));
            write_raw(c, reg, 4, v);
            return;
        }
        case Op::LvlQ: case Op::LvrQ: {  // unaligned halves of a quad load
            const std::uint32_t reg = ((w >> 16) & 31) | ((w & 1) << 5);
            Vec v = read_raw(c, reg, 4);
            const int k = static_cast<int>((address >> 2) & 3);
            if (in.op == Op::LvlQ)
                for (int i = 0; i <= k; ++i) v[3 - k + i] = std::bit_cast<float>(mem_lw((address & ~15u) + 4 * i));
            else
                for (int i = k; i < 4; ++i) v[i - k] = std::bit_cast<float>(mem_lw((address & ~15u) + 4 * i));
            write_raw(c, reg, 4, v);
            return;
        }
        default: unsupported(w);
    }
}

// sv.s / sv.q / svl.q / svr.q
extern "C" void vfpu_store(Context* c, uint32_t w, uint32_t address) {
    const auto in = ff7cc::mips::decode(w, 0);
    switch (in.op) {
        case Op::SvS: {
            const std::uint32_t reg = ((w >> 16) & 31) | ((w & 3) << 5);
            mem_sw(address, std::bit_cast<std::uint32_t>(read_raw(c, reg, 1)[0]));
            return;
        }
        case Op::SvQ: {
            const std::uint32_t reg = ((w >> 16) & 31) | ((w & 1) << 5);
            const Vec v = read_raw(c, reg, 4);
            for (int i = 0; i < 4; ++i) mem_sw(address + 4 * i, std::bit_cast<std::uint32_t>(v[i]));
            return;
        }
        case Op::SvlQ: case Op::SvrQ: {
            const std::uint32_t reg = ((w >> 16) & 31) | ((w & 1) << 5);
            const Vec v = read_raw(c, reg, 4);
            const int k = static_cast<int>((address >> 2) & 3);
            if (in.op == Op::SvlQ)
                for (int i = 0; i <= k; ++i) mem_sw((address & ~15u) + 4 * i, std::bit_cast<std::uint32_t>(v[3 - k + i]));
            else
                for (int i = k; i < 4; ++i) mem_sw((address & ~15u) + 4 * i, std::bit_cast<std::uint32_t>(v[i - k]));
            return;
        }
        default: unsupported(w);
    }
}
