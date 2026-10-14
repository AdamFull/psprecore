#include "psprecore/io/file.hpp"

#include <algorithm>
#include <array>

#include "psprecore/error.hpp"
#include "core/foundation/core/foundation.h"

namespace psprecore::io {

std::FILE* open_c_file(nx::fs::path_view path, const char* mode);

namespace {

constexpr std::size_t kCopyChunk = 1 << 20;

std::FILE* open_file(nx::fs::path_view path, File::Mode mode) {
    return open_c_file(path, mode == File::Mode::Read ? "rb" : "wb");
}

int seek64(std::FILE* f, std::uint64_t offset, int origin) {
#if defined(_WIN32)
    return _fseeki64(f, static_cast<long long>(offset), origin);
#else
    return fseeko(f, static_cast<off_t>(offset), origin);
#endif
}

std::int64_t tell64(std::FILE* f) {
#if defined(_WIN32)
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}

}  // namespace

// Paths are UTF-8; Windows opens files by UTF-16 name.
std::FILE* open_c_file(nx::fs::path_view path, const char* mode) {
    const nx::string name(path.native());
#if defined(_WIN32)
    std::FILE* f = nullptr;
    const nx::string m(mode);
    if (_wfopen_s(&f, reinterpret_cast<const wchar_t*>(name.u16string().c_str()),
                  reinterpret_cast<const wchar_t*>(m.u16string().c_str())) != 0)
        return nullptr;
    return f;
#else
    return std::fopen(name.c_str(), mode);
#endif
}

File::File(nx::fs::path_view path, Mode mode) : path_(path.native()) {
    file_ = open_file(path, mode);
    if (!file_) throw IoError("cannot open '" + path_ + "'");
}

File::~File() {
    if (file_) std::fclose(file_);
}

File::File(File&& other) noexcept : file_(other.file_), path_(std::move(other.path_)) {
    other.file_ = nullptr;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (file_) std::fclose(file_);
        file_ = other.file_;
        path_ = std::move(other.path_);
        other.file_ = nullptr;
    }
    return *this;
}

void File::close() {
    if (file_ && std::fclose(file_) != 0) {
        file_ = nullptr;
        throw IoError("error closing '" + path_.string() + "'");
    }
    file_ = nullptr;
}

std::uint64_t File::tell() {
    const std::int64_t pos = tell64(file_);
    if (pos < 0) throw IoError("tell failed on '" + path_.string() + "'");
    return static_cast<std::uint64_t>(pos);
}

void File::seek(std::uint64_t offset) {
    if (seek64(file_, offset, SEEK_SET) != 0) throw IoError("seek failed on '" + path_.string() + "'");
}

std::uint64_t File::size() {
    const std::uint64_t pos = tell();
    if (seek64(file_, 0, SEEK_END) != 0) throw IoError("seek failed on '" + path_.string() + "'");
    const std::uint64_t end = tell();
    seek(pos);
    return end;
}

std::size_t File::read_some(std::span<std::byte> out) {
    const std::size_t n = std::fread(out.data(), 1, out.size(), file_);
    if (n < out.size() && std::ferror(file_)) throw IoError("read failed on '" + path_.string() + "'");
    return n;
}

void File::read_exact(std::span<std::byte> out) {
    if (read_some(out) != out.size()) throw IoError("unexpected end of file in '" + path_.string() + "'");
}

void File::read_exact_at(std::uint64_t offset, std::span<std::byte> out) {
    seek(offset);
    read_exact(out);
}

void File::write(std::span<const std::byte> data) {
    if (std::fwrite(data.data(), 1, data.size(), file_) != data.size())
        throw IoError("write failed on '" + path_.string() + "'");
}

void File::write_zeros(std::uint64_t count) {
    static constexpr std::array<std::byte, 4096> kZeros{};
    while (count > 0) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(count, kZeros.size()));
        write(std::span(kZeros.data(), n));
        count -= n;
    }
}

void File::copy_from(File& src, std::uint64_t src_offset, std::uint64_t count) {
    nx::vector<std::byte> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(count, kCopyChunk)));
    src.seek(src_offset);
    while (count > 0) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(count, buffer.size()));
        src.read_exact(std::span(buffer.data(), n));
        write(std::span(buffer.data(), n));
        count -= n;
    }
}

nx::vector<std::byte> read_all(nx::fs::path_view path) {
    File f(path, File::Mode::Read);
    nx::vector<std::byte> data(static_cast<std::size_t>(f.size()));
    f.read_exact(data);
    return data;
}

void write_all(nx::fs::path_view path, std::span<const std::byte> data) {
    File f(path, File::Mode::Write);
    f.write(data);
    f.close();
}

}  // namespace psprecore::io
