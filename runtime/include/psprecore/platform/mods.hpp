#pragma once

// Mod folders: <data>/mods/<folder>/ (or --mods DIR). What a mod holds is up to the game (archive entries, text...)
// and the core (textures/, psprecore/gpu/texture_replacement.hpp). An optional mod.ini describes it:
//
//     name=French translation
//     language=Français
//
// A mod with a language is a language pack: only the one chosen in the settings (pause menu) applies; every other
// mod always does. Active mods apply in folder name order, a later one winning where two replace the same thing.

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore::platform {

struct ModInfo {
    nx::string path;      // the folder
    nx::string name;      // mod.ini's name, else the folder's
    nx::string language;  // empty: not a language pack
};

nx::vector<ModInfo> find_mods(nx::string_view mods_dir);  // in folder name order
// The folders that apply with `language` chosen (empty: the game's own language).
nx::vector<nx::string> active_mods(const nx::vector<ModInfo>& mods, nx::string_view language);
// The languages the packs offer, in order, without duplicates.
nx::vector<nx::string> mod_languages(const nx::vector<ModInfo>& mods);

}  // namespace psprecore::platform
