#pragma once

// Allegrex -> C17 translator (see docs/recompiler.md).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ff7cc/analysis/program.hpp"

namespace ff7cc::recomp {

struct Options {
    std::uint32_t base = 0x08804000;  // load address the code is translated for
};

struct Stats {
    std::size_t functions = 0;
    std::size_t instructions = 0;
    std::size_t jump_tables = 0;
    std::size_t tail_calls = 0;
    std::size_t null_calls = 0;  // jal 0x0 to unlinked weak symbols
    std::vector<std::string> warnings;  // unexpected control flow, each with its address
};

class Generator {
public:
    // `image` is the module relocated to options.base (prx::load_image).
    Generator(const analysis::Program& program, std::vector<std::byte> image, std::vector<analysis::Function> functions,
              Options options);

    // C source of one function (module-relative start).
    std::string function_source(const analysis::Function& fn);
    // `void f_XXXXXXXX(Context*);` for every function.
    std::string header() const;
    // Address -> function table and recomp_lookup().
    std::string lookup_source() const;

    const std::vector<analysis::Function>& functions() const { return functions_; }
    const Stats& stats() const { return stats_; }

private:
    std::uint32_t word(std::uint32_t abs) const;  // from the relocated image
    std::string name_of(std::uint32_t abs) const;
    bool is_function_start(std::uint32_t abs) const;

    const analysis::Program& program_;
    std::vector<std::byte> image_;
    std::vector<analysis::Function> functions_;
    Options options_;
    std::map<std::uint32_t, std::size_t> by_start_;      // absolute start -> index in functions_
    std::map<std::uint32_t, std::uint32_t> import_nids_;  // absolute stub address -> NID
    std::map<std::uint32_t, std::vector<std::uint32_t>> jump_tables_;  // absolute jr -> absolute targets
    Stats stats_;
};

}  // namespace ff7cc::recomp
