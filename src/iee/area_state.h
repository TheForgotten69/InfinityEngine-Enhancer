#pragma once

#include "iee/game/build_manifest.h"
#include "iee/game/runtime_types_x64.h"

namespace iee {
    struct AppContext;
}

namespace iee::area {
    // Resolves the currently active CGameArea from a CInfGame pointer using the
    // manifest's CInfGame offsets (visible area index -> areas array -> master area).
    const game::CGameArea *resolve_active_area(void *infGame, const game::BuildManifest &manifest);

    // Reads the view's world position (CInfinity::m_ptCurrentPosExact) via safe_read.
    bool read_area_scroll(const game::CGameArea *area, int &outOffsetX, int &outOffsetY);

    // Reads CInfinity::m_fZoom via the manifest offset (EEex docs +0x484).
    bool read_area_zoom(const game::CGameArea *area, const game::BuildManifest &manifest, float &outZoom);

    // The engine's exact screen->world affine transform, replicated from the
    // decompiled CInfinity::ScreenToWorld:
    //   world = nNew + rViewPort.size/rViewPortNotZoomed.size * (screen - rViewPortNotZoomed.origin)
    // expressed for the shader's `world = scroll + screen/zoom` model.
    struct ViewTransform {
        float scrollX{};
        float scrollY{};
        float zoom{1.0f};
    };

    bool read_view_transform(const game::CGameArea *area, ViewTransform &out);

    // Re-resolves the active area after LoadArea and caches its parsed WED into ctx.
    void refresh_wed_cache(AppContext &ctx, void *infGame);
}
