#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>

#include "iee/core/pattern_scanner.h"

namespace iee::game {
    // region Game Constants and Enums

    // Rendering primitive types used by the engine
    enum class DrawMode : int {
        Triangles = 2,
        TriangleStrip = 3
    };

    // Shader tone IDs for fragment shader selection
    enum class ShaderTone : int {
        None = 0, // No shader (avoid this - causes crashes)
        Grey = 1, // ???.glsl - greyscale rendering
        Seam = 8, // fpseam.glsl - enhanced tile rendering (default)
    };

    // Upscaled content detection thresholds
    namespace UpscaleThresholds {
        constexpr int UV_THRESHOLD = 1024; // UV coordinates above this indicate upscaled content
        constexpr int TEXTURE_ID_THRESHOLD = 10000; // Texture IDs above this indicate upscaled content
        constexpr int UI_TEXTURE_THRESHOLD = 100; // Textures below this are likely UI
        constexpr int DETECTION_SAMPLE_COUNT = 10; // Number of tiles to sample for detection. Lower than 10 cause bugs.
    }

    // Standard tile dimensions
    namespace TileDimensions {
        constexpr int STANDARD_SIZE = 64; // Standard tile size (64x64)
        constexpr int RENDER_QUAD_SIZE = 64; // Always render 64x64 quads regardless of scale
    }

    // Rendering flags and bit positions
    namespace RenderFlags {
        constexpr int GREY_TONE_BIT = 19; // Bit position for grey tone flag in render flags
        constexpr unsigned long GREY_TONE_MASK = 1UL << GREY_TONE_BIT;
    }

    // endregion

    // Forward declarations
    struct CResTileSet;
    struct CResTIS_MIN;

    // --------------------------------------------------------------------------------------
    // view_t — Resource view structure (24 bytes at 0x18 in CRes)
    // --------------------------------------------------------------------------------------
    struct view_t {
        std::byte data[24]{}; // Opaque view data
    };

    // --------------------------------------------------------------------------------------
    // CRes — Base resource class (88 bytes total)
    // --------------------------------------------------------------------------------------
    struct CRes {
        void *vfptr{}; // 0x00: Virtual function table pointer
        const char *resref{}; // 0x08: Resource reference string
        int32_t type{}; // 0x10: Resource type
        int32_t _pad0{}; // 0x14: Padding
        view_t view{}; // 0x18: Resource view (24 bytes)
        uint32_t nID{}; // 0x30: Resource ID
        int32_t zip_id{}; // 0x34: ZIP file ID
        int32_t override_id{}; // 0x38: Override ID
        int32_t _pad1{}; // 0x3C: Padding
        void *pData{}; // 0x40: Resource data pointer
        uint32_t nSize{}; // 0x48: Resource size
        uint32_t nCount{}; // 0x4C: Reference count
        bool bWasMalloced{}; // 0x50: Was allocated with malloc
        bool bLoaded{}; // 0x51: Is resource loaded
        std::byte _pad2[6]{}; // 0x52: Padding to 88 bytes
    };

    // --------------------------------------------------------------------------------------
    // ResFixedHeader_st — Resource file header (20 bytes)
    // --------------------------------------------------------------------------------------
    struct ResFixedHeader_st {
        uint32_t nFileType{}; // 0x00: File type identifier
        uint32_t nFileVersion{}; // 0x04: File version
        uint32_t nNumber{}; // 0x08: Number of entries
        uint32_t nSize{}; // 0x0C: Size of data
        uint32_t nTableOffset{}; // 0x10: Offset to table
    };

    // --------------------------------------------------------------------------------------
    // CSize — Size structure (8 bytes, wraps Windows SIZE)
    // --------------------------------------------------------------------------------------
    struct CSize {
        int32_t cx{}; // Width
        int32_t cy{}; // Height
    };

    // --------------------------------------------------------------------------------------
    // CResPVR — PVR texture resource (112 bytes total)
    // --------------------------------------------------------------------------------------
    struct CResPVR {
        CRes baseclass_0{}; // 0x00: Base CRes (88 bytes)
        int32_t texture{}; // 0x58: OpenGL texture ID
        int32_t format{}; // 0x5C: Texture format
        int32_t filtering{}; // 0x60: Filtering mode
        CSize size{}; // 0x64: Texture dimensions
        int32_t _pad0{}; // 0x6C: Padding to 112 bytes
    };

    // --------------------------------------------------------------------------------------
    // CResTileSet — Tileset resource (96 bytes total)
    // --------------------------------------------------------------------------------------
    struct CResTileSet {
        CRes baseclass_0{}; // 0x00: Base CRes (88 bytes)
        ResFixedHeader_st *h{}; // 0x58: Header pointer
    };

    // --------------------------------------------------------------------------------------
    // CResRef — 8-byte resource name (stable across IE builds)
    // --------------------------------------------------------------------------------------
    struct CResRef {
        std::array<char, 8> m_resRef{};

        CResRef() = default;

        explicit CResRef(const char *name) {
            // Zero-fill then copy up to 8 chars (no dependency on MSVC-specific *_s)
            if (name) {
                std::memcpy(m_resRef.data(), name, std::min<size_t>(8, std::strlen(name)));
            }
        }
    };

    // --------------------------------------------------------------------------------------
    // PVRZTileEntry — compact tile atlas entry (page, u, v)  (you already use this)
    // --------------------------------------------------------------------------------------
    struct PVRZTileEntry {
        int32_t page{};
        int32_t u{};
        int32_t v{};
    };

    // --------------------------------------------------------------------------------------
    // CResTile — Tile resource (24 bytes total) - exact layout from documentation
    // --------------------------------------------------------------------------------------
    struct CResTile {
        CResTileSet *tis{}; // 0x00: Parent tileset pointer
        int32_t tileIndex{}; // 0x08: Tile index within set
        int32_t _pad0{}; // 0x0C: Padding for alignment
        CResPVR *pvr{}; // 0x10: Associated PVR texture resource
    };

    // --------------------------------------------------------------------------------------
    // CResTIS_MIN (x64) — minimal subset you referenced:
    //   0x40: void*           tileTable           -> PVRZTileEntry[]
    //   0x48: uint32_t        tileDataBlockLen    == 12
    //   0x4C: uint32_t        tileDimension       == 64 or 256
    // Everything before 0x40 is opaque to us; keep as padding.
    // --------------------------------------------------------------------------------------
    struct CResTIS_MIN {
        std::byte _pad0[0x40]; // 0x00..0x3F (opaque header/vtable/etc.)
        void *tileTable; // 0x40 -> PVRZTileEntry[]
        uint32_t tileDataBlockLen; // 0x48
        uint32_t tileDimension; // 0x4C
    };

    struct TileInfo {
        CResTIS_MIN *tis{};
        int index{-1};
        const PVRZTileEntry *table{};
    };

    inline bool get_tile_info(void *vidTile, TileInfo &out, void *(*CRes_Demand)(void *)) {
        if (!vidTile) {
            return false;
        }

        out.tis = nullptr;
        out.index = -1;
        out.table = nullptr;

        // Read the CResTile pointer from the vidTile object (matching original behavior)
        auto readAddr = reinterpret_cast<uintptr_t>(vidTile) + GameOffsets::VID_TILE_RESOURCE_OFFSET;
        if (readAddr < 0x1000) {
            return false; // Suspicious read address
        }

        auto pRes = *reinterpret_cast<CResTile **>(readAddr);
        if (!pRes) {
            return false; // No resource attached
        }

        if (!pRes->tis) {
            return false; // No TIS resource loaded
        }

        // Safely call CRes_Demand if available to ensure resource is loaded
        if (CRes_Demand) {
            try {
                (void) CRes_Demand(pRes->tis); // Demand the TIS resource, not the CResTile
            } catch (...) {
                return false; // Exception during demand loading
            }
        }

        auto tis = (CResTIS_MIN *) pRes->tis;
        if (!tis) {
            return false; // TIS is null after demand
        }

        // Validate TIS structure - tileDataBlockLen is dynamic based on map size
        if (tis->tileDataBlockLen == 0) {
            return false; // Empty tile data
        }

        if (!tis->tileTable) {
            return false; // No tile table available
        }

        // Validate tile index bounds
        if (pRes->tileIndex < 0) {
            return false; // Invalid tile index
        }

        auto *table = static_cast<const PVRZTileEntry *>(tis->tileTable);

        // Basic bounds check - ensure we can read at least the entry
        PVRZTileEntry testEntry;
        if (!core::safe_read(table + pRes->tileIndex, testEntry)) {
            return false; // Can't safely read tile entry at index
        }

        out.tis = tis;
        out.index = pRes->tileIndex;
        out.table = table;

        return true;
    }

    inline bool get_tis_linear_tiles_flag(const CResTIS_MIN *tis) {
        if (!tis) {
            return false;
        }

        const void *flagAddr = (const char *) tis + GameOffsets::TIS_LINEAR_TILES_OFFSET;
        int flag = 0;
        if (!core::safe_read(flagAddr, flag)) {
            return false; // Couldn't read safely
        }

        return flag != 0;
    }
}
