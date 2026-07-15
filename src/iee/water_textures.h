#pragma once

#include <filesystem>

namespace iee::water {
// Loads optional compressed DDS overrides or the bundled raw IRGB fallbacks
// into retained CPU memory. This performs no GL work and can run during DLL
// initialization so the first rendered tile does not perform file I/O.
// All three texture stems are required; their decoded bytes are retained so a
// new GL context can recreate its texture objects without touching game state.
bool prepare_water_textures(const std::filesystem::path& dir);

// Uploads retained assets onto the reserved units declared in
// game/texture_units.h. Must run on the render thread with a current GL
// context. Returns false when preparation failed or GL upload is unavailable.
bool upload_water_textures();

// Ensures the retained assets exist in the current GL context and binds
// them to their reserved units. Must be called immediately before an IEE water
// draw.
bool ensure_water_textures_bound() noexcept;

// Deletes objects only when their owning context is current, then clears
// retained CPU and GL state. Intended for explicit, non-DllMain shutdown.
void release_water_textures() noexcept;
}  // namespace iee::water
