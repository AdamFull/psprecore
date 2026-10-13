#pragma once

// Guest memory: one 512 MiB reservation mirroring the PSP physical address space (addresses are masked with
// 0x1FFFFFFF, see recomp.h). Only the regions the PSP actually has are committed.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ff7cc::rt {

inline constexpr std::uint32_t kScratchpad = 0x00010000, kScratchpadSize = 0x4000;
inline constexpr std::uint32_t kVram = 0x04000000, kVramSize = 0x00800000;  // 2 MiB + mirrors
inline constexpr std::uint32_t kRam = 0x08000000, kRamSize = 0x02000000;    // 32 MiB (kernel + user)
inline constexpr std::uint32_t kUserRam = 0x08800000;                       // start of the user partition

class GuestMemory {
public:
    GuestMemory();   // reserves the space, commits PSP regions and publishes g_membase
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    std::uint8_t* ptr(std::uint32_t addr) const { return base_ + (addr & 0x1FFFFFFF); }
    bool valid(std::uint32_t addr, std::uint32_t size = 1) const;

    std::uint32_t read32(std::uint32_t addr) const;
    void write32(std::uint32_t addr, std::uint32_t value);
    void write(std::uint32_t addr, std::span<const std::byte> data);
    void fill(std::uint32_t addr, std::uint8_t value, std::uint32_t size);
    // NUL-terminated string (bounded).
    std::string string(std::uint32_t addr, std::size_t max = 1024) const;
    void write_string(std::uint32_t addr, std::string_view s);  // with terminator

private:
    std::uint8_t* base_ = nullptr;
};

// Simple first-fit allocator for a guest address range (user partition, stacks).
class GuestHeap {
public:
    GuestHeap(std::uint32_t begin, std::uint32_t end) : begin_(begin), end_(end) {}
    // Returns 0 on failure. `high` allocates from the top of the range (PSP_SMEM_High).
    std::uint32_t alloc(std::uint32_t size, std::uint32_t align = 16, bool high = false);
    std::uint32_t alloc_at(std::uint32_t addr, std::uint32_t size);
    bool free(std::uint32_t addr);
    std::uint32_t size_of(std::uint32_t addr) const;
    std::uint32_t free_bytes() const;

private:
    struct Block {
        std::uint32_t addr, size;
    };
    std::uint32_t begin_, end_;
    std::vector<Block> used_;  // sorted by address
};

}  // namespace ff7cc::rt
