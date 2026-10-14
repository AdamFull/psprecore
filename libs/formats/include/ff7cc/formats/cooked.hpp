#pragma once

// Cooked media: modern-format replacements for the game's ATRAC audio and PSMF movies, produced by `cccook`
// and looked up at runtime by a content key computed from what the game hands to sceAtrac/sceMpeg.
//
//   <root>/audio/<key>.ogg        RIFF ATRAC3/ATRAC3plus file -> Ogg Vorbis, raw decoder timeline (44.1 kHz)
//   <root>/movies/<key>.ivf       PSMF video -> AV1 in IVF, one frame per H.264 access unit
//   <root>/movies/<key>.ogg       PSMF audio -> Ogg Vorbis
//
// Keys are FNV-1a 64 hashes: of the RIFF header plus data chunk for ATRAC files, of the 2048-byte header for
// PSMF movies. Replacement media (HD movies, remastered music) can be dropped in under the same names.

#include <cstddef>
#include <cstdint>
#include <span>

#include "ff7cc/io/path.hpp"
#include "core/foundation/strings/utf8_string.h"

namespace ff7cc::cooked {

inline std::uint64_t fnv1a64(std::span<const std::byte> data) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const std::byte b : data) {
        h ^= static_cast<std::uint8_t>(b);
        h *= 0x100000001B3ull;
    }
    return h;
}

inline nx::string key_name(std::uint64_t key) {
    static constexpr char kHex[] = "0123456789abcdef";
    nx::string s(16, '0');
    for (int i = 15; i >= 0; --i, key >>= 4) s[static_cast<std::size_t>(i)] = kHex[key & 15];
    return s;
}

inline nx::string audio_path(nx::string_view root, std::uint64_t key) {
    return path_join(root, "audio", key_name(key) + ".ogg");
}
inline nx::string movie_video_path(nx::string_view root, std::uint64_t key) {
    return path_join(root, "movies", key_name(key) + ".ivf");
}
inline nx::string movie_audio_path(nx::string_view root, std::uint64_t key) {
    return path_join(root, "movies", key_name(key) + ".ogg");
}

}  // namespace ff7cc::cooked
