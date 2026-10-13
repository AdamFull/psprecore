#include "ff7cc/formats/elf.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>

#include "ff7cc/error.hpp"

namespace ff7cc::elf {

namespace {

template <typename T>
T load(std::span<const std::byte> data, std::size_t at) {
    if (at > data.size() || sizeof(T) > data.size() - at) throw FormatError("ELF data truncated");
    T v;
    std::memcpy(&v, data.data() + at, sizeof v);
    return v;
}

std::span<const std::byte> slice(std::span<const std::byte> data, std::size_t at, std::size_t size) {
    if (at > data.size() || size > data.size() - at) throw FormatError("ELF section out of bounds");
    return data.subspan(at, size);
}

std::string c_string(std::span<const std::byte> table, std::size_t at) {
    if (at >= table.size()) return {};
    const auto* begin = reinterpret_cast<const char*>(table.data()) + at;
    const auto* end = reinterpret_cast<const char*>(table.data()) + table.size();
    return std::string(begin, std::find(begin, end, '\0'));
}

}  // namespace

File::File(std::span<const std::byte> data) : data_(data) {
    if (data.size() < 52 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0) throw FormatError("not an ELF file");
    if (load<std::uint8_t>(data, 4) != 1 || load<std::uint8_t>(data, 5) != 1)
        throw FormatError("only little-endian ELF32 is supported");
    type_ = load<std::uint16_t>(data, 16);
    machine_ = load<std::uint16_t>(data, 18);
    entry_ = load<std::uint32_t>(data, 24);
    const auto phoff = load<std::uint32_t>(data, 28);
    const auto shoff = load<std::uint32_t>(data, 32);
    flags_ = load<std::uint32_t>(data, 36);
    const auto phentsize = load<std::uint16_t>(data, 42);
    const auto phnum = load<std::uint16_t>(data, 44);
    const auto shentsize = load<std::uint16_t>(data, 46);
    const auto shnum = load<std::uint16_t>(data, 48);
    const auto shstrndx = load<std::uint16_t>(data, 50);

    for (std::size_t i = 0; i < phnum; ++i) {
        const std::size_t at = phoff + i * phentsize;
        segments_.push_back(Segment{load<std::uint32_t>(data, at), load<std::uint32_t>(data, at + 4),
                                    load<std::uint32_t>(data, at + 8), load<std::uint32_t>(data, at + 12),
                                    load<std::uint32_t>(data, at + 16), load<std::uint32_t>(data, at + 20),
                                    load<std::uint32_t>(data, at + 24)});
    }

    std::vector<std::uint32_t> name_offsets;
    for (std::size_t i = 0; i < shnum; ++i) {
        const std::size_t at = shoff + i * shentsize;
        name_offsets.push_back(load<std::uint32_t>(data, at));
        Section s;
        s.type = load<std::uint32_t>(data, at + 4);
        s.flags = load<std::uint32_t>(data, at + 8);
        s.addr = load<std::uint32_t>(data, at + 12);
        s.offset = load<std::uint32_t>(data, at + 16);
        s.size = load<std::uint32_t>(data, at + 20);
        s.link = load<std::uint32_t>(data, at + 24);
        s.info = load<std::uint32_t>(data, at + 28);
        s.entsize = load<std::uint32_t>(data, at + 36);
        sections_.push_back(s);
    }
    if (shstrndx < sections_.size()) {
        const auto strtab = contents(sections_[shstrndx]);
        for (std::size_t i = 0; i < sections_.size(); ++i) sections_[i].name = c_string(strtab, name_offsets[i]);
    }
}

const Section* File::find_section(std::string_view name) const {
    for (const auto& s : sections_)
        if (s.name == name) return &s;
    return nullptr;
}

std::span<const std::byte> File::contents(const Section& section) const {
    if (section.type == kSectionNobits) return {};
    return slice(data_, section.offset, section.size);
}

std::vector<Symbol> File::symbols() const {
    std::vector<Symbol> out;
    for (const auto& s : sections_) {
        if (s.type != kSectionSymtab) continue;
        const auto table = contents(s);
        const auto strtab = s.link < sections_.size() ? contents(sections_[s.link]) : std::span<const std::byte>{};
        for (std::size_t at = 16; at + 16 <= table.size(); at += 16) {  // entry 0 is the null symbol
            Symbol sym;
            sym.name = c_string(strtab, load<std::uint32_t>(table, at));
            sym.value = load<std::uint32_t>(table, at + 4);
            sym.size = load<std::uint32_t>(table, at + 8);
            sym.info = load<std::uint8_t>(table, at + 12);
            sym.section = load<std::uint16_t>(table, at + 14);
            out.push_back(std::move(sym));
        }
    }
    return out;
}

std::vector<Relocation> File::relocations(const Section& section) const {
    if (section.type != kSectionRel && section.type != kSectionPspRel)
        throw FormatError("section '" + section.name + "' is not a REL relocation section");
    const auto table = contents(section);
    std::vector<Relocation> out;
    for (std::size_t at = 0; at + 8 <= table.size(); at += 8)
        out.push_back(Relocation{load<std::uint32_t>(table, at), load<std::uint32_t>(table, at + 4)});
    return out;
}

std::vector<ArchiveMember> read_archive(std::span<const std::byte> data) {
    if (data.size() < 8 || std::memcmp(data.data(), "!<arch>\n", 8) != 0) throw FormatError("not an ar archive");
    std::vector<ArchiveMember> out;
    std::span<const std::byte> long_names;
    for (std::size_t at = 8; at + 60 <= data.size();) {
        const auto field = [&](std::size_t off, std::size_t len) {
            std::string s(reinterpret_cast<const char*>(data.data()) + at + off, len);
            s.erase(s.find_last_not_of(' ') + 1);
            return s;
        };
        std::string name = field(0, 16);
        const auto size_text = field(48, 10);
        std::size_t size = 0;
        std::from_chars(size_text.data(), size_text.data() + size_text.size(), size);
        const auto body = slice(data, at + 60, size);

        if (name == "//") {
            long_names = body;
        } else if (name != "/" && name != "/SYM64/") {
            if (name.size() > 1 && name[0] == '/' && !long_names.empty()) {  // GNU long name reference
                std::size_t off = 0;
                std::from_chars(name.data() + 1, name.data() + name.size(), off);
                name = c_string(long_names, off);
                name = name.substr(0, name.find('\n'));
            }
            if (!name.empty() && name.back() == '/') name.pop_back();
            out.push_back(ArchiveMember{name, body});
        }
        at += 60 + size + (size & 1);
    }
    return out;
}

}  // namespace ff7cc::elf
