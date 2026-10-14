#include "gpu/vertex_pipeline.hpp"

#include <algorithm>

namespace ff7cc::rt::gpu {

using ge::State;
using ge::Vertex;

void VertexPipeline::configure(const State& s, bool texture_on, int tex_w, int tex_h, bool fog_on) {
    through = s.format.through;
    texture = texture_on;
    tex_width = static_cast<float>(tex_w);
    tex_height = static_cast<float>(tex_h);
    fog = fog_on;
    fog_end = s.flt(ge::FOG1);
    fog_slope = s.flt(ge::FOG2);
    lighting_on = s.on(ge::LIGHTINGENABLE) && !through;
    separate_specular = s.param(ge::LIGHTMODE) & 1;
    emissive = color24(s.param(ge::MATERIALEMISSIVE));
    mat_ambient = color24(s.param(ge::MATERIALAMBIENT));
    mat_alpha = static_cast<float>(s.param(ge::MATERIALALPHA) & 0xFF) / 255.0f;
    mat_diffuse = color24(s.param(ge::MATERIALDIFFUSE));
    mat_specular = color24(s.param(ge::MATERIALSPECULAR));
    specular_coef = s.flt(ge::MATERIALSPECULARCOEF);
    ambient = color24(s.param(ge::AMBIENTCOLOR));
    ambient_alpha = static_cast<float>(s.param(ge::AMBIENTALPHA) & 0xFF) / 255.0f;
    material_update = s.param(ge::MATERIALUPDATE) & 7;
    reverse_normal = s.on(ge::REVERSENORMAL);
    for (int i = 0; i < 4; ++i) {
        Light& l = lights[i];
        l.on = lighting_on && s.on(static_cast<std::uint8_t>(ge::LIGHTENABLE0 + i));
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
    map_mode = static_cast<int>(map & 3);
    proj_source = static_cast<int>((map >> 8) & 3);
    shade_u = static_cast<int>(shade & 3);
    shade_v = static_cast<int>((shade >> 8) & 3);
    tex_scale[0] = s.flt(ge::TEXSCALEU);
    tex_scale[1] = s.flt(ge::TEXSCALEV);
    tex_offset[0] = s.flt(ge::TEXOFFSETU);
    tex_offset[1] = s.flt(ge::TEXOFFSETV);
    for (int i = 0; i < 3; ++i) {
        vp_scale[i] = s.flt(static_cast<std::uint8_t>(ge::VIEWPORTXSCALE + i));
        vp_center[i] = s.flt(static_cast<std::uint8_t>(ge::VIEWPORTXCENTER + i));
    }
    offset_x = static_cast<float>(s.param(ge::OFFSETX) & 0xFFFF) / 16.0f;
    offset_y = static_cast<float>(s.param(ge::OFFSETY) & 0xFFFF) / 16.0f;
}

void VertexPipeline::lighting(Vec3 wp, Vec3 wn, const Vertex& v, float color[4], float spec[3]) const {
    const VertexPipeline& d = *this;
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

ClipVertex VertexPipeline::transform(const State& s, const Vertex& v) const {
    const VertexPipeline& d = *this;
    ClipVertex out{};
    out.fog = 1.0f;
    if (d.through) {
        out.clip[0] = v.pos[0];
        out.clip[1] = v.pos[1];
        out.clip[2] = v.pos[2];
        out.clip[3] = 1.0f;
        const float w = d.texture ? d.tex_width : 1.0f;
        const float h = d.texture ? d.tex_height : 1.0f;
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

    if (d.lighting_on) {
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

}  // namespace ff7cc::rt::gpu
