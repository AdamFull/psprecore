// Renders instructions like binutils' psp-objdump (Allegrex patch set), so the decoder can be diffed against it.

#include <cctype>
#include <cstdio>
#include <cstring>

#include "ff7cc/mips/allegrex.hpp"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/strings/format.h"

namespace ff7cc::mips {

namespace {

constexpr const char* kGpr[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                  "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                  "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"};
constexpr const char* kFpuCond[16] = {"f",  "un",   "eq",  "ueq", "olt", "ult", "ole", "ule",
                                      "sf", "ngle", "seq", "ngl", "lt",  "nge", "le",  "ngt"};
constexpr const char* kVcmpCond[16] = {"FL", "EQ", "LT", "LE", "TR", "NE", "GE", "GT",
                                       "EZ", "EN", "EI", "ES", "NZ", "NN", "NI", "NS"};
constexpr const char* kVcst[32] = {
    "VFPU_HUGE?0",   "VFPU_HUGE",     "VFPU_SQRT2", "VFPU_SQRT1_2",  "VFPU_2_SQRTPI", "VFPU_2_PI",
    "VFPU_1_PI",     "VFPU_PI_4",     "VFPU_PI_2",  "VFPU_PI",       "VFPU_E",        "VFPU_LOG2E",
    "VFPU_LOG10E",   "VFPU_LN2",      "VFPU_LN10",  "VFPU_2PI",      "VFPU_PI_6",     "VFPU_LOG10TWO",
    "VFPU_LOG2TEN",  "VFPU_SQRT3_2",  "VFPU_20",    "VFPU_21",       "VFPU_22",       "VFPU_23",
    "VFPU_24",       "VFPU_25",       "VFPU_26",    "VFPU_27",       "VFPU_28",       "VFPU_29",
    "VFPU_30",       "VFPU_31"};
constexpr char kSizeChar[5] = {'?', 's', 'p', 't', 'q'};

nx::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%x", v);
    return buf;
}

nx::string fpr(std::uint32_t r) { return "$f" + nx::format("{}", r); }

nx::string mem(std::int32_t off, std::uint32_t base) { return nx::format("{}", off) + "(" + kGpr[base] + ")"; }

// VFPU register name: S (single), C/R (column/row vector), M/E (matrix / transposed matrix).
nx::string vreg(std::uint32_t r, int size, bool matrix = false) {
    const unsigned m = (r >> 2) & 7, i = r & 3;
    const bool transposed = (r >> 5) & 1;
    char kind;
    unsigned start;
    switch (size) {
        case 1:
            kind = 'S';
            start = (r >> 5) & 3;
            break;
        case 2:
            kind = matrix ? (transposed ? 'E' : 'M') : (transposed ? 'R' : 'C');
            start = (r >> 6) & 1 ? 2 : 0;
            break;
        case 3:
            kind = matrix ? (transposed ? 'E' : 'M') : (transposed ? 'R' : 'C');
            start = (r >> 6) & 1;
            break;
        default:
            kind = matrix ? (transposed ? 'E' : 'M') : (transposed ? 'R' : 'C');
            start = 0;
            break;
    }
    char buf[16];
    // Transposed forms (R, E) name the start index before the row/column index.
    if (size > 1 && transposed)
        std::snprintf(buf, sizeof buf, "%c%u%u%u.%c", kind, m, start, i, kSizeChar[size]);
    else
        std::snprintf(buf, sizeof buf, "%c%u%u%u.%c", kind, m, i, start, kSizeChar[size]);
    return buf;
}

nx::string half_float(std::uint16_t h) {
    const int sign = h >> 15, exp = (h >> 10) & 31, frac = h & 0x3FF;
    float f;
    if (exp == 0) f = static_cast<float>(frac) / 1024.0f / 16384.0f;
    else if (exp == 31) f = frac ? 0.0f / 0.0f : 1.0f / 0.0f;
    else {
        f = 1.0f + static_cast<float>(frac) / 1024.0f;
        for (int e = exp - 15; e > 0; --e) f *= 2.0f;
        for (int e = exp - 15; e < 0; ++e) f /= 2.0f;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", sign ? -f : f);
    return buf;
}

nx::string prefix_lanes(std::uint32_t w, bool dest) {
    static constexpr const char* kSwz[4] = {"x", "y", "z", "w"};
    static constexpr const char* kConst[8] = {"0", "1", "2", "1/2", "3", "1/3", "1/4", "1/6"};
    nx::string out;
    for (int lane = 0; lane < 4; ++lane) {
        if (lane) out += ',';
        if (dest) {
            const auto sat = (w >> (lane * 2)) & 3;
            if ((w >> (8 + lane)) & 1) out += 'm';
            else if (sat == 1) out += "[0:1]";
            else if (sat == 3) out += "[-1:1]";
            continue;
        }
        const auto swz = (w >> (lane * 2)) & 3;
        const bool abs = (w >> (8 + lane)) & 1, cst = (w >> (12 + lane)) & 1, neg = (w >> (16 + lane)) & 1;
        if (neg) out += '-';
        if (cst) out += kConst[swz + (abs ? 4 : 0)];
        else if (abs) out += nx::string("|") + kSwz[swz] + "|";
        else out += kSwz[swz];
    }
    return out;
}

nx::string rot_lanes(std::uint32_t w, int size) {
    const auto imm = (w >> 16) & 31;
    const auto sin_pos = (imm >> 2) & 3, cos_pos = imm & 3;
    const bool neg = imm & 16;
    nx::string out = "[";
    for (int lane = 0; lane < size; ++lane) {
        if (lane) out += ',';
        if (static_cast<unsigned>(lane) == cos_pos) out += 'c';
        else if (static_cast<unsigned>(lane) == sin_pos || sin_pos == cos_pos) out += neg ? "-s" : "s";
        else out += '0';
    }
    return out + "]";
}

}  // namespace

nx::string format(const Instruction& in) {
    const auto w = in.word;
    const auto rs = in.rs(), rt = in.rt(), rd = in.rd(), sa = in.sa();
    const int sz = in.vsize();
    nx::string m;     // mnemonic
    nx::string args;  // operands
    const auto set = [&](nx::string mn, nx::string a = {}) {
        m = std::move(mn);
        args = std::move(a);
    };
    const auto vsuffix = [&](const char* base, int size) { return nx::string(base) + "." + kSizeChar[size]; };
    const auto r3 = [&](const char* mn) { set(mn, nx::string(kGpr[rd]) + "," + kGpr[rs] + "," + kGpr[rt]); };
    const auto br = [&]() { return hex(in.branch_target()); };

    switch (in.op) {
        case Op::Invalid: set("(bad)"); break;
        case Op::Sll:
            if (w == 0) set("nop");
            else set("sll", nx::string(kGpr[rd]) + "," + kGpr[rt] + "," + hex(sa));
            break;
        case Op::Srl: set("srl", nx::string(kGpr[rd]) + "," + kGpr[rt] + "," + hex(sa)); break;
        case Op::Rotr: set("ror", nx::string(kGpr[rd]) + "," + kGpr[rt] + "," + hex(sa)); break;
        case Op::Sra: set("sra", nx::string(kGpr[rd]) + "," + kGpr[rt] + "," + hex(sa)); break;
        case Op::Sllv: set("sllv", nx::string(kGpr[rd]) + "," + kGpr[rt] + "," + kGpr[rs]); break;
        case Op::Srlv: set("srlv", nx::string(kGpr[rd]) + "," + kGpr[rt] + "," + kGpr[rs]); break;
        case Op::Rotrv: set("rorv", nx::string(kGpr[rd]) + "," + kGpr[rt] + "," + kGpr[rs]); break;
        case Op::Srav: set("srav", nx::string(kGpr[rd]) + "," + kGpr[rt] + "," + kGpr[rs]); break;
        case Op::Jr: set("jr", kGpr[rs]); break;
        case Op::Jalr: set("jalr", rd == 31 ? nx::string(kGpr[rs]) : nx::string(kGpr[rd]) + "," + kGpr[rs]); break;
        case Op::Movz: r3("movz"); break;
        case Op::Movn: r3("movn"); break;
        case Op::Syscall: set("syscall", (w >> 6) & 0xFFFFF ? hex((w >> 6) & 0xFFFFF) : ""); break;
        case Op::Break: set("break", (w >> 6) & 0xFFFFF ? hex((w >> 6) & 0xFFFFF) : ""); break;
        case Op::Sync: set("sync"); break;
        case Op::Mfhi: set("mfhi", kGpr[rd]); break;
        case Op::Mthi: set("mthi", kGpr[rs]); break;
        case Op::Mflo: set("mflo", kGpr[rd]); break;
        case Op::Mtlo: set("mtlo", kGpr[rs]); break;
        case Op::Clz: set("clz", nx::string(kGpr[rd]) + "," + kGpr[rs]); break;
        case Op::Clo: set("clo", nx::string(kGpr[rd]) + "," + kGpr[rs]); break;
        case Op::Mult: set("mult", nx::string(kGpr[rs]) + "," + kGpr[rt]); break;
        case Op::Multu: set("multu", nx::string(kGpr[rs]) + "," + kGpr[rt]); break;
        case Op::Div: set("div", nx::string("zero,") + kGpr[rs] + "," + kGpr[rt]); break;
        case Op::Divu: set("divu", nx::string("zero,") + kGpr[rs] + "," + kGpr[rt]); break;
        case Op::Madd: set("madd", nx::string(kGpr[rs]) + "," + kGpr[rt]); break;
        case Op::Maddu: set("maddu", nx::string(kGpr[rs]) + "," + kGpr[rt]); break;
        case Op::Msub: set("msub", nx::string(kGpr[rs]) + "," + kGpr[rt]); break;
        case Op::Msubu: set("msubu", nx::string(kGpr[rs]) + "," + kGpr[rt]); break;
        case Op::Add: r3("add"); break;
        case Op::Addu:
            if (rt == 0) set("move", nx::string(kGpr[rd]) + "," + kGpr[rs]);
            else r3("addu");
            break;
        case Op::Sub:
            if (rs == 0) set("neg", nx::string(kGpr[rd]) + "," + kGpr[rt]);
            else r3("sub");
            break;
        case Op::Subu:
            if (rs == 0) set("negu", nx::string(kGpr[rd]) + "," + kGpr[rt]);
            else r3("subu");
            break;
        case Op::And: r3("and"); break;
        case Op::Or:
            if (rt == 0) set("move", nx::string(kGpr[rd]) + "," + kGpr[rs]);
            else r3("or");
            break;
        case Op::Xor: r3("xor"); break;
        case Op::Nor: r3("nor"); break;
        case Op::Slt: r3("slt"); break;
        case Op::Sltu: r3("sltu"); break;
        case Op::Max: r3("max"); break;
        case Op::Min: r3("min"); break;
        case Op::Bltz: set("bltz", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::Bgez: set("bgez", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::Bltzl: set("bltzl", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::Bgezl: set("bgezl", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::Bltzal: set("bltzal", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::Bgezal:
            if (rs == 0) set("bal", br());
            else set("bgezal", nx::string(kGpr[rs]) + "," + br());
            break;
        case Op::Bltzall: set("bltzall", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::Bgezall: set("bgezall", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::J: set("j", hex(in.jump_target())); break;
        case Op::Jal: set("jal", hex(in.jump_target())); break;
        case Op::Beq:
            if (rs == 0 && rt == 0) set("b", br());
            else if (rt == 0) set("beqz", nx::string(kGpr[rs]) + "," + br());
            else set("beq", nx::string(kGpr[rs]) + "," + kGpr[rt] + "," + br());
            break;
        case Op::Bne:
            if (rt == 0) set("bnez", nx::string(kGpr[rs]) + "," + br());
            else set("bne", nx::string(kGpr[rs]) + "," + kGpr[rt] + "," + br());
            break;
        case Op::Beql:
            if (rt == 0) set("beqzl", nx::string(kGpr[rs]) + "," + br());
            else set("beql", nx::string(kGpr[rs]) + "," + kGpr[rt] + "," + br());
            break;
        case Op::Bnel:
            if (rt == 0) set("bnezl", nx::string(kGpr[rs]) + "," + br());
            else set("bnel", nx::string(kGpr[rs]) + "," + kGpr[rt] + "," + br());
            break;
        case Op::Blez: set("blez", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::Bgtz: set("bgtz", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::Blezl: set("blezl", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::Bgtzl: set("bgtzl", nx::string(kGpr[rs]) + "," + br()); break;
        case Op::Addi: set("addi", nx::string(kGpr[rt]) + "," + kGpr[rs] + "," + nx::format("{}", in.simm())); break;
        case Op::Addiu:
            if (rs == 0) set("li", nx::string(kGpr[rt]) + "," + nx::format("{}", in.simm()));
            else set("addiu", nx::string(kGpr[rt]) + "," + kGpr[rs] + "," + nx::format("{}", in.simm()));
            break;
        case Op::Slti: set("slti", nx::string(kGpr[rt]) + "," + kGpr[rs] + "," + nx::format("{}", in.simm())); break;
        case Op::Sltiu: set("sltiu", nx::string(kGpr[rt]) + "," + kGpr[rs] + "," + nx::format("{}", in.simm())); break;
        case Op::Andi: set("andi", nx::string(kGpr[rt]) + "," + kGpr[rs] + "," + hex(in.uimm())); break;
        case Op::Ori:
            if (rs == 0) set("li", nx::string(kGpr[rt]) + "," + hex(in.uimm()));
            else set("ori", nx::string(kGpr[rt]) + "," + kGpr[rs] + "," + hex(in.uimm()));
            break;
        case Op::Xori: set("xori", nx::string(kGpr[rt]) + "," + kGpr[rs] + "," + hex(in.uimm())); break;
        case Op::Lui: set("lui", nx::string(kGpr[rt]) + "," + hex(in.uimm())); break;
        case Op::Ext: set("ext", nx::string(kGpr[rt]) + "," + kGpr[rs] + "," + hex(sa) + "," + hex(rd + 1)); break;
        case Op::Ins: set("ins", nx::string(kGpr[rt]) + "," + kGpr[rs] + "," + hex(sa) + "," + hex(rd + 1 - sa)); break;
        case Op::Wsbh: set("wsbh", nx::string(kGpr[rd]) + "," + kGpr[rt]); break;
        case Op::Wsbw: set("wsbw", nx::string(kGpr[rd]) + "," + kGpr[rt]); break;
        case Op::Seb: set("seb", nx::string(kGpr[rd]) + "," + kGpr[rt]); break;
        case Op::Seh: set("seh", nx::string(kGpr[rd]) + "," + kGpr[rt]); break;
        case Op::Bitrev: set("bitrev", nx::string(kGpr[rd]) + "," + kGpr[rt]); break;
        case Op::Lb: set("lb", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Lh: set("lh", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Lwl: set("lwl", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Lw: set("lw", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Lbu: set("lbu", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Lhu: set("lhu", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Lwr: set("lwr", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Sb: set("sb", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Sh: set("sh", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Swl: set("swl", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Sw: set("sw", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Swr: set("swr", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Ll: set("ll", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Sc: set("sc", nx::string(kGpr[rt]) + "," + mem(in.simm(), rs)); break;
        case Op::Cache: set("cache", hex(rt) + "," + mem(in.simm(), rs)); break;
        case Op::Halt: set("halt"); break;
        case Op::Mfic: set("mfic", nx::string(kGpr[rt]) + "," + nx::format("{}", rd)); break;
        case Op::Mtic: set("mtic", nx::string(kGpr[rt]) + "," + nx::format("{}", rd)); break;
        case Op::Mfc0: set("mfc0", nx::string(kGpr[rt]) + ",$" + nx::format("{}", rd)); break;
        case Op::Mtc0: set("mtc0", nx::string(kGpr[rt]) + ",$" + nx::format("{}", rd)); break;
        case Op::Eret: set("eret"); break;

        // FPU
        case Op::Lwc1: set("lwc1", fpr(in.ft()) + "," + mem(in.simm(), rs)); break;
        case Op::Swc1: set("swc1", fpr(in.ft()) + "," + mem(in.simm(), rs)); break;
        case Op::Mfc1: set("mfc1", nx::string(kGpr[rt]) + "," + fpr(in.fs())); break;
        case Op::Mtc1: set("mtc1", nx::string(kGpr[rt]) + "," + fpr(in.fs())); break;
        case Op::Cfc1: set("cfc1", nx::string(kGpr[rt]) + ",$" + nx::format("{}", in.fs())); break;
        case Op::Ctc1: set("ctc1", nx::string(kGpr[rt]) + ",$" + nx::format("{}", in.fs())); break;
        case Op::Bc1f: set("bc1f", br()); break;
        case Op::Bc1t: set("bc1t", br()); break;
        case Op::Bc1fl: set("bc1fl", br()); break;
        case Op::Bc1tl: set("bc1tl", br()); break;
        case Op::AddS: set("add.s", fpr(in.fd()) + "," + fpr(in.fs()) + "," + fpr(in.ft())); break;
        case Op::SubS: set("sub.s", fpr(in.fd()) + "," + fpr(in.fs()) + "," + fpr(in.ft())); break;
        case Op::MulS: set("mul.s", fpr(in.fd()) + "," + fpr(in.fs()) + "," + fpr(in.ft())); break;
        case Op::DivS: set("div.s", fpr(in.fd()) + "," + fpr(in.fs()) + "," + fpr(in.ft())); break;
        case Op::SqrtS: set("sqrt.s", fpr(in.fd()) + "," + fpr(in.fs())); break;
        case Op::AbsS: set("abs.s", fpr(in.fd()) + "," + fpr(in.fs())); break;
        case Op::MovS: set("mov.s", fpr(in.fd()) + "," + fpr(in.fs())); break;
        case Op::NegS: set("neg.s", fpr(in.fd()) + "," + fpr(in.fs())); break;
        case Op::RoundWS: set("round.w.s", fpr(in.fd()) + "," + fpr(in.fs())); break;
        case Op::TruncWS: set("trunc.w.s", fpr(in.fd()) + "," + fpr(in.fs())); break;
        case Op::CeilWS: set("ceil.w.s", fpr(in.fd()) + "," + fpr(in.fs())); break;
        case Op::FloorWS: set("floor.w.s", fpr(in.fd()) + "," + fpr(in.fs())); break;
        case Op::CvtSW: set("cvt.s.w", fpr(in.fd()) + "," + fpr(in.fs())); break;
        case Op::CvtWS: set("cvt.w.s", fpr(in.fd()) + "," + fpr(in.fs())); break;
        case Op::CCondS: set(nx::string("c.") + kFpuCond[w & 15] + ".s", fpr(in.fs()) + "," + fpr(in.ft())); break;

        // VFPU loads/stores and transfers
        case Op::LvS:
        case Op::SvS: {
            const auto vt = ((w >> 16) & 31) | ((w & 3) << 5);
            set(in.op == Op::LvS ? "lv.s" : "sv.s", vreg(vt, 1) + "," + mem(static_cast<std::int16_t>(w & 0xFFFC), rs));
            break;
        }
        case Op::LvQ: case Op::SvQ: case Op::LvlQ: case Op::LvrQ: case Op::SvlQ: case Op::SvrQ: {
            static constexpr const char* kNames[] = {"lv.q", "sv.q", "lvl.q", "lvr.q", "svl.q", "svr.q"};
            const int idx = in.op == Op::LvQ ? 0 : in.op == Op::SvQ ? 1 : in.op == Op::LvlQ ? 2 : in.op == Op::LvrQ ? 3 : in.op == Op::SvlQ ? 4 : 5;
            const auto vt = ((w >> 16) & 31) | ((w & 1) << 5);
            nx::string a = vreg(vt, 4) + "," + mem(static_cast<std::int16_t>(w & 0xFFFC), rs);
            if ((in.op == Op::LvQ || in.op == Op::SvQ) && (w & 2)) a += ",wb";
            set(kNames[idx], a);
            break;
        }
        case Op::Mfv: set("mfv", nx::string(kGpr[rt]) + "," + vreg(in.vd(), 1)); break;
        case Op::Mtv: set("mtv", nx::string(kGpr[rt]) + "," + vreg(in.vd(), 1)); break;
        case Op::Mfvc: set("mfvc", nx::string(kGpr[rt]) + ",$" + nx::format("{}", in.vd())); break;
        case Op::Mtvc: set("mtvc", nx::string(kGpr[rt]) + ",$" + nx::format("{}", in.vd())); break;
        case Op::Bvf: case Op::Bvt: case Op::Bvfl: case Op::Bvtl: {
            static constexpr const char* kNames[] = {"bvf", "bvt", "bvfl", "bvtl"};
            set(kNames[(w >> 16) & 3], nx::format("{}", (w >> 18) & 7) + "," + br());
            break;
        }

        // VFPU binary ops
        case Op::Vadd: case Op::Vsub: case Op::Vsbn: case Op::Vdiv: case Op::Vmul: case Op::Vmin: case Op::Vmax:
        case Op::Vscmp: case Op::Vsge: case Op::Vslt: case Op::Vcrs: {
            const char* name = in.op == Op::Vadd ? "vadd" : in.op == Op::Vsub ? "vsub" : in.op == Op::Vsbn ? "vsbn"
                             : in.op == Op::Vdiv ? "vdiv" : in.op == Op::Vmul ? "vmul" : in.op == Op::Vmin ? "vmin"
                             : in.op == Op::Vmax ? "vmax" : in.op == Op::Vscmp ? "vscmp" : in.op == Op::Vsge ? "vsge"
                             : in.op == Op::Vslt ? "vslt" : "vcrs";
            set(vsuffix(name, sz), vreg(in.vd(), sz) + "," + vreg(in.vs(), sz) + "," + vreg(in.vt(), sz));
            break;
        }
        case Op::Vdot: case Op::Vhdp: case Op::Vdet:
            set(vsuffix(in.op == Op::Vdot ? "vdot" : in.op == Op::Vhdp ? "vhdp" : "vdet", sz),
                vreg(in.vd(), 1) + "," + vreg(in.vs(), sz) + "," + vreg(in.vt(), sz));
            break;
        case Op::Vscl: set(vsuffix("vscl", sz), vreg(in.vd(), sz) + "," + vreg(in.vs(), sz) + "," + vreg(in.vt(), 1)); break;
        case Op::Vcmp: {
            const auto cond = w & 15;
            const nx::string a = nx::string(kVcmpCond[cond]) + "," + vreg(in.vs(), sz) + "," + vreg(in.vt(), sz);
            set(vsuffix("vcmp", sz), a);
            break;
        }

        // VFPU unary group
        case Op::Vmov: case Op::Vabs: case Op::Vneg: case Op::Vsat0: case Op::Vsat1: case Op::Vrcp: case Op::Vrsq:
        case Op::Vsin: case Op::Vcos: case Op::Vexp2: case Op::Vlog2: case Op::Vsqrt: case Op::Vasin: case Op::Vnrcp:
        case Op::Vnsin: case Op::Vrexp2: case Op::Vsrt1: case Op::Vsrt2: case Op::Vbfy1: case Op::Vbfy2:
        case Op::Vocp: case Op::Vsrt3: case Op::Vsrt4: case Op::Vsgn: case Op::Vsbz: case Op::Vlgb: {
            nx::string name(op_name(in.op));
            for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            set(vsuffix(name.c_str(), sz), vreg(in.vd(), sz) + "," + vreg(in.vs(), sz));
            break;
        }
        case Op::Vidt: case Op::Vzero: case Op::Vone: case Op::Vrndi: case Op::Vrndf1: case Op::Vrndf2: {
            nx::string name(op_name(in.op));
            for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            set(vsuffix(name.c_str(), sz), vreg(in.vd(), sz));
            break;
        }
        case Op::Vsocp: set(vsuffix("vsocp", sz), vreg(in.vd(), sz * 2 > 4 ? 4 : sz * 2) + "," + vreg(in.vs(), sz)); break;
        case Op::Vrnds: set(vsuffix("vrnds", sz), vreg(in.vs(), sz)); break;
        case Op::Vfad: case Op::Vavg:
            set(vsuffix(in.op == Op::Vfad ? "vfad" : "vavg", sz), vreg(in.vd(), 1) + "," + vreg(in.vs(), sz));
            break;
        case Op::Vf2h: set(vsuffix("vf2h", sz), vreg(in.vd(), sz / 2 > 0 ? sz / 2 : 1) + "," + vreg(in.vs(), sz)); break;
        case Op::Vh2f: set(vsuffix("vh2f", sz), vreg(in.vd(), sz * 2 > 4 ? 4 : sz * 2) + "," + vreg(in.vs(), sz)); break;
        case Op::Vi2uc: case Op::Vi2c:
            set(vsuffix(in.op == Op::Vi2uc ? "vi2uc" : "vi2c", sz), vreg(in.vd(), 1) + "," + vreg(in.vs(), sz));
            break;
        case Op::Vi2us: case Op::Vi2s:
            set(vsuffix(in.op == Op::Vi2us ? "vi2us" : "vi2s", sz), vreg(in.vd(), sz / 2 > 0 ? sz / 2 : 1) + "," + vreg(in.vs(), sz));
            break;
        case Op::Vuc2i: case Op::Vc2i:
            set(vsuffix(in.op == Op::Vuc2i ? "vuc2i" : "vc2i", sz), vreg(in.vd(), 4) + "," + vreg(in.vs(), sz));
            break;
        case Op::Vus2i: case Op::Vs2i:
            set(vsuffix(in.op == Op::Vus2i ? "vus2i" : "vs2i", sz), vreg(in.vd(), sz * 2 > 4 ? 4 : sz * 2) + "," + vreg(in.vs(), sz));
            break;
        case Op::Vt4444: case Op::Vt5551: case Op::Vt5650: {
            const char* name = in.op == Op::Vt4444 ? "vt4444" : in.op == Op::Vt5551 ? "vt5551" : "vt5650";
            set(vsuffix(name, sz), vreg(in.vd(), sz / 2 > 0 ? sz / 2 : 1) + "," + vreg(in.vs(), sz));
            break;
        }
        case Op::Vmfvc: set(vsuffix("vmfvc", sz), vreg(in.vd(), sz) + ",$" + nx::format("{}", in.vs() + 128)); break;
        case Op::Vmtvc: set(vsuffix("vmtvc", sz), "$" + nx::format("{}", in.vd() + 128) + "," + vreg(in.vs(), sz)); break;
        case Op::Vcst: set(vsuffix("vcst", sz), vreg(in.vd(), sz) + "," + kVcst[(w >> 16) & 31]); break;
        case Op::Vf2in: case Op::Vf2iz: case Op::Vf2iu: case Op::Vf2id: case Op::Vi2f: {
            const char* name = in.op == Op::Vf2in ? "vf2in" : in.op == Op::Vf2iz ? "vf2iz" : in.op == Op::Vf2iu ? "vf2iu"
                             : in.op == Op::Vf2id ? "vf2id" : "vi2f";
            set(vsuffix(name, sz), vreg(in.vd(), sz) + "," + vreg(in.vs(), sz) + "," + nx::format("{}", (w >> 16) & 31));
            break;
        }
        case Op::Vcmovt: case Op::Vcmovf:
            set(vsuffix(in.op == Op::Vcmovt ? "vcmovt" : "vcmovf", sz),
                vreg(in.vd(), sz) + "," + vreg(in.vs(), sz) + "," + nx::format("{}", (w >> 16) & 7));
            break;
        case Op::Vwbn: set(vsuffix("vwbn", sz), vreg(in.vd(), sz) + "," + vreg(in.vs(), sz) + "," + hex((w >> 16) & 0xFF)); break;

        // VFPU prefixes / immediates
        case Op::Vpfxs: set("vpfxs", prefix_lanes(w, false)); break;
        case Op::Vpfxt: set("vpfxt", prefix_lanes(w, false)); break;
        case Op::Vpfxd: set("vpfxd", prefix_lanes(w, true)); break;
        case Op::Viim: set("viim.s", vreg(in.vt(), 1) + "," + nx::format("{}", in.simm())); break;
        case Op::Vfim: set("vfim.s", vreg(in.vt(), 1) + "," + half_float(static_cast<std::uint16_t>(w & 0xFFFF))); break;

        // VFPU matrix ops
        case Op::Vmmul: set(vsuffix("vmmul", sz), vreg(in.vd(), sz, true) + "," + vreg(in.vs() ^ 0x20, sz, true) + "," + vreg(in.vt(), sz, true)); break;
        case Op::Vtfm2: case Op::Vtfm3: case Op::Vtfm4: {
            const char* name = in.op == Op::Vtfm2 ? "vtfm2" : in.op == Op::Vtfm3 ? "vtfm3" : "vtfm4";
            set(vsuffix(name, sz), vreg(in.vd(), sz) + "," + vreg(in.vs(), sz, true) + "," + vreg(in.vt(), sz));
            break;
        }
        case Op::Vhtfm2: case Op::Vhtfm3: case Op::Vhtfm4: {
            const char* name = in.op == Op::Vhtfm2 ? "vhtfm2" : in.op == Op::Vhtfm3 ? "vhtfm3" : "vhtfm4";
            const int n = sz + 1;
            set(vsuffix(name, n), vreg(in.vd(), n) + "," + vreg(in.vs(), n, true) + "," + vreg(in.vt(), n));
            break;
        }
        case Op::Vmscl: set(vsuffix("vmscl", sz), vreg(in.vd(), sz, true) + "," + vreg(in.vs(), sz, true) + "," + vreg(in.vt(), 1)); break;
        case Op::Vcrsp: set("vcrsp.t", vreg(in.vd(), 3) + "," + vreg(in.vs(), 3) + "," + vreg(in.vt(), 3)); break;
        case Op::Vqmul: set("vqmul.q", vreg(in.vd(), 4) + "," + vreg(in.vs(), 4) + "," + vreg(in.vt(), 4)); break;
        case Op::Vmmov: set(vsuffix("vmmov", sz), vreg(in.vd(), sz, true) + "," + vreg(in.vs(), sz, true)); break;
        case Op::Vmidt: set(vsuffix("vmidt", sz), vreg(in.vd(), sz, true)); break;
        case Op::Vmzero: set(vsuffix("vmzero", sz), vreg(in.vd(), sz, true)); break;
        case Op::Vmone: set(vsuffix("vmone", sz), vreg(in.vd(), sz, true)); break;
        case Op::Vrot: set(vsuffix("vrot", sz), vreg(in.vd(), sz) + "," + vreg(in.vs(), 1) + "," + rot_lanes(w, sz)); break;
        case Op::Vnop: set("vnop"); break;
        case Op::Vsync: set("vsync"); break;
        case Op::Vflush: set("vflush"); break;
    }
    return args.empty() ? m : m + "\t" + args;
}

}  // namespace ff7cc::mips
