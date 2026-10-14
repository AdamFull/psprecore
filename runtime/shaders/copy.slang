// Copies an image into the bound color attachment texel for texel (both the same size): refreshes a multisampled
// render target from its resolved image after the CPU wrote into it (every sample gets the texel).

[[vk::binding(0, 0)]] Sampler2D source;

struct VertexOutput {
    float4 position : SV_Position;
};

// One triangle covering the attachment.
[shader("vertex")]
VertexOutput vertex_main(uint id : SV_VulkanVertexID) {
    VertexOutput o;
    const float2 p = float2(float((id << 1) & 2), float(id & 2));
    o.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 fragment_main(float4 position : SV_Position) : SV_Target {
    return source.Load(int3(int2(position.xy), 0));
}
