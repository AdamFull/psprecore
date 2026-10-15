#include "psprecore/formats/ktx2.hpp"

#include <algorithm>
#include <cstring>

namespace psprecore::ktx2 {

namespace {

constexpr std::uint8_t kIdentifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
constexpr std::size_t kHeaderSize = 80, kLevelEntrySize = 24;

void put32(nx::vector<std::byte>& out, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + static_cast<std::size_t>(i)] = static_cast<std::byte>(v >> (8 * i));
}

void put64(nx::vector<std::byte>& out, std::size_t at, std::uint64_t v) {
    put32(out, at, static_cast<std::uint32_t>(v));
    put32(out, at + 4, static_cast<std::uint32_t>(v >> 32));
}

std::uint32_t get32(std::span<const std::byte> in, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(in[at + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}

std::uint64_t get64(std::span<const std::byte> in, std::size_t at) {
    return get32(in, at) | static_cast<std::uint64_t>(get32(in, at + 4)) << 32;
}

std::size_t align(std::size_t n, std::size_t to) { return (n + to - 1) / to * to; }

int level_extent(int size, std::uint32_t level) { return std::max(1, size >> level); }

}  // namespace

nx::vector<std::byte> write(const Texture& texture) {
    // Data format descriptor: one basic block describing a 4x4 block of 16 bytes (one 128-bit sample).
    constexpr std::size_t kDfdSize = 4 + 24 + 16;
    const std::size_t dfd_offset = kHeaderSize + kLevelEntrySize * texture.levels;
    std::size_t end = align(dfd_offset + kDfdSize, 16);

    // Level data, smallest level first (as the format asks); offsets 16-byte aligned (the block size).
    nx::vector<std::size_t> source_offset(texture.levels), file_offset(texture.levels), size(texture.levels);
    for (std::uint32_t l = 0, at = 0; l < texture.levels; ++l) {
        source_offset[l] = at;
        size[l] = level_size(level_extent(texture.width, l), level_extent(texture.height, l));
        at += static_cast<std::uint32_t>(size[l]);
    }
    for (std::uint32_t l = texture.levels; l-- > 0;) {
        file_offset[l] = end;
        end = align(end + size[l], 16);
    }

    nx::vector<std::byte> out(end);
    std::memcpy(out.data(), kIdentifier, sizeof kIdentifier);
    put32(out, 12, static_cast<std::uint32_t>(texture.format));
    put32(out, 16, 1);  // typeSize: 1 for block-compressed formats
    put32(out, 20, static_cast<std::uint32_t>(texture.width));
    put32(out, 24, static_cast<std::uint32_t>(texture.height));
    put32(out, 28, 0);  // pixelDepth: 2D
    put32(out, 32, 0);  // layerCount: not an array
    put32(out, 36, 1);  // faceCount
    put32(out, 40, texture.levels);
    put32(out, 44, 0);  // supercompressionScheme: none
    put32(out, 48, static_cast<std::uint32_t>(dfd_offset));
    put32(out, 52, static_cast<std::uint32_t>(kDfdSize));
    put32(out, 56, 0), put32(out, 60, 0);  // no key/value data
    put64(out, 64, 0), put64(out, 72, 0);  // no supercompression global data
    for (std::uint32_t l = 0; l < texture.levels; ++l) {
        const std::size_t entry = kHeaderSize + kLevelEntrySize * l;
        put64(out, entry, file_offset[l]);
        put64(out, entry + 8, size[l]);
        put64(out, entry + 16, size[l]);
        std::memcpy(out.data() + file_offset[l], texture.data.data() + source_offset[l], size[l]);
    }

    const std::size_t d = dfd_offset;
    put32(out, d, static_cast<std::uint32_t>(kDfdSize));
    put32(out, d + 4, 0);                        // vendor Khronos, basic descriptor
    put32(out, d + 8, 2 | (24 + 16) << 16);      // version 1.3, block size
    const std::uint32_t model = texture.format == Format::BC7 ? 134 : 162;  // KHR_DF_MODEL_BC7 / _ASTC
    put32(out, d + 12, model | 1 << 8 | 1 << 16);  // BT.709 primaries, linear transfer, straight alpha
    put32(out, d + 16, 3 | 3 << 8);              // texel block 4x4 (dimensions minus one)
    put32(out, d + 20, 16), put32(out, d + 24, 0);  // 16 bytes in plane 0
    put32(out, d + 28, 0 | 127u << 16);          // sample: bit offset 0, 128 bits, channel 0 (the block data)
    put32(out, d + 32, 0);                       // sample position
    put32(out, d + 36, 0), put32(out, d + 40, 0xFFFFFFFFu);  // lower, upper
    return out;
}

bool read(std::span<const std::byte> file, Texture& texture) {
    if (file.size() < kHeaderSize || std::memcmp(file.data(), kIdentifier, sizeof kIdentifier) != 0) return false;
    const std::uint32_t format = get32(file, 12);
    if (format != static_cast<std::uint32_t>(Format::BC7) && format != static_cast<std::uint32_t>(Format::ASTC4x4))
        return false;
    const std::uint32_t width = get32(file, 20), height = get32(file, 24), levels = get32(file, 40);
    if (get32(file, 28) != 0 || get32(file, 32) > 1 || get32(file, 36) != 1 || get32(file, 44) != 0) return false;
    if (width == 0 || height == 0 || width > 16384 || height > 16384 || levels == 0 || levels > 15) return false;
    if ((std::max(width, height) >> (levels - 1)) == 0) return false;  // more levels than a full chain
    if (file.size() < kHeaderSize + kLevelEntrySize * levels) return false;

    texture.format = static_cast<Format>(format);
    texture.width = static_cast<int>(width);
    texture.height = static_cast<int>(height);
    texture.levels = levels;
    texture.data.clear();
    for (std::uint32_t l = 0; l < levels; ++l) {
        const std::size_t entry = kHeaderSize + kLevelEntrySize * l;
        const std::uint64_t offset = get64(file, entry), length = get64(file, entry + 8);
        const std::size_t expected = level_size(level_extent(texture.width, l), level_extent(texture.height, l));
        if (length != expected || offset > file.size() || length > file.size() - offset) return false;
        const auto* src = reinterpret_cast<const std::uint8_t*>(file.data() + offset);
        texture.data.insert(texture.data.end(), src, src + length);
    }
    return true;
}

}  // namespace psprecore::ktx2
