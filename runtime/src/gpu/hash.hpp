#pragma once

// Content hashing shared by the renderers (texture caches).

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ff7cc::rt::gpu {

// Fast non-cryptographic hash. Four independent lanes keep the multipliers busy (several bytes per cycle).
inline std::uint64_t hash_bytes(const void* data, std::size_t size, std::uint64_t h = 0x9E3779B97F4A7C15ull) {
    const auto* b = static_cast<const std::uint8_t*>(data);
    constexpr std::uint64_t kMul = 0xFF51AFD7ED558CCDull;
    std::size_t i = 0;
    if (size >= 32) {
        std::uint64_t lane[4] = {h, h ^ 0x2545F4914F6CDD1Dull, h ^ 0x94D049BB133111EBull, h ^ 0xBF58476D1CE4E5B9ull};
        for (; i + 32 <= size; i += 32)
            for (int k = 0; k < 4; ++k) {
                std::uint64_t w;
                std::memcpy(&w, b + i + k * 8, 8);
                lane[k] = (lane[k] ^ w) * kMul;
                lane[k] ^= lane[k] >> 29;
            }
        h = lane[0] ^ (lane[1] * 3) ^ (lane[2] * 5) ^ (lane[3] * 7);
    }
    for (; i + 8 <= size; i += 8) {
        std::uint64_t w;
        std::memcpy(&w, b + i, 8);
        h = (h ^ w) * kMul;
        h ^= h >> 32;
    }
    for (; i < size; ++i) h = (h ^ b[i]) * 0x100000001B3ull;
    return (h ^ (h >> 31)) * kMul;
}

}  // namespace ff7cc::rt::gpu
