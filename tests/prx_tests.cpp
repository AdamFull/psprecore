#include <cstring>
#include <string>
#include <string_view>

#include "ff7cc/error.hpp"
#include "ff7cc/formats/elf.hpp"
#include "ff7cc/formats/prx.hpp"
#include "test_main.hpp"
#include "core/foundation/core/foundation.h"

using namespace ff7cc;

namespace {

void put32(nx::vector<std::byte>& v, std::size_t at, std::uint32_t x) {
    if (v.size() < at + 4) v.resize(at + 4);
    std::memcpy(v.data() + at, &x, 4);
}
void put16(nx::vector<std::byte>& v, std::size_t at, std::uint16_t x) {
    if (v.size() < at + 2) v.resize(at + 2);
    std::memcpy(v.data() + at, &x, 2);
}
std::uint32_t get32(const nx::vector<std::byte>& v, std::size_t at) {
    std::uint32_t x;
    std::memcpy(&x, v.data() + at, 4);
    return x;
}

// Minimal ELF32 with one PT_LOAD segment holding `words` at vaddr 0, no sections.
nx::vector<std::byte> tiny_elf(const nx::vector<std::uint32_t>& words) {
    nx::vector<std::byte> f(52 + 32);
    std::memcpy(f.data(), "\x7f" "ELF\x01\x01\x01", 7);
    put16(f, 16, elf::kTypePrx);
    put16(f, 18, elf::kMachineMips);
    put32(f, 28, 52);  // phoff
    put16(f, 42, 32);  // phentsize
    put16(f, 44, 1);   // phnum
    put16(f, 46, 40);  // shentsize
    const auto size = static_cast<std::uint32_t>(words.size() * 4);
    put32(f, 52, 1);         // PT_LOAD
    put32(f, 52 + 4, 84);    // offset
    put32(f, 52 + 16, size); // filesz
    put32(f, 52 + 20, size); // memsz
    for (std::size_t i = 0; i < words.size(); ++i) put32(f, 84 + 4 * i, words[i]);
    return f;
}

nx::vector<std::byte> bytes(std::string_view s) {
    nx::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

}  // namespace

TEST_CASE("prx relocation: HI16/LO16 pairs, R26 and R32") {
    const auto data = tiny_elf({
        0x3C020007,  // 0x00 lui   v0, 0x7
        0x3C030007,  // 0x04 lui   v1, 0x7       (second HI16 sharing the same LO16)
        0x2442A918,  // 0x08 addiu v0, v0, -0x56E8  -> 0x0006A918
        0x0C0012DF,  // 0x0C jal   0x4B7C
        0x00001234,  // 0x10 .word 0x1234
    });
    const elf::File f(data);
    prx::Module m;
    m.segments = f.segments();
    m.relocations = {{0x00, prx::kRelHi16, 0, 0}, {0x04, prx::kRelHi16, 0, 0}, {0x08, prx::kRelLo16, 0, 0},
                     {0x0C, prx::kRel26, 0, 0},   {0x10, prx::kRel32, 0, 0}};
    const auto image = prx::load_image(f, m, 0x08804000);
    // 0x6A918 + 0x08804000 = 0x0886E918 -> hi 0x0887 (rounded for the negative low half), lo 0xE918.
    CHECK(get32(image, 0x00) == 0x3C020887);
    CHECK(get32(image, 0x04) == 0x3C030887);
    CHECK(get32(image, 0x08) == 0x2442E918);
    CHECK(get32(image, 0x0C) == (0x0C000000u | ((0x08804000u + 0x4B7C) >> 2 & 0x03FFFFFF)));
    CHECK(get32(image, 0x10) == 0x08804000u + 0x1234);
}

TEST_CASE("prx relocation: dangling HI16 is rejected") {
    const auto data = tiny_elf({0x3C020007});
    const elf::File f(data);
    prx::Module m;
    m.segments = f.segments();
    m.relocations = {{0x00, prx::kRelHi16, 0, 0}};
    CHECK_THROWS_AS(prx::load_image(f, m, 0), FormatError);
}

TEST_CASE("ar archive with GNU long names") {
    std::string ar = "!<arch>\n";
    const auto member = [&](std::string name, std::string body) {
        name.resize(16, ' ');
        std::string size = std::to_string(body.size());
        size.resize(10, ' ');
        ar += name + std::string(32, ' ') + size + "`\n" + body;
        if (body.size() & 1) ar += '\n';
    };
    member("//", "a_very_long_member_name.o/\n");
    member("/0", "abc");
    member("short.o/", "xy");
    const auto data = bytes(ar);
    const auto members = elf::read_archive(data);
    CHECK(members.size() == 2);
    CHECK(members[0].name == "a_very_long_member_name.o");
    CHECK(members[0].data.size() == 3);
    CHECK(members[1].name == "short.o");
    CHECK(std::memcmp(members[1].data.data(), "xy", 2) == 0);
}

TEST_CASE("elf rejects non-ELF input") {
    const auto junk = bytes(std::string(64, 'x'));
    CHECK_THROWS_AS(elf::File{junk}, FormatError);
}
