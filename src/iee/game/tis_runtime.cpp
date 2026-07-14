#include "tis_runtime.h"

#include <limits>

#include "iee/core/pattern_scanner.h"

namespace iee::game {
namespace {
template <typename T>
bool safe_read_offset(const void* base, std::uintptr_t offset, T& out) noexcept {
  if (!base) return false;
  const auto address = reinterpret_cast<std::uintptr_t>(base);
  if (offset > (std::numeric_limits<std::uintptr_t>::max)() - address) return false;
  return core::safe_read(reinterpret_cast<const void*>(address + offset), out);
}
}  // namespace

bool read_tis_tile_entry(const TileInfo& tileInfo, std::uint32_t index,
                         PVRZTileEntry& out) noexcept {
  out = {};
  if (!tileInfo.table || index >= tileInfo.tileCount) {
    return false;
  }

  const auto tableAddress = reinterpret_cast<std::uintptr_t>(tileInfo.table);
  constexpr auto entrySize = sizeof(PVRZTileEntry);
  if (index > (std::numeric_limits<std::uintptr_t>::max() - tableAddress) / entrySize) {
    return false;
  }
  const auto entryAddress = tableAddress + static_cast<std::uintptr_t>(index) * entrySize;
  return core::safe_read(reinterpret_cast<const void*>(entryAddress), out);
}

bool get_tile_info(void* vidTile, const BuildManifest& manifest, TileInfo& out,
                   void* (*CRes_Demand)(void*)) {
  out = {};

  if (!vidTile) {
    return false;
  }

  CResTile* resource = nullptr;
  if (!safe_read_offset(vidTile, manifest.offsets.vidTileResource, resource) || !resource) {
    return false;
  }

  CResTile resourceSnapshot;
  if (!core::safe_read(resource, resourceSnapshot) || !resourceSnapshot.tis) {
    return false;
  }

  if (CRes_Demand) {
    try {
      (void)CRes_Demand(resourceSnapshot.tis);
    } catch (...) {
      return false;
    }
  }

  CResTileSet tilesetSnapshot;
  if (!core::safe_read(resourceSnapshot.tis, tilesetSnapshot)) {
    return false;
  }

  if (!tilesetSnapshot.baseclass_0.pData ||
      tilesetSnapshot.baseclass_0.nSize < sizeof(PVRZTileEntry) ||
      tilesetSnapshot.baseclass_0.nCount == 0 || resourceSnapshot.tileIndex < 0 ||
      static_cast<std::uint32_t>(resourceSnapshot.tileIndex) >=
          tilesetSnapshot.baseclass_0.nCount) {
    return false;
  }

  out.resource = resource;
  out.tileset = resourceSnapshot.tis;
  out.table = static_cast<const PVRZTileEntry*>(tilesetSnapshot.baseclass_0.pData);
  out.header = tilesetSnapshot.h;
  out.index = resourceSnapshot.tileIndex;
  out.tileDataBlockLen = tilesetSnapshot.baseclass_0.nSize;
  out.tileCount = tilesetSnapshot.baseclass_0.nCount;

  if (!read_tis_tile_entry(out, static_cast<std::uint32_t>(out.index), out.entry)) {
    out = {};
    return false;
  }

  return true;
}

bool get_tis_linear_tiles_flag(const CResTileSet* tis, const BuildManifest& manifest) {
  if (!tis) {
    return false;
  }

  int flag = 0;
  return safe_read_offset(tis, manifest.offsets.tisLinearTilesFlag, flag) && flag != 0;
}

std::optional<std::uint32_t> get_tis_header_tile_dimension(const TileInfo& tileInfo,
                                                           const BuildManifest& manifest) {
  if (!tileInfo.header) {
    return std::nullopt;
  }

  std::uint32_t tileDimension = 0;
  if (!safe_read_offset(tileInfo.header, manifest.offsets.tisHeaderTileDimension, tileDimension)) {
    return std::nullopt;
  }

  return tileDimension;
}
}  // namespace iee::game
