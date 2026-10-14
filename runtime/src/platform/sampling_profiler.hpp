#pragma once

// A minimal sampling profiler for performance work (Windows): a background thread samples the instruction pointer of
// every other thread of the process ~1000 times a second; at the end the samples are attributed to functions through
// the PDB (dbghelp) and the hottest are printed. Threads that are waiting (in the kernel) are left out. Elsewhere it
// does nothing (use simpleperf on Android).

#include <cstdio>
#include <memory>

namespace ff7cc::platform {

class SamplingProfiler {
public:
    SamplingProfiler();  // starts sampling
    ~SamplingProfiler();
    SamplingProfiler(const SamplingProfiler&) = delete;
    SamplingProfiler& operator=(const SamplingProfiler&) = delete;

    // Stops sampling and prints the `top` functions with their share of the samples.
    void report(std::FILE* out, int top = 40);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ff7cc::platform
