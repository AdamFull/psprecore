/* Interface between recompiled code (generated/ by psprecomp) and the runtime. C17, header-only helpers.
 * See docs/recompiler.md. */
#ifndef PSPRECORE_RECOMP_H
#define PSPRECORE_RECOMP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CPU state ------------------------------------------------------------------------------------ */

enum { VFPU_PFXS = 0, VFPU_PFXT, VFPU_PFXD, VFPU_CC, VFPU_INF4, VFPU_RSV5, VFPU_RSV6, VFPU_REV, VFPU_RCX0 };

typedef struct Context {
    uint32_t r[32];  /* GPRs; r[0] is never written */
    uint32_t hi, lo;
    union {
        float f[32];
        uint32_t fu[32];
    };
    uint32_t fcr31;  /* bit 23: condition, bits 0-1: rounding mode */
    union {
        float v[128];
        uint32_t vu[128];
    };
    uint32_t vfpu_ctrl[16];  /* prefixes, condition codes, random state (VFPU_* indices) */
    uint32_t interrupts_enabled;
} Context;

typedef void (*RecompFunc)(Context* ctx);

/* Power-on state of a thread context: default FPU control, identity VFPU source prefixes. */
static inline void recomp_context_init(Context* ctx) {
    memset(ctx, 0, sizeof *ctx);
    ctx->fcr31 = 0x00000E00u;
    ctx->vfpu_ctrl[VFPU_PFXS] = 0xE4u;
    ctx->vfpu_ctrl[VFPU_PFXT] = 0xE4u;
}

/* ---- Memory: one host block mirrors the PSP's physical space; the mask folds cached/uncached/kernel ---- */

extern uint8_t* g_membase;
#define RECOMP_MEM(addr) (g_membase + ((uint32_t)(addr) & 0x1FFFFFFFu))

static inline uint32_t mem_lw(uint32_t a) { uint32_t v; memcpy(&v, RECOMP_MEM(a), 4); return v; }
static inline uint32_t mem_lh(uint32_t a) { int16_t v; memcpy(&v, RECOMP_MEM(a), 2); return (uint32_t)(int32_t)v; }
static inline uint32_t mem_lhu(uint32_t a) { uint16_t v; memcpy(&v, RECOMP_MEM(a), 2); return v; }
static inline uint32_t mem_lb(uint32_t a) { return (uint32_t)(int32_t)(int8_t)*RECOMP_MEM(a); }
static inline uint32_t mem_lbu(uint32_t a) { return *RECOMP_MEM(a); }
static inline void mem_sw(uint32_t a, uint32_t v) { memcpy(RECOMP_MEM(a), &v, 4); }
static inline void mem_sh(uint32_t a, uint32_t v) { uint16_t h = (uint16_t)v; memcpy(RECOMP_MEM(a), &h, 2); }
static inline void mem_sb(uint32_t a, uint32_t v) { *RECOMP_MEM(a) = (uint8_t)v; }

/* Unaligned word access (little-endian semantics). */
static inline uint32_t mem_lwl(uint32_t a, uint32_t rt) {
    const uint32_t shift = (a & 3) * 8, w = mem_lw(a & ~3u);
    return (rt & (0x00FFFFFFu >> shift)) | (w << (24 - shift));
}
static inline uint32_t mem_lwr(uint32_t a, uint32_t rt) {
    const uint32_t shift = (a & 3) * 8, w = mem_lw(a & ~3u);
    return (rt & ~(0xFFFFFFFFu >> shift)) | (w >> shift);
}
static inline void mem_swl(uint32_t a, uint32_t rt) {
    const uint32_t shift = (a & 3) * 8, w = mem_lw(a & ~3u);
    mem_sw(a & ~3u, (w & ~(0xFFFFFFFFu >> (24 - shift))) | (rt >> (24 - shift)));
}
static inline void mem_swr(uint32_t a, uint32_t rt) {
    const uint32_t shift = (a & 3) * 8, w = mem_lw(a & ~3u);
    mem_sw(a & ~3u, (w & ~(0xFFFFFFFFu << shift)) | (rt << shift));
}

/* ---- Integer helpers ---------------------------------------------------------------------------- */

static inline void cpu_div(Context* ctx, uint32_t rs, uint32_t rt) {
    const int32_t a = (int32_t)rs, b = (int32_t)rt;
    if (b == 0) {  /* Allegrex: no trap; TODO verify exact results on hardware */
        ctx->lo = a < 0 ? 1u : 0xFFFFFFFFu;
        ctx->hi = rs;
    } else if (a == INT32_MIN && b == -1) {
        ctx->lo = (uint32_t)INT32_MIN;
        ctx->hi = 0;
    } else {
        ctx->lo = (uint32_t)(a / b);
        ctx->hi = (uint32_t)(a % b);
    }
}
static inline void cpu_divu(Context* ctx, uint32_t rs, uint32_t rt) {
    if (rt == 0) {
        ctx->lo = 0xFFFFFFFFu;
        ctx->hi = rs;
    } else {
        ctx->lo = rs / rt;
        ctx->hi = rs % rt;
    }
}
static inline uint64_t cpu_hilo(const Context* ctx) { return ((uint64_t)ctx->hi << 32) | ctx->lo; }
static inline void cpu_set_hilo(Context* ctx, uint64_t v) { ctx->hi = (uint32_t)(v >> 32); ctx->lo = (uint32_t)v; }
static inline uint32_t cpu_clz(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32u; }
static inline uint32_t cpu_bitrev(uint32_t v) {
    uint32_t r = 0;
    for (int i = 0; i < 32; ++i) r |= ((v >> i) & 1u) << (31 - i);
    return r;
}
static inline uint32_t cpu_wsbh(uint32_t v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }
static inline uint32_t cpu_wsbw(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t cpu_rotr(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

/* ---- FPU helpers -------------------------------------------------------------------------------- */

static inline uint32_t fpu_to_int(float f, int mode) { /* 0 nearest-even, 1 truncate, 2 ceil, 3 floor */
    if (f != f) return 0x7FFFFFFFu;
    double d = mode == 0 ? nearbyint((double)f) : mode == 1 ? trunc((double)f) : mode == 2 ? ceil((double)f) : floor((double)f);
    if (d >= 2147483647.0) return 0x7FFFFFFFu;
    if (d <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)d;
}
static inline void fpu_compare(Context* ctx, uint32_t cond, float a, float b) {
    const int unordered = (a != a) || (b != b);
    const int result = ((cond & 1) && unordered) || ((cond & 2) && !unordered && a == b) ||
                       ((cond & 4) && !unordered && a < b);
    ctx->fcr31 = (ctx->fcr31 & ~(1u << 23)) | ((uint32_t)result << 23);
}
static inline uint32_t fpu_cond(const Context* ctx) { return (ctx->fcr31 >> 23) & 1u; }

/* ---- Services implemented by the runtime --------------------------------------------------------- */

void recomp_call(Context* ctx, uint32_t target);             /* indirect call through the lookup table */
void hle_call(Context* ctx, uint32_t nid);                   /* imported system function */
void recomp_trap(Context* ctx, uint32_t pc, uint32_t word);  /* break, syscall, unexpected control flow */
uint32_t recomp_mfic(Context* ctx);
void recomp_mtic(Context* ctx, uint32_t value);

/* VFPU: one entry point per instruction class; `word` is a compile-time constant at every call site. */
void vfpu_op(Context* ctx, uint32_t word);                         /* arithmetic, transfers, prefixes */
void vfpu_load(Context* ctx, uint32_t word, uint32_t address);     /* lv.s, lv.q, lvl.q, lvr.q */
void vfpu_store(Context* ctx, uint32_t word, uint32_t address);    /* sv.s, sv.q, svl.q, svr.q */
static inline uint32_t vfpu_cond(const Context* ctx, uint32_t bit) { return (ctx->vfpu_ctrl[VFPU_CC] >> bit) & 1u; }

/* Address -> function, for indirect calls (generated lookup.c). */
RecompFunc recomp_lookup(uint32_t address);

#ifdef __cplusplus
}
#endif

#endif /* PSPRECORE_RECOMP_H */
