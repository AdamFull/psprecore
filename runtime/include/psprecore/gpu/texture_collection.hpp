#pragma once

// Collecting the game's textures (--tex-collect DIR), for editing them by hand or baking HD versions offline: a
// game's own tool finds where they come from in its files and lays them out for editing, psptex pack turns the
// edited pictures into replacements (psprecore/gpu/texture_replacement.hpp); psptex bake upscales a collection.
// Each texture is written once, as the game has it (level 0), to DIR/<content hash>.png (the hash texture
// replacement matches), its raw texels as the GE reads them to DIR/<content hash>.texels (to find them in the
// game's files), and listed in DIR/index.tsv:
//
//     <content hash> <width> <height> <GE format> <2d|3d> <repeat|repeat-u|repeat-v|clamp>
//
// 2d: drawn in through mode (the interface, text); 3d: by the 3D pipeline. The last column is the wrap mode (an
// upscaler pads a repeating texture with its opposite edges first, so the tiles still meet). A texture is taken
// from a draw once its content has stayed the same for 30 frames, which leaves out streamed ones (movie frames and
// the like). Collections grow across sessions (entries already listed are skipped), so replays and play sessions
// add up. Textures with button icons (psprecore/gpu/button_icons.hpp) are left out: their icons are redrawn per pad
// at the game's resolution.

#include <cstdint>
#include <span>

#include "core/foundation/strings/utf8_string.h"

namespace psprecore::rt::gpu {

void start_texture_collection(nx::string_view dir);
bool collecting_textures();
void collect_texture(std::uint64_t content_hash, const std::uint32_t* rgba, int width, int height, int format,
                     bool two_d, bool repeat_u, bool repeat_v, std::span<const std::uint8_t> texels);

}  // namespace psprecore::rt::gpu
