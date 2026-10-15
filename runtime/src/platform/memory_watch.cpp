#include "platform/memory_watch.hpp"

#ifdef _WIN32

// Before <windows.h>: its TRANSPARENT macro (wingdi.h) collides with a name in nx::map.
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/core/foundation.h"

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>

#include "psprecore/recomp/recomp.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/threading/sync.h"

namespace psprecore::platform {

namespace {

struct Hits {
    std::uint64_t count = 0;
    std::uint32_t first = 0xFFFFFFFFu, last = 0;  // offsets into the range
};

// The watch as the exception handler sees it.
struct WatchState {
    std::uintptr_t lo = 0, hi = 0;            // the watched host range
    std::uintptr_t page_lo = 0, page_hi = 0;  // the guard pages covering it
    std::uint32_t guest = 0;                  // the range's guest address
    void* handler = nullptr;
    std::atomic<bool> active{false};
    nx::mutex mutex;
    nx::hash_map<std::uint64_t, Hits> hits;  // by instruction pointer

    void protect(bool guard) const {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(page_lo), page_hi - page_lo,
                       guard ? PAGE_READWRITE | PAGE_GUARD : PAGE_READWRITE, &old);
    }
};

WatchState* g_watch = nullptr;
thread_local bool tl_rearm = false;

LONG CALLBACK on_exception(EXCEPTION_POINTERS* e) {
    WatchState* w = g_watch;
    if (!w) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == STATUS_GUARD_PAGE_VIOLATION) {
        const auto at = static_cast<std::uintptr_t>(e->ExceptionRecord->ExceptionInformation[1]);
        if (at < w->page_lo || at >= w->page_hi) return EXCEPTION_CONTINUE_SEARCH;
        if (at >= w->lo && at < w->hi) {
            nx::scoped_lock lock(w->mutex);
            Hits& h = w->hits[e->ContextRecord->Rip];
            const auto offset = static_cast<std::uint32_t>(at - w->lo);
            ++h.count;
            h.first = std::min(h.first, offset);
            h.last = std::max(h.last, offset);
        }
        // The guard is gone after this access: step over the instruction, then arm the pages again.
        e->ContextRecord->EFlags |= 0x100;
        tl_rearm = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == STATUS_SINGLE_STEP && tl_rearm) {
        tl_rearm = false;
        if (w->active.load(std::memory_order_relaxed)) w->protect(true);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

struct MemoryWatch::Impl : WatchState {
    void stop() {
        if (!active) return;
        active = false;
        protect(false);
        RemoveVectoredExceptionHandler(handler);
        g_watch = nullptr;
    }
};

MemoryWatch::MemoryWatch(std::uint32_t address, std::uint32_t size) : impl_(nx::make_unique<Impl>()) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const std::uintptr_t page = si.dwPageSize;
    Impl& w = *impl_;
    w.lo = reinterpret_cast<std::uintptr_t>(RECOMP_MEM(address));
    w.hi = w.lo + size;
    w.page_lo = w.lo / page * page;
    w.page_hi = (w.hi + page - 1) / page * page;
    w.guest = address;
    g_watch = &w;
    w.handler = AddVectoredExceptionHandler(1, on_exception);
    w.active = true;
    w.protect(true);
    std::fprintf(stderr, "watch: 0x%08X..0x%08X\n", address, address + size);
}

MemoryWatch::~MemoryWatch() { impl_->stop(); }

void MemoryWatch::report(std::FILE* out) {
    impl_->stop();
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    nx::map<nx::string, Hits> by_function;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (const auto& [ip, h] : impl_->hits) {
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 displacement = 0;
        Hits& f = by_function[SymFromAddr(process, ip, &displacement, sym) ? nx::string(sym->Name) : nx::string("?")];
        f.count += h.count;
        f.first = std::min(f.first, h.first);
        f.last = std::max(f.last, h.last);
    }
    SymCleanup(process);
    nx::vector<std::pair<std::uint64_t, nx::string>> sorted;
    for (const auto& [name, h] : by_function) sorted.emplace_back(h.count, name);
    std::sort(sorted.rbegin(), sorted.rend());
    std::fprintf(out, "watch: %zu functions (accesses, guest addresses touched)\n", sorted.size());
    for (const auto& [n, name] : sorted) {
        const Hits& h = by_function[name];
        std::fprintf(out, "  %8llu  0x%08X..0x%08X  %s\n", static_cast<unsigned long long>(n), impl_->guest + h.first,
                     impl_->guest + h.last, name.c_str());
    }
}

}  // namespace psprecore::platform

#else

namespace psprecore::platform {

struct MemoryWatch::Impl {};
MemoryWatch::MemoryWatch(std::uint32_t, std::uint32_t) {}
MemoryWatch::~MemoryWatch() = default;
void MemoryWatch::report(std::FILE* out) { std::fprintf(out, "watch: only available on Windows\n"); }

}  // namespace psprecore::platform

#endif
