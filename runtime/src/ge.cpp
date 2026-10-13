#include "ff7cc/runtime/ge.hpp"

#include <cstdio>

#include "ff7cc/runtime/memory.hpp"

namespace ff7cc::rt::ge {

namespace {

// GE command names (public knowledge of the command set, as used by the homebrew PSPSDK's libgu).
constexpr std::array<std::string_view, 256> kNames = [] {
    std::array<std::string_view, 256> n{};
    const auto set = [&](int i, std::string_view s) { n[static_cast<std::size_t>(i)] = s; };
    set(0x00, "NOP"); set(0x01, "VADDR"); set(0x02, "IADDR"); set(0x04, "PRIM"); set(0x05, "BEZIER");
    set(0x06, "SPLINE"); set(0x07, "BOUNDINGBOX"); set(0x08, "JUMP"); set(0x09, "BJUMP"); set(0x0A, "CALL");
    set(0x0B, "RET"); set(0x0C, "END"); set(0x0E, "SIGNAL"); set(0x0F, "FINISH"); set(0x10, "BASE");
    set(0x12, "VERTEXTYPE"); set(0x13, "OFFSETADDR"); set(0x14, "ORIGIN"); set(0x15, "REGION1"); set(0x16, "REGION2");
    set(0x17, "LIGHTINGENABLE"); set(0x18, "LIGHTENABLE0"); set(0x19, "LIGHTENABLE1"); set(0x1A, "LIGHTENABLE2");
    set(0x1B, "LIGHTENABLE3"); set(0x1C, "DEPTHCLAMPENABLE"); set(0x1D, "CULLFACEENABLE"); set(0x1E, "TEXTUREMAPENABLE");
    set(0x1F, "FOGENABLE"); set(0x20, "DITHERENABLE"); set(0x21, "ALPHABLENDENABLE"); set(0x22, "ALPHATESTENABLE");
    set(0x23, "ZTESTENABLE"); set(0x24, "STENCILTESTENABLE"); set(0x25, "ANTIALIASENABLE"); set(0x26, "PATCHCULLENABLE");
    set(0x27, "COLORTESTENABLE"); set(0x28, "LOGICOPENABLE"); set(0x2A, "BONEMATRIXNUMBER"); set(0x2B, "BONEMATRIXDATA");
    for (int i = 0; i < 8; ++i) n[0x2C + i] = "MORPHWEIGHT";
    set(0x36, "PATCHDIVISION"); set(0x37, "PATCHPRIMITIVE"); set(0x38, "PATCHFACING");
    set(0x3A, "WORLDMATRIXNUMBER"); set(0x3B, "WORLDMATRIXDATA"); set(0x3C, "VIEWMATRIXNUMBER"); set(0x3D, "VIEWMATRIXDATA");
    set(0x3E, "PROJMATRIXNUMBER"); set(0x3F, "PROJMATRIXDATA"); set(0x40, "TGENMATRIXNUMBER"); set(0x41, "TGENMATRIXDATA");
    set(0x42, "VIEWPORTXSCALE"); set(0x43, "VIEWPORTYSCALE"); set(0x44, "VIEWPORTZSCALE"); set(0x45, "VIEWPORTXCENTER");
    set(0x46, "VIEWPORTYCENTER"); set(0x47, "VIEWPORTZCENTER"); set(0x48, "TEXSCALEU"); set(0x49, "TEXSCALEV");
    set(0x4A, "TEXOFFSETU"); set(0x4B, "TEXOFFSETV"); set(0x4C, "OFFSETX"); set(0x4D, "OFFSETY");
    set(0x50, "SHADEMODE"); set(0x51, "REVERSENORMAL"); set(0x53, "MATERIALUPDATE"); set(0x54, "MATERIALEMISSIVE");
    set(0x55, "MATERIALAMBIENT"); set(0x56, "MATERIALDIFFUSE"); set(0x57, "MATERIALSPECULAR"); set(0x58, "MATERIALALPHA");
    set(0x5B, "MATERIALSPECULARCOEF"); set(0x5C, "AMBIENTCOLOR"); set(0x5D, "AMBIENTALPHA"); set(0x5E, "LIGHTMODE");
    for (int i = 0; i < 4; ++i) n[0x5F + i] = "LIGHTTYPE";
    for (int i = 0x63; i <= 0x6E; ++i) n[i] = "LIGHTPOSITION";
    for (int i = 0x6F; i <= 0x7A; ++i) n[i] = "LIGHTDIRECTION";
    for (int i = 0x7B; i <= 0x86; ++i) n[i] = "LIGHTATTENUATION";
    for (int i = 0x87; i <= 0x8A; ++i) n[i] = "LIGHTSPOTEXPONENT";
    for (int i = 0x8B; i <= 0x8E; ++i) n[i] = "LIGHTSPOTCUTOFF";
    for (int i = 0x8F; i <= 0x9A; ++i) n[i] = "LIGHTCOLOR";
    set(0x9B, "CULL"); set(0x9C, "FRAMEBUFPTR"); set(0x9D, "FRAMEBUFWIDTH"); set(0x9E, "ZBUFPTR"); set(0x9F, "ZBUFWIDTH");
    for (int i = 0; i < 8; ++i) { n[0xA0 + i] = "TEXADDR"; n[0xA8 + i] = "TEXBUFWIDTH"; n[0xB8 + i] = "TEXSIZE"; }
    set(0xB0, "CLUTADDR"); set(0xB1, "CLUTADDRUPPER"); set(0xB2, "TRANSFERSRC"); set(0xB3, "TRANSFERSRCW");
    set(0xB4, "TRANSFERDST"); set(0xB5, "TRANSFERDSTW"); set(0xC0, "TEXMAPMODE"); set(0xC1, "TEXSHADELS");
    set(0xC2, "TEXMODE"); set(0xC3, "TEXFORMAT"); set(0xC4, "LOADCLUT"); set(0xC5, "CLUTFORMAT"); set(0xC6, "TEXFILTER");
    set(0xC7, "TEXWRAP"); set(0xC8, "TEXLEVEL"); set(0xC9, "TEXFUNC"); set(0xCA, "TEXENVCOLOR"); set(0xCB, "TEXFLUSH");
    set(0xCC, "TEXSYNC"); set(0xCD, "FOG1"); set(0xCE, "FOG2"); set(0xCF, "FOGCOLOR"); set(0xD0, "TEXLODSLOPE");
    set(0xD2, "FRAMEBUFPIXFORMAT"); set(0xD3, "CLEARMODE"); set(0xD4, "SCISSOR1"); set(0xD5, "SCISSOR2");
    set(0xD6, "MINZ"); set(0xD7, "MAXZ"); set(0xD8, "COLORTEST"); set(0xD9, "COLORREF"); set(0xDA, "COLORTESTMASK");
    set(0xDB, "ALPHATEST"); set(0xDC, "STENCILTEST"); set(0xDD, "STENCILOP"); set(0xDE, "ZTEST"); set(0xDF, "BLENDMODE");
    set(0xE0, "BLENDFIXEDA"); set(0xE1, "BLENDFIXEDB"); for (int i = 0; i < 4; ++i) n[0xE2 + i] = "DITHER";
    set(0xE6, "LOGICOP"); set(0xE7, "ZWRITEDISABLE"); set(0xE8, "MASKRGB"); set(0xE9, "MASKALPHA");
    set(0xEA, "TRANSFERSTART"); set(0xEB, "TRANSFERSRCPOS"); set(0xEC, "TRANSFERDSTPOS"); set(0xEE, "TRANSFERSIZE");
    return n;
}();

constexpr const char* kPrims[8] = {"points", "lines", "line_strip", "triangles", "triangle_strip", "triangle_fan",
                                   "sprites", "?"};

}  // namespace

std::string_view command_name(std::uint8_t cmd) {
    const auto n = kNames[cmd];
    return n.empty() ? std::string_view("UNKNOWN") : n;
}

std::string describe(std::uint32_t word) {
    const std::uint8_t cmd = static_cast<std::uint8_t>(word >> 24);
    const std::uint32_t p = word & 0xFFFFFF;
    char buf[96];
    if (cmd == PRIM)
        std::snprintf(buf, sizeof buf, "PRIM %s x%u", kPrims[(p >> 16) & 7], p & 0xFFFF);
    else
        std::snprintf(buf, sizeof buf, "%s 0x%06X", std::string(command_name(cmd)).c_str(), p);
    return buf;
}

int Processor::enqueue(std::uint32_t list, std::uint32_t stall, bool) {
    const int id = next_id_++;
    auto& l = lists_[id];
    l.pc = list & 0x0FFFFFFF;
    l.stall = stall & 0x0FFFFFFF;
    ++stats_.lists;
    run(l);
    // Keep only unfinished lists (plus a little history for list_done()).
    for (auto it = lists_.begin(); it != lists_.end() && lists_.size() > 64;)
        it = it->second.done ? lists_.erase(it) : std::next(it);
    return id;
}

void Processor::update_stall(int id, std::uint32_t stall) {
    const auto it = lists_.find(id);
    if (it == lists_.end() || it->second.done) return;
    it->second.stall = stall & 0x0FFFFFFF;
    run(it->second);
}

bool Processor::list_done(int id) const {
    const auto it = lists_.find(id);
    return it == lists_.end() || it->second.done;
}

void Processor::run(List& l) {
    for (int guard = 0; guard < 10'000'000; ++guard) {
        if (l.done || (l.stall && l.pc == l.stall)) return;
        const std::uint32_t pc = l.pc;
        const std::uint32_t word = memory_.read32(pc);
        const std::uint8_t cmd = static_cast<std::uint8_t>(word >> 24);
        const std::uint32_t p = word & 0xFFFFFF;
        l.pc += 4;
        ++stats_.commands;
        regs_[cmd] = word;
        if (on_command) on_command(pc, word);

        switch (cmd) {
            case BASE: base_ = (p << 8) & 0xFF000000; break;
            case OFFSETADDR: offset_ = p << 8; break;
            case ORIGIN: offset_ = pc; break;
            case VADDR: vaddr_ = address(p); break;
            case IADDR: iaddr_ = address(p); break;
            case JUMP: l.pc = address(p & ~3u); break;
            case CALL:
                l.call_stack.push_back(l.pc);
                l.call_stack.push_back(offset_);
                l.pc = address(p & ~3u);
                break;
            case RET:
                if (l.call_stack.size() >= 2) {
                    offset_ = l.call_stack.back();
                    l.call_stack.pop_back();
                    l.pc = l.call_stack.back();
                    l.call_stack.pop_back();
                }
                break;
            case FINISH: break;  // END follows
            case END: l.done = true; return;
            case PRIM: {
                Draw d{pc, (p >> 16) & 7, p & 0xFFFF, regs_[VERTEXTYPE] & 0xFFFFFF, vaddr_, iaddr_};
                ++stats_.draws;
                stats_.vertices += d.count;
                if (on_draw) on_draw(d);
                // The GE advances the vertex (or index) pointer past the consumed data; the exact stride is
                // computed by the vertex decoder (Phase 5). Lists in practice set VADDR before each PRIM.
                break;
            }
            case TRANSFERSTART: ++stats_.transfers; break;
            default: break;
        }
    }
    std::fprintf(stderr, "GE: list did not terminate (pc 0x%08X)\n", l.pc);
    l.done = true;
}

}  // namespace ff7cc::rt::ge
