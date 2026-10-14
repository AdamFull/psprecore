#pragma once

// Cooked movie video: AV1 frames in an IVF file, decoded with dav1d and converted to PSP pixel formats.

#include <cstdint>
#include <cstdio>
#include "core/foundation/platform/filesystem.h"
#include <memory>

#include "core/foundation/core/foundation.h"

struct Dav1dContext;

namespace psprecore::rt::media {

class Av1Video {
public:
    static std::unique_ptr<Av1Video> open(nx::fs::path_view path);  // null if missing/invalid
    ~Av1Video();
    Av1Video(const Av1Video&) = delete;
    Av1Video& operator=(const Av1Video&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }

    // Decodes the next frame (display order) into `rgba` (width*height, R in the low byte). False at the end.
    bool next(nx::vector<std::uint32_t>& rgba);

private:
    Av1Video() = default;
    bool read_packet(nx::vector<std::uint8_t>& out);

    nx::fs::stream_ptr file_;
    ::Dav1dContext* decoder_ = nullptr;
    int width_ = 0, height_ = 0;
    bool eof_ = false;
    nx::vector<std::uint8_t> pending_;  // packet not yet accepted by the decoder
    std::size_t pending_offset_ = 0;
};

}  // namespace psprecore::rt::media
