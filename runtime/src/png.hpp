// A minimal PNG writer for frame snapshots: 8-bit RGB, stored (uncompressed) deflate.
#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace willow {

// `rgb` holds width * height pixels, three bytes each, row by row.
bool write_png(const std::filesystem::path &path, int width, int height, const std::vector<uint8_t> &rgb);

}  // namespace willow
