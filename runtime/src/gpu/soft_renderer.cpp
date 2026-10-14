#include "gpu/soft_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "ff7cc/runtime/memory.hpp"
#include "gpu/texture.hpp"

namespace ff7cc::rt::gpu {

namespace {

using ge::Prim;
using ge::State;
using ge::Vertex;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(Vec3 v) {
    const float l = std::sqrt(dot(v, v));
    return l > 0.0f ? v * (1.0f / l) : v;
}
// 4x3 matrices are stored column by column: x' = m0*x + m3*y + m6*z + m9*w.
Vec3 mul43(const float* m, Vec3 v, float w = 1.0f) {
    return {m[0] * v.x + m[3] * v.y + m[6] * v.z + m[9] * w, m[1] * v.x + m[4] * v.y + m[7] * v.z + m[10] * w,
            m[2] * v.x + m[5] * v.y + m[8] * v.z + m[11] * w};
}
void mul44(const float* m, Vec3 v, float out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = m[i] * v.x + m[4 + i] * v.y + m[8 + i] * v.z + m[12 + i];
}
Vec3 color24(std::uint32_t p) {
    return {static_cast<float>(p & 0xFF) / 255.0f, static_cast<float>((p >> 8) & 0xFF) / 255.0f,
            static_cast<float>((p >> 16) & 0xFF) / 255.0f};
}
Vec3 vec_reg(const State& s, int cmd) {
    return {s.flt(static_cast<std::uint8_t>(cmd)), s.flt(static_cast<std::uint8_t>(cmd + 1)),
            s.flt(static_cast<std::uint8_t>(cmd + 2))};
}

// Vertex after transform, before clipping (clip space) and after projection (screen space). Texture coordinates
// are kept projective (s, t, q) so perspective-correct interpolation and texture projection share one path.
struct ClipVertex {
    float clip[4];
    float s, t, q;
    float color[4];  // 0..255
    float spec[3];   // separate specular, 0..255
    float fog;       // 1 = no fog
};
struct ScreenVertex {
    float x, y, z, iw;  // drawing-space pixels, depth 0..65535, 1/w
    float s, t, q;      // multiplied by iw
    float color[4];
    float spec[3];
    float fog;
};

bool compare(int func, int a, int b) {
    switch (func) {
        case 0: return false;
        case 1: return true;
        case 2: return a == b;
        case 3: return a != b;
        case 4: return a < b;
        case 5: return a <= b;
        case 6: return a > b;
        default: return a >= b;
    }
}

struct Light {
    bool on = false;
    int type = 0, kind = 0;  // type: 0 directional, 1 point, 2 spot; kind: 0 diffuse, 1 +specular, 2 powered diffuse
    Vec3 pos, dir, att, ambient, diffuse, specular;
    float exponent = 0, cutoff = 0;
};

struct Setup {
    // Targets.
    std::uint8_t* fb = nullptr;
    std::uint8_t* zb = nullptr;
    int fb_format = 3, bpp = 4, fb_stride = 512, z_stride = 512;
    int sx1 = 0, sy1 = 0, sx2 = 479, sy2 = 271;
    bool through = false;
    // Clear mode.
    bool clear = false, clear_color = false, clear_stencil = false, clear_depth = false;
    // Texturing.
    bool texture = false;
    TextureSampler sampler;
    int tex_func = 0;
    bool tex_alpha = false, tex_double = false;
    int env[3] = {};
    bool min_linear = false, mag_linear = false, mipmap = false;
    int lod_mode = 0;
    float lod_bias = 0;
    // Per-fragment operations.
    bool fog = false;
    int fog_color[3] = {};
    bool alpha_test = false, color_test = false, stencil_test = false, depth_test = false, z_write = false;
    int at_func = 0, at_ref = 0, at_mask = 0xFF;
    int ct_func = 0;
    std::uint32_t ct_ref = 0, ct_mask = 0;
    int st_func = 0, st_ref = 0, st_mask = 0xFF, op_sfail = 0, op_zfail = 0, op_zpass = 0;
    int z_func = 0;
    bool depth_range = false;
    int min_z = 0, max_z = 65535;
    bool blend = false;
    int blend_a = 0, blend_b = 0, blend_eq = 0;
    int fix_a[3] = {}, fix_b[3] = {};
    bool dither = false;
    int dither_matrix[4][4] = {};
    bool logic = false;
    int logic_op = 3;
    std::uint32_t write_mask = 0xFFFFFFFF;
    bool gouraud = true;
    // Culling.
    bool cull = false;
    int cull_mode = 0;
    // Vertex processing.
    bool lighting = false, separate_specular = false;
    Light lights[4];
    Vec3 emissive, mat_ambient, mat_diffuse, mat_specular, ambient;
    float mat_alpha = 1, ambient_alpha = 1, specular_coef = 0;
    std::uint32_t material_update = 0;
    bool reverse_normal = false;
    int map_mode = 0, proj_source = 0, shade_u = 0, shade_v = 0;
    float tex_scale[2] = {1, 1}, tex_offset[2] = {0, 0};
    float fog_end = 0, fog_slope = 0;
    float vp_scale[3] = {}, vp_center[3] = {};
    float offset_x = 0, offset_y = 0;
};

std::uint16_t load16(const std::uint8_t* p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}
void store16(std::uint8_t* p, std::uint32_t v) {
    const auto x = static_cast<std::uint16_t>(v);
    std::memcpy(p, &x, 2);
}

// Framebuffer pixels as RGBA8; the alpha byte holds the stencil value.
std::uint32_t read_pixel(const std::uint8_t* p, int format) {
    switch (format) {
        case 0: return decode_5650(load16(p)) & 0x00FFFFFF;
        case 1: return decode_5551(load16(p));
        case 2: return decode_4444(load16(p));
        default: {
            std::uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }
    }
}
void write_pixel(std::uint8_t* p, int format, std::uint32_t c) {
    const std::uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
    switch (format) {
        case 0: store16(p, (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11)); break;
        case 1: store16(p, (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15)); break;
        case 2: store16(p, (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12)); break;
        default: std::memcpy(p, &c, 4); break;
    }
}

int stencil_op(int op, int stencil, int ref) {
    switch (op) {
        case 1: return 0;
        case 2: return ref;
        case 3: return ~stencil & 0xFF;
        case 4: return std::min(stencil + 1, 255);
        case 5: return std::max(stencil - 1, 0);
        default: return stencil;
    }
}

std::uint32_t logic_op(int op, std::uint32_t s, std::uint32_t d) {
    switch (op) {
        case 0: return 0;
        case 1: return s & d;
        case 2: return s & ~d;
        case 3: return s;
        case 4: return ~s & d;
        case 5: return d;
        case 6: return s ^ d;
        case 7: return s | d;
        case 8: return ~(s | d);
        case 9: return ~(s ^ d);
        case 10: return ~d;
        case 11: return s | ~d;
        case 12: return ~s;
        case 13: return ~s | d;
        case 14: return ~(s & d);
        default: return 0xFFFFFFFF;
    }
}

class SoftRenderer final : public ge::Renderer {
public:
    explicit SoftRenderer(GuestMemory& memory) : memory_(memory) {}

    void draw(const State& s, Prim prim, const std::vector<Vertex>& vertices) override;
    void transfer(const State& s) override;

private:
    void configure(const State& s);
    ClipVertex transform(const State& s, const Vertex& v) const;
    ScreenVertex project(const ClipVertex& v) const;
    void lighting(Vec3 world_pos, Vec3 world_normal, const Vertex& v, float color[4], float spec[3]) const;

    void triangle(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c);
    void raster_triangle(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c, const ScreenVertex& provoking);
    void sprite(const ClipVertex& a, const ClipVertex& b);
    void line(const ClipVertex& a, const ClipVertex& b);
    void point(const ClipVertex& a);
    void pick_level(float texels_per_pixel, int& level, bool& linear) const;
    void pixel(int x, int y, int z, const float color[4], const float spec[3], float fog, float u, float v,
               int level, bool linear);

    GuestMemory& memory_;
    Setup d_;
    std::vector<ClipVertex> clip_;
};

std::unique_ptr<ge::Renderer> make_soft_renderer_impl(GuestMemory& memory) {
    return std::make_unique<SoftRenderer>(memory);
}

void SoftRenderer::configure(const State& s) {
    Setup& d = d_;
    d.through = s.format.through;
    d.fb_format = static_cast<int>(s.param(ge::FRAMEBUFPIXFORMAT) & 3);
    d.bpp = d.fb_format == 3 ? 4 : 2;
    d.fb_stride = std::max<int>(static_cast<int>(s.param(ge::FRAMEBUFWIDTH) & 0x7FC), 4);
    d.z_stride = std::max<int>(static_cast<int>(s.param(ge::ZBUFWIDTH) & 0x7FC), 4);
    d.fb = memory_.ptr(0x04000000 | (s.param(ge::FRAMEBUFPTR) & 0x1FFFF0));
    d.zb = memory_.ptr(0x04000000 | (s.param(ge::ZBUFPTR) & 0x1FFFF0));
    const std::uint32_t sc1 = s.param(ge::SCISSOR1), sc2 = s.param(ge::SCISSOR2);
    d.sx1 = static_cast<int>(sc1 & 0x3FF);
    d.sy1 = static_cast<int>((sc1 >> 10) & 0x3FF);
    d.sx2 = std::min(static_cast<int>(sc2 & 0x3FF), d.fb_stride - 1);
    d.sy2 = std::min(static_cast<int>((sc2 >> 10) & 0x3FF), 511);

    const std::uint32_t clear = s.param(ge::CLEARMODE);
    d.clear = clear & 1;
    d.clear_color = (clear >> 8) & 1;
    d.clear_stencil = (clear >> 9) & 1;
    d.clear_depth = (clear >> 10) & 1;

    d.texture = s.on(ge::TEXTUREMAPENABLE) && !d.clear;
    if (d.texture) {
        d.sampler.setup(s, memory_);
        const std::uint32_t func = s.param(ge::TEXFUNC);
        d.tex_func = static_cast<int>(func & 7);
        d.tex_alpha = (func >> 8) & 1;
        d.tex_double = (func >> 16) & 1;
        const std::uint32_t env = s.param(ge::TEXENVCOLOR);
        for (int i = 0; i < 3; ++i) d.env[i] = static_cast<int>((env >> (i * 8)) & 0xFF);
        const std::uint32_t filter = s.param(ge::TEXFILTER);
        d.min_linear = filter & 1;
        d.mipmap = (filter & 4) != 0;
        d.mag_linear = (filter >> 8) & 1;
        const std::uint32_t level = s.param(ge::TEXLEVEL);
        d.lod_mode = static_cast<int>(level & 3);
        d.lod_bias = static_cast<float>(static_cast<std::int8_t>((level >> 16) & 0xFF)) / 16.0f;
    }

    d.fog = s.on(ge::FOGENABLE) && !d.through && !d.clear;
    const std::uint32_t fog_color = s.param(ge::FOGCOLOR);
    for (int i = 0; i < 3; ++i) d.fog_color[i] = static_cast<int>((fog_color >> (i * 8)) & 0xFF);
    d.fog_end = s.flt(ge::FOG1);
    d.fog_slope = s.flt(ge::FOG2);

    d.alpha_test = s.on(ge::ALPHATESTENABLE);
    const std::uint32_t at = s.param(ge::ALPHATEST);
    d.at_func = static_cast<int>(at & 7);
    d.at_ref = static_cast<int>((at >> 8) & 0xFF);
    d.at_mask = static_cast<int>((at >> 16) & 0xFF);
    d.color_test = s.on(ge::COLORTESTENABLE);
    d.ct_func = static_cast<int>(s.param(ge::COLORTEST) & 3);
    d.ct_ref = s.param(ge::COLORREF);
    d.ct_mask = s.param(ge::COLORTESTMASK);
    d.stencil_test = s.on(ge::STENCILTESTENABLE);
    const std::uint32_t st = s.param(ge::STENCILTEST), so = s.param(ge::STENCILOP);
    d.st_func = static_cast<int>(st & 7);
    d.st_ref = static_cast<int>((st >> 8) & 0xFF);
    d.st_mask = static_cast<int>((st >> 16) & 0xFF);
    d.op_sfail = static_cast<int>(so & 7);
    d.op_zfail = static_cast<int>((so >> 8) & 7);
    d.op_zpass = static_cast<int>((so >> 16) & 7);
    d.depth_test = s.on(ge::ZTESTENABLE);
    d.z_func = static_cast<int>(s.param(ge::ZTEST) & 7);
    d.z_write = d.depth_test && !s.on(ge::ZWRITEDISABLE);
    d.min_z = static_cast<int>(s.param(ge::MINZ) & 0xFFFF);
    d.max_z = static_cast<int>(s.param(ge::MAXZ) & 0xFFFF);
    d.depth_range = !d.through && d.max_z != 0;

    d.blend = s.on(ge::ALPHABLENDENABLE);
    const std::uint32_t bm = s.param(ge::BLENDMODE);
    d.blend_a = static_cast<int>(bm & 15);
    d.blend_b = static_cast<int>((bm >> 4) & 15);
    d.blend_eq = static_cast<int>((bm >> 8) & 7);
    for (int i = 0; i < 3; ++i) {
        d.fix_a[i] = static_cast<int>((s.param(ge::BLENDFIXEDA) >> (i * 8)) & 0xFF);
        d.fix_b[i] = static_cast<int>((s.param(ge::BLENDFIXEDB) >> (i * 8)) & 0xFF);
    }
    d.dither = s.on(ge::DITHERENABLE);
    for (int y = 0; y < 4; ++y) {
        const std::uint32_t row = s.param(static_cast<std::uint8_t>(ge::DITHER0 + y));
        for (int x = 0; x < 4; ++x) {
            const int v = static_cast<int>((row >> (x * 4)) & 15);
            d.dither_matrix[y][x] = v >= 8 ? v - 16 : v;
        }
    }
    d.logic = s.on(ge::LOGICOPENABLE);
    d.logic_op = static_cast<int>(s.param(ge::LOGICOP) & 15);
    d.write_mask = ~((s.param(ge::MASKRGB) & 0xFFFFFF) | ((s.param(ge::MASKALPHA) & 0xFF) << 24));
    d.gouraud = s.param(ge::SHADEMODE) & 1;
    d.cull = s.on(ge::CULLFACEENABLE) && !d.through;
    d.cull_mode = static_cast<int>(s.param(ge::CULL) & 1);

    // Vertex processing state.
    d.lighting = s.on(ge::LIGHTINGENABLE) && !d.through;
    d.separate_specular = s.param(ge::LIGHTMODE) & 1;
    d.emissive = color24(s.param(ge::MATERIALEMISSIVE));
    d.mat_ambient = color24(s.param(ge::MATERIALAMBIENT));
    d.mat_alpha = static_cast<float>(s.param(ge::MATERIALALPHA) & 0xFF) / 255.0f;
    d.mat_diffuse = color24(s.param(ge::MATERIALDIFFUSE));
    d.mat_specular = color24(s.param(ge::MATERIALSPECULAR));
    d.specular_coef = s.flt(ge::MATERIALSPECULARCOEF);
    d.ambient = color24(s.param(ge::AMBIENTCOLOR));
    d.ambient_alpha = static_cast<float>(s.param(ge::AMBIENTALPHA) & 0xFF) / 255.0f;
    d.material_update = s.param(ge::MATERIALUPDATE) & 7;
    d.reverse_normal = s.on(ge::REVERSENORMAL);
    for (int i = 0; i < 4; ++i) {
        Light& l = d.lights[i];
        l.on = d.lighting && s.on(static_cast<std::uint8_t>(ge::LIGHTENABLE0 + i));
        if (!l.on) continue;
        const std::uint32_t type = s.param(static_cast<std::uint8_t>(ge::LIGHTTYPE0 + i));
        l.kind = static_cast<int>(type & 3);
        l.type = static_cast<int>((type >> 8) & 3);
        l.pos = vec_reg(s, ge::LIGHTPOSITION0 + 3 * i);
        l.dir = normalize(vec_reg(s, ge::LIGHTDIRECTION0 + 3 * i));
        l.att = vec_reg(s, ge::LIGHTATTENUATION0 + 3 * i);
        l.exponent = s.flt(static_cast<std::uint8_t>(ge::LIGHTSPOTEXPONENT0 + i));
        l.cutoff = s.flt(static_cast<std::uint8_t>(ge::LIGHTSPOTCUTOFF0 + i));
        l.ambient = color24(s.param(static_cast<std::uint8_t>(ge::LIGHTCOLOR0 + 3 * i)));
        l.diffuse = color24(s.param(static_cast<std::uint8_t>(ge::LIGHTCOLOR0 + 3 * i + 1)));
        l.specular = color24(s.param(static_cast<std::uint8_t>(ge::LIGHTCOLOR0 + 3 * i + 2)));
    }
    const std::uint32_t map = s.param(ge::TEXMAPMODE), shade = s.param(ge::TEXSHADELS);
    d.map_mode = static_cast<int>(map & 3);
    d.proj_source = static_cast<int>((map >> 8) & 3);
    d.shade_u = static_cast<int>(shade & 3);
    d.shade_v = static_cast<int>((shade >> 8) & 3);
    d.tex_scale[0] = s.flt(ge::TEXSCALEU);
    d.tex_scale[1] = s.flt(ge::TEXSCALEV);
    d.tex_offset[0] = s.flt(ge::TEXOFFSETU);
    d.tex_offset[1] = s.flt(ge::TEXOFFSETV);
    for (int i = 0; i < 3; ++i) {
        d.vp_scale[i] = s.flt(static_cast<std::uint8_t>(ge::VIEWPORTXSCALE + i));
        d.vp_center[i] = s.flt(static_cast<std::uint8_t>(ge::VIEWPORTXCENTER + i));
    }
    d.offset_x = static_cast<float>(s.param(ge::OFFSETX) & 0xFFFF) / 16.0f;
    d.offset_y = static_cast<float>(s.param(ge::OFFSETY) & 0xFFFF) / 16.0f;
}

void SoftRenderer::lighting(Vec3 wp, Vec3 wn, const Vertex& v, float color[4], float spec[3]) const {
    const Setup& d = d_;
    const bool has_color = v.color[0] >= 0.0f;
    const Vec3 vc{v.color[0], v.color[1], v.color[2]};
    const Vec3 m_ambient = has_color && (d.material_update & 1) ? vc : d.mat_ambient;
    const float m_alpha = has_color && (d.material_update & 1) ? v.color[3] : d.mat_alpha;
    const Vec3 m_diffuse = has_color && (d.material_update & 2) ? vc : d.mat_diffuse;
    const Vec3 m_specular = has_color && (d.material_update & 4) ? vc : d.mat_specular;
    const auto mul = [](Vec3 a, Vec3 b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; };

    Vec3 n = normalize(d.reverse_normal ? wn * -1.0f : wn);
    Vec3 diffuse_sum, specular_sum;
    Vec3 sum = d.emissive + mul(d.ambient, m_ambient);
    for (const Light& l : d.lights) {
        if (!l.on) continue;
        Vec3 to_light;
        float att = 1.0f;
        if (l.type == 0) {
            to_light = normalize(l.pos);
        } else {
            to_light = l.pos - wp;
            const float dist = std::sqrt(dot(to_light, to_light));
            to_light = dist > 0.0f ? to_light * (1.0f / dist) : to_light;
            const float denom = l.att.x + l.att.y * dist + l.att.z * dist * dist;
            att = denom > 0.0f ? std::min(1.0f / denom, 1.0f) : 1.0f;
            if (l.type == 2) {
                const float spot = dot(to_light * -1.0f, l.dir);
                att = spot >= l.cutoff ? att * std::pow(std::max(spot, 0.0f), l.exponent) : 0.0f;
            }
        }
        float ndotl = dot(n, to_light);
        if (l.kind == 2) ndotl = ndotl > 0.0f ? std::pow(ndotl, d.specular_coef) : 0.0f;
        sum = sum + mul(l.ambient, m_ambient) * att;
        if (ndotl > 0.0f) {
            diffuse_sum = diffuse_sum + mul(l.diffuse, m_diffuse) * (att * ndotl);
            if (l.kind == 1) {
                const Vec3 h = normalize(to_light + Vec3{0, 0, 1});
                const float ndoth = std::max(dot(n, h), 0.0f);
                specular_sum = specular_sum + mul(l.specular, m_specular) * (att * std::pow(ndoth, d.specular_coef));
            }
        }
    }
    sum = sum + diffuse_sum;
    if (!d.separate_specular) {
        sum = sum + specular_sum;
        specular_sum = {};
    }
    color[0] = std::clamp(sum.x, 0.0f, 1.0f) * 255.0f;
    color[1] = std::clamp(sum.y, 0.0f, 1.0f) * 255.0f;
    color[2] = std::clamp(sum.z, 0.0f, 1.0f) * 255.0f;
    color[3] = std::clamp(d.ambient_alpha * m_alpha, 0.0f, 1.0f) * 255.0f;
    spec[0] = std::clamp(specular_sum.x, 0.0f, 1.0f) * 255.0f;
    spec[1] = std::clamp(specular_sum.y, 0.0f, 1.0f) * 255.0f;
    spec[2] = std::clamp(specular_sum.z, 0.0f, 1.0f) * 255.0f;
}

ClipVertex SoftRenderer::transform(const State& s, const Vertex& v) const {
    const Setup& d = d_;
    ClipVertex out{};
    out.fog = 1.0f;
    if (d.through) {
        out.clip[0] = v.pos[0];
        out.clip[1] = v.pos[1];
        out.clip[2] = v.pos[2];
        out.clip[3] = 1.0f;
        const float w = d.texture ? static_cast<float>(d.sampler.width()) : 1.0f;
        const float h = d.texture ? static_cast<float>(d.sampler.height()) : 1.0f;
        out.s = v.uv[0] / w;
        out.t = v.uv[1] / h;
        out.q = 1.0f;
        if (v.color[0] >= 0.0f) {
            for (int i = 0; i < 4; ++i) out.color[i] = v.color[i] * 255.0f;
        } else {
            out.color[0] = d.mat_ambient.x * 255.0f;
            out.color[1] = d.mat_ambient.y * 255.0f;
            out.color[2] = d.mat_ambient.z * 255.0f;
            out.color[3] = d.mat_alpha * 255.0f;
        }
        return out;
    }

    Vec3 pos{v.pos[0], v.pos[1], v.pos[2]}, normal{v.normal[0], v.normal[1], v.normal[2]};
    if (s.format.weight) {  // skinning: blend the bone transforms by the vertex weights
        Vec3 p, n;
        for (int i = 0; i < s.format.weights; ++i) {
            if (v.weights[i] == 0.0f) continue;
            p = p + mul43(s.bones[i], pos) * v.weights[i];
            n = n + mul43(s.bones[i], normal, 0.0f) * v.weights[i];
        }
        pos = p;
        normal = n;
    }
    const Vec3 world = mul43(s.world, pos);
    const Vec3 world_normal = mul43(s.world, normal, 0.0f);
    const Vec3 view = mul43(s.view, world);
    mul44(s.proj, view, out.clip);

    if (d.lighting) {
        lighting(world, world_normal, v, out.color, out.spec);
    } else if (v.color[0] >= 0.0f) {
        for (int i = 0; i < 4; ++i) out.color[i] = v.color[i] * 255.0f;
    } else {
        out.color[0] = d.mat_ambient.x * 255.0f;
        out.color[1] = d.mat_ambient.y * 255.0f;
        out.color[2] = d.mat_ambient.z * 255.0f;
        out.color[3] = d.mat_alpha * 255.0f;
    }

    out.q = 1.0f;
    switch (d.map_mode) {
        case 1: {  // projection through the texture generation matrix
            Vec3 src;
            switch (d.proj_source) {
                case 0: src = pos; break;
                case 1: src = {v.uv[0], v.uv[1], 0.0f}; break;
                case 2: src = normalize(normal); break;
                default: src = normal; break;
            }
            const Vec3 t = mul43(s.tgen, src);
            out.s = t.x;
            out.t = t.y;
            out.q = t.z;
            break;
        }
        case 2: {  // environment map from two light directions
            const Vec3 n = normalize(world_normal);
            out.s = (1.0f + dot(normalize(vec_reg(s, ge::LIGHTPOSITION0 + 3 * d.shade_u)), n)) * 0.5f;
            out.t = (1.0f + dot(normalize(vec_reg(s, ge::LIGHTPOSITION0 + 3 * d.shade_v)), n)) * 0.5f;
            break;
        }
        default:
            out.s = v.uv[0] * d.tex_scale[0] + d.tex_offset[0];
            out.t = v.uv[1] * d.tex_scale[1] + d.tex_offset[1];
            break;
    }
    if (d.fog) out.fog = std::clamp((view.z + d.fog_end) * d.fog_slope, 0.0f, 1.0f);
    return out;
}

ScreenVertex SoftRenderer::project(const ClipVertex& v) const {
    ScreenVertex o{};
    if (d_.through) {
        o.x = v.clip[0];
        o.y = v.clip[1];
        o.z = v.clip[2];
        o.iw = 1.0f;
    } else {
        o.iw = 1.0f / v.clip[3];
        o.x = v.clip[0] * o.iw * d_.vp_scale[0] + d_.vp_center[0] - d_.offset_x;
        o.y = v.clip[1] * o.iw * d_.vp_scale[1] + d_.vp_center[1] - d_.offset_y;
        o.z = std::clamp(v.clip[2] * o.iw * d_.vp_scale[2] + d_.vp_center[2], 0.0f, 65535.0f);
    }
    o.s = v.s * o.iw;
    o.t = v.t * o.iw;
    o.q = v.q * o.iw;
    std::copy(std::begin(v.color), std::end(v.color), o.color);
    std::copy(std::begin(v.spec), std::end(v.spec), o.spec);
    o.fog = v.fog;
    return o;
}

void SoftRenderer::draw(const State& s, Prim prim, const std::vector<Vertex>& vertices) {
    configure(s);
    clip_.resize(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) clip_[i] = transform(s, vertices[i]);
    const auto& v = clip_;
    const std::size_t n = v.size();
    switch (prim) {
        case Prim::Points:
            for (std::size_t i = 0; i < n; ++i) point(v[i]);
            break;
        case Prim::Lines:
            for (std::size_t i = 0; i + 1 < n; i += 2) line(v[i], v[i + 1]);
            break;
        case Prim::LineStrip:
            for (std::size_t i = 0; i + 1 < n; ++i) line(v[i], v[i + 1]);
            break;
        case Prim::Triangles:
            for (std::size_t i = 0; i + 2 < n; i += 3) triangle(v[i], v[i + 1], v[i + 2]);
            break;
        case Prim::TriangleStrip:
            for (std::size_t i = 0; i + 2 < n; ++i) {
                if (i & 1) triangle(v[i + 1], v[i], v[i + 2]);
                else triangle(v[i], v[i + 1], v[i + 2]);
            }
            break;
        case Prim::TriangleFan:
            for (std::size_t i = 1; i + 1 < n; ++i) triangle(v[0], v[i], v[i + 1]);
            break;
        case Prim::Sprites:
            for (std::size_t i = 0; i + 1 < n; i += 2) sprite(v[i], v[i + 1]);
            break;
    }
}

void SoftRenderer::triangle(const ClipVertex& a, const ClipVertex& b, const ClipVertex& c) {
    if (d_.through) {
        const ScreenVertex sc = project(c);
        raster_triangle(project(a), project(b), sc, sc);
        return;
    }
    // Clip against the near plane (z >= -w); the screen-space rasterizer handles the other planes via scissor.
    ClipVertex in[8] = {a, b, c}, out[8];
    int count = 3;
    {
        int o = 0;
        for (int i = 0; i < count; ++i) {
            const ClipVertex& p = in[i];
            const ClipVertex& q = in[(i + 1) % count];
            const float dp = p.clip[2] + p.clip[3], dq = q.clip[2] + q.clip[3];
            if (dp >= 0.0f) out[o++] = p;
            if ((dp >= 0.0f) != (dq >= 0.0f)) {
                const float t = dp / (dp - dq);
                ClipVertex r;
                const auto lerp = [t](float x, float y) { return x + (y - x) * t; };
                for (int k = 0; k < 4; ++k) r.clip[k] = lerp(p.clip[k], q.clip[k]);
                r.s = lerp(p.s, q.s);
                r.t = lerp(p.t, q.t);
                r.q = lerp(p.q, q.q);
                for (int k = 0; k < 4; ++k) r.color[k] = lerp(p.color[k], q.color[k]);
                for (int k = 0; k < 3; ++k) r.spec[k] = lerp(p.spec[k], q.spec[k]);
                r.fog = lerp(p.fog, q.fog);
                out[o++] = r;
            }
        }
        count = o;
    }
    if (count < 3) return;
    for (int i = 0; i < count; ++i)
        if (out[i].clip[3] <= 1e-6f) return;  // degenerate after clipping
    ScreenVertex sv[8];
    for (int i = 0; i < count; ++i) sv[i] = project(out[i]);
    ScreenVertex provoking{};  // flat shading takes the last vertex's colors, even if it was clipped away
    std::copy(std::begin(c.color), std::end(c.color), provoking.color);
    std::copy(std::begin(c.spec), std::end(c.spec), provoking.spec);
    for (int i = 1; i + 1 < count; ++i) raster_triangle(sv[0], sv[i], sv[i + 1], provoking);
}

void SoftRenderer::pick_level(float texels_per_pixel, int& level, bool& linear) const {
    float lod = texels_per_pixel > 0.0f ? std::log2(texels_per_pixel) : 0.0f;
    lod = d_.lod_mode == 1 ? d_.lod_bias : lod + d_.lod_bias;
    if (lod > 0.0f) {
        linear = d_.min_linear;
        level = d_.mipmap ? std::clamp(static_cast<int>(lod + 0.5f), 0, d_.sampler.levels() - 1) : 0;
    } else {
        linear = d_.mag_linear;
        level = 0;
    }
}

void SoftRenderer::raster_triangle(const ScreenVertex& a0, const ScreenVertex& b0, const ScreenVertex& c0,
                                   const ScreenVertex& provoking) {
    float area = (b0.x - a0.x) * (c0.y - a0.y) - (c0.x - a0.x) * (b0.y - a0.y);
    if (area == 0.0f || !std::isfinite(area)) return;
    // Screen y points down, so positive area is clockwise on screen. CULL=1 keeps clockwise faces.
    if (d_.cull && ((d_.cull_mode == 1) ? area < 0.0f : area > 0.0f)) return;
    const ScreenVertex* a = &a0;
    const ScreenVertex* b = &b0;
    const ScreenVertex* c = &c0;
    if (area < 0.0f) {
        std::swap(b, c);
        area = -area;
    }

    int level = 0;
    bool linear = false;
    if (d_.texture) {
        const float w = static_cast<float>(d_.sampler.width()), h = static_cast<float>(d_.sampler.height());
        const float ua = a->s / a->q * w, va = a->t / a->q * h;
        const float ub = b->s / b->q * w, vb = b->t / b->q * h;
        const float uc = c->s / c->q * w, vc = c->t / c->q * h;
        const float tex_area = std::fabs((ub - ua) * (vc - va) - (uc - ua) * (vb - va));
        pick_level(std::sqrt(tex_area / area), level, linear);
    }

    const int x0 = std::max(d_.sx1, static_cast<int>(std::floor(std::min({a->x, b->x, c->x}))));
    const int x1 = std::min(d_.sx2, static_cast<int>(std::ceil(std::max({a->x, b->x, c->x}))));
    const int y0 = std::max(d_.sy1, static_cast<int>(std::floor(std::min({a->y, b->y, c->y}))));
    const int y1 = std::min(d_.sy2, static_cast<int>(std::ceil(std::max({a->y, b->y, c->y}))));
    if (x0 > x1 || y0 > y1) return;

    // Edge (p -> q) owns its pixels-on-the-line when it is a top or left edge.
    const auto top_left = [](const ScreenVertex* p, const ScreenVertex* q) {
        const float dy = q->y - p->y, dx = q->x - p->x;
        return dy < 0.0f || (dy == 0.0f && dx > 0.0f);
    };
    const bool tl0 = top_left(b, c), tl1 = top_left(c, a), tl2 = top_left(a, b);
    const float inv_area = 1.0f / area;
    const bool flat = !d_.gouraud;
    float color[4], spec[3];
    for (int y = y0; y <= y1; ++y) {
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = x0; x <= x1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float w0 = (c->x - b->x) * (py - b->y) - (c->y - b->y) * (px - b->x);
            const float w1 = (a->x - c->x) * (py - c->y) - (a->y - c->y) * (px - c->x);
            const float w2 = (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;
            if ((w0 == 0.0f && !tl0) || (w1 == 0.0f && !tl1) || (w2 == 0.0f && !tl2)) continue;
            const float l0 = w0 * inv_area, l1 = w1 * inv_area, l2 = w2 * inv_area;
            const int z = static_cast<int>(l0 * a->z + l1 * b->z + l2 * c->z);
            if (flat) {
                std::copy(std::begin(provoking.color), std::end(provoking.color), color);
                std::copy(std::begin(provoking.spec), std::end(provoking.spec), spec);
            } else {
                for (int k = 0; k < 4; ++k) color[k] = l0 * a->color[k] + l1 * b->color[k] + l2 * c->color[k];
                for (int k = 0; k < 3; ++k) spec[k] = l0 * a->spec[k] + l1 * b->spec[k] + l2 * c->spec[k];
            }
            float u = 0, v = 0;
            if (d_.texture) {
                const float q = l0 * a->q + l1 * b->q + l2 * c->q;
                u = (l0 * a->s + l1 * b->s + l2 * c->s) / q;
                v = (l0 * a->t + l1 * b->t + l2 * c->t) / q;
            }
            const float fog = l0 * a->fog + l1 * b->fog + l2 * c->fog;
            pixel(x, y, z, color, spec, fog, u, v, level, linear);
        }
    }
}

void SoftRenderer::sprite(const ClipVertex& ca, const ClipVertex& cb) {
    const ScreenVertex a = project(ca), b = project(cb);
    float x0 = a.x, x1 = b.x, y0 = a.y, y1 = b.y;
    float u0 = a.s / a.q, u1 = b.s / b.q, v0 = a.t / a.q, v1 = b.t / b.q;
    if (x0 > x1) {
        std::swap(x0, x1);
        std::swap(u0, u1);
    }
    if (y0 > y1) {
        std::swap(y0, y1);
        std::swap(v0, v1);
    }
    if (x1 - x0 <= 0.0f || y1 - y0 <= 0.0f) return;
    const int ix0 = std::max(d_.sx1, static_cast<int>(std::ceil(x0 - 0.5f)));
    const int ix1 = std::min(d_.sx2, static_cast<int>(std::ceil(x1 - 0.5f)) - 1);
    const int iy0 = std::max(d_.sy1, static_cast<int>(std::ceil(y0 - 0.5f)));
    const int iy1 = std::min(d_.sy2, static_cast<int>(std::ceil(y1 - 0.5f)) - 1);
    const float du = (u1 - u0) / (x1 - x0), dv = (v1 - v0) / (y1 - y0);
    int level = 0;
    bool linear = false;
    if (d_.texture)
        pick_level(std::max(std::fabs(du) * static_cast<float>(d_.sampler.width()),
                            std::fabs(dv) * static_cast<float>(d_.sampler.height())),
                   level, linear);
    const int z = static_cast<int>(b.z);
    for (int y = iy0; y <= iy1; ++y) {
        const float v = v0 + (static_cast<float>(y) + 0.5f - y0) * dv;
        for (int x = ix0; x <= ix1; ++x) {
            const float u = u0 + (static_cast<float>(x) + 0.5f - x0) * du;
            pixel(x, y, z, b.color, b.spec, b.fog, u, v, level, linear);
        }
    }
}

void SoftRenderer::line(const ClipVertex& ca, const ClipVertex& cb) {
    if (!d_.through && (ca.clip[2] + ca.clip[3] < 0.0f || cb.clip[2] + cb.clip[3] < 0.0f)) return;
    const ScreenVertex a = project(ca), b = project(cb);
    const float dx = b.x - a.x, dy = b.y - a.y;
    const int steps = static_cast<int>(std::ceil(std::max(std::fabs(dx), std::fabs(dy))));
    if (steps <= 0 || steps > 4096) return;
    float color[4], spec[3];
    for (int i = 0; i < steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const int x = static_cast<int>(std::floor(a.x + dx * t)), y = static_cast<int>(std::floor(a.y + dy * t));
        if (x < d_.sx1 || x > d_.sx2 || y < d_.sy1 || y > d_.sy2) continue;
        for (int k = 0; k < 4; ++k) color[k] = d_.gouraud ? a.color[k] + (b.color[k] - a.color[k]) * t : b.color[k];
        for (int k = 0; k < 3; ++k) spec[k] = a.spec[k] + (b.spec[k] - a.spec[k]) * t;
        const float q = a.q + (b.q - a.q) * t;
        const float u = (a.s + (b.s - a.s) * t) / q, v = (a.t + (b.t - a.t) * t) / q;
        pixel(x, y, static_cast<int>(a.z + (b.z - a.z) * t), color, spec, a.fog + (b.fog - a.fog) * t, u, v, 0,
              d_.mag_linear);
    }
}

void SoftRenderer::point(const ClipVertex& ca) {
    if (!d_.through && ca.clip[2] + ca.clip[3] < 0.0f) return;
    const ScreenVertex a = project(ca);
    const int x = static_cast<int>(std::floor(a.x)), y = static_cast<int>(std::floor(a.y));
    if (x < d_.sx1 || x > d_.sx2 || y < d_.sy1 || y > d_.sy2) return;
    pixel(x, y, static_cast<int>(a.z), a.color, a.spec, a.fog, a.s / a.q, a.t / a.q, 0, false);
}

void SoftRenderer::pixel(int x, int y, int z, const float color[4], const float spec[3], float fog, float u,
                         float v, int level, bool linear) {
    const Setup& d = d_;
    std::uint8_t* fp = d.fb + (static_cast<std::ptrdiff_t>(y) * d.fb_stride + x) * d.bpp;
    std::uint8_t* zp = d.zb + (static_cast<std::ptrdiff_t>(y) * d.z_stride + x) * 2;
    const std::uint32_t dst = read_pixel(fp, d.fb_format);
    int c[4];
    for (int i = 0; i < 4; ++i) c[i] = std::clamp(static_cast<int>(color[i] + 0.5f), 0, 255);

    if (d.clear) {
        std::uint32_t out = dst;
        if (d.clear_color) out = (out & 0xFF000000) | rgba(static_cast<std::uint32_t>(c[0]), static_cast<std::uint32_t>(c[1]), static_cast<std::uint32_t>(c[2]), 0);
        if (d.clear_stencil) out = (out & 0x00FFFFFF) | (static_cast<std::uint32_t>(c[3]) << 24);
        if (d.clear_depth) store16(zp, static_cast<std::uint32_t>(std::clamp(z, 0, 65535)));
        write_pixel(fp, d.fb_format, out);
        return;
    }

    if (d.texture) {
        const std::uint32_t t = d.sampler.sample(u, v, level, linear);
        const int tc[4] = {static_cast<int>(t & 0xFF), static_cast<int>((t >> 8) & 0xFF),
                           static_cast<int>((t >> 16) & 0xFF), static_cast<int>(t >> 24)};
        const auto mul = [](int a, int b) { return (a * b + 127) / 255; };
        switch (d.tex_func) {
            case 0:  // modulate
                for (int i = 0; i < 3; ++i) c[i] = mul(c[i], tc[i]);
                if (d.tex_alpha) c[3] = mul(c[3], tc[3]);
                break;
            case 1:  // decal
                for (int i = 0; i < 3; ++i) c[i] = d.tex_alpha ? (tc[i] * tc[3] + c[i] * (255 - tc[3]) + 127) / 255 : tc[i];
                break;
            case 2:  // blend with the environment color
                for (int i = 0; i < 3; ++i) c[i] = (c[i] * (255 - tc[i]) + d.env[i] * tc[i] + 127) / 255;
                if (d.tex_alpha) c[3] = mul(c[3], tc[3]);
                break;
            case 3:  // replace
                for (int i = 0; i < 3; ++i) c[i] = tc[i];
                if (d.tex_alpha) c[3] = tc[3];
                break;
            default:  // add
                for (int i = 0; i < 3; ++i) c[i] = std::min(c[i] + tc[i], 255);
                if (d.tex_alpha) c[3] = mul(c[3], tc[3]);
                break;
        }
        if (d.tex_double)
            for (int i = 0; i < 3; ++i) c[i] = std::min(c[i] * 2, 255);
    }
    for (int i = 0; i < 3; ++i) c[i] = std::min(c[i] + static_cast<int>(spec[i] + 0.5f), 255);
    if (d.fog) {
        const int f = static_cast<int>(fog * 255.0f + 0.5f);
        for (int i = 0; i < 3; ++i) c[i] = (c[i] * f + d.fog_color[i] * (255 - f) + 127) / 255;
    }

    if (d.alpha_test && !compare(d.at_func, c[3] & d.at_mask, d.at_ref & d.at_mask)) return;
    if (d.color_test) {
        const std::uint32_t rgb = rgba(static_cast<std::uint32_t>(c[0]), static_cast<std::uint32_t>(c[1]), static_cast<std::uint32_t>(c[2]), 0);
        const bool equal = (rgb & d.ct_mask) == (d.ct_ref & d.ct_mask);
        if (d.ct_func == 0 || (d.ct_func == 2 && !equal) || (d.ct_func == 3 && equal)) return;
    }
    z = std::clamp(z, 0, 65535);
    if (d.depth_range && (z < d.min_z || z > d.max_z)) return;

    int stencil = static_cast<int>(dst >> 24);
    const auto write_stencil_only = [&](int value) {
        const std::uint32_t out = (dst & 0x00FFFFFF) | (static_cast<std::uint32_t>(value) << 24);
        write_pixel(fp, d.fb_format, (out & d.write_mask) | (dst & ~d.write_mask));
    };
    if (d.stencil_test && !compare(d.st_func, d.st_ref & d.st_mask, stencil & d.st_mask)) {
        write_stencil_only(stencil_op(d.op_sfail, stencil, d.st_ref));
        return;
    }
    if (d.depth_test && !compare(d.z_func, z, load16(zp))) {
        if (d.stencil_test) write_stencil_only(stencil_op(d.op_zfail, stencil, d.st_ref));
        return;
    }
    if (d.stencil_test) stencil = stencil_op(d.op_zpass, stencil, d.st_ref);
    if (d.z_write) store16(zp, static_cast<std::uint32_t>(z));

    if (d.blend) {
        const int dc[3] = {static_cast<int>(dst & 0xFF), static_cast<int>((dst >> 8) & 0xFF),
                           static_cast<int>((dst >> 16) & 0xFF)};
        const int sa = c[3], da = static_cast<int>(dst >> 24);
        const auto factor = [&](int code, bool source, int ch) {
            switch (code) {
                case 0: return source ? dc[ch] : c[ch];
                case 1: return 255 - (source ? dc[ch] : c[ch]);
                case 2: return sa;
                case 3: return 255 - sa;
                case 4: return da;
                case 5: return 255 - da;
                case 6: return std::min(sa * 2, 255);
                case 7: return 255 - std::min(sa * 2, 255);
                case 8: return std::min(da * 2, 255);
                case 9: return 255 - std::min(da * 2, 255);
                default: return source ? d.fix_a[ch] : d.fix_b[ch];
            }
        };
        int out[3];
        for (int i = 0; i < 3; ++i) {
            const int s = c[i] * factor(d.blend_a, true, i), t = dc[i] * factor(d.blend_b, false, i);
            switch (d.blend_eq) {
                case 0: out[i] = (s + t + 127) / 255; break;
                case 1: out[i] = (s - t) / 255; break;
                case 2: out[i] = (t - s) / 255; break;
                case 3: out[i] = std::min(c[i], dc[i]); break;
                case 4: out[i] = std::max(c[i], dc[i]); break;
                default: out[i] = std::abs(c[i] - dc[i]); break;
            }
        }
        for (int i = 0; i < 3; ++i) c[i] = std::clamp(out[i], 0, 255);
    }
    if (d.dither) {
        const int k = d.dither_matrix[y & 3][x & 3];
        for (int i = 0; i < 3; ++i) c[i] = std::clamp(c[i] + k, 0, 255);
    }
    std::uint32_t out = rgba(static_cast<std::uint32_t>(c[0]), static_cast<std::uint32_t>(c[1]),
                             static_cast<std::uint32_t>(c[2]), static_cast<std::uint32_t>(stencil));
    if (d.logic) out = logic_op(d.logic_op, out, dst);
    write_pixel(fp, d.fb_format, (out & d.write_mask) | (dst & ~d.write_mask));
}

void SoftRenderer::transfer(const State& s) {
    const std::uint32_t src_w = s.param(ge::TRANSFERSRCW), dst_w = s.param(ge::TRANSFERDSTW);
    const std::uint32_t src = ge::vram_canonical(((src_w & 0xFF0000) << 8) | (s.param(ge::TRANSFERSRC) & 0xFFFFF0));
    const std::uint32_t dst = ge::vram_canonical(((dst_w & 0xFF0000) << 8) | (s.param(ge::TRANSFERDST) & 0xFFFFF0));
    const std::uint32_t src_stride = src_w & 0xFFF8, dst_stride = dst_w & 0xFFF8;
    const std::uint32_t sp = s.param(ge::TRANSFERSRCPOS), dp = s.param(ge::TRANSFERDSTPOS), sz = s.param(ge::TRANSFERSIZE);
    const std::uint32_t sx = sp & 0x3FF, sy = (sp >> 10) & 0x3FF, dx = dp & 0x3FF, dy = (dp >> 10) & 0x3FF;
    const std::uint32_t w = (sz & 0x3FF) + 1, h = ((sz >> 10) & 0x3FF) + 1;
    const std::uint32_t bpp = (s.param(ge::TRANSFERSTART) & 1) ? 4 : 2;
    for (std::uint32_t y = 0; y < h; ++y)
        std::memmove(memory_.ptr(dst + ((dy + y) * dst_stride + dx) * bpp),
                     memory_.ptr(src + ((sy + y) * src_stride + sx) * bpp), w * bpp);
}

}  // namespace

std::unique_ptr<ge::Renderer> make_soft_renderer(GuestMemory& memory) { return make_soft_renderer_impl(memory); }

}  // namespace ff7cc::rt::gpu
