// GE fixed-function pipeline for the Vulkan renderer. Vertices arrive transformed and lit (gpu/vertex_pipeline):
// this shader does texturing, the texture function, color doubling, separate specular, fog, and the alpha and
// color tests. Blending, depth, stencil, logic op and write masks are fixed-function Vulkan state.
//
// Per-pipeline features are specialization constants, so every pipeline only contains the code its GE state
// needs (cheap on mobile GPUs).

[[vk::constant_id(0)]] const bool kTexture = false;
[[vk::constant_id(1)]] const int kTexFunc = 0;          // 0 modulate, 1 decal, 2 blend, 3 replace, 4 add
[[vk::constant_id(2)]] const bool kTexAlpha = false;    // texture alpha participates (RGBA vs RGB)
[[vk::constant_id(3)]] const bool kColorDouble = false;
[[vk::constant_id(4)]] const bool kFog = false;
[[vk::constant_id(5)]] const int kAlphaTest = 1;        // GE compare function; 1 = always
[[vk::constant_id(6)]] const int kColorTest = 1;        // 0 never, 1 always, 2 equal, 3 not equal
[[vk::constant_id(7)]] const bool kClearMode = false;
[[vk::constant_id(8)]] const bool kFixedSourceBlend = false;  // multiply by BLENDFIXEDA (Vulkan has one constant)

struct Constants {
    float4 env_color;      // texture environment color (blend function)
    float4 fog_color;
    float4 fixed_a;        // BLENDFIXEDA, applied in the shader when kFixedSourceBlend
    uint alpha_ref, alpha_mask;
    uint color_ref, color_mask;
};
[[vk::push_constant]] ConstantBuffer<Constants> pc;

[[vk::binding(0, 0)]] Sampler2D tex;

struct VertexInput {
    [[vk::location(0)]] float4 position;  // Vulkan clip space
    [[vk::location(1)]] float3 uvq;       // projective texture coordinates
    [[vk::location(2)]] float4 color;     // primary color
    [[vk::location(3)]] float4 spec_fog;  // separate specular (rgb) and fog factor (a, 1 = no fog)
};

struct Varyings {
    [[vk::location(0)]] float3 uvq;
    [[vk::location(1)]] float4 color;
    [[vk::location(2)]] float4 spec_fog;
};

struct VertexOutput {
    float4 position : SV_Position;
    Varyings varyings;
    float point_size : SV_PointSize;
};

[shader("vertex")]
VertexOutput vertex_main(VertexInput input) {
    VertexOutput o;
    o.position = input.position;
    o.varyings.uvq = input.uvq;
    o.varyings.color = input.color;
    o.varyings.spec_fog = input.spec_fog;
    o.point_size = 1.0;
    return o;
}

bool compare(int func, uint a, uint b) {
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

uint to_byte(float v) { return uint(clamp(v, 0.0, 1.0) * 255.0 + 0.5); }

[shader("fragment")]
float4 fragment_main(Varyings input) : SV_Target {
    float4 c = input.color;
    if (kClearMode) return c;

    if (kTexture) {
        const float4 t = tex.Sample(input.uvq.xy / input.uvq.z);
        switch (kTexFunc) {
            case 0:  // modulate
                c.rgb *= t.rgb;
                if (kTexAlpha) c.a *= t.a;
                break;
            case 1:  // decal
                c.rgb = kTexAlpha ? lerp(c.rgb, t.rgb, t.a) : t.rgb;
                break;
            case 2:  // blend with the environment color
                c.rgb = lerp(c.rgb, pc.env_color.rgb, t.rgb);
                if (kTexAlpha) c.a *= t.a;
                break;
            case 3:  // replace
                c.rgb = t.rgb;
                if (kTexAlpha) c.a = t.a;
                break;
            default:  // add
                c.rgb = min(c.rgb + t.rgb, 1.0);
                if (kTexAlpha) c.a *= t.a;
                break;
        }
        if (kColorDouble) c.rgb = min(c.rgb * 2.0, 1.0);
    }
    c.rgb = min(c.rgb + input.spec_fog.rgb, 1.0);
    if (kFog) c.rgb = lerp(pc.fog_color.rgb, c.rgb, saturate(input.spec_fog.a));

    if (kAlphaTest != 1 && !compare(kAlphaTest, to_byte(c.a) & pc.alpha_mask, pc.alpha_ref & pc.alpha_mask)) discard;
    if (kColorTest != 1) {
        const uint rgb = to_byte(c.r) | (to_byte(c.g) << 8) | (to_byte(c.b) << 16);
        const bool equal = (rgb & pc.color_mask) == (pc.color_ref & pc.color_mask);
        if (kColorTest == 0 || (kColorTest == 2 && !equal) || (kColorTest == 3 && equal)) discard;
    }
    if (kFixedSourceBlend) c.rgb *= pc.fixed_a.rgb;
    return c;
}
