#include "psprecore/formats/prx.hpp"

#include <algorithm>
#include <cstring>

#include "psprecore/error.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/strings/format.h"

namespace psprecore::prx {

namespace {

template <typename T>
T load(std::span<const std::byte> data, std::size_t at) {
    if (at > data.size() || sizeof(T) > data.size() - at) throw FormatError("PRX data out of bounds");
    T v;
    std::memcpy(&v, data.data() + at, sizeof v);
    return v;
}

// Module-relative virtual address -> file offset (file-backed part of a loadable segment only).
std::size_t file_offset(const nx::vector<elf::Segment>& segments, std::uint32_t vaddr, std::size_t size = 1) {
    for (const auto& s : segments) {
        if (s.type != 1) continue;  // PT_LOAD
        if (vaddr >= s.vaddr && vaddr - s.vaddr + size <= s.filesz) return s.offset + (vaddr - s.vaddr);
    }
    throw FormatError("address 0x" + nx::format("{}", vaddr) + " is not backed by the module file");
}

nx::string read_c_string(const elf::File& f, const nx::vector<elf::Segment>& segs, std::uint32_t vaddr) {
    const auto data = f.data();
    std::size_t at = file_offset(segs, vaddr);
    nx::string s;
    while (at < data.size() && data[at] != std::byte{0}) s += static_cast<char>(data[at++]);
    return s;
}

}  // namespace

std::uint32_t read_word(const elf::File& file, const Module& module, std::uint32_t vaddr) {
    return load<std::uint32_t>(file.data(), file_offset(module.segments, vaddr, 4));
}

Module parse(const elf::File& file) {
    if (file.machine() != elf::kMachineMips) throw FormatError("not a MIPS module");
    Module m;
    m.segments = file.segments();
    m.entry = file.entry();
    const auto data = file.data();

    // Module info: the .rodata.sceModuleInfo section, or (stripped PRX) the file offset in segment 0's paddr.
    std::size_t info_at = 0;
    if (const auto* s = file.find_section(".rodata.sceModuleInfo"))
        info_at = s->offset;
    else if (!m.segments.empty())
        info_at = m.segments[0].paddr & 0x7FFFFFFF;
    else
        throw FormatError("module info not found");
    m.attributes = load<std::uint16_t>(data, info_at);
    m.version = load<std::uint16_t>(data, info_at + 2);
    for (std::size_t i = 0; i < 28 && data[info_at + 4 + i] != std::byte{0}; ++i)
        m.name += static_cast<char>(data[info_at + 4 + i]);
    m.gp = load<std::uint32_t>(data, info_at + 32);
    const auto ent_top = load<std::uint32_t>(data, info_at + 36);
    const auto ent_end = load<std::uint32_t>(data, info_at + 40);
    const auto stub_top = load<std::uint32_t>(data, info_at + 44);
    const auto stub_end = load<std::uint32_t>(data, info_at + 48);

    const auto word = [&](std::uint32_t vaddr) { return read_word(file, m, vaddr); };

    for (std::uint32_t at = stub_top; at < stub_end;) {
        const auto entry = file_offset(m.segments, at, 20);
        Import imp;
        const auto name_ptr = load<std::uint32_t>(data, entry);
        imp.library = name_ptr ? read_c_string(file, m.segments, name_ptr) : "";
        imp.version = load<std::uint16_t>(data, entry + 4);
        imp.attributes = load<std::uint16_t>(data, entry + 6);
        const auto entsize = load<std::uint8_t>(data, entry + 8);
        const auto var_count = load<std::uint8_t>(data, entry + 9);
        const auto func_count = load<std::uint16_t>(data, entry + 10);
        const auto nid_table = load<std::uint32_t>(data, entry + 12);
        const auto stub_table = load<std::uint32_t>(data, entry + 16);
        for (std::uint32_t i = 0; i < func_count; ++i)
            imp.functions.push_back({word(nid_table + 4 * i), stub_table + 8 * i});
        if (var_count && entsize >= 6) {
            const auto var_table = load<std::uint32_t>(data, entry + 20);
            for (std::uint32_t i = 0; i < var_count; ++i) imp.variable_nids.push_back(word(var_table + 8 * i + 4));
        }
        m.imports.push_back(std::move(imp));
        if (entsize == 0) throw FormatError("import entry with zero size");
        at += entsize * 4u;
    }

    for (std::uint32_t at = ent_top; at < ent_end;) {
        const auto entry = file_offset(m.segments, at, 16);
        Export exp;
        const auto name_ptr = load<std::uint32_t>(data, entry);
        exp.library = name_ptr ? read_c_string(file, m.segments, name_ptr) : "";
        exp.version = load<std::uint16_t>(data, entry + 4);
        exp.attributes = load<std::uint16_t>(data, entry + 6);
        const auto entsize = load<std::uint8_t>(data, entry + 8);
        const auto var_count = load<std::uint8_t>(data, entry + 9);
        const auto func_count = load<std::uint16_t>(data, entry + 10);
        const auto table = load<std::uint32_t>(data, entry + 12);
        const std::uint32_t total = func_count + var_count;
        for (std::uint32_t i = 0; i < total; ++i) {
            const Export::Entry e{word(table + 4 * i), word(table + 4 * (total + i))};
            (i < func_count ? exp.functions : exp.variables).push_back(e);
        }
        m.exports.push_back(std::move(exp));
        if (entsize == 0) throw FormatError("export entry with zero size");
        at += entsize * 4u;
    }

    for (const auto& s : file.sections()) {
        if (s.type != elf::kSectionPspRel) continue;
        for (const auto& r : file.relocations(s))
            m.relocations.push_back(Relocation{r.offset, static_cast<std::uint8_t>(r.info & 0xFF),
                                               static_cast<std::uint8_t>((r.info >> 8) & 0xFF),
                                               static_cast<std::uint8_t>((r.info >> 16) & 0xFF)});
    }
    return m;
}

nx::vector<std::byte> load_image(const elf::File& file, const Module& module, std::uint32_t base) {
    std::uint32_t end = 0;
    for (const auto& s : module.segments)
        if (s.type == 1) end = std::max(end, s.vaddr + s.memsz);
    nx::vector<std::byte> image(end);
    for (const auto& s : module.segments) {
        if (s.type != 1) continue;
        const auto src = file.data().subspan(s.offset, s.filesz);
        std::copy(src.begin(), src.end(), image.begin() + s.vaddr);
    }

    const auto seg_base = [&](std::uint8_t index) -> std::uint32_t {
        if (index >= module.segments.size()) throw FormatError("relocation references a missing segment");
        return module.segments[index].vaddr;
    };
    const auto get = [&](std::uint32_t at) {
        if (at + 4 > image.size()) throw FormatError("relocation outside the module image");
        std::uint32_t v;
        std::memcpy(&v, image.data() + at, 4);
        return v;
    };
    const auto put = [&](std::uint32_t at, std::uint32_t v) { std::memcpy(image.data() + at, &v, 4); };

    // HI16 entries are resolved by the next LO16 (the full address needs both halves).
    nx::vector<std::uint32_t> pending_hi;
    for (const auto& r : module.relocations) {
        const std::uint32_t at = seg_base(r.offset_segment) + r.offset;
        const std::uint32_t delta = base + seg_base(r.address_segment);
        const std::uint32_t w = get(at);
        switch (r.type) {
            case kRelNone: break;
            case kRel32: put(at, w + delta); break;
            case kRel26: {
                const std::uint32_t target = ((w & 0x03FFFFFF) << 2) + delta;
                put(at, (w & 0xFC000000) | ((target >> 2) & 0x03FFFFFF));
                break;
            }
            case kRelHi16: pending_hi.push_back(at); break;
            case kRelLo16: {
                const auto lo = static_cast<std::int32_t>(static_cast<std::int16_t>(w & 0xFFFF));
                for (const auto hi_at : pending_hi) {
                    const std::uint32_t hw = get(hi_at);
                    const std::uint32_t full = ((hw & 0xFFFF) << 16) + static_cast<std::uint32_t>(lo) + delta;
                    put(hi_at, (hw & 0xFFFF0000) | (((full + 0x8000) >> 16) & 0xFFFF));
                }
                pending_hi.clear();
                put(at, (w & 0xFFFF0000) | ((static_cast<std::uint32_t>(lo) + delta) & 0xFFFF));
                break;
            }
            default: throw FormatError("unsupported PSP relocation type " + nx::format("{}", r.type));
        }
    }
    if (!pending_hi.empty()) throw FormatError("HI16 relocation without a following LO16");
    return image;
}

nx::vector<NidName> nids_from_stub_archive(std::span<const std::byte> archive) {
    nx::vector<NidName> out;
    for (const auto& member : elf::read_archive(archive)) {
        if (member.data.size() < 4 || std::memcmp(member.data.data(), "\x7f" "ELF", 4) != 0) continue;
        const elf::File obj(member.data);
        const auto* nid_section = obj.find_section(".rodata.sceNid");
        if (!nid_section) continue;
        const auto nids = obj.contents(*nid_section);

        nx::string library;
        nx::vector<elf::Symbol> functions;
        for (const auto& sym : obj.symbols()) {
            if (sym.name.starts_with("__stub_module_") && library.empty()) library = sym.name.substr(14);
            if (sym.binding() == 1 && sym.section != 0 && sym.section < obj.sections().size() &&
                obj.sections()[sym.section].name.starts_with(".sceStub.text"))
                functions.push_back(sym);
        }
        std::sort(functions.begin(), functions.end(), [](const auto& a, const auto& b) { return a.value < b.value; });
        for (std::size_t i = 0; i < functions.size() && 4 * i + 4 <= nids.size(); ++i)
            out.push_back(NidName{library, load<std::uint32_t>(nids, 4 * i), functions[i].name});
    }
    return out;
}

}  // namespace psprecore::prx
