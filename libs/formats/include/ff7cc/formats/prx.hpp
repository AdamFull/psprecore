#pragma once

// Sony PSP PRX modules on top of the ELF reader: module info, imports/exports (NIDs) and PSP relocations.
//
// PSP relocation entries are REL entries whose info field packs:
//   bits 0-7   MIPS relocation type (2 = 32, 4 = 26, 5 = HI16, 6 = LO16)
//   bits 8-15  index of the segment the patched location belongs to (offset is relative to it)
//   bits 16-23 index of the segment the referenced address belongs to (the value is relative to it)

#include <cstdint>
#include <span>
#include <string>

#include "ff7cc/formats/elf.hpp"
#include "core/foundation/core/foundation.h"

namespace ff7cc::prx {

enum RelocType : std::uint8_t { kRelNone = 0, kRel32 = 2, kRel26 = 4, kRelHi16 = 5, kRelLo16 = 6 };

struct Relocation {
    std::uint32_t offset = 0;  // relative to segments[offset_segment].vaddr
    std::uint8_t type = 0;
    std::uint8_t offset_segment = 0;
    std::uint8_t address_segment = 0;
};

struct Import {
    std::string library;
    std::uint16_t version = 0;
    std::uint16_t attributes = 0;
    struct Function {
        std::uint32_t nid = 0;
        std::uint32_t stub = 0;  // address of the 8-byte syscall stub in the module
    };
    nx::vector<Function> functions;
    nx::vector<std::uint32_t> variable_nids;
};

struct Export {
    std::string library;  // empty for the module's own "system" export table
    std::uint16_t version = 0;
    std::uint16_t attributes = 0;
    struct Entry {
        std::uint32_t nid = 0;
        std::uint32_t address = 0;
    };
    nx::vector<Entry> functions;
    nx::vector<Entry> variables;
};

struct Module {
    std::string name;
    std::uint16_t attributes = 0;
    std::uint16_t version = 0;
    std::uint32_t gp = 0;
    std::uint32_t entry = 0;
    nx::vector<elf::Segment> segments;
    nx::vector<Import> imports;
    nx::vector<Export> exports;
    nx::vector<Relocation> relocations;
};

// Parses a decrypted PRX (ELF type 0xFFA0) such as BOOT.BIN. Addresses are module-relative (base 0).
Module parse(const elf::File& file);

// Reads a 32-bit word at a module-relative virtual address from the file image (throws if not file-backed).
std::uint32_t read_word(const elf::File& file, const Module& module, std::uint32_t vaddr);

// Returns a copy of the loadable image (all segments, BSS zero-filled) relocated to `base`, starting at `base`.
nx::vector<std::byte> load_image(const elf::File& file, const Module& module, std::uint32_t base);

// NID -> function name database, built from PSPSDK-style stub libraries (lib*.a).
struct NidName {
    std::string library;
    std::uint32_t nid = 0;
    std::string name;
};
// Extracts NID/name pairs from one stub archive; members without a .rodata.sceNid section are ignored.
nx::vector<NidName> nids_from_stub_archive(std::span<const std::byte> archive);

}  // namespace ff7cc::prx
