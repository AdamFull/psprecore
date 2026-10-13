#pragma once

// Allegrex (PSP CPU) instruction decoder: MIPS32 integer core, Allegrex extensions, FPU (COP1) and VFPU.
//
// decode() classifies a word into an Op; operand fields are read with the accessors on Instruction.
// format() renders an instruction the way binutils' psp-objdump does, which lets the decoder be checked
// against the SDK disassembler instruction by instruction.

#include <cstdint>
#include <string>
#include <string_view>

namespace ff7cc::mips {

// clang-format off
#define FF7CC_ALLEGREX_OPS(X) \
    X(Invalid) \
    /* integer */ \
    X(Sll) X(Srl) X(Rotr) X(Sra) X(Sllv) X(Srlv) X(Rotrv) X(Srav) X(Jr) X(Jalr) X(Movz) X(Movn) X(Syscall) \
    X(Break) X(Sync) X(Mfhi) X(Mthi) X(Mflo) X(Mtlo) X(Clz) X(Clo) X(Mult) X(Multu) X(Div) X(Divu) X(Madd) \
    X(Maddu) X(Add) X(Addu) X(Sub) X(Subu) X(And) X(Or) X(Xor) X(Nor) X(Slt) X(Sltu) X(Max) X(Min) X(Msub) \
    X(Msubu) X(Bltz) X(Bgez) X(Bltzl) X(Bgezl) X(Bltzal) X(Bgezal) X(Bltzall) X(Bgezall) X(J) X(Jal) X(Beq) \
    X(Bne) X(Blez) X(Bgtz) X(Addi) X(Addiu) X(Slti) X(Sltiu) X(Andi) X(Ori) X(Xori) X(Lui) X(Beql) X(Bnel) \
    X(Blezl) X(Bgtzl) X(Ext) X(Ins) X(Wsbh) X(Wsbw) X(Seb) X(Bitrev) X(Seh) X(Lb) X(Lh) X(Lwl) X(Lw) X(Lbu) \
    X(Lhu) X(Lwr) X(Sb) X(Sh) X(Swl) X(Sw) X(Swr) X(Cache) X(Ll) X(Sc) X(Halt) X(Mfic) X(Mtic) \
    /* COP0 (kernel only; decoded so they are not mistaken for data) */ \
    X(Mfc0) X(Mtc0) X(Eret) \
    /* FPU */ \
    X(Lwc1) X(Swc1) X(Mfc1) X(Cfc1) X(Mtc1) X(Ctc1) X(Bc1f) X(Bc1t) X(Bc1fl) X(Bc1tl) X(AddS) X(SubS) X(MulS) \
    X(DivS) X(SqrtS) X(AbsS) X(MovS) X(NegS) X(RoundWS) X(TruncWS) X(CeilWS) X(FloorWS) X(CvtSW) X(CvtWS) \
    X(CCondS) \
    /* VFPU: loads/stores and transfers */ \
    X(LvS) X(SvS) X(LvQ) X(SvQ) X(LvlQ) X(LvrQ) X(SvlQ) X(SvrQ) X(Mfv) X(Mfvc) X(Mtv) X(Mtvc) X(Bvf) X(Bvt) \
    X(Bvfl) X(Bvtl) \
    /* VFPU: binary vector ops */ \
    X(Vadd) X(Vsub) X(Vsbn) X(Vdiv) X(Vmul) X(Vdot) X(Vscl) X(Vhdp) X(Vcrs) X(Vdet) X(Vcmp) X(Vmin) X(Vmax) \
    X(Vscmp) X(Vsge) X(Vslt) \
    /* VFPU: unary group (opcode 0x34) */ \
    X(Vmov) X(Vabs) X(Vneg) X(Vidt) X(Vsat0) X(Vsat1) X(Vzero) X(Vone) X(Vrcp) X(Vrsq) X(Vsin) X(Vcos) X(Vexp2) \
    X(Vlog2) X(Vsqrt) X(Vasin) X(Vnrcp) X(Vnsin) X(Vrexp2) X(Vrnds) X(Vrndi) X(Vrndf1) X(Vrndf2) X(Vf2h) X(Vh2f) \
    X(Vsbz) X(Vlgb) X(Vuc2i) X(Vc2i) X(Vus2i) X(Vs2i) X(Vi2uc) X(Vi2c) X(Vi2us) X(Vi2s) X(Vsrt1) X(Vsrt2) \
    X(Vbfy1) X(Vbfy2) X(Vocp) X(Vsocp) X(Vfad) X(Vavg) X(Vsrt3) X(Vsrt4) X(Vsgn) X(Vmfvc) X(Vmtvc) X(Vt4444) \
    X(Vt5551) X(Vt5650) X(Vcst) X(Vf2in) X(Vf2iz) X(Vf2iu) X(Vf2id) X(Vi2f) X(Vcmovt) X(Vcmovf) X(Vwbn) \
    /* VFPU: prefixes, immediates, matrix ops, misc */ \
    X(Vpfxs) X(Vpfxt) X(Vpfxd) X(Viim) X(Vfim) X(Vmmul) X(Vhtfm2) X(Vtfm2) X(Vhtfm3) X(Vtfm3) X(Vhtfm4) \
    X(Vtfm4) X(Vmscl) X(Vcrsp) X(Vqmul) X(Vmmov) X(Vmidt) X(Vmzero) X(Vmone) X(Vrot) X(Vnop) X(Vsync) X(Vflush)
// clang-format on

enum class Op : std::uint16_t {
#define FF7CC_X(name) name,
    FF7CC_ALLEGREX_OPS(FF7CC_X)
#undef FF7CC_X
};

std::string_view op_name(Op op);  // enum spelling, e.g. "AddS"

// VFPU vector size encoded in bits 7 and 15 (1 = .s, 2 = .p, 3 = .t, 4 = .q).
inline int vfpu_size(std::uint32_t w) { return 1 + (((w >> 7) & 1) | (((w >> 15) & 1) << 1)); }

struct Instruction {
    std::uint32_t word = 0;
    std::uint32_t pc = 0;
    Op op = Op::Invalid;

    std::uint32_t rs() const { return (word >> 21) & 31; }
    std::uint32_t rt() const { return (word >> 16) & 31; }
    std::uint32_t rd() const { return (word >> 11) & 31; }
    std::uint32_t sa() const { return (word >> 6) & 31; }
    std::uint32_t fs() const { return (word >> 11) & 31; }
    std::uint32_t ft() const { return (word >> 16) & 31; }
    std::uint32_t fd() const { return (word >> 6) & 31; }
    std::int32_t simm() const { return static_cast<std::int16_t>(word & 0xFFFF); }
    std::uint32_t uimm() const { return word & 0xFFFF; }
    // Branch target (pc + 4 + offset * 4) and jump target within the current 256 MiB region.
    std::uint32_t branch_target() const { return pc + 4 + static_cast<std::uint32_t>(simm() * 4); }
    std::uint32_t jump_target() const { return ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2); }

    // VFPU register fields (7-bit register numbers).
    std::uint32_t vd() const { return word & 0x7F; }
    std::uint32_t vs() const { return (word >> 8) & 0x7F; }
    std::uint32_t vt() const { return (word >> 16) & 0x7F; }
    int vsize() const { return vfpu_size(word); }

    bool is_branch() const;       // conditional or unconditional PC-relative branch (has a delay slot)
    bool is_likely() const;       // branch-likely: delay slot only executes when taken
    bool is_jump() const;         // j, jal, jr, jalr
    bool is_call() const { return op == Op::Jal || op == Op::Jalr || op == Op::Bltzal || op == Op::Bgezal ||
                                  op == Op::Bltzall || op == Op::Bgezall; }
    bool is_vfpu() const { return op >= Op::LvS; }
};

Instruction decode(std::uint32_t word, std::uint32_t pc);

// "mnemonic\toperands" exactly as psp-objdump prints it (mnemonic only if there are no operands).
std::string format(const Instruction& insn);

}  // namespace ff7cc::mips
