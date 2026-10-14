#pragma once

// Paths are UTF-8 nx::strings handled by nx::fs (core/foundation/platform/filesystem.h). These helpers keep the
// common cases short.

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/utf8_string.h"

namespace psprecore {

// first / rest... with one separator between components (an absolute component restarts the path).
template <class... Rest>
nx::string path_join(nx::string_view first, const Rest&... rest) {
    nx::string out(first);
    ((out = nx::fs::path::join(out, nx::string_view(rest))), ...);
    return out;
}

inline bool is_file(nx::fs::path_view path) { return nx::fs::status(path) == nx::fs::file_type::regular; }
inline bool is_directory(nx::fs::path_view path) { return nx::fs::status(path) == nx::fs::file_type::directory; }

// The last component ("a/b/c.txt" -> "c.txt"), its name without the extension ("c"), and the parent ("a/b").
inline nx::string file_name(nx::fs::path_view path) { return nx::string(path.filename().native()); }
inline nx::string file_stem(nx::fs::path_view path) { return nx::string(path.stem().native()); }
inline nx::string parent_path(nx::fs::path_view path) { return nx::string(path.parent_path().native()); }

}  // namespace psprecore
