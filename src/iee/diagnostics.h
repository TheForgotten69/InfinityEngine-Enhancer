#pragma once
#include <cstdint>
#include <optional>

#include "iee/game/tis_runtime.h"

namespace iee::diagnostics {
    void log_pointer_dwords(const char *label, const void *address);

    void log_tis_header_diagnostics(void *vidTile,
                                    const game::TileInfo &tileInfo,
                                    const char *reason,
                                    std::optional<std::uint32_t> detectedTileDimension,
                                    bool includeCandidatePointers);
}
