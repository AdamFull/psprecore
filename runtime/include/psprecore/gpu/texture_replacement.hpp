#pragma once

// Texture replacement: pictures in mod folders that stand in for the game's textures. A texture is matched by its
// content hash (texels of every level and, for palette formats, the palette: rt::gpu::texture_content_hash), the
// number --tex-dump starts its file names with:
//
//     mods/<mod>/textures/<16 hex digits>[anything].png      e.g. 3F2A..._title-logo.png
//
// Any size (texture coordinates are normalised, so a 2x or 4x picture just looks sharper), RGBA PNG up to 2048x2048.
// Mods apply in order (psprecore/platform/mods.hpp); a later one wins. Pictures are read when first needed.

#include <cstdint>
#include <span>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore::rt::gpu {

struct ReplacementImage {
    nx::vector<std::uint32_t> rgba;
    int width = 0, height = 0;
};

// Finds the pictures in <mod>/textures of the active mods (call once at start).
void scan_texture_replacements(std::span<const nx::string> mods);
bool have_texture_replacements();
// The picture for a texture, or null (none, or it could not be read).
const ReplacementImage* texture_replacement(std::uint64_t content_hash);

}  // namespace psprecore::rt::gpu
