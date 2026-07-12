#pragma once

#include <filesystem>

namespace iee::water {
    // Uploads the bundled water textures (raw IRGB blobs) onto reserved units:
    //   unit 3 = iee_water_normal.rgba (tiling normal map)
    //   unit 4 = iee_water_dudv.rgba   (tiling DuDv distortion map)
    //   unit 5 = iee_water_foam.rgba   (tiling foam mask)
    // Must run on the render thread with the GL context current.
    // All three files are required; their decoded bytes are retained so a new
    // GL context can recreate its texture objects without touching game state.
    bool load_water_textures(const std::filesystem::path &dir);

    // Ensures the retained assets exist in the current GL context and binds
    // them to units 3..5. Must be called immediately before an IEE water draw.
    bool ensure_water_textures_bound() noexcept;

    // Deletes objects only when their owning context is current, then clears
    // retained CPU and GL state. Intended for explicit, non-DllMain shutdown.
    void release_water_textures() noexcept;
}
