# psprecore

Static recompilation of PSP games into native programs for PC (Windows/Linux) and Android: a MIPS (Allegrex)
recompiler that turns a game's executable into C, and the runtime that runs the result — HLE kernel and system
libraries, the GE graphics chip on Vulkan (and a software reference renderer), SAS/ATRAC audio, MPEG video,
save data — with a front end (window, controllers, touch controls, pause menu, recording and replay).

Each game lives in its own repository that uses psprecore as a git submodule and adds what is specific to it
(for example [ff7ccrecomp](https://github.com/AdamFull/ff7ccrecomp), *Crisis Core: Final Fantasy VII*).

**This repository contains no game code or assets, and no PSP SDK code.** Games are recompiled from the user's own
copy; everything derived from it is generated locally and never committed.

## Layout

| Folder | Contents |
| --- | --- |
| `libs/mips` | Allegrex instruction decoder and disassembler (checked against psp-objdump) |
| `libs/analysis` | Function discovery, control flow, jump tables |
| `libs/recomp` | The C code generator |
| `libs/formats` | ELF/PRX, ATRAC3, PSMF, cooked media; file and text helpers |
| `libs/nx-foundation` | [nx-foundation](https://github.com/AdamFull/nx-foundation) (submodule): containers, strings, threads, files, command line |
| `runtime` | The runtime (`psprecore::rt`), the GE renderers, the front end (`psprecore::platform`), shaders |
| `tools` | `psprecomp` (recompiler), `psprx` (PRX inspector, NIDs, disassembly); Python analysis scripts |
| `tests` | Unit tests (no game data needed); `tests/allegrex` for `scripts/check_allegrex.ps1` |
| `docs` | [How the recompiler works](docs/recompiler.md) |

## Building

Requirements: CMake ≥ 3.25, Ninja, LLVM/Clang, the Vulkan SDK (slangc). On Windows, also Visual Studio 2022 (for
the Windows SDK and CRT used by `clang-cl`); `scripts\build.ps1` sets up that environment itself.

```
git submodule update --init --recursive
scripts\build.ps1                     # Windows: configure, build, run tests (-Preset, -NoTest, -Target)
cmake --preset linux-x64-debug && cmake --build --preset linux-x64-debug && ctest --preset linux-x64-debug
```

## Using it for a game

A game repository adds psprecore as a submodule and builds its executable with `psprecore_add_game`:

```cmake
add_subdirectory(psprecore)
psprecore_add_game(mygame
    GENERATED_DIR "${PROJECT_SOURCE_DIR}/generated"   # psprecomp output for the user's BOOT.BIN
    OVERRIDES_DIR "${PROJECT_SOURCE_DIR}/game"        # optional: hand-written functions (GAME_FUNC)
    SOURCES src/main.cpp)
```

Its `main()` describes the game and runs the front end:

```cpp
#include "psprecore/platform/frontend.hpp"

constexpr psprecore::platform::GameInfo kGame{
    .title = "My Game", .name = "mygame", .folder = "My Game (EU)", .load_base = 0x08804000,
    .mount = nullptr,  // optional: archive mapping for raw disc reads, mods (psprecore/runtime/io.hpp)
};

int main(int argc, char** argv) { return psprecore::platform::run_game(kGame, argc, argv); }
```

Games can also register the textures that contain their button icons (`psprecore/gpu/button_icons.hpp`), so the
icons follow the controller in use.

```
psprx info    BOOT.BIN                  # module info, segments, imports/exports, relocation stats
psprx nids    <pspsdk>/psp/sdk/lib nids.tsv   # NID names from a local SDK's stub libraries
psprx imports BOOT.BIN nids.tsv         # imported system functions with names
psprx disasm  BOOT.BIN                  # objdump-compatible listing (scripts/check_allegrex.ps1 diffs it)
psprecomp     BOOT.BIN generated [--overrides game]   # translate to C (see docs/recompiler.md)
```

The NID database is generated locally and not committed. `tools/analysis/nids_from_ppsspp.py` fills names the
homebrew SDK lacks from a PPSSPP debug log (names only; no emulator code is used).

Licensed under the MIT License. PSP is a trademark of Sony Interactive Entertainment; this project is not
affiliated with or endorsed by Sony.
