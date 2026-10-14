// sceDisplay, sceGe_user, sceCtrl. Display lists are drawn by the software reference renderer.

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "ff7cc/runtime/ge.hpp"
#include "ff7cc/runtime/hle.hpp"
#include "ff7cc/runtime/kernel.hpp"
#include "gpu/soft_renderer.hpp"

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
    struct Callback {
        std::uint32_t signal_func, signal_arg, finish_func, finish_arg;
    };
    std::map<int, Callback> callbacks;  // sceGeSetCallback
} g_display;

std::function<std::unique_ptr<ge::Renderer>(GuestMemory&)> g_renderer_factory;
std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)> g_flip_handler;

std::uint32_t g_pad_buttons = 0;
std::uint8_t g_pad_lx = 128, g_pad_ly = 128;

ge::Processor& gpu() {
    if (!g_display.ge) {
        g_display.ge = std::make_unique<ge::Processor>(mem());
        g_display.ge->set_renderer(g_renderer_factory ? g_renderer_factory(mem()) : gpu::make_soft_renderer(mem()));
        g_display.ge->on_command = [](std::uint32_t pc, std::uint32_t word) {
            if (g_display.dump && g_display.frames == g_display.dump_frame)
                std::fprintf(g_display.dump, "%08X: %08X  %s\n", pc, word, ge::describe(word).c_str());
        };
        // GE interrupts: the handlers run as guest calls inside the sceGe call that is executing the list.
        g_display.ge->on_signal = [](std::uint32_t id) {
            for (const auto& [_, cb] : g_display.callbacks)
                if (cb.signal_func) kernel().call_guest(cb.signal_func, {id, cb.signal_arg});
        };
        g_display.ge->on_finish = [](std::uint32_t id) {
            for (const auto& [_, cb] : g_display.callbacks)
                if (cb.finish_func) kernel().call_guest(cb.finish_func, {id, cb.finish_arg});
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
    if (g_flip_handler && g_display.framebuffer) g_flip_handler(g_display.framebuffer, g_display.stride, g_display.format);
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
// sceGeSetCallback(SceGeCbParam*): {signal func, signal cookie, finish func, finish cookie}.
void SetCallback(Context& c) {
    const std::uint32_t p = arg(c, 0);
    const int id = g_display.next_callback_id++;
    g_display.callbacks[id] = {mem().read32(p), mem().read32(p + 4), mem().read32(p + 8), mem().read32(p + 12)};
    ret(c, static_cast<std::uint32_t>(id));
}
void UnsetCallback(Context& c) {
    g_display.callbacks.erase(static_cast<int>(arg(c, 0)));
    ret(c, 0);
}
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
        mem().write32(p + 4, g_pad_buttons);
        *mem().ptr(p + 8) = g_pad_lx;
        *mem().ptr(p + 9) = g_pad_ly;
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
    {"sceGeUnsetCallback", UnsetCallback},
    {"sceGeContinue", ReturnZero},
    {"sceGeBreak", ReturnZero},
    {"sceCtrlSetSamplingCycle", ReturnZero},
    {"sceCtrlSetSamplingMode", ReturnZero},
    {"sceCtrlReadBufferPositive", ReadBufferPositive},
};

}  // namespace

void hle_install_display() { hle_bind(kBindings); }

void set_ge_renderer_factory(std::function<std::unique_ptr<ge::Renderer>(GuestMemory&)> factory) {
    g_renderer_factory = std::move(factory);
}

void set_flip_handler(std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)> handler) {
    g_flip_handler = std::move(handler);
}

void set_pad(std::uint32_t buttons, std::uint8_t lx, std::uint8_t ly) {
    g_pad_buttons = buttons;
    g_pad_lx = lx;
    g_pad_ly = ly;
}

bool read_framebuffer(std::vector<std::uint32_t>& rgba, int& width, int& height) {
    if (!g_display.framebuffer) return false;
    width = 480;
    height = 272;
    rgba.resize(static_cast<std::size_t>(width) * height);
    const std::uint32_t format = g_display.format, stride = g_display.stride ? g_display.stride : 512;
    const std::uint32_t bpp = format == 3 ? 4 : 2;
    const auto expand = [](std::uint32_t v, int bits) { return (v << (8 - bits)) | (v >> (2 * bits - 8)); };
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = mem().ptr(g_display.framebuffer + static_cast<std::uint32_t>(y) * stride * bpp);
        std::uint32_t* out = rgba.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            if (bpp == 4) {  // 8888 is stored as R,G,B,A bytes, like VK_FORMAT_R8G8B8A8
                std::uint32_t p;
                std::memcpy(&p, row + x * 4, 4);
                out[x] = p | 0xFF000000u;
                continue;
            }
            const std::uint32_t p = static_cast<std::uint32_t>(row[x * 2] | (row[x * 2 + 1] << 8));
            std::uint32_t r, g, b;
            switch (format) {
                case 0: r = expand(p & 31, 5); g = expand((p >> 5) & 63, 6); b = expand(p >> 11, 5); break;            // 565
                case 1: r = expand(p & 31, 5); g = expand((p >> 5) & 31, 5); b = expand((p >> 10) & 31, 5); break;     // 5551
                default: r = (p & 15) * 17; g = ((p >> 4) & 15) * 17; b = ((p >> 8) & 15) * 17; break;               // 4444
            }
            out[x] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
    }
    return true;
}

void set_ge_dump(std::uint64_t frame, const char* path) {
    g_display.dump_frame = frame;
    g_display.dump = std::fopen(path, "w");
}

std::uint64_t display_frame_count() { return g_display.frames; }

}  // namespace ff7cc::rt
