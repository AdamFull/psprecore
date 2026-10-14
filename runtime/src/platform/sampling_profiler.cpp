#include "platform/sampling_profiler.hpp"

#ifdef _WIN32

// Before <windows.h>: its TRANSPARENT macro (wingdi.h) collides with a name in nx::map.
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/core/foundation.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>

#include "core/foundation/strings/utf8_string.h"


namespace psprecore::platform {

struct SamplingProfiler::Impl {
    std::atomic<bool> stop{false};
    std::thread sampler;
    DWORD sampler_id = 0;
    nx::vector<std::uint64_t> samples;  // instruction pointers

    void run() {
        sampler_id = GetCurrentThreadId();
        nx::hash_map<DWORD, HANDLE> threads;
        nx::vector<std::uint64_t> batch;
        batch.reserve(256);
        for (int tick = 0; !stop.load(std::memory_order_relaxed); ++tick) {
            if (tick % 200 == 0) refresh(threads);
            batch.clear();
            for (const auto& [id, handle] : threads) {
                // Nothing may allocate while a thread is suspended (it could hold the heap lock).
                if (SuspendThread(handle) == static_cast<DWORD>(-1)) continue;
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                const bool ok = GetThreadContext(handle, &ctx) != 0;
                ResumeThread(handle);
                if (ok && batch.size() < batch.capacity()) batch.push_back(ctx.Rip);
            }
            samples.insert(samples.end(), batch.begin(), batch.end());
            Sleep(1);
        }
        for (const auto& [_, handle] : threads) CloseHandle(handle);
    }

    void refresh(nx::hash_map<DWORD, HANDLE>& threads) const {
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        const DWORD pid = GetCurrentProcessId();
        for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == sampler_id || threads.count(te.th32ThreadID)) continue;
            const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                        te.th32ThreadID);
            if (h) threads.emplace(te.th32ThreadID, h);
        }
        CloseHandle(snap);
    }
};

SamplingProfiler::SamplingProfiler() : impl_(std::make_unique<Impl>()) {
    impl_->samples.reserve(1 << 20);
    impl_->sampler = std::thread([this] { impl_->run(); });
}

SamplingProfiler::~SamplingProfiler() {
    impl_->stop = true;
    if (impl_->sampler.joinable()) impl_->sampler.join();
}

void SamplingProfiler::report(std::FILE* out, int top) {
    impl_->stop = true;
    if (impl_->sampler.joinable()) impl_->sampler.join();
    const HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);
    nx::hash_map<std::uint64_t, std::uint64_t> by_address;
    for (const std::uint64_t ip : impl_->samples) ++by_address[ip];
    nx::map<nx::string, std::uint64_t> by_function;
    std::uint64_t counted = 0;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512];
    for (const auto& [ip, n] : by_address) {
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof module;
        const nx::string mod = SymGetModuleInfo64(process, ip, &module) ? module.ModuleName : "?";
        // Threads blocked in the kernel sit in system call stubs: not work.
        if (mod == "ntdll" || mod == "win32u") continue;
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 displacement = 0;
        const nx::string name = SymFromAddr(process, ip, &displacement, sym) ? sym->Name : "?";
        by_function[name + "  (" + mod + ")"] += n;
        counted += n;
    }
    SymCleanup(process);
    nx::vector<std::pair<std::uint64_t, nx::string>> sorted;
    for (const auto& [name, n] : by_function) sorted.emplace_back(n, name);
    std::sort(sorted.rbegin(), sorted.rend());
    std::fprintf(out, "profile: %llu samples outside waits\n", static_cast<unsigned long long>(counted));
    for (int i = 0; i < top && i < static_cast<int>(sorted.size()); ++i)
        std::fprintf(out, "  %5.1f%%  %s\n", 100.0 * static_cast<double>(sorted[i].first) / static_cast<double>(counted),
                     sorted[i].second.c_str());
}

}  // namespace psprecore::platform

#else

namespace psprecore::platform {

struct SamplingProfiler::Impl {};
SamplingProfiler::SamplingProfiler() = default;
SamplingProfiler::~SamplingProfiler() = default;
void SamplingProfiler::report(std::FILE* out, int) { std::fprintf(out, "profile: only available on Windows\n"); }

}  // namespace psprecore::platform

#endif
