#pragma once

// The runtime's front end: command line, window, audio, renderers, pause menu, recording and replay around the
// recompiled game. A game's main() describes the game and hands over:
//
//     int main(int argc, char** argv) { return ff7cc::platform::run_game(kGame, argc, argv); }

#include <cstdint>

#include "core/foundation/platform/filesystem.h"

namespace ff7cc::platform {

struct GameInfo {
    const char* title = "";   // window title
    const char* name = "";    // short name: <name>.log, <name>.ini, usage text
    const char* folder = "";  // the extracted disc folder's default name in the data directory (Android)
    std::uint32_t load_base = 0x08804000;  // where the recompiled module was translated for (ccrecomp)
    // After the disc is mounted (rt::set_storage), before the game starts: the game's own setup, e.g. its archive
    // for raw disc reads and mods (`mods_dir`: --mods, or <data>/mods).
    void (*mount)(nx::string_view mods_dir) = nullptr;
};

int run_game(const GameInfo& game, int argc, char** argv);

}  // namespace ff7cc::platform
