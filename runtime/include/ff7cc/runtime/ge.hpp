#pragma once

// GE (PSP GPU) command processor. Display lists are sequences of 32-bit words: command in bits 24-31,
// parameter in bits 0-23. This module walks lists in guest memory (jumps, calls, BASE/OFFSET addressing,
// stall addresses) and keeps the register state; draw calls are handed to a backend (none yet, see Phase 5).

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ff7cc::rt {

class GuestMemory;

namespace ge {

enum Cmd : std::uint8_t {
    NOP = 0x00, VADDR = 0x01, IADDR = 0x02, PRIM = 0x04, BEZIER = 0x05, SPLINE = 0x06, BOUNDINGBOX = 0x07,
    JUMP = 0x08, BJUMP = 0x09, CALL = 0x0A, RET = 0x0B, END = 0x0C, SIGNAL = 0x0E, FINISH = 0x0F,
    BASE = 0x10, VERTEXTYPE = 0x12, OFFSETADDR = 0x13, ORIGIN = 0x14,
    TRANSFERSTART = 0xEA,
};

std::string_view command_name(std::uint8_t cmd);

struct Draw {
    std::uint32_t list_pc = 0;  // address of the PRIM command
    std::uint32_t prim = 0;     // GE primitive type (0 points .. 6 sprites)
    std::uint32_t count = 0;
    std::uint32_t vertex_type = 0;
    std::uint32_t vertices = 0, indices = 0;  // guest addresses
};

struct Stats {
    std::uint64_t lists = 0, commands = 0, draws = 0, vertices = 0, transfers = 0;
};

class Processor {
public:
    explicit Processor(GuestMemory& memory) : memory_(memory) {}

    // sceGeListEnQueue(Head): runs the list until it ends or reaches the stall address. Returns a list id.
    int enqueue(std::uint32_t list, std::uint32_t stall, bool head);
    // sceGeListUpdateStallAddr: resumes a stalled list.
    void update_stall(int id, std::uint32_t stall);
    bool list_done(int id) const;

    const std::array<std::uint32_t, 256>& registers() const { return regs_; }
    const Stats& stats() const { return stats_; }

    // Optional listeners.
    std::function<void(const Draw&)> on_draw;
    std::function<void(std::uint32_t pc, std::uint32_t word)> on_command;  // every executed command (dumps)

private:
    struct List {
        std::uint32_t pc = 0, stall = 0;
        bool done = false;
        std::vector<std::uint32_t> call_stack;
    };
    void run(List& list);
    std::uint32_t address(std::uint32_t param) const { return ((base_ | param) + offset_) & 0x0FFFFFFF; }

    GuestMemory& memory_;
    std::array<std::uint32_t, 256> regs_{};
    std::uint32_t base_ = 0, offset_ = 0, vaddr_ = 0, iaddr_ = 0;
    std::map<int, List> lists_;
    int next_id_ = 1;
    Stats stats_;
};

// One-line human-readable decoding of a command word (used by --ge-dump).
std::string describe(std::uint32_t word);

}  // namespace ge
}  // namespace ff7cc::rt
