// IoFileMgrForUser and sceUmdUser.
//
// disc0:/... maps to the extracted disc folder, ms0:/... to the host save folder. The game reads discimg.pkg
// through the raw UMD block devices (umd0:/umd1:): seeks are absolute sectors and reads are sector counts.
// sceIoGetstat reports discimg.pkg at kPkgLbn, the LBN PPSSPP assigns, so logs stay comparable.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/io.hpp"
#include "ff7cc/runtime/kernel.hpp"

namespace fs = std::filesystem;

namespace ff7cc::rt {

namespace {

constexpr std::uint32_t kSector = 2048, kPkgLbn = 0x463;
constexpr std::uint32_t kErrorNotFound = 0x80010002, kErrorBadFile = 0x80020323;

fs::path g_game_dir, g_save_dir;

GuestMemory& mem() { return kernel().memory(); }

struct OpenFile {
    std::fstream stream;
    bool block = false;        // umd0:/umd1:
    std::uint64_t sector = 0;  // block devices: current absolute LBN
    std::int64_t async_result = 0;
    bool async_pending = false;
    std::string path;
};
std::map<int, OpenFile> g_files;
int g_next_fd = 3;

struct OpenDir {
    std::vector<fs::directory_entry> entries;
    std::size_t next = 0;
};
std::map<int, OpenDir> g_dirs;

std::fstream g_pkg;  // discimg.pkg for block reads

std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Guest path -> host path (empty if the device is not backed by files).
fs::path host_path(const std::string& guest) {
    const auto colon = guest.find(':');
    if (colon == std::string::npos) return {};
    const std::string device = lower(guest.substr(0, colon));
    std::string rest = guest.substr(colon + 1);
    while (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) rest.erase(rest.begin());
    if (device == "disc0" || device == "umd0" || device == "umd1") return rest.empty() ? fs::path{} : g_game_dir / rest;
    if (device == "ms0" || device == "fatms0") return g_save_dir / rest;
    return {};
}

bool is_block_device(const std::string& guest) {
    const std::string l = lower(guest);
    return l == "umd0:" || l == "umd1:" || l == "umd0:/" || l == "umd1:/";
}

std::int64_t block_read(OpenFile& f, std::uint32_t buffer, std::uint32_t sectors) {
    std::vector<char> data(static_cast<std::size_t>(sectors) * kSector, 0);
    if (f.sector >= kPkgLbn && g_pkg) {
        g_pkg.clear();
        g_pkg.seekg(static_cast<std::streamoff>((f.sector - kPkgLbn) * kSector));
        g_pkg.read(data.data(), static_cast<std::streamsize>(data.size()));
    }
    std::memcpy(mem().ptr(buffer), data.data(), data.size());
    f.sector += sectors;
    return sectors;
}

std::int64_t do_read(int fd, std::uint32_t buffer, std::uint32_t size) {
    const auto it = g_files.find(fd);
    if (it == g_files.end()) return static_cast<std::int32_t>(kErrorBadFile);
    OpenFile& f = it->second;
    if (f.block) return block_read(f, buffer, size);
    f.stream.read(reinterpret_cast<char*>(mem().ptr(buffer)), size);
    const auto n = f.stream.gcount();
    f.stream.clear();
    return n;
}

std::int64_t do_seek(int fd, std::int64_t offset, int whence) {
    const auto it = g_files.find(fd);
    if (it == g_files.end()) return static_cast<std::int32_t>(kErrorBadFile);
    OpenFile& f = it->second;
    if (f.block) {
        f.sector = whence == 0 ? static_cast<std::uint64_t>(offset) : f.sector + offset;
        return static_cast<std::int64_t>(f.sector);
    }
    f.stream.clear();
    f.stream.seekg(offset, whence == 0 ? std::ios::beg : whence == 1 ? std::ios::cur : std::ios::end);
    f.stream.seekp(f.stream.tellg());
    return f.stream.tellg();
}

int do_open(const std::string& path, std::uint32_t flags) {
    OpenFile f;
    f.path = path;
    if (is_block_device(path)) {
        f.block = true;
    } else {
        const fs::path host = host_path(path);
        if (host.empty()) return static_cast<int>(kErrorNotFound);
        std::ios::openmode mode = std::ios::binary;
        if (flags & 0x0001) mode |= std::ios::in;    // PSP_O_RDONLY
        if (flags & 0x0002) mode |= std::ios::out;   // PSP_O_WRONLY
        if (flags & 0x0400) mode |= std::ios::trunc; // PSP_O_TRUNC
        if ((flags & 0x0200) && !fs::exists(host)) {  // PSP_O_CREAT
            fs::create_directories(host.parent_path());
            std::ofstream(host, std::ios::binary).close();
        }
        f.stream.open(host, mode);
        if (!f.stream) return static_cast<int>(kErrorNotFound);
    }
    const int fd = g_next_fd++;
    g_files.emplace(fd, std::move(f));
    return fd;
}

void write64(std::uint32_t addr, std::int64_t v) {
    mem().write32(addr, static_cast<std::uint32_t>(v));
    mem().write32(addr + 4, static_cast<std::uint32_t>(static_cast<std::uint64_t>(v) >> 32));
}

// SceIoStat: mode, attr, size (u64), 3 × ScePspDateTime (16 bytes), private[6].
void write_stat(std::uint32_t addr, const fs::path& host, std::uint32_t lbn) {
    mem().fill(addr, 0, 88);
    const bool dir = fs::is_directory(host);
    std::error_code ec;
    const std::uint64_t size = dir ? 0 : fs::file_size(host, ec);
    mem().write32(addr, dir ? 0x11FF : 0x21FF);
    mem().write32(addr + 4, dir ? 0x10 : 0x20);
    write64(addr + 8, static_cast<std::int64_t>(size));
    mem().write32(addr + 64, lbn);
}

// ---- IoFileMgrForUser --------------------------------------------------------------------------

void Open(Context& c) {
    const std::string path = mem().string(arg(c, 0));
    const int fd = do_open(path, arg(c, 1));
    if (kernel().trace_hle) std::fprintf(stderr, "  sceIoOpen(%s) -> %d\n", path.c_str(), fd);
    ret(c, static_cast<std::uint32_t>(fd));
}
void OpenAsync(Context& c) {
    const int fd = do_open(mem().string(arg(c, 0)), arg(c, 1));
    if (fd > 0) g_files[fd].async_result = fd;
    ret(c, static_cast<std::uint32_t>(fd));
}
void Close(Context& c) { ret(c, g_files.erase(static_cast<int>(arg(c, 0))) ? 0 : kErrorBadFile); }
void CloseAsync(Context& c) {
    const int fd = static_cast<int>(arg(c, 0));
    if (!g_files.contains(fd)) return ret(c, kErrorBadFile);
    g_files[fd].async_result = 0;
    g_files[fd].async_pending = true;  // closed after the result is collected
    ret(c, 0);
}
void Read(Context& c) { ret(c, static_cast<std::uint32_t>(do_read(static_cast<int>(arg(c, 0)), arg(c, 1), arg(c, 2)))); }
void ReadAsync(Context& c) {
    const int fd = static_cast<int>(arg(c, 0));
    if (!g_files.contains(fd)) return ret(c, kErrorBadFile);
    g_files[fd].async_result = do_read(fd, arg(c, 1), arg(c, 2));
    ret(c, 0);
}
// 64-bit offset arrives in a2/a3 (aligned register pair); whence in t0.
void Lseek(Context& c) {
    const std::int64_t off = static_cast<std::int64_t>(static_cast<std::uint64_t>(arg(c, 2)) | (static_cast<std::uint64_t>(arg(c, 3)) << 32));
    ret64(c, static_cast<std::uint64_t>(do_seek(static_cast<int>(arg(c, 0)), off, static_cast<int>(arg(c, 4)))));
}
void LseekAsync(Context& c) {
    const int fd = static_cast<int>(arg(c, 0));
    if (!g_files.contains(fd)) return ret(c, kErrorBadFile);
    const std::int64_t off = static_cast<std::int64_t>(static_cast<std::uint64_t>(arg(c, 2)) | (static_cast<std::uint64_t>(arg(c, 3)) << 32));
    g_files[fd].async_result = do_seek(fd, off, static_cast<int>(arg(c, 4)));
    ret(c, 0);
}
void WaitAsync(Context& c) {
    const int fd = static_cast<int>(arg(c, 0));
    const auto it = g_files.find(fd);
    if (it == g_files.end()) return ret(c, kErrorBadFile);
    if (arg(c, 1)) write64(arg(c, 1), it->second.async_result);
    if (it->second.async_pending) g_files.erase(it);  // pending close
    ret(c, 0);
}
void Write(Context& c) {
    const int fd = static_cast<int>(arg(c, 0));
    const std::uint32_t size = arg(c, 2);
    if (fd == 1 || fd == 2) {
        std::fwrite(mem().ptr(arg(c, 1)), 1, size, stderr);
        return ret(c, size);
    }
    const auto it = g_files.find(fd);
    if (it == g_files.end()) return ret(c, kErrorBadFile);
    it->second.stream.write(reinterpret_cast<const char*>(mem().ptr(arg(c, 1))), size);
    ret(c, size);
}
void Getstat(Context& c) {
    const std::string path = mem().string(arg(c, 0));
    const fs::path host = host_path(path);
    if (host.empty() || !fs::exists(host)) return ret(c, kErrorNotFound);
    write_stat(arg(c, 1), host, lower(host.filename().string()) == "discimg.pkg" ? kPkgLbn : 0);
    ret(c, 0);
}
void Mkdir(Context& c) {
    const fs::path host = host_path(mem().string(arg(c, 0)));
    std::error_code ec;
    if (!host.empty()) fs::create_directories(host, ec);
    ret(c, 0);
}
void Rename(Context& c) {
    std::error_code ec;
    fs::rename(host_path(mem().string(arg(c, 0))), host_path(mem().string(arg(c, 1))), ec);
    ret(c, ec ? kErrorNotFound : 0);
}
void Dopen(Context& c) {
    const fs::path host = host_path(mem().string(arg(c, 0)));
    if (host.empty() || !fs::is_directory(host)) return ret(c, kErrorNotFound);
    OpenDir d;
    for (const auto& e : fs::directory_iterator(host)) d.entries.push_back(e);
    const int fd = g_next_fd++;
    g_dirs.emplace(fd, std::move(d));
    ret(c, static_cast<std::uint32_t>(fd));
}
// SceIoDirent: SceIoStat (88 bytes), name[256], private pointer, dummy.
void Dread(Context& c) {
    const auto it = g_dirs.find(static_cast<int>(arg(c, 0)));
    if (it == g_dirs.end()) return ret(c, kErrorBadFile);
    OpenDir& d = it->second;
    if (d.next >= d.entries.size()) return ret(c, 0);
    const auto& e = d.entries[d.next++];
    const std::uint32_t out = arg(c, 1);
    write_stat(out, e.path(), 0);
    const std::string name = e.path().filename().string().substr(0, 255);
    mem().write_string(out + 88, name);
    ret(c, 1);
}
void Dclose(Context& c) { ret(c, g_dirs.erase(static_cast<int>(arg(c, 0))) ? 0 : kErrorBadFile); }
void Devctl(Context& c) {
    if (kernel().trace_hle)
        std::fprintf(stderr, "  sceIoDevctl(%s, %08x)\n", mem().string(arg(c, 0)).c_str(), arg(c, 1));
    ret(c, 0);
}

// ---- sceUmdUser --------------------------------------------------------------------------------

void ReturnZero(Context& c) { ret(c, 0); }
void ReturnOne(Context& c) { ret(c, 1); }
constexpr std::uint32_t kUmdReady = 0x32;  // present | inited | ready
int g_umd_callback = 0;
void UmdGetDriveStat(Context& c) { ret(c, kUmdReady); }
// The game learns that the disc is readable from its UMD callback (it gates movie streaming on it).
void UmdRegisterCallback(Context& c) {
    g_umd_callback = static_cast<int>(arg(c, 0));
    kernel().notify_callback(g_umd_callback, kUmdReady);
    ret(c, 0);
}
void UmdUnRegisterCallback(Context& c) {
    g_umd_callback = 0;
    ret(c, 0);
}
void UmdActivate(Context& c) {
    if (g_umd_callback) kernel().notify_callback(g_umd_callback, kUmdReady);
    ret(c, 0);
}

constexpr HleBinding kBindings[] = {
    {"sceIoOpen", Open},           {"sceIoOpenAsync", OpenAsync},     {"sceIoClose", Close},
    {"sceIoCloseAsync", CloseAsync}, {"sceIoRead", Read},             {"sceIoReadAsync", ReadAsync},
    {"sceIoLseek", Lseek},         {"sceIoLseekAsync", LseekAsync},   {"sceIoWaitAsync", WaitAsync},
    {"sceIoWaitAsyncCB", WaitAsync}, {"sceIoWrite", Write},           {"sceIoGetstat", Getstat},
    {"sceIoMkdir", Mkdir},         {"sceIoRename", Rename},           {"sceIoDopen", Dopen},
    {"sceIoDread", Dread},         {"sceIoDclose", Dclose},           {"sceIoDevctl", Devctl},
    {"sceIoIoctl", ReturnZero},
    {"sceUmdCheckMedium", ReturnOne}, {"sceUmdActivate", UmdActivate}, {"sceUmdGetDriveStat", UmdGetDriveStat},
    {"sceUmdWaitDriveStatCB", ReturnZero}, {"sceUmdWaitDriveStatWithTimer", ReturnZero},
    {"sceUmdRegisterUMDCallBack", UmdRegisterCallback}, {"sceUmdUnRegisterUMDCallBack", UmdUnRegisterCallback},
    {"sceUmdGetErrorStat", ReturnZero},
};

}  // namespace

void set_storage(const fs::path& game_dir, const fs::path& save_dir) {
    g_game_dir = game_dir;
    g_save_dir = save_dir;
    g_pkg.open(game_dir / "PSP_GAME" / "USRDIR" / "discimg.pkg", std::ios::in | std::ios::binary);
    if (!g_pkg) std::fprintf(stderr, "warning: discimg.pkg not found under %s\n", game_dir.string().c_str());
}

const fs::path& save_dir() { return g_save_dir; }

void hle_install_io() { hle_bind(kBindings); }

}  // namespace ff7cc::rt
