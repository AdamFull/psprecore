#include "ff7cc/runtime/ge.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

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

VertexFormat VertexFormat::decode(std::uint32_t vt) {
    VertexFormat f;
    f.raw = vt;
    f.tc = vt & 3;
    f.color = (vt >> 2) & 7;
    f.normal = (vt >> 5) & 3;
    f.pos = (vt >> 7) & 3;
    f.weight = (vt >> 9) & 3;
    f.index = (vt >> 11) & 3;
    f.weights = static_cast<int>((vt >> 14) & 7) + 1;
    f.morphs = static_cast<int>((vt >> 18) & 7) + 1;
    f.through = (vt >> 23) & 1;
    int offset = 0, max_align = 1;
    const auto place = [&](int size, int align) {
        offset = (offset + align - 1) & ~(align - 1);
        const int at = offset;
        offset += size;
        max_align = std::max(max_align, align);
        return at;
    };
    static constexpr int kScalar[4] = {0, 1, 2, 4};  // none, 8-bit, 16-bit, float
    if (f.weight) place(kScalar[f.weight] * f.weights, kScalar[f.weight]);
    if (f.tc) f.tc_off = place(kScalar[f.tc] * 2, kScalar[f.tc]);
    if (f.color >= 4) f.color_off = place(f.color == 7 ? 4 : 2, f.color == 7 ? 4 : 2);
    if (f.normal) f.normal_off = place(kScalar[f.normal] * 3, kScalar[f.normal]);
    f.pos_off = place(kScalar[f.pos ? f.pos : 3] * 3, kScalar[f.pos ? f.pos : 3]);
    f.size = (offset + max_align - 1) & ~(max_align - 1);
    return f;
}

namespace {

// Reads component `i` of an attribute with encoding `kind` (1: 8-bit, 2: 16-bit, 3: float).
float component(const std::uint8_t* p, int kind, int i, bool is_signed, float scale) {
    switch (kind) {
        case 1: return (is_signed ? static_cast<float>(static_cast<std::int8_t>(p[i])) : static_cast<float>(p[i])) * scale;
        case 2: {
            std::uint16_t v;
            std::memcpy(&v, p + i * 2, 2);
            return (is_signed ? static_cast<float>(static_cast<std::int16_t>(v)) : static_cast<float>(v)) * scale;
        }
        case 3: {
            float v;
            std::memcpy(&v, p + i * 4, 4);
            return v;
        }
        default: return 0.0f;
    }
}

void decode_color(const std::uint8_t* p, int kind, float out[4]) {
    std::uint32_t c = 0;
    std::memcpy(&c, p, kind == 7 ? 4 : 2);
    switch (kind) {
        case 4:  // 5650
            out[0] = static_cast<float>(c & 31) / 31.0f;
            out[1] = static_cast<float>((c >> 5) & 63) / 63.0f;
            out[2] = static_cast<float>((c >> 11) & 31) / 31.0f;
            out[3] = 1.0f;
            break;
        case 5:  // 5551
            out[0] = static_cast<float>(c & 31) / 31.0f;
            out[1] = static_cast<float>((c >> 5) & 31) / 31.0f;
            out[2] = static_cast<float>((c >> 10) & 31) / 31.0f;
            out[3] = static_cast<float>((c >> 15) & 1);
            break;
        case 6:  // 4444
            for (int i = 0; i < 4; ++i) out[i] = static_cast<float>((c >> (i * 4)) & 15) / 15.0f;
            break;
        default:  // 8888
            for (int i = 0; i < 4; ++i) out[i] = static_cast<float>((c >> (i * 8)) & 255) / 255.0f;
            break;
    }
}

// Decodes one vertex at `p`, blending morph targets with `morph_weights`. A vertex without a color component
// gets color[0] = -1 so the renderer substitutes the material color.
void decode_vertex(const VertexFormat& f, const std::uint8_t* p, const float* morph_weights, Vertex& v) {
    v = Vertex{};
    std::fill(std::begin(v.color), std::end(v.color), 0.0f);
    const bool through = f.through;
    const int morphs = through ? 1 : f.morphs;
    for (int m = 0; m < morphs; ++m) {
        const std::uint8_t* t = p + m * f.size;
        const float w = morphs == 1 ? 1.0f : morph_weights[m];
        if (f.weight)
            for (int i = 0; i < f.weights; ++i)
                v.weights[i] += w * component(t, f.weight, i, false, f.weight == 1 ? 1.0f / 128 : 1.0f / 32768);
        if (f.tc) {
            const float scale = through ? 1.0f : f.tc == 1 ? 1.0f / 128 : 1.0f / 32768;
            for (int i = 0; i < 2; ++i) v.uv[i] += w * component(t + f.tc_off, f.tc, i, false, scale);
        }
        if (f.color >= 4) {
            float c[4];
            decode_color(t + f.color_off, f.color, c);
            for (int i = 0; i < 4; ++i) v.color[i] += w * c[i];
        }
        if (f.normal)
            for (int i = 0; i < 3; ++i)
                v.normal[i] += w * component(t + f.normal_off, f.normal, i, true,
                                             f.normal == 1 ? 1.0f / 128 : 1.0f / 32768);
        const int pk = f.pos ? f.pos : 3;
        if (through) {  // screen coordinates; z is unsigned
            for (int i = 0; i < 3; ++i) v.pos[i] = component(t + f.pos_off, pk, i, i < 2, 1.0f);
        } else {
            for (int i = 0; i < 3; ++i)
                v.pos[i] += w * component(t + f.pos_off, pk, i, true, pk == 1 ? 1.0f / 128 : 1.0f / 32768);
        }
    }
    if (f.color < 4) v.color[0] = -1.0f;
}

}  // namespace

void Processor::draw(std::uint32_t p) {
    const auto prim = static_cast<Prim>((p >> 16) & 7);
    const std::uint32_t count = p & 0xFFFF;
    state_.format = VertexFormat::decode(state_.param(VERTEXTYPE));
    const VertexFormat& f = state_.format;
    ++stats_.draws;
    stats_.vertices += count;
    if (on_draw) on_draw(Draw{0, static_cast<std::uint32_t>(prim), count, f.raw, vaddr_, iaddr_});

    float morph_weights[8];
    for (int i = 0; i < 8; ++i) morph_weights[i] = state_.flt(static_cast<std::uint8_t>(MORPHWEIGHT0 + i));
    const int index_size = f.index_size();
    vertices_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t n = i;
        if (index_size == 1) {
            n = *memory_.ptr(iaddr_ + i);
        } else if (index_size == 2) {
            std::uint16_t x;
            std::memcpy(&x, memory_.ptr(iaddr_ + i * 2), 2);
            n = x;
        } else if (index_size == 4) {
            n = memory_.read32(iaddr_ + i * 4);
        }
        decode_vertex(f, memory_.ptr(vram_canonical(vaddr_ + n * static_cast<std::uint32_t>(f.stride()))),
                      morph_weights, vertices_[i]);
    }
    // The GE leaves the pointer it consumed just past the data, so consecutive PRIMs can continue.
    if (index_size)
        iaddr_ += count * static_cast<std::uint32_t>(index_size);
    else
        vaddr_ += count * static_cast<std::uint32_t>(f.stride());

    if (renderer_ && prim <= Prim::Sprites && count) renderer_->draw(state_, prim, vertices_);
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
    if (renderer_) renderer_->begin_batch();
    for (int guard = 0; guard < 10'000'000; ++guard) {
        if (l.done || (l.stall && l.pc == l.stall)) return;
        const std::uint32_t pc = l.pc;
        const std::uint32_t word = memory_.read32(pc);
        const std::uint8_t cmd = static_cast<std::uint8_t>(word >> 24);
        const std::uint32_t p = word & 0xFFFFFF;
        l.pc += 4;
        ++stats_.commands;
        state_.regs[cmd] = word;
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
            case SIGNAL:
                l.signal = p;
                l.signal_pending = true;
                break;
            case FINISH:
                l.finish = p;
                l.finish_pending = true;
                break;
            case END:
                if (l.signal_pending) {  // END completes the SIGNAL; the list continues (sceGuSignal modes)
                    l.signal_pending = false;
                    const std::uint32_t behaviour = (l.signal >> 16) & 0xFF, id = l.signal & 0xFFFF;
                    switch (behaviour) {
                        case 1:  // wait: the GE resumes once the handler returns
                        case 2:  // no wait
                            if (on_signal) on_signal(id);
                            break;
                        case 3:  // pause until sceGeContinue: not modelled, the list continues
                            if (on_signal) on_signal(id);
                            [[fallthrough]];
                        default:
                            if (behaviour < 32 && !(warned_signals_ & (1u << behaviour))) {
                                std::fprintf(stderr, "GE: signal behaviour %u (id 0x%X) is not supported; continuing\n",
                                             behaviour, id);
                                warned_signals_ |= 1u << behaviour;
                            }
                            break;
                    }
                    break;
                }
                l.done = true;
                if (l.finish_pending) {
                    l.finish_pending = false;
                    if (on_finish) on_finish(l.finish & 0xFFFF);
                }
                return;
            case PRIM: draw(p); break;
            case BEZIER:
            case SPLINE:
                if (!warned_patches_) std::fprintf(stderr, "GE: bezier/spline patches are not rendered yet\n");
                warned_patches_ = true;
                break;
            case BOUNDINGBOX: bbox_visible_ = true; break;  // conservative: always "maybe visible"
            case BJUMP:
                if (!bbox_visible_) l.pc = address(p & ~3u);
                break;
            case WORLDMATRIXNUMBER: world_index_ = p & 15; break;
            case WORLDMATRIXDATA: if (world_index_ < 12) state_.world[world_index_++] = f24(p); break;
            case VIEWMATRIXNUMBER: view_index_ = p & 15; break;
            case VIEWMATRIXDATA: if (view_index_ < 12) state_.view[view_index_++] = f24(p); break;
            case PROJMATRIXNUMBER: proj_index_ = p & 15; break;
            case PROJMATRIXDATA: if (proj_index_ < 16) state_.proj[proj_index_++] = f24(p); break;
            case TGENMATRIXNUMBER: tgen_index_ = p & 15; break;
            case TGENMATRIXDATA: if (tgen_index_ < 12) state_.tgen[tgen_index_++] = f24(p); break;
            case BONEMATRIXNUMBER: bone_index_ = p & 127; break;
            case BONEMATRIXDATA:
                if (bone_index_ < 96) {
                    state_.bones[bone_index_ / 12][bone_index_ % 12] = f24(p);
                    ++bone_index_;
                }
                break;
            case LOADCLUT: {
                const std::uint32_t addr = vram_canonical(((state_.param(CLUTADDRUPPER) & 0x0F0000) << 8) |
                                                          (state_.param(CLUTADDR) & 0xFFFFF0));
                const std::uint32_t bytes = std::min<std::uint32_t>((p & 0x3F) * 32, 1024);
                if (addr) std::memcpy(state_.clut.data(), memory_.ptr(addr), bytes);
                break;
            }
            case TRANSFERSTART:
                ++stats_.transfers;
                if (renderer_) renderer_->transfer(state_);
                break;
            default: break;
        }
    }
    std::fprintf(stderr, "GE: list did not terminate (pc 0x%08X)\n", l.pc);
    l.done = true;
}

}  // namespace ff7cc::rt::ge
