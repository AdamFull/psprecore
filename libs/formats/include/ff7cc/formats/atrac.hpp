#pragma once

// RIFF WAVE files holding ATRAC3 (format tag 0x0270) or ATRAC3plus (WAVE_FORMAT_EXTENSIBLE with the ATRAC3plus
// GUID), as used for the game's music and voice clips and handed whole to sceAtrac.
//
// Sample positions: a decoder produces frames * samples_per_frame "raw" samples. The first `delay` of them are
// encoder delay; the playable stream is `total_samples` long starting there. Loop points from the `smpl` chunk
// are raw positions.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ff7cc::atrac {

enum class Codec : std::uint8_t { Atrac3, Atrac3Plus };

struct Loop {
    std::uint32_t start = 0, end = 0;  // raw sample positions (inclusive end)
};

struct Info {
    Codec codec = Codec::Atrac3Plus;
    int channels = 2;
    std::uint32_t sample_rate = 44100;
    std::uint32_t block_align = 0;         // bytes per frame
    std::uint16_t codec_params = 0;        // ATRAC3plus: frame size code (bytes = ((p & 0x3FF) + 1) * 8)
    std::uint32_t total_samples = 0;       // fact[0]
    std::uint32_t delay = 0;               // fact[1]
    std::vector<Loop> loops;               // smpl chunk
    std::uint32_t data_offset = 0, data_size = 0;

    int samples_per_frame() const { return codec == Codec::Atrac3Plus ? 2048 : 1024; }
    std::uint32_t frames() const { return block_align ? data_size / block_align : 0; }
};

// Parses the header; returns false if this is not a RIFF ATRAC3/ATRAC3plus file.
bool parse(std::span<const std::byte> file, Info& out);

// Builds a RIFF ATRAC3plus file from raw frames (e.g. a movie's audio stream) so ordinary decoders accept it.
std::vector<std::byte> make_atrac3plus_riff(std::span<const std::byte> frames, std::uint16_t codec_params, int channels);

}  // namespace ff7cc::atrac
