#pragma once

// Static analysis of a PRX module (module-relative addresses, i.e. load base 0): function discovery and
// recovery of names from the game's own registration calls.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ff7cc/formats/elf.hpp"
#include "ff7cc/formats/prx.hpp"
#include "ff7cc/mips/allegrex.hpp"

namespace ff7cc::analysis {

class Program {
public:
    Program(const elf::File& file, prx::Module module);

    const prx::Module& module() const { return module_; }
    std::uint32_t text_begin() const { return text_begin_; }
    std::uint32_t text_end() const { return text_end_; }
    bool in_text(std::uint32_t addr) const { return addr >= text_begin_ && addr < text_end_; }

    std::optional<std::uint32_t> word(std::uint32_t addr) const;  // file-backed memory only
    mips::Instruction insn(std::uint32_t addr) const;
    // NUL-terminated printable string at `addr` (empty if not a plausible string).
    std::string string_at(std::uint32_t addr, std::size_t max_len = 128) const;

    // Relocation patching the word at `addr`, if any.
    const prx::Relocation* relocation_at(std::uint32_t addr) const;
    std::uint32_t segment_base(std::uint8_t index) const;

private:
    prx::Module module_;
    std::vector<std::byte> image_;  // unrelocated loadable image, BSS zero-filled
    std::uint32_t text_begin_ = 0, text_end_ = 0;
    std::map<std::uint32_t, std::size_t> reloc_by_addr_;
};

struct Function {
    std::uint32_t start = 0;
    std::uint32_t end = 0;  // exclusive
    std::string name;       // empty if unknown
    enum class Origin : std::uint8_t { Entry, Export, Import, Call, Pointer, Gap } origin = Origin::Gap;
};

// Switch jump tables: `jr rX` (rX != ra) whose target is loaded from a table of R32-relocated code addresses.
// The table address is recovered by evaluating the load's base register backwards (lui/addiu/ori/addu);
// the entry count comes from a preceding `sltiu idx, N` bound, or else the run of consecutive R32 entries.
struct JumpTable {
    std::uint32_t jr = 0;     // address of the jr instruction
    std::uint32_t table = 0;  // address of the table
    std::vector<std::uint32_t> targets;
};
std::map<std::uint32_t, JumpTable> find_jump_tables(const Program& program);

// Every instruction of .text ends up in exactly one function (sorted by start).
// Jump-table targets are never treated as function starts.
std::vector<Function> discover_functions(const Program& program);

struct Registration {
    std::uint32_t call_site = 0;  // address of the jal
    std::uint32_t registrar = 0;  // called function
    std::string name;             // string argument
    std::uint32_t handler = 0;    // code-pointer argument
};

// Calls of the form f("Name", handler, ...) — string in a0 and code pointer in a1 — found by simulating
// constant loads (lui/addiu/ori + relocations) linearly through each function.
std::vector<Registration> find_registrations(const Program& program, const std::vector<Function>& functions);

}  // namespace ff7cc::analysis
