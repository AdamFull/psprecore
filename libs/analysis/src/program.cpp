#include "ff7cc/analysis/program.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <set>

#include "ff7cc/error.hpp"

namespace ff7cc::analysis {

using mips::Op;

namespace {

constexpr std::uint32_t kRa = 31, kSp = 29;

bool is_jr_ra(const mips::Instruction& in) { return in.op == Op::Jr && in.rs() == kRa; }

// GPR written by an instruction (0 = none / $zero). Covers everything the simulation can't model exactly.
std::uint32_t written_gpr(const mips::Instruction& in) {
    switch (in.op) {
        case Op::Sll: case Op::Srl: case Op::Rotr: case Op::Sra: case Op::Sllv: case Op::Srlv: case Op::Rotrv:
        case Op::Srav: case Op::Movz: case Op::Movn: case Op::Mfhi: case Op::Mflo: case Op::Clz: case Op::Clo:
        case Op::Add: case Op::Addu: case Op::Sub: case Op::Subu: case Op::And: case Op::Or: case Op::Xor:
        case Op::Nor: case Op::Slt: case Op::Sltu: case Op::Max: case Op::Min: case Op::Wsbh: case Op::Wsbw:
        case Op::Seb: case Op::Seh: case Op::Bitrev: case Op::Jalr:
            return in.rd();
        case Op::Addi: case Op::Addiu: case Op::Slti: case Op::Sltiu: case Op::Andi: case Op::Ori: case Op::Xori:
        case Op::Lui: case Op::Lb: case Op::Lh: case Op::Lwl: case Op::Lw: case Op::Lbu: case Op::Lhu: case Op::Lwr:
        case Op::Ll: case Op::Sc: case Op::Ext: case Op::Ins: case Op::Mfc1: case Op::Cfc1: case Op::Mfv:
        case Op::Mfvc: case Op::Mfic:
            return in.rt();
        case Op::Jal: case Op::Bltzal: case Op::Bgezal: case Op::Bltzall: case Op::Bgezall:
            return kRa;
        default:
            return 0;
    }
}

struct Value {
    bool known = false;
    std::uint32_t value = 0;
    std::uint8_t segment = 0;  // address segment from relocations (value is relative to its base)
};

}  // namespace

Program::Program(const elf::File& file, prx::Module module) : module_(std::move(module)) {
    std::uint32_t end = 0;
    for (const auto& s : module_.segments)
        if (s.type == 1) end = std::max(end, s.vaddr + s.memsz);
    image_.resize(end);
    for (const auto& s : module_.segments) {
        if (s.type != 1) continue;
        const auto src = file.data().subspan(s.offset, s.filesz);
        std::copy(src.begin(), src.end(), image_.begin() + s.vaddr);
    }
    const auto* text = file.find_section(".text");
    if (!text) throw FormatError("module has no .text section");
    text_begin_ = text->addr;
    text_end_ = text->addr + text->size;
    for (const auto& s : file.sections())
        if (s.name.starts_with(".sceStub.text")) text_end_ = std::max(text_end_, s.addr + s.size);
    for (std::size_t i = 0; i < module_.relocations.size(); ++i) {
        const auto& r = module_.relocations[i];
        reloc_by_addr_.emplace(segment_base(r.offset_segment) + r.offset, i);
    }
}

std::uint32_t Program::segment_base(std::uint8_t index) const {
    return index < module_.segments.size() ? module_.segments[index].vaddr : 0;
}

std::optional<std::uint32_t> Program::word(std::uint32_t addr) const {
    if (addr % 4 || addr + 4 > image_.size()) return std::nullopt;
    std::uint32_t w;
    std::memcpy(&w, image_.data() + addr, 4);
    return w;
}

mips::Instruction Program::insn(std::uint32_t addr) const { return mips::decode(word(addr).value_or(0), addr); }

std::string Program::string_at(std::uint32_t addr, std::size_t max_len) const {
    std::string s;
    for (std::uint32_t a = addr; a < image_.size() && s.size() <= max_len; ++a) {
        const auto c = std::to_integer<unsigned char>(image_[a]);
        if (c == 0) return s.size() >= 2 ? s : std::string();
        if (c < 0x20 || c >= 0x7F) return {};
        s += static_cast<char>(c);
    }
    return {};
}

const prx::Relocation* Program::relocation_at(std::uint32_t addr) const {
    const auto it = reloc_by_addr_.find(addr);
    return it == reloc_by_addr_.end() ? nullptr : &module_.relocations[it->second];
}

std::vector<Function> discover_functions(const Program& p) {
    std::map<std::uint32_t, Function> funcs;
    const auto seed = [&](std::uint32_t addr, Function::Origin origin, std::string name = {}) {
        if (!p.in_text(addr) || addr % 4) return;
        auto [it, inserted] = funcs.try_emplace(addr, Function{addr, 0, std::move(name), origin});
        if (!inserted && static_cast<int>(origin) < static_cast<int>(it->second.origin)) {
            it->second.origin = origin;
            if (!name.empty()) it->second.name = std::move(name);
        }
    };

    seed(p.module().entry, Function::Origin::Entry, "module_entry");
    for (const auto& exp : p.module().exports)
        for (const auto& f : exp.functions) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "export_%08X", f.nid);
            seed(f.address, Function::Origin::Export, f.nid == 0xD632ACDB ? "module_start" : f.nid == 0xCEE8593C ? "module_stop" : buf);
        }
    for (const auto& imp : p.module().imports)
        for (const auto& f : imp.functions) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s_%08X", imp.library.c_str(), f.nid);
            seed(f.stub, Function::Origin::Import, buf);
        }

    // Direct calls.
    for (std::uint32_t pc = p.text_begin(); pc < p.text_end(); pc += 4) {
        const auto in = p.insn(pc);
        if (in.op == Op::Jal) seed(in.jump_target(), Function::Origin::Call);
        else if (in.op == Op::Bgezal && in.rs() == 0) seed(in.branch_target(), Function::Origin::Call);
    }

    // Code pointers (R32 in data, HI16/LO16 pairs in code). Jump-table entries also point into .text,
    // so a pointer only counts if it looks like a function start.
    const auto looks_like_start = [&](std::uint32_t addr) {
        if (funcs.contains(addr)) return true;
        const auto first = p.insn(addr);
        if (first.op == Op::Addiu && first.rt() == kSp && first.rs() == kSp && first.simm() < 0) return true;
        return addr >= p.text_begin() + 8 && is_jr_ra(p.insn(addr - 8));
    };
    std::vector<std::uint32_t> pointers;
    std::uint32_t hi_value = 0;
    for (const auto& r : p.module().relocations) {
        const std::uint32_t at = p.segment_base(r.offset_segment) + r.offset;
        const auto w = p.word(at);
        if (!w) continue;
        if (r.type == prx::kRel32 && r.address_segment == 0) pointers.push_back(*w);
        if (r.type == prx::kRelHi16) hi_value = (*w & 0xFFFF) << 16;
        if (r.type == prx::kRelLo16 && r.address_segment == 0)
            pointers.push_back(hi_value + static_cast<std::uint32_t>(static_cast<std::int16_t>(*w & 0xFFFF)));
    }
    for (auto ptr : pointers)
        if (p.in_text(ptr) && ptr % 4 == 0 && looks_like_start(ptr)) seed(ptr, Function::Origin::Pointer);

    // Gap filling: a function ends after `jr ra` + delay slot when no earlier branch jumps past it and no
    // computed jump (switch) precedes it; the next non-nop instruction then starts a new function.
    seed(p.text_begin(), Function::Origin::Gap);
    for (auto it = funcs.begin(); it != funcs.end(); ++it) {
        const std::uint32_t start = it->first;
        const auto next_it = std::next(it);
        const std::uint32_t limit = next_it == funcs.end() ? p.text_end() : next_it->first;
        std::uint32_t furthest = 0;
        bool computed_jump = false;
        for (std::uint32_t pc = start; pc < limit; pc += 4) {
            const auto in = p.insn(pc);
            if (in.is_branch()) furthest = std::max(furthest, in.branch_target());
            if (in.op == Op::J && in.jump_target() >= start && in.jump_target() < limit)
                furthest = std::max(furthest, in.jump_target());
            if (in.op == Op::Jr && in.rs() != kRa) computed_jump = true;
            if (!is_jr_ra(in) || computed_jump || furthest > pc + 4) continue;
            std::uint32_t next = pc + 8;
            while (next < limit && p.word(next).value_or(1) == 0) next += 4;  // nop padding
            if (next < limit) seed(next, Function::Origin::Gap);  // becomes std::next(it), scanned next
            break;
        }
    }

    std::vector<Function> out;
    for (auto& [a, f] : funcs) out.push_back(std::move(f));
    for (std::size_t i = 0; i < out.size(); ++i) out[i].end = i + 1 < out.size() ? out[i + 1].start : p.text_end();
    return out;
}

std::vector<Registration> find_registrations(const Program& p, const std::vector<Function>& functions) {
    std::vector<Registration> out;
    for (const auto& f : functions) {
        if (f.origin == Function::Origin::Import) continue;
        std::array<Value, 32> regs{};
        const auto exec = [&](std::uint32_t pc) {
            const auto in = p.insn(pc);
            const auto* reloc = p.relocation_at(pc);
            const std::uint8_t seg = reloc ? reloc->address_segment : 0;
            Value result;
            std::uint32_t dest = 0;
            switch (in.op) {
                case Op::Lui:
                    dest = in.rt();
                    result = {true, in.uimm() << 16, seg};
                    break;
                case Op::Addiu:
                    dest = in.rt();
                    if (in.rs() == 0) result = {true, static_cast<std::uint32_t>(in.simm()), 0};
                    else if (regs[in.rs()].known)
                        result = {true, regs[in.rs()].value + static_cast<std::uint32_t>(in.simm()),
                                  reloc ? seg : regs[in.rs()].segment};
                    break;
                case Op::Ori:
                    dest = in.rt();
                    if (in.rs() == 0) result = {true, in.uimm(), 0};
                    else if (regs[in.rs()].known) result = {true, regs[in.rs()].value | in.uimm(), regs[in.rs()].segment};
                    break;
                case Op::Addu: case Op::Or:
                    dest = in.rd();
                    if (in.rt() == 0 && in.rs() != 0) result = regs[in.rs()];
                    else if (in.rs() == 0 && in.rt() != 0) result = regs[in.rt()];
                    break;
                default:
                    dest = written_gpr(in);
                    break;
            }
            if (dest != 0) regs[dest] = result;
        };
        const auto address = [&](const Value& v) { return v.value + p.segment_base(v.segment); };

        for (std::uint32_t pc = f.start; pc < f.end; pc += 4) {
            const auto in = p.insn(pc);
            if (in.op != Op::Jal && in.op != Op::Jalr) {
                exec(pc);
                continue;
            }
            exec(pc + 4);  // delay slot runs before the call
            if (in.op == Op::Jal && regs[4].known && regs[5].known) {
                const auto name = p.string_at(address(regs[4]));
                const auto handler = address(regs[5]);
                if (!name.empty() && p.in_text(handler))
                    out.push_back(Registration{pc, in.jump_target(), name, handler});
            }
            for (std::uint32_t r : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 24u, 25u, 31u})
                regs[r] = {};
            pc += 4;  // skip the delay slot (already executed)
        }
    }
    return out;
}

}  // namespace ff7cc::analysis
