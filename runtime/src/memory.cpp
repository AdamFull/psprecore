#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "ff7cc/recomp/recomp.h"
#include "ff7cc/runtime/memory.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#endif

uint8_t* g_membase = nullptr;

namespace ff7cc::rt {

namespace {

constexpr std::size_t kSpace = 0x20000000;  // 512 MiB

void* reserve(std::size_t size) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

void commit(std::uint8_t* base, std::uint32_t offset, std::uint32_t size) {
#if defined(_WIN32)
    if (!VirtualAlloc(base + offset, size, MEM_COMMIT, PAGE_READWRITE)) throw std::runtime_error("guest memory commit failed");
#else
    if (mprotect(base + offset, size, PROT_READ | PROT_WRITE) != 0) throw std::runtime_error("guest memory commit failed");
#endif
}

void release(void* p, std::size_t size) {
#if defined(_WIN32)
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

}  // namespace

GuestMemory::GuestMemory() {
    base_ = static_cast<std::uint8_t*>(reserve(kSpace));
    if (!base_) throw std::runtime_error("cannot reserve 512 MiB of address space for guest memory");
    commit(base_, kScratchpad, kScratchpadSize);
    commit(base_, kVram, kVramSize);
    commit(base_, kRam, kRamSize);
    g_membase = base_;
}

GuestMemory::~GuestMemory() {
    if (g_membase == base_) g_membase = nullptr;
    release(base_, kSpace);
}

bool GuestMemory::valid(std::uint32_t addr, std::uint32_t size) const {
    const std::uint32_t a = addr & 0x1FFFFFFF;
    const auto in = [&](std::uint32_t begin, std::uint32_t len) { return a >= begin && a - begin + size <= len; };
    return in(kScratchpad, kScratchpadSize) || in(kVram, kVramSize) || in(kRam, kRamSize);
}

std::uint32_t GuestMemory::read32(std::uint32_t addr) const {
    std::uint32_t v;
    std::memcpy(&v, ptr(addr), 4);
    return v;
}

void GuestMemory::write32(std::uint32_t addr, std::uint32_t value) { std::memcpy(ptr(addr), &value, 4); }

void GuestMemory::write(std::uint32_t addr, std::span<const std::byte> data) {
    std::memcpy(ptr(addr), data.data(), data.size());
}

void GuestMemory::fill(std::uint32_t addr, std::uint8_t value, std::uint32_t size) { std::memset(ptr(addr), value, size); }

nx::string GuestMemory::string(std::uint32_t addr, std::size_t max) const {
    nx::string s;
    if (!addr) return s;
    for (const auto* p = reinterpret_cast<const char*>(ptr(addr)); s.size() < max && *p; ++p) s += *p;
    return s;
}

void GuestMemory::write_string(std::uint32_t addr, nx::string_view s) {
    std::memcpy(ptr(addr), s.data(), s.size());
    *ptr(addr + static_cast<std::uint32_t>(s.size())) = 0;
}

// ---------------------------------------------------------------------------------------------

std::uint32_t GuestHeap::alloc(std::uint32_t size, std::uint32_t align, bool high) {
    if (size == 0) size = 1;
    size = (size + 15) & ~15u;
    if (align < 16) align = 16;
    // Walk the gaps between used blocks.
    nx::vector<std::pair<std::uint32_t, std::uint32_t>> gaps;  // [begin, end)
    std::uint32_t cursor = begin_;
    for (const auto& b : used_) {
        if (b.addr > cursor) gaps.emplace_back(cursor, b.addr);
        cursor = std::max(cursor, b.addr + b.size);
    }
    if (end_ > cursor) gaps.emplace_back(cursor, end_);
    if (high) std::reverse(gaps.begin(), gaps.end());
    for (const auto& [g0, g1] : gaps) {
        std::uint32_t addr;
        if (high) {
            if (g1 - g0 < size) continue;
            addr = (g1 - size) & ~(align - 1);
            if (addr < g0) continue;
        } else {
            addr = (g0 + align - 1) & ~(align - 1);
            if (addr < g0 || addr + size > g1) continue;
        }
        used_.insert(std::upper_bound(used_.begin(), used_.end(), addr,
                                      [](std::uint32_t a, const Block& b) { return a < b.addr; }),
                     Block{addr, size});
        return addr;
    }
    return 0;
}

std::uint32_t GuestHeap::alloc_at(std::uint32_t addr, std::uint32_t size) {
    size = (size + 15) & ~15u;
    if (addr < begin_ || addr + size > end_) return 0;
    for (const auto& b : used_)
        if (addr < b.addr + b.size && b.addr < addr + size) return 0;
    used_.insert(std::upper_bound(used_.begin(), used_.end(), addr,
                                  [](std::uint32_t a, const Block& b) { return a < b.addr; }),
                 Block{addr, size});
    return addr;
}

bool GuestHeap::free(std::uint32_t addr) {
    const auto it = std::find_if(used_.begin(), used_.end(), [&](const Block& b) { return b.addr == addr; });
    if (it == used_.end()) return false;
    used_.erase(it);
    return true;
}

std::uint32_t GuestHeap::size_of(std::uint32_t addr) const {
    for (const auto& b : used_)
        if (b.addr == addr) return b.size;
    return 0;
}

std::uint32_t GuestHeap::free_bytes() const {
    std::uint32_t used = 0;
    for (const auto& b : used_) used += b.size;
    return (end_ - begin_) - used;
}

}  // namespace ff7cc::rt
