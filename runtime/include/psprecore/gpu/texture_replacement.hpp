#pragma once

// Texture replacement: pictures that stand in for the game's textures. A texture is matched by its content hash
// (texels of every level and, for palette formats, the palette: rt::gpu::texture_content_hash), the number
// --tex-dump starts its file names with. Pictures come from mod folders:
//
//     mods/<mod>/textures/<16 hex digits>[anything].png      e.g. 3F2A..._title-logo.png
//
// and from baked HD textures (psptex bake, from a --tex-collect collection; the "HD textures" setting):
//
//     cooked/textures/<16 hex digits>.png
//
// Any size (texture coordinates are normalised, so a 2x or 4x picture just looks sharper), RGBA PNG up to 2048x2048.
// Mods apply in order (psprecore/platform/mods.hpp), a later one winning, and every mod wins over baked textures.
// Pictures load on a worker thread when first needed (the game's texture is drawn until then) and get mipmaps.
// Loaded pictures stay in memory up to a budget, least recently used ones going first.
//
// Palette textures (4- and 8-bit indices) may instead get an index map, which keeps working with every palette the
// game draws them with (coloured text, palette swaps):
//
//     mods/<mod>/textures/indexed/<16 hex digits>[anything].png
//
// matched by the texels alone (texture_texels_hash; --tex-dump writes these maps into indexed/): a grey picture
// whose brightness is the palette index, index x 17 for 16 colours (0, 17, ..., 255), the index itself for 256.
// Draw an upscaled map with the same grey levels (no smoothing between them). Index maps get no mipmaps.

#include <cstdint>
#include <span>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore::rt::gpu {

struct ReplacementImage {
    nx::vector<std::uint32_t> rgba;  // level 0, then each mipmap (half the size of the one before, down to 1x1)
    int width = 0, height = 0;
    std::uint32_t levels = 1;
};

// Finds the pictures in <mod>/textures of the active mods (call once at start).
void scan_texture_replacements(std::span<const nx::string> mods);
// Finds baked HD textures in `dir` (<cooked>/textures; call once at start).
void scan_baked_textures(nx::string_view dir);
bool have_baked_textures();
// The "HD textures" setting; true when it changed (textures already drawn need uploading again).
bool enable_baked_textures(bool enabled);

enum class Lookup { None, Loading, Ready };
struct FoundImage {
    Lookup state = Lookup::None;
    const ReplacementImage* image = nullptr;  // Ready: valid until the next lookup
};

// The picture for a texture. Loading: being read (draw the game's texture and ask again later). Lookups come from
// one thread (the renderer's).
FoundImage texture_replacement(std::uint64_t content_hash);
bool have_indexed_replacements();
// The index map for a palette texture (texture_texels_hash).
FoundImage indexed_replacement(std::uint64_t texels_hash);

}  // namespace psprecore::rt::gpu
