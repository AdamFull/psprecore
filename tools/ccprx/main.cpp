// ccprx — inspect PSP PRX modules (BOOT.BIN): module info, imports by name, relocated memory images.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "ff7cc/error.hpp"
#include "ff7cc/formats/elf.hpp"
#include "ff7cc/formats/prx.hpp"
#include "ff7cc/io/file.hpp"
#include "ff7cc/mips/allegrex.hpp"

namespace fs = std::filesystem;
using namespace ff7cc;

namespace {

void usage() {
    std::fputs(
        "usage: ccprx <command> [args]\n"
        "\n"
        "  info    <module>                      module info, segments, imports/exports, relocations\n"
        "  imports <module> [nids.tsv]           every imported function (named when a NID database is given)\n"
        "  nids    <sdk-lib-dir> <nids.tsv>      build a NID database from PSPSDK stub libraries (lib*.a)\n"
        "  image   <module> <base> <out.bin>     write the loaded, relocated image (e.g. base 0x08804000)\n"
        "  disasm  <module> [section]            objdump-style listing (default .text, module-relative)\n",
        stderr);
}

std::map<std::uint32_t, std::string> load_nids(const fs::path& path) {
    std::map<std::uint32_t, std::string> out;
    std::ifstream in(path);
    if (!in) throw IoError("cannot read NID database '" + path.string() + "'");
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string library, nid, name;
        if (std::getline(fields, library, '\t') && std::getline(fields, nid, '\t') && std::getline(fields, name))
            out.emplace(static_cast<std::uint32_t>(std::stoul(nid, nullptr, 16)), name);
    }
    return out;
}

const char* reloc_name(std::uint8_t type) {
    switch (type) {
        case prx::kRelNone: return "NONE";
        case prx::kRel32: return "32";
        case prx::kRel26: return "26";
        case prx::kRelHi16: return "HI16";
        case prx::kRelLo16: return "LO16";
        default: return "?";
    }
}

int cmd_info(const fs::path& path) {
    const auto data = io::read_all(path);
    const elf::File f(data);
    const auto m = prx::parse(f);
    std::printf("module  %s  (attributes 0x%04x, version 0x%04x)\n", m.name.c_str(), m.attributes, m.version);
    std::printf("elf     type 0x%04x, entry 0x%08x, gp 0x%08x, %zu sections\n", f.type(), m.entry, m.gp,
                f.sections().size());
    for (std::size_t i = 0; i < m.segments.size(); ++i) {
        const auto& s = m.segments[i];
        std::printf("segment %zu  vaddr 0x%08x  filesz 0x%08x  memsz 0x%08x  flags %c%c%c\n", i, s.vaddr, s.filesz,
                    s.memsz, s.flags & 4 ? 'r' : '-', s.flags & 2 ? 'w' : '-', s.flags & 1 ? 'x' : '-');
    }
    std::size_t funcs = 0;
    for (const auto& imp : m.imports) funcs += imp.functions.size();
    std::printf("imports %zu libraries, %zu functions\n", m.imports.size(), funcs);
    for (const auto& exp : m.exports)
        std::printf("export  %-20s %zu functions, %zu variables\n", exp.library.empty() ? "(module)" : exp.library.c_str(),
                    exp.functions.size(), exp.variables.size());
    std::map<std::string, std::size_t> by_type;
    for (const auto& r : m.relocations)
        ++by_type[std::string(reloc_name(r.type)) + " seg" + std::to_string(r.offset_segment) + "->seg" +
                  std::to_string(r.address_segment)];
    std::printf("relocations %zu\n", m.relocations.size());
    for (const auto& [k, n] : by_type) std::printf("  %-18s %zu\n", k.c_str(), n);
    return 0;
}

int cmd_imports(const fs::path& path, const fs::path& nid_path) {
    const auto data = io::read_all(path);
    const elf::File f(data);
    const auto m = prx::parse(f);
    const auto names = nid_path.empty() ? std::map<std::uint32_t, std::string>{} : load_nids(nid_path);
    std::size_t named = 0, total = 0;
    std::puts("library\tnid\tstub\tname");
    for (const auto& imp : m.imports) {
        for (const auto& fn : imp.functions) {
            const auto it = names.find(fn.nid);
            ++total;
            if (it != names.end()) ++named;
            std::printf("%s\t%08X\t0x%08x\t%s\n", imp.library.c_str(), fn.nid, fn.stub,
                        it != names.end() ? it->second.c_str() : "?");
        }
    }
    if (!nid_path.empty()) std::fprintf(stderr, "%zu of %zu imports named\n", named, total);
    return 0;
}

int cmd_nids(const fs::path& dir, const fs::path& out_path) {
    std::map<std::pair<std::string, std::uint32_t>, std::string> db;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".a") continue;
        try {
            for (auto& n : prx::nids_from_stub_archive(io::read_all(e.path())))
                db.emplace(std::pair(n.library, n.nid), n.name);
        } catch (const FormatError&) {
            // Not a stub library (or not an archive we understand): skip it.
        }
    }
    std::ofstream out(out_path, std::ios::binary);
    if (!out) throw IoError("cannot write '" + out_path.string() + "'");
    out << "# library\tnid\tname (generated by ccprx nids from " << dir.filename().string() << ")\n";
    char nid[16];
    for (const auto& [key, name] : db) {
        std::snprintf(nid, sizeof nid, "%08X", key.second);
        out << key.first << '\t' << nid << '\t' << name << '\n';
    }
    std::printf("%zu NIDs written to %s\n", db.size(), out_path.string().c_str());
    return 0;
}

int cmd_image(const fs::path& path, std::string_view base_text, const fs::path& out_path) {
    const auto data = io::read_all(path);
    const elf::File f(data);
    const auto m = prx::parse(f);
    const auto base = static_cast<std::uint32_t>(std::stoul(std::string(base_text), nullptr, 0));
    const auto image = prx::load_image(f, m, base);
    io::write_all(out_path, image);
    std::printf("wrote %zu bytes (%zu relocations applied) for base 0x%08x\n", image.size(), m.relocations.size(),
                base);
    return 0;
}

int cmd_disasm(const fs::path& path, std::string_view section_name) {
    const auto data = io::read_all(path);
    const elf::File f(data);
    const auto* section = f.find_section(section_name);
    if (!section) throw FormatError("no section '" + std::string(section_name) + "'");
    const auto bytes = f.contents(*section);
    std::string out;
    char buf[48];
    for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        std::uint32_t word;
        std::memcpy(&word, bytes.data() + i, 4);
        const auto pc = section->addr + static_cast<std::uint32_t>(i);
        std::snprintf(buf, sizeof buf, "%8x:\t%08x \t", pc, word);
        out += buf;
        out += mips::format(mips::decode(word, pc));
        out += '\n';
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        usage();
        return args.empty() ? 2 : 0;
    }
    try {
        const auto cmd = args[0];
        if (cmd == "info" && args.size() == 2) return cmd_info(fs::path(args[1]));
        if (cmd == "imports" && (args.size() == 2 || args.size() == 3))
            return cmd_imports(fs::path(args[1]), args.size() == 3 ? fs::path(args[2]) : fs::path());
        if (cmd == "nids" && args.size() == 3) return cmd_nids(fs::path(args[1]), fs::path(args[2]));
        if (cmd == "disasm" && (args.size() == 2 || args.size() == 3))
            return cmd_disasm(fs::path(args[1]), args.size() == 3 ? args[2] : std::string_view(".text"));
        if (cmd == "image" && args.size() == 4) return cmd_image(fs::path(args[1]), args[2], fs::path(args[3]));
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ccprx: %s\n", e.what());
        return 1;
    }
}
