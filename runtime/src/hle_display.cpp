// sceDisplay, sceGe_user, sceCtrl. Display lists are recorded but not rendered yet (Phase 5).

#include <cstdio>
#include <memory>

#include "ff7cc/runtime/ge.hpp"
#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/kernel.hpp"

namespace ff7cc::rt {

namespace {

GuestMemory& mem() { return kernel().memory(); }

struct DisplayState {
    std::uint32_t framebuffer = 0, stride = 512, format = 3;
    std::uint64_t lists = 0, frames = 0;
    int next_callback_id = 1;
    std::unique_ptr<ge::Processor> ge;
    std::uint64_t dump_frame = ~0ull;
    std::FILE* dump = nullptr;
} g_display;

ge::Processor& gpu() {
    if (!g_display.ge) {
        g_display.ge = std::make_unique<ge::Processor>(mem());
        g_display.ge->on_command = [](std::uint32_t pc, std::uint32_t word) {
            if (g_display.dump && g_display.frames == g_display.dump_frame)
                std::fprintf(g_display.dump, "%08X: %08X  %s\n", pc, word, ge::describe(word).c_str());
        };
    }
    return *g_display.ge;
}

// ---- sceDisplay --------------------------------------------------------------------------------

void SetMode(Context& c) { ret(c, 0); }
void SetFrameBuf(Context& c) {
    if (g_display.dump && g_display.frames == g_display.dump_frame) {
        const auto& st = gpu().stats();
        std::fprintf(g_display.dump, "--- end of frame %llu (totals: %llu lists, %llu draws, %llu vertices)\n",
                     static_cast<unsigned long long>(g_display.frames), static_cast<unsigned long long>(st.lists),
                     static_cast<unsigned long long>(st.draws), static_cast<unsigned long long>(st.vertices));
        std::fclose(g_display.dump);
        g_display.dump = nullptr;
        std::fprintf(stderr, "GE dump of frame %llu written\n", static_cast<unsigned long long>(g_display.dump_frame));
    }
    ++g_display.frames;
    g_display.framebuffer = arg(c, 0);
    g_display.stride = arg(c, 1);
    g_display.format = arg(c, 2);
    ret(c, 0);
}
void GetFrameBuf(Context& c) {
    if (arg(c, 0)) mem().write32(arg(c, 0), g_display.framebuffer);
    if (arg(c, 1)) mem().write32(arg(c, 1), g_display.stride);
    if (arg(c, 2)) mem().write32(arg(c, 2), g_display.format);
    ret(c, 0);
}
void WaitVblankStartCB(Context& c) {
    kernel().wait(WaitType::Vblank, 0, 0, true);
    ret(c, 0);
}

// ---- sceGe_user --------------------------------------------------------------------------------

void EdramGetAddr(Context& c) { ret(c, 0x04000000); }
void ListEnQueue(Context& c) {
    Kernel& k = kernel();
    if (g_display.lists++ == 0)
        std::fprintf(stderr, "*** first GE display list at 0x%08X (stall 0x%08X) after %.3f s of guest time, vblank %llu\n",
                     arg(c, 0), arg(c, 1), static_cast<double>(k.now()) / 1e6,
                     static_cast<unsigned long long>(k.vblank_count()));
    ret(c, static_cast<std::uint32_t>(gpu().enqueue(arg(c, 0), arg(c, 1), false)));
}
void UpdateStallAddr(Context& c) {
    gpu().update_stall(static_cast<int>(arg(c, 0)), arg(c, 1));
    ret(c, 0);
}
void ListSync(Context& c) { ret(c, 0); }  // lists complete immediately until the renderer exists
void DrawSync(Context& c) { ret(c, 0); }
void SetCallback(Context& c) { ret(c, static_cast<std::uint32_t>(g_display.next_callback_id++)); }
void ReturnZero(Context& c) { ret(c, 0); }

// ---- sceCtrl -----------------------------------------------------------------------------------

// sceCtrlReadBufferPositive(data, count): blocks until the next sample (once per vblank).
void ReadBufferPositive(Context& c) {
    Kernel& k = kernel();
    k.wait(WaitType::Vblank, 0, 0, false);
    const std::uint32_t data = arg(c, 0), count = arg(c, 1);
    for (std::uint32_t i = 0; i < count; ++i) {  // SceCtrlData: timestamp, buttons, lx, ly, reserved[6]
        const std::uint32_t p = data + i * 16;
        mem().fill(p, 0, 16);
        mem().write32(p, static_cast<std::uint32_t>(k.now()));
        *mem().ptr(p + 8) = 128;
        *mem().ptr(p + 9) = 128;
    }
    ret(c, count);
}

constexpr HleBinding kBindings[] = {
    {"sceDisplaySetMode", SetMode},
    {"sceDisplaySetFrameBuf", SetFrameBuf},
    {"sceDisplayGetFrameBuf", GetFrameBuf},
    {"sceDisplayWaitVblankStartCB", WaitVblankStartCB},
    {"sceGeEdramGetAddr", EdramGetAddr},
    {"sceGeListEnQueue", ListEnQueue},
    {"sceGeListEnQueueHead", ListEnQueue},
    {"sceGeListUpdateStallAddr", UpdateStallAddr},
    {"sceGeListSync", ListSync},
    {"sceGeDrawSync", DrawSync},
    {"sceGeSetCallback", SetCallback},
    {"sceGeUnsetCallback", ReturnZero},
    {"sceGeContinue", ReturnZero},
    {"sceGeBreak", ReturnZero},
    {"sceCtrlSetSamplingCycle", ReturnZero},
    {"sceCtrlSetSamplingMode", ReturnZero},
    {"sceCtrlReadBufferPositive", ReadBufferPositive},
};

}  // namespace

void hle_install_display() { hle_bind(kBindings); }

void set_ge_dump(std::uint64_t frame, const char* path) {
    g_display.dump_frame = frame;
    g_display.dump = std::fopen(path, "w");
}

}  // namespace ff7cc::rt
