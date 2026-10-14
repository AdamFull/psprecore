#pragma once

// Allegrex -> C17 translator (see docs/recompiler.md).

#include <cstdint>

#include "psprecore/analysis/program.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore::recomp {

struct Options {
    std::uint32_t base = 0x08804000;  // load address the code is translated for
    // Functions replaced by hand-written code (GAME_FUNC in game/): their translation is emitted as
    // f_XXXXXXXX_orig, and every call and the lookup table reach the replacement f_XXXXXXXX.
    nx::set<std::uint32_t> overrides;
};

struct Stats {
    std::size_t functions = 0;
    std::size_t instructions = 0;
    std::size_t jump_tables = 0;
    std::size_t tail_calls = 0;
    std::size_t null_calls = 0;  // jal 0x0 to unlinked weak symbols
    std::size_t overrides = 0;   // functions emitted as _orig because game/ replaces them
    nx::vector<nx::string> warnings;  // unexpected control flow, each with its address
};

class Generator {
public:
    // `image` is the module relocated to options.base (prx::load_image).
    Generator(const analysis::Program& program, nx::vector<std::byte> image, nx::vector<analysis::Function> functions,
              Options options);

    // C source of one function (module-relative start).
    nx::string function_source(const analysis::Function& fn);
    // `void f_XXXXXXXX(Context*);` for every function.
    nx::string header() const;
    // Address -> function table and recomp_lookup().
    nx::string lookup_source() const;

    const nx::vector<analysis::Function>& functions() const { return functions_; }
    const Stats& stats() const { return stats_; }

private:
    std::uint32_t word(std::uint32_t abs) const;  // from the relocated image
    nx::string name_of(std::uint32_t abs) const;
    bool is_function_start(std::uint32_t abs) const;

    const analysis::Program& program_;
    nx::vector<std::byte> image_;
    nx::vector<analysis::Function> functions_;
    Options options_;
    nx::map<std::uint32_t, std::size_t> by_start_;      // absolute start -> index in functions_
    nx::map<std::uint32_t, std::uint32_t> import_nids_;  // absolute stub address -> NID
    nx::map<std::uint32_t, nx::vector<std::uint32_t>> jump_tables_;  // absolute jr -> absolute targets
    Stats stats_;
};

}  // namespace psprecore::recomp
