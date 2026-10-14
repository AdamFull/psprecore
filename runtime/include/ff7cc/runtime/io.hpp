#pragma once

#include "core/foundation/platform/filesystem.h"

namespace ff7cc::rt {

// Where disc0:/ (the extracted disc: folder containing PSP_GAME) and ms0:/ (saves) point on the host.
void set_storage(nx::string_view game_dir, nx::string_view save_dir);
const nx::string& save_dir();

// Mods: each folder in `mods_dir` (applied in name order, later ones win) may hold pkg/NNNNN.<ext> files that
// replace archive entry NNNNN. The game sees a patched discimg.fse whose replaced entries point past the end of
// discimg.pkg, and block reads there are served from the mod files. Call after set_storage().
void set_mods(nx::string_view mods_dir);

}  // namespace ff7cc::rt
