#pragma once

// GE vertex processing shared by the software and Vulkan renderers: skinning, world/view/projection transforms,
// lighting, texture coordinate generation and fog, producing clip-space vertices.

#include <cmath>
#include <cstdint>

#include "ff7cc/runtime/ge.hpp"

namespace ff7cc::rt::gpu {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 normalize(Vec3 v) {
    const float l = std::sqrt(dot(v, v));
    return l > 0.0f ? v * (1.0f / l) : v;
}
// 4x3 matrices are stored column by column: x' = m0*x + m3*y + m6*z + m9*w.
inline Vec3 mul43(const float* m, Vec3 v, float w = 1.0f) {
    return {m[0] * v.x + m[3] * v.y + m[6] * v.z + m[9] * w, m[1] * v.x + m[4] * v.y + m[7] * v.z + m[10] * w,
            m[2] * v.x + m[5] * v.y + m[8] * v.z + m[11] * w};
}
inline void mul44(const float* m, Vec3 v, float out[4]) {
    for (int i = 0; i < 4; ++i) out[i] = m[i] * v.x + m[4 + i] * v.y + m[8 + i] * v.z + m[12 + i];
}
inline Vec3 color24(std::uint32_t p) {
    return {static_cast<float>(p & 0xFF) / 255.0f, static_cast<float>((p >> 8) & 0xFF) / 255.0f,
            static_cast<float>((p >> 16) & 0xFF) / 255.0f};
}
inline Vec3 vec_reg(const ge::State& s, int cmd) {
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

struct Light {
    bool on = false;
    int type = 0, kind = 0;  // type: 0 directional, 1 point, 2 spot; kind: 0 diffuse, 1 +specular, 2 powered diffuse
    Vec3 pos, dir, att, ambient, diffuse, specular;
    float exponent = 0, cutoff = 0;
};

class VertexPipeline {
public:
    // Captures the vertex state of a draw. Through-mode texture coordinates are normalized by the level-0
    // texture size; `fog` enables fog factors (non-through, non-clear draws with FOGENABLE).
    void configure(const ge::State& s, bool texture, int tex_width, int tex_height, bool fog);
    ClipVertex transform(const ge::State& s, const ge::Vertex& v) const;

    bool through = false;
    // Viewport mapping from clip space to drawing-space pixels (screen = ndc * scale + center - offset).
    float vp_scale[3] = {}, vp_center[3] = {};
    float offset_x = 0, offset_y = 0;

private:
    void lighting(Vec3 world_pos, Vec3 world_normal, const ge::Vertex& v, float color[4], float spec[3]) const;

    bool texture = false, fog = false;
    float tex_width = 1, tex_height = 1;
    bool lighting_on = false, separate_specular = false;
    Light lights[4];
    Vec3 emissive, mat_ambient, mat_diffuse, mat_specular, ambient;
    float mat_alpha = 1, ambient_alpha = 1, specular_coef = 0;
    std::uint32_t material_update = 0;
    bool reverse_normal = false;
    int map_mode = 0, proj_source = 0, shade_u = 0, shade_v = 0;
    float tex_scale[2] = {1, 1}, tex_offset[2] = {0, 0};
    float fog_end = 0, fog_slope = 0;
    // Per draw: world, view and projection combined (clip = wvp * pos, 4x4 column major), the view-space z row
    // (fog), and whether the world-space position / the normal are needed at all (lighting, texture generation).
    float wvp[16] = {};
    float view_z[4] = {};
    bool need_world = false, need_normal = false;
};

}  // namespace ff7cc::rt::gpu
