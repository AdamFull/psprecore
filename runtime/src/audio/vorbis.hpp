#pragma once

// Ogg Vorbis file reader (stb_vorbis) for cooked audio: sequential decoding with sample-exact seeking.

#include <cstdint>
#include <filesystem>
#include <memory>

struct stb_vorbis;

namespace ff7cc::rt::audio {

class VorbisStream {
public:
    static std::unique_ptr<VorbisStream> open(const std::filesystem::path& path);  // null if missing/invalid
    ~VorbisStream();
    VorbisStream(const VorbisStream&) = delete;
    VorbisStream& operator=(const VorbisStream&) = delete;

    int channels() const { return channels_; }
    std::uint32_t sample_rate() const { return sample_rate_; }
    std::uint64_t length() const { return length_; }  // samples per channel

    // Decodes up to `frames` interleaved s16 frames starting at `position` (seeking only when needed).
    // Returns the frames produced; past the end the rest is left untouched.
    std::uint32_t read(std::uint64_t position, std::int16_t* out, std::uint32_t frames);

private:
    VorbisStream() = default;
    ::stb_vorbis* vorbis_ = nullptr;
    int channels_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::uint64_t length_ = 0, cursor_ = 0;
};

}  // namespace ff7cc::rt::audio
