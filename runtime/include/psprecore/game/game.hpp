#pragma once

// Writing game code in C++: hand-written replacements for recompiled functions (see docs/decompilation.md).
//
//     GAME_FUNC(08998F70, game_memcpy) {          // replaces f_08998F70 everywhere (calls and pointers)
//         const u32 dst = arg(ctx, 0);            // `ctx` is the guest CPU state (Context)
//         ...
//         ret(ctx, dst);
//     }
//
// The recompiled original stays available as f_XXXXXXXX_orig (GAME_ORIGINAL(XXXXXXXX) calls it), which makes
// wrappers easy: do something, then run the original. Functions follow the PSP calling convention: arguments in
// a0-a3/t0-t3 (f12-f19 for floats), results in v0/v1 (f0); the callee may clobber caller-saved registers.
//
// After adding or removing a GAME_FUNC, regenerate the recompiled code so the original is renamed:
//     psprecomp <BOOT.BIN> generated --overrides game

#include <cstdint>
#include <cstring>

#include "psprecore/recomp/recomp.h"
extern "C" {
#include "funcs.h"  // generated: f_XXXXXXXX and f_XXXXXXXX_orig prototypes
}

#define GAME_FUNC(address, name)                                \
    static void name(Context* ctx);                             \
    extern "C" void f_##address(Context* ctx) { name(ctx); }    \
    static void name(Context* ctx)

#define GAME_ORIGINAL(address) f_##address##_orig(ctx)

namespace psprecore::game {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;

// ---- registers --------------------------------------------------------------------------------------

enum Reg : int { zero = 0, at, v0, v1, a0, a1, a2, a3, t0, t1, t2, t3, t4, t5, t6, t7,
                 s0, s1, s2, s3, s4, s5, s6, s7, t8, t9, k0, k1, gp, sp, fp, ra };

inline u32 arg(const Context* ctx, int i) {
    static constexpr int kRegs[8] = {a0, a1, a2, a3, t0, t1, t2, t3};
    return ctx->r[kRegs[i]];
}
inline float farg(const Context* ctx, int i) { return ctx->f[12 + i]; }
inline void ret(Context* ctx, u32 value) { ctx->r[v0] = value; }
inline void fret(Context* ctx, float value) { ctx->f[0] = value; }

// ---- guest memory (addresses as the game sees them; cached/uncached mirrors fold together) ----------

template <class T>
T load(u32 address) {
    T v;
    std::memcpy(&v, RECOMP_MEM(address), sizeof v);
    return v;
}
template <class T>
void store(u32 address, T value) {
    std::memcpy(RECOMP_MEM(address), &value, sizeof value);
}
inline u8* host(u32 address) { return RECOMP_MEM(address); }
inline const char* c_str(u32 address) { return reinterpret_cast<const char*>(RECOMP_MEM(address)); }

// ---- calling game code ------------------------------------------------------------------------------

// Calls a recompiled (or replaced) function with up to 8 integer arguments; returns v0. Like a MIPS call it
// clobbers argument and temporary registers. Call f_XXXXXXXX(ctx) directly when the registers are set up.
template <class... Args>
u32 call(Context* ctx, RecompFunc fn, Args... args) {
    static_assert(sizeof...(Args) <= 8, "at most 8 integer arguments");
    const u32 values[] = {static_cast<u32>(args)..., 0u};
    for (std::size_t i = 0; i < sizeof...(Args); ++i) ctx->r[i < 4 ? a0 + static_cast<int>(i) : t0 + static_cast<int>(i) - 4] = values[i];
    fn(ctx);
    return ctx->r[v0];
}
// The same through a code address (function pointers held by the game).
template <class... Args>
u32 call_address(Context* ctx, u32 address, Args... args) {
    static_assert(sizeof...(Args) <= 8, "at most 8 integer arguments");
    const u32 values[] = {static_cast<u32>(args)..., 0u};
    for (std::size_t i = 0; i < sizeof...(Args); ++i) ctx->r[i < 4 ? a0 + static_cast<int>(i) : t0 + static_cast<int>(i) - 4] = values[i];
    recomp_call(ctx, address);
    return ctx->r[v0];
}

}  // namespace psprecore::game
