/* Links every recompiled function; reports the size of the function table. Does not execute game code yet. */
#include <stdio.h>

#include "funcs.h"

int main(void) {
    printf("recompiled code linked: lookup(entry) = %p\n", (void*)recomp_lookup(0x089878A0u));
    return recomp_lookup(0x089878A0u) ? 0 : 1;
}
