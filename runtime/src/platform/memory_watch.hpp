#pragma once

// A reverse-engineering aid (Windows): which recompiled functions access a range of guest memory. The host pages
// mirroring the range become guard pages; every access raises an exception whose handler records the faulting
// instruction if it touched the range, steps over it (trap flag) and arms the pages again. The report names the
// functions through the PDB (f_XXXXXXXX = the guest function at that address). Elsewhere it does nothing.

#include <cstdint>
#include <cstdio>
#include <memory>

#include "core/foundation/core/foundation.h"

namespace psprecore::platform {

class MemoryWatch {
public:
    MemoryWatch(std::uint32_t address, std::uint32_t size);  // starts watching (one watch at a time)
    ~MemoryWatch();
    MemoryWatch(const MemoryWatch&) = delete;
    MemoryWatch& operator=(const MemoryWatch&) = delete;

    // Stops watching and prints each function that accessed the range, with its access count.
    void report(std::FILE* out);

private:
    struct Impl;
    nx::unique_ptr<Impl> impl_;
};

}  // namespace psprecore::platform
