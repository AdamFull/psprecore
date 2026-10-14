#pragma once

// Cooked movie video: AV1 frames in an IVF file, decoded with dav1d and converted to PSP pixel formats.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

struct Dav1dContext;

namespace ff7cc::rt::media {

class Av1Video {
public:
    static std::unique_ptr<Av1Video> open(const std::filesystem::path& path);  // null if missing/invalid
    ~Av1Video();
    Av1Video(const Av1Video&) = delete;
    Av1Video& operator=(const Av1Video&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }

    // Decodes the next frame (display order) into `rgba` (width*height, R in the low byte). False at the end.
    bool next(std::vector<std::uint32_t>& rgba);

private:
    Av1Video() = default;
    bool read_packet(std::vector<std::uint8_t>& out);

    std::FILE* file_ = nullptr;
    ::Dav1dContext* decoder_ = nullptr;
    int width_ = 0, height_ = 0;
    bool eof_ = false;
    std::vector<std::uint8_t> pending_;  // packet not yet accepted by the decoder
    std::size_t pending_offset_ = 0;
};

}  // namespace ff7cc::rt::media
