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

    // Reads the area's CInfinity scroll offsets via safe_read.
    bool read_area_scroll(const game::CGameArea *area, int &outOffsetX, int &outOffsetY);

    // Re-resolves the active area after LoadArea and caches its parsed WED into ctx.
    void refresh_wed_cache(AppContext &ctx, void *infGame);
}
