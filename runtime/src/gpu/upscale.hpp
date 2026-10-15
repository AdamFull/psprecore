#pragma once

// 2x edge-directed upscaling of decoded textures (the texture upscaling setting). Every source texel is kept; the new
// texels between them are interpolated along the direction in which the colour changes least, so diagonal edges stay
// smooth lines instead of the steps texel doubling keeps or the blur bilinear magnification gives. Colours are
// interpolated premultiplied by alpha, so transparent texels do not bleed their colour into cut-out edges.

#include <cstdint>

namespace psprecore::rt::gpu {

// `dst` holds (2w) x (2h) texels; both are RGBA8 (R in the low byte).
void upscale_2x(const std::uint32_t* src, int w, int h, std::uint32_t* dst);

}  // namespace psprecore::rt::gpu
