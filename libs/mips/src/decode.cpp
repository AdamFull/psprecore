#include "ff7cc/mips/allegrex.hpp"

#include <array>

#include "core/foundation/strings/utf8_string.h"

namespace ff7cc::mips {

namespace {

constexpr std::array kOpNames{
#define FF7CC_X(name) nx::string_view(#name),
    FF7CC_ALLEGREX_OPS(FF7CC_X)
#undef FF7CC_X
};

Op special(std::uint32_t w) {
    const auto rs = (w >> 21) & 31, sa = (w >> 6) & 31;
    switch (w & 63) {
        case 0x00: return Op::Sll;
        case 0x02: return rs == 0 ? Op::Srl : rs == 1 ? Op::Rotr : Op::Invalid;
        case 0x03: return Op::Sra;
        case 0x04: return Op::Sllv;
        case 0x06: return sa == 0 ? Op::Srlv : sa == 1 ? Op::Rotrv : Op::Invalid;
        case 0x07: return Op::Srav;
        case 0x08: return Op::Jr;
        case 0x09: return Op::Jalr;
        case 0x0A: return Op::Movz;
        case 0x0B: return Op::Movn;
        case 0x0C: return Op::Syscall;
        case 0x0D: return Op::Break;
        case 0x0F: return Op::Sync;
        case 0x10: return Op::Mfhi;
        case 0x11: return Op::Mthi;
        case 0x12: return Op::Mflo;
        case 0x13: return Op::Mtlo;
        case 0x16: return Op::Clz;
        case 0x17: return Op::Clo;
        case 0x18: return Op::Mult;
        case 0x19: return Op::Multu;
        case 0x1A: return Op::Div;
        case 0x1B: return Op::Divu;
        case 0x1C: return Op::Madd;
        case 0x1D: return Op::Maddu;
        case 0x20: return Op::Add;
        case 0x21: return Op::Addu;
        case 0x22: return Op::Sub;
        case 0x23: return Op::Subu;
        case 0x24: return Op::And;
        case 0x25: return Op::Or;
        case 0x26: return Op::Xor;
        case 0x27: return Op::Nor;
        case 0x2A: return Op::Slt;
        case 0x2B: return Op::Sltu;
        case 0x2C: return Op::Max;
        case 0x2D: return Op::Min;
        case 0x2E: return Op::Msub;
        case 0x2F: return Op::Msubu;
        default: return Op::Invalid;
    }
}

Op regimm(std::uint32_t w) {
    switch ((w >> 16) & 31) {
        case 0x00: return Op::Bltz;
        case 0x01: return Op::Bgez;
        case 0x02: return Op::Bltzl;
        case 0x03: return Op::Bgezl;
        case 0x10: return Op::Bltzal;
        case 0x11: return Op::Bgezal;
        case 0x12: return Op::Bltzall;
        case 0x13: return Op::Bgezall;
        default: return Op::Invalid;
    }
}

Op cop0(std::uint32_t w) {
    switch ((w >> 21) & 31) {
        case 0x00: return Op::Mfc0;
        case 0x04: return Op::Mtc0;
        case 0x10: return (w & 63) == 0x18 ? Op::Eret : Op::Invalid;
        default: return Op::Invalid;
    }
}

Op cop1(std::uint32_t w) {
    switch ((w >> 21) & 31) {
        case 0x00: return Op::Mfc1;
        case 0x02: return Op::Cfc1;
        case 0x04: return Op::Mtc1;
        case 0x06: return Op::Ctc1;
        case 0x08: {
            constexpr Op kBc[] = {Op::Bc1f, Op::Bc1t, Op::Bc1fl, Op::Bc1tl};
            return kBc[(w >> 16) & 3];
        }
        case 0x10:
            switch (w & 63) {
                case 0x00: return Op::AddS;
                case 0x01: return Op::SubS;
                case 0x02: return Op::MulS;
                case 0x03: return Op::DivS;
                case 0x04: return Op::SqrtS;
                case 0x05: return Op::AbsS;
                case 0x06: return Op::MovS;
                case 0x07: return Op::NegS;
                case 0x0C: return Op::RoundWS;
                case 0x0D: return Op::TruncWS;
                case 0x0E: return Op::CeilWS;
                case 0x0F: return Op::FloorWS;
                case 0x24: return Op::CvtWS;
                default: return (w & 0x30) == 0x30 ? Op::CCondS : Op::Invalid;
            }
        case 0x14: return (w & 63) == 0x20 ? Op::CvtSW : Op::Invalid;
        default: return Op::Invalid;
    }
}

Op cop2(std::uint32_t w) {
    switch ((w >> 21) & 31) {
        case 0x03: return (w & 0x80) ? Op::Mfvc : Op::Mfv;
        case 0x07: return (w & 0x80) ? Op::Mtvc : Op::Mtv;
        case 0x08: {
            constexpr Op kBv[] = {Op::Bvf, Op::Bvt, Op::Bvfl, Op::Bvtl};
            return kBv[(w >> 16) & 3];
        }
        default: return Op::Invalid;
    }
}

Op vfpu4(std::uint32_t w) {
    switch ((w >> 24) & 3) {
        case 0: {
            const auto f = (w >> 16) & 0xFF;
            if (f >= 0x60 && f <= 0x7F) return Op::Vcst;
            switch (f) {
                case 0x00: return Op::Vmov;
                case 0x01: return Op::Vabs;
                case 0x02: return Op::Vneg;
                case 0x03: return Op::Vidt;
                case 0x04: return Op::Vsat0;
                case 0x05: return Op::Vsat1;
                case 0x06: return Op::Vzero;
                case 0x07: return Op::Vone;
                case 0x10: return Op::Vrcp;
                case 0x11: return Op::Vrsq;
                case 0x12: return Op::Vsin;
                case 0x13: return Op::Vcos;
                case 0x14: return Op::Vexp2;
                case 0x15: return Op::Vlog2;
                case 0x16: return Op::Vsqrt;
                case 0x17: return Op::Vasin;
                case 0x18: return Op::Vnrcp;
                case 0x1A: return Op::Vnsin;
                case 0x1C: return Op::Vrexp2;
                case 0x20: return Op::Vrnds;
                case 0x21: return Op::Vrndi;
                case 0x22: return Op::Vrndf1;
                case 0x23: return Op::Vrndf2;
                case 0x32: return Op::Vf2h;
                case 0x33: return Op::Vh2f;
                case 0x36: return Op::Vsbz;
                case 0x37: return Op::Vlgb;
                case 0x38: return Op::Vuc2i;
                case 0x39: return Op::Vc2i;
                case 0x3A: return Op::Vus2i;
                case 0x3B: return Op::Vs2i;
                case 0x3C: return Op::Vi2uc;
                case 0x3D: return Op::Vi2c;
                case 0x3E: return Op::Vi2us;
                case 0x3F: return Op::Vi2s;
                case 0x40: return Op::Vsrt1;
                case 0x41: return Op::Vsrt2;
                case 0x42: return Op::Vbfy1;
                case 0x43: return Op::Vbfy2;
                case 0x44: return Op::Vocp;
                case 0x45: return Op::Vsocp;
                case 0x46: return Op::Vfad;
                case 0x47: return Op::Vavg;
                case 0x48: return Op::Vsrt3;
                case 0x49: return Op::Vsrt4;
                case 0x4A: return Op::Vsgn;
                case 0x50: return Op::Vmfvc;
                case 0x51: return Op::Vmtvc;
                case 0x59: return Op::Vt4444;
                case 0x5A: return Op::Vt5551;
                case 0x5B: return Op::Vt5650;
                default: return Op::Invalid;
            }
        }
        case 2:
            switch ((w >> 21) & 7) {
                case 0: return Op::Vf2in;
                case 1: return Op::Vf2iz;
                case 2: return Op::Vf2iu;
                case 3: return Op::Vf2id;
                case 4: return Op::Vi2f;
                case 5: return (w & 0x00080000) ? Op::Vcmovf : Op::Vcmovt;
                default: return Op::Invalid;
            }
        case 3: return Op::Vwbn;
        default: return Op::Invalid;
    }
}

Op vfpu6(std::uint32_t w) {
    const int size = vfpu_size(w);
    switch ((w >> 23) & 7) {
        case 0: return Op::Vmmul;
        case 1: return size == 2 ? Op::Vtfm2 : size == 1 ? Op::Vhtfm2 : Op::Invalid;
        case 2: return size == 3 ? Op::Vtfm3 : size == 2 ? Op::Vhtfm3 : Op::Invalid;
        case 3: return size == 4 ? Op::Vtfm4 : size == 3 ? Op::Vhtfm4 : Op::Invalid;
        case 4: return Op::Vmscl;
        case 5: return size == 3 ? Op::Vcrsp : size == 4 ? Op::Vqmul : Op::Invalid;
        case 7:
            if (w & 0x00200000) return Op::Vrot;
            switch ((w >> 16) & 31) {
                case 0: return Op::Vmmov;
                case 3: return Op::Vmidt;
                case 6: return Op::Vmzero;
                case 7: return Op::Vmone;
                default: return Op::Invalid;
            }
        default: return Op::Invalid;
    }
}

Op classify(std::uint32_t w) {
    switch (w >> 26) {
        case 0x00: return special(w);
        case 0x01: return regimm(w);
        case 0x02: return Op::J;
        case 0x03: return Op::Jal;
        case 0x04: return Op::Beq;
        case 0x05: return Op::Bne;
        case 0x06: return Op::Blez;
        case 0x07: return Op::Bgtz;
        case 0x08: return Op::Addi;
        case 0x09: return Op::Addiu;
        case 0x0A: return Op::Slti;
        case 0x0B: return Op::Sltiu;
        case 0x0C: return Op::Andi;
        case 0x0D: return Op::Ori;
        case 0x0E: return Op::Xori;
        case 0x0F: return Op::Lui;
        case 0x10: return cop0(w);
        case 0x11: return cop1(w);
        case 0x12: return cop2(w);
        case 0x14: return Op::Beql;
        case 0x15: return Op::Bnel;
        case 0x16: return Op::Blezl;
        case 0x17: return Op::Bgtzl;
        case 0x18: {
            constexpr Op k[] = {Op::Vadd, Op::Vsub, Op::Vsbn, Op::Invalid, Op::Invalid, Op::Invalid, Op::Invalid, Op::Vdiv};
            return k[(w >> 23) & 7];
        }
        case 0x19: {
            constexpr Op k[] = {Op::Vmul, Op::Vdot, Op::Vscl, Op::Invalid, Op::Vhdp, Op::Vcrs, Op::Vdet, Op::Invalid};
            return k[(w >> 23) & 7];
        }
        case 0x1B: {
            constexpr Op k[] = {Op::Vcmp, Op::Invalid, Op::Vmin, Op::Vmax, Op::Invalid, Op::Vscmp, Op::Vsge, Op::Vslt};
            return k[(w >> 23) & 7];
        }
        case 0x1C:
            switch (w & 63) {
                case 0x00: return Op::Halt;
                case 0x24: return Op::Mfic;
                case 0x26: return Op::Mtic;
                default: return Op::Invalid;
            }
        case 0x1F:
            switch (w & 63) {
                case 0x00: return Op::Ext;
                case 0x04: return Op::Ins;
                case 0x20:
                    switch ((w >> 6) & 31) {
                        case 0x02: return Op::Wsbh;
                        case 0x03: return Op::Wsbw;
                        case 0x10: return Op::Seb;
                        case 0x14: return Op::Bitrev;
                        case 0x18: return Op::Seh;
                        default: return Op::Invalid;
                    }
                default: return Op::Invalid;
            }
        case 0x20: return Op::Lb;
        case 0x21: return Op::Lh;
        case 0x22: return Op::Lwl;
        case 0x23: return Op::Lw;
        case 0x24: return Op::Lbu;
        case 0x25: return Op::Lhu;
        case 0x26: return Op::Lwr;
        case 0x28: return Op::Sb;
        case 0x29: return Op::Sh;
        case 0x2A: return Op::Swl;
        case 0x2B: return Op::Sw;
        case 0x2E: return Op::Swr;
        case 0x2F: return Op::Cache;
        case 0x30: return Op::Ll;
        case 0x31: return Op::Lwc1;
        case 0x32: return Op::LvS;
        case 0x34: return vfpu4(w);
        case 0x35: return (w & 2) ? Op::LvrQ : Op::LvlQ;
        case 0x36: return Op::LvQ;
        case 0x37:
            switch ((w >> 24) & 3) {
                case 0: return Op::Vpfxs;
                case 1: return Op::Vpfxt;
                case 2: return Op::Vpfxd;
                default: return (w & 0x00800000) ? Op::Vfim : Op::Viim;
            }
        case 0x38: return Op::Sc;
        case 0x39: return Op::Swc1;
        case 0x3A: return Op::SvS;
        case 0x3C: return vfpu6(w);
        case 0x3D: return (w & 2) ? Op::SvrQ : Op::SvlQ;
        case 0x3E: return Op::SvQ;
        case 0x3F:
            if (w == 0xFFFF0000) return Op::Vnop;
            if (w == 0xFFFF0320) return Op::Vsync;
            if (w == 0xFFFF040D) return Op::Vflush;
            return Op::Invalid;
        default: return Op::Invalid;
    }
}

}  // namespace

nx::string_view op_name(Op op) { return kOpNames[static_cast<std::size_t>(op)]; }

Instruction decode(std::uint32_t word, std::uint32_t pc) { return Instruction{word, pc, classify(word)}; }

bool Instruction::is_branch() const {
    switch (op) {
        case Op::Beq: case Op::Bne: case Op::Blez: case Op::Bgtz: case Op::Beql: case Op::Bnel: case Op::Blezl:
        case Op::Bgtzl: case Op::Bltz: case Op::Bgez: case Op::Bltzl: case Op::Bgezl: case Op::Bltzal:
        case Op::Bgezal: case Op::Bltzall: case Op::Bgezall: case Op::Bc1f: case Op::Bc1t: case Op::Bc1fl:
        case Op::Bc1tl: case Op::Bvf: case Op::Bvt: case Op::Bvfl: case Op::Bvtl:
            return true;
        default:
            return false;
    }
}

bool Instruction::is_likely() const {
    switch (op) {
        case Op::Beql: case Op::Bnel: case Op::Blezl: case Op::Bgtzl: case Op::Bltzl: case Op::Bgezl:
        case Op::Bltzall: case Op::Bgezall: case Op::Bc1fl: case Op::Bc1tl: case Op::Bvfl: case Op::Bvtl:
            return true;
        default:
            return false;
    }
}

bool Instruction::is_jump() const { return op == Op::J || op == Op::Jal || op == Op::Jr || op == Op::Jalr; }

}  // namespace ff7cc::mips
