#pragma once

// Texture replacement: pictures in mod folders that stand in for the game's textures. A texture is matched by its
// content hash (texels of every level and, for palette formats, the palette: rt::gpu::texture_content_hash), the
// number --tex-dump starts its file names with:
//
//     mods/<mod>/textures/<16 hex digits>[anything].png      e.g. 3F2A..._title-logo.png
//
// Any size (texture coordinates are normalised, so a 2x or 4x picture just looks sharper), RGBA PNG up to 2048x2048.
// Mods apply in order (psprecore/platform/mods.hpp); a later one wins. Pictures are read when first needed.
//
// Palette textures (4- and 8-bit indices) may instead get an index map, which keeps working with every palette the
// game draws them with (coloured text, palette swaps):
//
//     mods/<mod>/textures/indexed/<16 hex digits>[anything].png
//
// matched by the texels alone (texture_texels_hash; --tex-dump writes these maps into indexed/): a grey picture
// whose brightness is the palette index, index x 17 for 16 colours (0, 17, ..., 255), the index itself for 256.
// Draw an upscaled map with the same grey levels (no smoothing between them).

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
bool have_indexed_replacements();
// The index map for a palette texture (texture_texels_hash), or null.
const ReplacementImage* indexed_replacement(std::uint64_t texels_hash);

}  // namespace psprecore::rt::gpu
