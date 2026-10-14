// Decoder sanity checks that need no SDK. The exhaustive check against psp-objdump is scripts/check_allegrex.ps1.

#include "psprecore/mips/allegrex.hpp"
#include "test_main.hpp"
#include "core/foundation/strings/utf8_string.h"

using namespace psprecore::mips;

namespace {

nx::string fmt(std::uint32_t word, std::uint32_t pc = 0) { return format(decode(word, pc)); }

}  // namespace

TEST_CASE("mips: integer forms and pseudo-ops") {
    CHECK(fmt(0x00000000) == "nop");
    CHECK(fmt(0x27BDFFF0) == "addiu\tsp,sp,-16");
    CHECK(fmt(0x00808025) == "move\ts0,a0");
    CHECK(fmt(0x34020001) == "li\tv0,0x1");
    CHECK(fmt(0x03E00008) == "jr\tra");
    CHECK(fmt(0x0C0012DF) == "jal\t0x4b7c");
    CHECK(fmt(0x1000FFF4, 0x4647C) == "b\t0x46450");
    CHECK(fmt(0x7E4DE000) == "ext\tt5,s2,0x0,0x1d");
    CHECK(fmt(0x7C0CFE04) == "ins\tt4,zero,0x18,0x8");
}

TEST_CASE("mips: FPU and VFPU forms") {
    CHECK(fmt(0x460C003C) == "c.lt.s\t$f0,$f12");
    CHECK(fmt(0xD8C40000) == "lv.q\tC100.q,0(a2)");
    CHECK(fmt(0xF008A08C) == "vmmul.q\tM300.q,M000.q,M200.q");
    CHECK(fmt(0xF3B1C481) == "vrot.q\tC010.q,S102.s,[-s,c,0,0]");
    CHECK(fmt(0xDE00083F) == "vpfxd\t[-1:1],[-1:1],[-1:1],m");
    CHECK(fmt(0xD00024E6) == "vmov.p\tR122.p,R100.p");
    CHECK(fmt(0xD0650005) == "vcst.s\tS110.s,VFPU_2_PI");
}

TEST_CASE("mips: control-flow classification") {
    const auto beql = decode(0x5109FFBD, 0x108);
    CHECK(beql.op == Op::Beql && beql.is_branch() && beql.is_likely());
    CHECK(beql.branch_target() == 0);
    // The jump region comes from the top 4 bits of pc + 4 (0 for PSP user memory at 0x08xxxxxx).
    const auto jal = decode(0x0C0012DF, 0x08804000);
    CHECK(jal.is_jump() && jal.is_call());
    CHECK(jal.jump_target() == 0x00004B7C);
    CHECK(decode(0x0C0012DF, 0x98804000).jump_target() == 0x90004B7C);
    CHECK(!decode(0x27BDFFF0, 0).is_branch());
    CHECK(decode(0x49000003, 0).op == Op::Bvf);
    CHECK(decode(0xD006006C, 0).is_vfpu());
    CHECK(decode(0xFC000000, 0).op == Op::Invalid);
}
