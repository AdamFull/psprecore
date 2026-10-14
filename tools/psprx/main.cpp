// psprx — inspect PSP PRX modules (BOOT.BIN): module info, imports by name, relocated memory images.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>

#include "psprecore/analysis/program.hpp"
#include "psprecore/cli.hpp"
#include "psprecore/error.hpp"
#include "psprecore/formats/elf.hpp"
#include "psprecore/formats/prx.hpp"
#include "psprecore/io/file.hpp"
#include "psprecore/mips/allegrex.hpp"
#include "psprecore/text.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/map.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

using namespace psprecore;

namespace {

// library \t nid (hex) \t name; lines without all three fields are skipped.
nx::map<std::uint32_t, nx::string> load_nids(nx::fs::path_view path) {
    nx::map<std::uint32_t, nx::string> out;
    const auto file = nx::fs::file_read_text(path);
    if (!file) throw IoError(nx::string("cannot read NID database '") + path.native() + "'");
    const nx::string_view text = file.value();
    for (std::size_t pos = 0; pos < text.size();) {
        const std::size_t nl = text.find('\n', pos);
        const std::size_t end = nl == nx::string_view::npos ? text.size() : nl;
        nx::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() || line[0] == '#') continue;
        const std::size_t t1 = line.find('\t');
        const std::size_t t2 = t1 == nx::string_view::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == nx::string_view::npos || t2 + 1 == line.size()) continue;
        nx::string_view nid = line.substr(t1 + 1, t2 - t1 - 1);
        if (nid.starts_with("0x") || nid.starts_with("0X")) nid.remove_prefix(2);
        out.emplace(static_cast<std::uint32_t>(parse_u64(nid, 16)), nx::string(line.substr(t2 + 1)));
    }
    return out;
}

// Decimal or 0x-prefixed hex, at most 32 bits.
std::uint32_t parse_address(nx::string_view text) {
    const std::uint64_t value = parse_u64(text, 0);
    if (value > 0xFFFFFFFFu) throw Error(nx::string("address out of range: '") + text + "'");
    return static_cast<std::uint32_t>(value);
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

int cmd_info(nx::fs::path_view path) {
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
    nx::map<nx::string, std::size_t> by_type;
    for (const auto& r : m.relocations)
        ++by_type[nx::format("{} seg{}->seg{}", reloc_name(r.type), r.offset_segment, r.address_segment)];
    std::printf("relocations %zu\n", m.relocations.size());
    for (const auto& [k, n] : by_type) std::printf("  %-18s %zu\n", k.c_str(), n);
    return 0;
}

int cmd_imports(nx::fs::path_view path, nx::fs::path_view nid_path) {
    const auto data = io::read_all(path);
    const elf::File f(data);
    const auto m = prx::parse(f);
    const auto names = nid_path.empty() ? nx::map<std::uint32_t, nx::string>{} : load_nids(nid_path);
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

int cmd_nids(nx::fs::path_view dir, nx::fs::path_view out_path) {
    nx::map<std::pair<nx::string, std::uint32_t>, nx::string> db;
    const auto entries = nx::fs::directory_entries(dir);
    if (!entries) throw IoError(nx::string("cannot list directory '") + dir.native() + "'");
    for (const auto& e : entries.value()) {
        if (e.type != nx::fs::file_type::regular || nx::fs::path_view(e.path).extension() != ".a") continue;
        try {
            for (auto& n : prx::nids_from_stub_archive(io::read_all(e.path)))
                db.emplace(std::pair(n.library, n.nid), n.name);
        } catch (const FormatError&) {
            // Not a stub library (or not an archive we understand): skip it.
        }
    }
    nx::string out = nx::format("# library\tnid\tname (generated by psprx nids from {})\n", dir.filename().native());
    for (const auto& [key, name] : db) out += nx::format("{}\t{:08X}\t{}\n", key.first, key.second, name);
    if (!nx::fs::file_write_text(out_path, out)) throw IoError(nx::string("cannot write '") + out_path.native() + "'");
    std::printf("%zu NIDs written to %s\n", db.size(), nx::string(out_path.native()).c_str());
    return 0;
}

int cmd_image(nx::fs::path_view path, nx::string_view base_text, nx::fs::path_view out_path) {
    const auto data = io::read_all(path);
    const elf::File f(data);
    const auto m = prx::parse(f);
    const auto base = parse_address(base_text);
    const auto image = prx::load_image(f, m, base);
    io::write_all(out_path, image);
    std::printf("wrote %zu bytes (%zu relocations applied) for base 0x%08x\n", image.size(), m.relocations.size(),
                base);
    return 0;
}

// Function list with recovered names. Registered script natives are named "scr_<Name>"; when a handler is
// registered under several names, or a name maps to several handlers, the registrar address disambiguates.
int cmd_symbols(nx::fs::path_view path, nx::fs::path_view nid_path, nx::fs::path_view out_path,
                std::uint32_t ghidra_base, nx::fs::path_view ghidra_path) {
    const auto data = io::read_all(path);
    const elf::File f(data);
    analysis::Program program(f, prx::parse(f));
    auto functions = analysis::discover_functions(program);
    const auto registrations = analysis::find_registrations(program, functions);

    const auto nids = nid_path.empty() ? nx::map<std::uint32_t, nx::string>{} : load_nids(nid_path);
    nx::map<std::uint32_t, nx::string> import_names;
    for (const auto& imp : program.module().imports)
        for (const auto& fn : imp.functions)
            if (const auto it = nids.find(fn.nid); it != nids.end()) import_names[fn.stub] = it->second;

    nx::map<std::uint32_t, std::size_t> per_registrar;
    nx::map<nx::string, nx::set<std::uint32_t>> handlers_by_name;
    for (const auto& r : registrations) {
        ++per_registrar[r.registrar];
        handlers_by_name[r.name].insert(r.handler);
    }
    // Prefix by registrar: kernel thread/callback creation vs. the script native registries.
    const auto prefix_for = [&](std::uint32_t registrar) -> nx::string {
        const auto it = import_names.find(registrar);
        if (it == import_names.end()) return "scr_";
        if (it->second.contains("CreateThread")) return "thread_";
        if (it->second.contains("CreateCallback")) return "callback_";
        return it->second + "_";
    };
    const auto identifier = [](nx::string s) {
        for (auto& c : s)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
        return s;
    };
    nx::map<std::uint32_t, nx::string> handler_names;
    for (const auto& r : registrations) {
        nx::string name = prefix_for(r.registrar) + identifier(r.name);
        if (handlers_by_name[r.name].size() > 1) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "_%x", r.registrar);
            name += buf;
        }
        handler_names.try_emplace(r.handler, name);
    }

    nx::map<nx::string, std::size_t> by_origin;
    std::size_t named = 0;
    nx::string out = "# address (module-relative)\tsize\torigin\tname\n";
    nx::string ghidra;
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
        const nx::string& name = fn.name;
        std::snprintf(buf, sizeof buf, "%08x\t%u\t%s\t", fn.start, fn.end - fn.start, origin);
        out += buf;
        out += name;
        out += '\n';
        if (!ghidra_path.empty()) {  // ImportSymbolsScript.py format: "<name> <address> f" creates a function there
            if (name.empty())
                std::snprintf(buf, sizeof buf, "FUN_%08x 0x%08x f\n", ghidra_base + fn.start, ghidra_base + fn.start);
            else
                std::snprintf(buf, sizeof buf, "%s 0x%08x f\n", name.c_str(), ghidra_base + fn.start);
            ghidra += buf;
        }
    }
    if (!nx::fs::file_write_text(out_path, out)) throw IoError(nx::string("cannot write '") + out_path.native() + "'");
    if (!ghidra_path.empty() && !nx::fs::file_write_text(ghidra_path, ghidra))
        throw IoError(nx::string("cannot write '") + ghidra_path.native() + "'");
    std::fprintf(stderr, "%zu functions (%zu named):", functions.size(), named);
    for (const auto& [k, n] : by_origin) std::fprintf(stderr, " %s=%zu", k.c_str(), n);
    std::fprintf(stderr, "\n%zu registrations through %zu registrar functions:", registrations.size(), per_registrar.size());
    for (const auto& [addr, n] : per_registrar) std::fprintf(stderr, " %x(%zu)", addr, n);
    std::fprintf(stderr, "\n");
    return 0;
}

int cmd_disasm(nx::fs::path_view path, nx::string_view section_name) {
    const auto data = io::read_all(path);
    const elf::File f(data);
    const auto* section = f.find_section(section_name);
    if (!section) throw FormatError(nx::string("no section '") + section_name + "'");
    const auto bytes = f.contents(*section);
    nx::string out;
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
    nx::string module, nids, out, section = ".text", base, ghidra;
    nx::cli::Parser cli("psprx", "Inspects PSP PRX modules (e.g. BOOT.BIN).");
    cli.command("info", "module info, segments, imports/exports, relocations").positional("module", module, "the PRX");
    cli.command("imports", "every imported function (named when a NID database is given)")
        .positional("module", module, "the PRX")
        .positional("nids", nids, "NID database (psprx nids)", {.required = false});
    cli.command("nids", "build a NID database from PSPSDK stub libraries (lib*.a)")
        .positional("sdk-lib-dir", module, "the SDK's psp/sdk/lib folder")
        .positional("out", out, "the database to write (.tsv)");
    cli.command("image", "write the loaded, relocated image")
        .positional("module", module, "the PRX")
        .positional("base", base, "load address, e.g. 0x08804000")
        .positional("out", out, "the image to write");
    cli.command("disasm", "objdump-style listing (module-relative addresses)")
        .positional("module", module, "the PRX or ELF object")
        .positional("section", section, "section to list (default: .text)", {.required = false});
    cli.command("symbols", "function list with recovered names, optionally as a Ghidra import")
        .positional("module", module, "the PRX")
        .positional("nids", nids, "NID database")
        .positional("out", out, "symbols to write (.tsv)")
        .positional("base", base, "load address of the Ghidra program", {.required = false})
        .positional("ghidra", ghidra, "Ghidra symbol file to write (needs <base>)", {.required = false});
    nx::string command;
    if (const int code = parse_command_line(cli, argc, argv, &command); code >= 0) return code;
    try {
        if (command == "info") return cmd_info(module);
        if (command == "imports") return cmd_imports(module, nids);
        if (command == "nids") return cmd_nids(module, out);
        if (command == "image") return cmd_image(module, base, out);
        if (command == "disasm") return cmd_disasm(module, section);
        if (command == "symbols") {
            if (base.empty() != ghidra.empty()) throw Error("symbols: give both <base> and <ghidra>, or neither");
            return cmd_symbols(module, nids, out, base.empty() ? 0 : parse_address(base), ghidra);
        }
        std::fputs(cli.help().c_str(), stderr);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "psprx: %s\n", e.what());
        return 1;
    }
}
