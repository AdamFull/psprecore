# Static recompiler design

`psprecomp` translates every function of BOOT.BIN (found by `libs/analysis`) into C17 that compiles natively for
x64 and ARM64. The generated code depends only on `runtime/include/psprecore/recomp/recomp.h`.

## Inputs and outputs

- Input: BOOT.BIN, relocated at PPSSPP's load base `0x08804000` (`prx::load_image`). Every constant in the
  code (`lui`/`addiu` pairs, jump targets, jump tables) is therefore final.
- Output (git-ignored `generated/`): `funcs_NNN.c` (about 200 functions each), `funcs.h` (prototypes) and
  `lookup.c` (an address → function table for indirect calls).
- At run time the user's BOOT.BIN is loaded and relocated at the same base to provide the data
  (`.rodata`/`.data`). The generated code contains no game data.

## Execution model

| Guest | Generated C |
|---|---|
| GPRs, HI/LO, FPU, FPU condition, VFPU registers/control | fields of `Context` (`ctx->r[n]`, `ctx->f[n]`, …) |
| Memory | `g_membase + (addr & 0x1FFFFFFF)`: one 512 MiB host block. The mask folds the uncached (`0x4…`) and kernel (`0x8…`) mirrors onto RAM/VRAM |
| Function | `void f_08812340(Context* ctx)` |
| `jal f` | delay slot, `ctx->r[31] = pc + 8`, `f_xxx(ctx)` |
| `jr ra` | delay slot, `return` |
| `jalr r` | delay slot, `recomp_call(ctx, target)` (lookup table) |
| `jr r` (switch) | jump table recovered from relocations → `switch` over the table's targets |
| `j` / branch to another function | tail call: `f_xxx(ctx); return;` |
| Branch | the condition is computed first, then the delay slot, then `goto` (likely-branches run the delay slot only when taken) |
| Import stub call | `hle_call(ctx, NID)` |
| VFPU instruction | `vfpu_exec(ctx, WORD)` — `static inline` with a constant word, so the decode switch folds away |

Writes to `$zero` are dropped. Integer division follows Allegrex results for division by zero and overflow
instead of trapping.

## Hand-written replacements

`--overrides game` scans `game/` for `GAME_FUNC(XXXXXXXX, ...)` and emits those functions as `f_XXXXXXXX_orig`;
see [decompilation.md](decompilation.md).

## Not covered yet

- Faster register handling (locals instead of `ctx->r`), and VFPU code generated as plain SIMD.
