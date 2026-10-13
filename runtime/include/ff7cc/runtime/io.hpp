#pragma once

#include <filesystem>

namespace ff7cc::rt {

// Where disc0:/ (the extracted disc: folder containing PSP_GAME) and ms0:/ (saves) point on the host.
void set_storage(const std::filesystem::path& game_dir, const std::filesystem::path& save_dir);

}  // namespace ff7cc::rt
