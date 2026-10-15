#pragma once

// The runtime's front end: command line, window, audio, renderers, pause menu, recording and replay around the
// recompiled game. A game's main() describes the game and hands over:
//
//     int main(int argc, char** argv) { return psprecore::platform::run_game(kGame, argc, argv); }

#include <cstdint>
#include <span>

#include "core/foundation/platform/filesystem.h"

namespace psprecore::platform {

struct GameInfo {
    const char* title = "";   // window title
    const char* name = "";    // short name: <name>.log, <name>.ini, usage text
    const char* folder = "";  // the extracted disc folder's default name in the data directory (Android)
    std::uint32_t load_base = 0x08804000;  // where the recompiled module was translated for (psprecomp)
    // After the disc is mounted (rt::set_storage), before the game starts: the game's own setup, e.g. its archive
    // for raw disc reads and mods (`mods`: the active mod folders in order, psprecore/platform/mods.hpp).
    void (*mount)(std::span<const nx::string> mods) = nullptr;
    // After the module is loaded into guest memory, before it runs: patches to the game's own data.
    void (*loaded)() = nullptr;
};

int run_game(const GameInfo& game, int argc, char** argv);

}  // namespace psprecore::platform
