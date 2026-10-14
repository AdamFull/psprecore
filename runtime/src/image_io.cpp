// Image files: screenshots (stb_image_write) and PNG loading for the overlay (stb_image).

#include <cstring>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_DEPRECATION
#include <stb_image_write.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include "ff7cc/runtime/hle.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace ff7cc::rt {

bool write_png(const nx::string& path, const std::uint32_t* rgba, int width, int height) {
    return stbi_write_png(path.c_str(), width, height, 4, rgba, width * 4) != 0;
}

bool read_png(const nx::string& path, nx::vector<std::uint32_t>& rgba, int& width, int& height) {
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) return false;
    rgba.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::memcpy(rgba.data(), pixels, rgba.size() * 4);
    stbi_image_free(pixels);
    return true;
}

}  // namespace ff7cc::rt
