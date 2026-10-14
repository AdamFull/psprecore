// ccprx — inspect PSP PRX modules (BOOT.BIN): module info, imports by name, relocated memory images.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "ff7cc/analysis/program.hpp"
#include "ff7cc/error.hpp"
#include "ff7cc/formats/elf.hpp"
#include "ff7cc/formats/prx.hpp"
#include "ff7cc/io/file.hpp"
#include "ff7cc/mips/allegrex.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"

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

nx::map<std::uint32_t, std::string> load_nids(const fs::path& path) {
    nx::map<std::uint32_t, std::string> out;
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
    nx::map<std::string, std::size_t> by_type;
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
    const auto names = nid_path.empty() ? nx::map<std::uint32_t, std::string>{} : load_nids(nid_path);
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
    nx::map<std::pair<std::string, std::uint32_t>, std::string> db;
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

// Function list with recovered names. Registered script natives are named "scr_<Name>"; when a handler is
// registered under several names, or a name maps to several handlers, the registrar address disambiguates.
int cmd_symbols(const fs::path& path, const fs::path& nid_path, const fs::path& out_path, std::uint32_t ghidra_base,
                const fs::path& ghidra_path) {
    const auto data = io::read_all(path);
    const elf::File f(data);
    analysis::Program program(f, prx::parse(f));
    auto functions = analysis::discover_functions(program);
    const auto registrations = analysis::find_registrations(program, functions);

    const auto nids = nid_path.empty() ? nx::map<std::uint32_t, std::string>{} : load_nids(nid_path);
    nx::map<std::uint32_t, std::string> import_names;
    for (const auto& imp : program.module().imports)
        for (const auto& fn : imp.functions)
            if (const auto it = nids.find(fn.nid); it != nids.end()) import_names[fn.stub] = it->second;

    nx::map<std::uint32_t, std::size_t> per_registrar;
    nx::map<std::string, nx::set<std::uint32_t>> handlers_by_name;
    for (const auto& r : registrations) {
        ++per_registrar[r.registrar];
        handlers_by_name[r.name].insert(r.handler);
    }
    // Prefix by registrar: kernel thread/callback creation vs. the script native registries.
    const auto prefix_for = [&](std::uint32_t registrar) -> std::string {
        const auto it = import_names.find(registrar);
        if (it == import_names.end()) return "scr_";
        if (it->second.find("CreateThread") != std::string::npos) return "thread_";
        if (it->second.find("CreateCallback") != std::string::npos) return "callback_";
        return it->second + "_";
    };
    const auto identifier = [](std::string s) {
        for (auto& c : s)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
        return s;
    };
    nx::map<std::uint32_t, std::string> handler_names;
    for (const auto& r : registrations) {
        std::string name = prefix_for(r.registrar) + identifier(r.name);
        if (handlers_by_name[r.name].size() > 1) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "_%x", r.registrar);
            name += buf;
        }
        handler_names.try_emplace(r.handler, name);
    }

    nx::map<std::string, std::size_t> by_origin;
    std::size_t named = 0;
    std::ofstream out(out_path, std::ios::binary);
    if (!out) throw IoError("cannot write '" + out_path.string() + "'");
    out << "# address (module-relative)\tsize\torigin\tname\n";
    std::ofstream ghidra;
    if (!ghidra_path.empty()) ghidra.open(ghidra_path, std::ios::binary);
    static constexpr const char* kOrigin[] = {"entry", "export", "import", "call", "pointer", "gap"};
    char buf[160];
    for (auto& fn : functions) {
        if (const auto it = import_names.find(fn.start); it != import_names.end()) fn.name = it->second;
        else if (const auto h = handler_names.find(fn.start); h != handler_names.end()) fn.name = h->second;
        else if (per_registrar.contains(fn.start)) {
            std::snprintf(buf, sizeof buf, "register_%x", fn.start);
            fn.name = buf;
        }
        if (!fn.name.empty()) ++named;
        const char* origin = kOrigin[static_cast<int>(fn.origin)];
        ++by_origin[origin];
        const std::string name = fn.name.empty() ? "" : fn.name;
        std::snprintf(buf, sizeof buf, "%08x\t%u\t%s\t", fn.start, fn.end - fn.start, origin);
        out << buf << name << '\n';
        if (ghidra) {  // ImportSymbolsScript.py format: "<name> <address> f" creates a function there
            if (name.empty())
                std::snprintf(buf, sizeof buf, "FUN_%08x 0x%08x f\n", ghidra_base + fn.start, ghidra_base + fn.start);
            else
                std::snprintf(buf, sizeof buf, "%s 0x%08x f\n", name.c_str(), ghidra_base + fn.start);
            ghidra << buf;
        }
    }
    std::fprintf(stderr, "%zu functions (%zu named):", functions.size(), named);
    for (const auto& [k, n] : by_origin) std::fprintf(stderr, " %s=%zu", k.c_str(), n);
    std::fprintf(stderr, "\n%zu registrations through %zu registrar functions:", registrations.size(), per_registrar.size());
    for (const auto& [addr, n] : per_registrar) std::fprintf(stderr, " %x(%zu)", addr, n);
    std::fprintf(stderr, "\n");
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
    const nx::vector<std::string_view> args(argv + 1, argv + argc);
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
        if (cmd == "symbols" && (args.size() == 4 || args.size() == 6))
            return cmd_symbols(fs::path(args[1]), fs::path(args[2]), fs::path(args[3]),
                               args.size() == 6 ? static_cast<std::uint32_t>(std::stoul(std::string(args[4]), nullptr, 0)) : 0,
                               args.size() == 6 ? fs::path(args[5]) : fs::path());
        if (cmd == "image" && args.size() == 4) return cmd_image(fs::path(args[1]), args[2], fs::path(args[3]));
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ccprx: %s\n", e.what());
        return 1;
    }
}
