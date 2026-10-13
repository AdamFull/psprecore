/* C-side runtime services for recompiled code. VFPU entry points are placeholders until vfpu.cpp exists. */
#include <stdio.h>
#include <stdlib.h>

#include "ff7cc/recomp/recomp.h"

void recomp_call(Context* ctx, uint32_t target) {
    const RecompFunc f = recomp_lookup(target);
    if (!f) {
        fprintf(stderr, "recomp_call: no function at 0x%08X (ra 0x%08X)\n", target, ctx->r[31]);
        abort();
    }
    f(ctx);
}

void recomp_trap(Context* ctx, uint32_t pc, uint32_t word) {
    (void)ctx;
    fprintf(stderr, "recomp_trap at 0x%08X (word 0x%08X)\n", pc, word);
    abort();
}

uint32_t recomp_mfic(Context* ctx) { return ctx->interrupts_enabled; }
void recomp_mtic(Context* ctx, uint32_t value) { ctx->interrupts_enabled = value; }

void vfpu_op(Context* ctx, uint32_t word) {
    (void)ctx;
    fprintf(stderr, "vfpu_op: 0x%08X not implemented\n", word);
    abort();
}
void vfpu_load(Context* ctx, uint32_t word, uint32_t address) {
    (void)ctx;
    fprintf(stderr, "vfpu_load: 0x%08X @ 0x%08X not implemented\n", word, address);
    abort();
}
void vfpu_store(Context* ctx, uint32_t word, uint32_t address) {
    (void)ctx;
    fprintf(stderr, "vfpu_store: 0x%08X @ 0x%08X not implemented\n", word, address);
    abort();
}
