#pragma once

#include <filesystem>

namespace iee::water {
    // Uploads the bundled water textures (raw IRGB blobs) onto reserved units:
    //   unit 3 = iee_water_normal.rgba (tiling normal map)
    //   unit 4 = iee_water_dudv.rgba   (tiling DuDv distortion map)
    //   unit 5 = iee_water_foam.rgba   (tiling foam mask)
    // Must run on the render thread with the GL context current.
    // Missing files are logged and skipped; returns true if any uploaded.
    bool load_water_textures(const std::filesystem::path &dir);
}
