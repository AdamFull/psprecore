#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>

#include "core/foundation/core/foundation.h"

namespace ff7cc::io {

// Thin RAII wrapper over FILE* with 64-bit offsets. Every failure throws IoError.
class File {
public:
    enum class Mode { Read, Write };

    File() = default;
    File(const std::filesystem::path& path, Mode mode);
    ~File();

    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool is_open() const { return file_ != nullptr; }
    const std::filesystem::path& path() const { return path_; }

    std::uint64_t size();
    std::uint64_t tell();
    void seek(std::uint64_t offset);

    void read_exact(std::span<std::byte> out);
    void read_exact_at(std::uint64_t offset, std::span<std::byte> out);
    // Reads up to out.size() bytes; returns the count (short only at end of file).
    std::size_t read_some(std::span<std::byte> out);

    void write(std::span<const std::byte> data);
    void write_zeros(std::uint64_t count);
    // Streams `count` bytes from `src` at `src_offset` to the current position.
    void copy_from(File& src, std::uint64_t src_offset, std::uint64_t count);

    void close();

private:
    std::FILE* file_ = nullptr;
    std::filesystem::path path_;
};

nx::vector<std::byte> read_all(const std::filesystem::path& path);
void write_all(const std::filesystem::path& path, std::span<const std::byte> data);

}  // namespace ff7cc::io
