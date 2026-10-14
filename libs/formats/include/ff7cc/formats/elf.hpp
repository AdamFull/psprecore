#pragma once

// Minimal little-endian ELF32 reader (MIPS): headers, sections, symbols and relocations.

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace ff7cc::elf {

inline constexpr std::uint16_t kTypeRel = 1;
inline constexpr std::uint16_t kTypeExec = 2;
inline constexpr std::uint16_t kTypePrx = 0xFFA0;  // Sony PSP relocatable module
inline constexpr std::uint16_t kMachineMips = 8;

inline constexpr std::uint32_t kSectionNobits = 8;
inline constexpr std::uint32_t kSectionSymtab = 2;
inline constexpr std::uint32_t kSectionRel = 9;
inline constexpr std::uint32_t kSectionPspRel = 0x700000A0;  // PSP relocations (REL format, extended info)

struct Section {
    nx::string name;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint32_t addr = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t link = 0;
    std::uint32_t info = 0;
    std::uint32_t entsize = 0;
};

struct Segment {
    std::uint32_t type = 0;
    std::uint32_t offset = 0;
    std::uint32_t vaddr = 0;
    std::uint32_t paddr = 0;
    std::uint32_t filesz = 0;
    std::uint32_t memsz = 0;
    std::uint32_t flags = 0;
};

struct Symbol {
    nx::string name;
    std::uint32_t value = 0;
    std::uint32_t size = 0;
    std::uint8_t info = 0;
    std::uint16_t section = 0;

    std::uint8_t binding() const { return info >> 4; }  // 0 local, 1 global, 2 weak
    std::uint8_t type() const { return info & 15; }     // 1 object, 2 function
};

struct Relocation {
    std::uint32_t offset = 0;
    std::uint32_t info = 0;
};

class File {
public:
    // Parses `data`; the span must outlive the File (section contents are views into it).
    explicit File(std::span<const std::byte> data);

    std::uint16_t type() const { return type_; }
    std::uint16_t machine() const { return machine_; }
    std::uint32_t entry() const { return entry_; }
    std::uint32_t flags() const { return flags_; }

    const nx::vector<Section>& sections() const { return sections_; }
    const nx::vector<Segment>& segments() const { return segments_; }
    const Section* find_section(nx::string_view name) const;

    // Raw contents of a section (empty for NOBITS).
    std::span<const std::byte> contents(const Section& section) const;
    nx::vector<Symbol> symbols() const;
    nx::vector<Relocation> relocations(const Section& section) const;

    std::span<const std::byte> data() const { return data_; }

private:
    std::span<const std::byte> data_;
    std::uint16_t type_ = 0, machine_ = 0;
    std::uint32_t entry_ = 0, flags_ = 0;
    nx::vector<Section> sections_;
    nx::vector<Segment> segments_;
};

// Iterates the members of a Unix "ar" archive (GNU long-name tables supported).
struct ArchiveMember {
    nx::string name;
    std::span<const std::byte> data;
};
nx::vector<ArchiveMember> read_archive(std::span<const std::byte> data);

}  // namespace ff7cc::elf
