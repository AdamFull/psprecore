// Image files for screenshots (stb_image_write).

#include <string>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_DEPRECATION
#include <stb_image_write.h>

#include "ff7cc/runtime/hle.hpp"

namespace ff7cc::rt {

bool write_png(const std::string& path, const std::uint32_t* rgba, int width, int height) {
    return stbi_write_png(path.c_str(), width, height, 4, rgba, width * 4) != 0;
}

}  // namespace ff7cc::rt
