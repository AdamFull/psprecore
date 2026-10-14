#pragma once

// Ogg Vorbis file reader (stb_vorbis) for cooked audio: sequential decoding with sample-exact seeking.

#include <cstdint>
#include <memory>

#include "core/foundation/containers/blob.h"
#include "core/foundation/platform/filesystem.h"

struct stb_vorbis;

namespace psprecore::rt::audio {

class VorbisStream {
    struct Key {
        explicit Key() = default;
    };

public:
    static nx::unique_ptr<VorbisStream> open(nx::fs::path_view path);  // null if missing/invalid
    explicit VorbisStream(Key) {}  // open() only
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
    nx::blob<std::uint8_t> file_;  // the whole .ogg (stb_vorbis decodes from memory)
    ::stb_vorbis* vorbis_ = nullptr;
    int channels_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::uint64_t length_ = 0, cursor_ = 0;
};

}  // namespace psprecore::rt::audio
