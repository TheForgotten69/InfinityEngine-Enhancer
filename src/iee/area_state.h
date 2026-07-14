#pragma once

#include "iee/game/build_manifest.h"
#include "iee/game/runtime_types_x64.h"

namespace iee {
struct AppContext;
}

namespace iee::area {
// Resolves the currently active CGameArea from a CInfGame pointer using the
// manifest's CInfGame offsets (visible area index -> areas array -> master area).
const game::CGameArea* resolve_active_area(void* infGame, const game::BuildManifest& manifest);

// The engine's exact screen->world affine transform, replicated from the
// decompiled CInfinity::ScreenToWorld — expressed resolution-independently:
// the engine's "screen" coords are UI-SCALED LOGICAL pixels (rViewPortNotZoomed
// is the logical window, e.g. 2364x1314 inside a 3840x2135 GL viewport), so
// the only safe currency for the shader is the view's WORLD-pixel size:
//   world = scroll + physicalPx * viewWorldSize / physicalViewportSize
struct ViewTransform {
  float scrollX{};
  float scrollY{};
  float viewWorldW{};  // rViewPort.width  (world px visible)
  float viewWorldH{};  // rViewPort.height
};

bool read_view_transform(const game::CGameArea* area, ViewTransform& out);

// Re-resolves the active area after LoadArea and caches its parsed WED into ctx.
void refresh_wed_cache(AppContext& ctx, void* infGame);

// Publishes an immediate CPU-side no-liquid generation. The next render
// thread flush replaces any previous area's GPU mask before drawing.
void reset_gpu_area_state() noexcept;

// Uploads the latest CPU-prepared area mask into the current GL context.
// Safe to call every Seam pass; work occurs only for a new generation or
// a recreated context.
bool flush_pending_gpu_upload() noexcept;

// Ensures the latest generation is uploaded and binds it to the reserved area-mask unit.
bool bind_area_texture() noexcept;

// Best-effort explicit shutdown; never call from DllMain.
void release_gpu_area_resources() noexcept;
}  // namespace iee::area
