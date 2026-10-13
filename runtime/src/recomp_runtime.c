/* C-side runtime services for recompiled code (indirect calls, traps, interrupt flag). */
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
