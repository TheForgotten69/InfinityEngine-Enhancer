#include "diagnostics.h"

#include <array>

#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"

namespace iee::diagnostics {
    void log_pointer_dwords(const char *label, const void *address) {
        const auto value = reinterpret_cast<std::uintptr_t>(address);
        if (!address) {
            LOG_WARN("  {} = null", label);
            return;
        }

        std::array<std::uint32_t, 6> words{};
        if (!core::safe_read(address, words)) {
            LOG_WARN("  {} @ 0x{:X}: unreadable", label, value);
            return;
        }

        LOG_WARN("  {} @ 0x{:X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                 label,
                 value,
                 words[0],
                 words[1],
                 words[2],
                 words[3],
                 words[4],
                 words[5]);
    }

    void log_tis_header_diagnostics(void *vidTile,
                                    const game::TileInfo &tileInfo,
                                    const char *reason,
                                    std::optional<std::uint32_t> detectedTileDimension,
                                    bool includeCandidatePointers) {
        game::CResTileSet tilesetSnapshot{};
        const bool haveTilesetSnapshot = tileInfo.tileset && core::safe_read(tileInfo.tileset, tilesetSnapshot);

        const auto dataPointer = haveTilesetSnapshot ? tilesetSnapshot.baseclass_0.pData : nullptr;
        const auto dataSize = haveTilesetSnapshot ? tilesetSnapshot.baseclass_0.nSize : 0U;
        const auto dataCount = haveTilesetSnapshot ? tilesetSnapshot.baseclass_0.nCount : 0U;
        const auto hasEntry = tileInfo.table && tileInfo.index >= 0 &&
                              static_cast<std::uint32_t>(tileInfo.index) < tileInfo.tileCount;
        const auto page = hasEntry ? tileInfo.entry.page : -1;
        const auto u = hasEntry ? tileInfo.entry.u : -1;
        const auto v = hasEntry ? tileInfo.entry.v : -1;

        if (detectedTileDimension) {
            LOG_WARN("{}: vidTile=0x{:X} resource=0x{:X} tileset=0x{:X} header=0x{:X} pData=0x{:X} nSize=0x{:X} nCount=0x{:X} tileCount=0x{:X} tileIndex={} page={} uv=({}, {}) detectedTileDimension=0x{:X}",
                     reason,
                     reinterpret_cast<std::uintptr_t>(vidTile),
                     reinterpret_cast<std::uintptr_t>(tileInfo.resource),
                     reinterpret_cast<std::uintptr_t>(tileInfo.tileset),
                     reinterpret_cast<std::uintptr_t>(tileInfo.header),
                     reinterpret_cast<std::uintptr_t>(dataPointer),
                     dataSize,
                     dataCount,
                     tileInfo.tileCount,
                     tileInfo.index,
                     page,
                     u,
                     v,
                     *detectedTileDimension);
        } else {
            LOG_WARN("{}: vidTile=0x{:X} resource=0x{:X} tileset=0x{:X} header=0x{:X} pData=0x{:X} nSize=0x{:X} nCount=0x{:X} tileCount=0x{:X} tileIndex={} page={} uv=({}, {})",
                     reason,
                     reinterpret_cast<std::uintptr_t>(vidTile),
                     reinterpret_cast<std::uintptr_t>(tileInfo.resource),
                     reinterpret_cast<std::uintptr_t>(tileInfo.tileset),
                     reinterpret_cast<std::uintptr_t>(tileInfo.header),
                     reinterpret_cast<std::uintptr_t>(dataPointer),
                     dataSize,
                     dataCount,
                     tileInfo.tileCount,
                     tileInfo.index,
                     page,
                     u,
                     v);
        }

        log_pointer_dwords("header", tileInfo.header);
        log_pointer_dwords("pData", dataPointer);

        if (!includeCandidatePointers || !tileInfo.tileset) {
            return;
        }

        const auto *tilesetBytes = reinterpret_cast<const std::byte *>(tileInfo.tileset);
        constexpr std::size_t candidateSlots = 4;
        const auto baseOffset = sizeof(game::CRes);

        for (std::size_t slot = 0; slot < candidateSlots; ++slot) {
            const auto offset = baseOffset + slot * sizeof(void *);
            std::uintptr_t candidate = 0;
            if (!core::safe_read(tilesetBytes + offset, candidate)) {
                LOG_WARN("  tileset[+0x{:X}] unreadable", offset);
                continue;
            }

            LOG_WARN("  tileset[+0x{:X}] = 0x{:X}", offset, candidate);

            const auto *candidatePtr = reinterpret_cast<const void *>(candidate);
            if (!candidatePtr || candidatePtr == tileInfo.header || candidatePtr == dataPointer) {
                continue;
            }

            LOG_WARN("  candidate(+0x{:X}) dump follows", offset);
            log_pointer_dwords("candidate", candidatePtr);
        }
    }
}
