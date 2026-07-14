#include "water_textures.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "iee/core/gl_state_guard.h"
#include "iee/core/logger.h"
#include "iee/game/dds_texture.h"
#include "iee/game/opengl_types.h"
#include "iee/game/texture_units.h"

namespace iee::water {
namespace {
struct RawImage {
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<std::uint8_t> rgba;
};

struct TextureAsset {
  RawImage raw;
  game::DdsTexture dds;
  std::string sourceFile;

  [[nodiscard]] bool compressed() const noexcept { return !dds.empty(); }
  [[nodiscard]] std::uint32_t width() const noexcept {
    return compressed() ? dds.width : raw.width;
  }
  [[nodiscard]] std::uint32_t height() const noexcept {
    return compressed() ? dds.height : raw.height;
  }
};

struct Entry {
  const char* stem;
  unsigned unit;
};

constexpr std::array<Entry, 3> kEntries{{
    {"iee_water_normal", game::texture_units::WaterNormal},
    {"iee_water_dudv", game::texture_units::WaterDudv},
    {"iee_water_foam", game::texture_units::WaterFoam},
}};

std::mutex g_textureMutex;
std::array<TextureAsset, kEntries.size()> g_assets;
std::array<unsigned, kEntries.size()> g_textures{};
HGLRC g_ownerContext{};
HGLRC g_uploadBlockedContext{};
bool g_assetsLoaded{};
bool g_uploaded{};

// Reads the authoring-time blob format: "IRGB" magic, u32 width,
// u32 height (little endian), then width*height*4 bytes of RGBA.
bool read_irgb(const std::filesystem::path& path, RawImage& out) {
  out = {};
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  char magic[4] = {};
  file.read(magic, 4);
  if (std::memcmp(magic, "IRGB", 4) != 0) {
    LOG_WARN("Water texture {} has a bad magic header", path.filename().string());
    return false;
  }
  std::uint32_t dims[2] = {};
  file.read(reinterpret_cast<char*>(dims), sizeof(dims));
  if (dims[0] == 0 || dims[1] == 0 || dims[0] > 8192 || dims[1] > 8192) {
    LOG_WARN("Water texture {} has implausible dimensions {}x{}", path.filename().string(), dims[0],
             dims[1]);
    return false;
  }
  constexpr std::size_t kMaximumRawTextureBytes = 128 * 1024 * 1024;
  const std::size_t byteCount = std::size_t{dims[0]} * dims[1] * 4;
  if (byteCount > kMaximumRawTextureBytes) {
    LOG_WARN("Water texture {} exceeds the 128 MiB decoded-size safety limit",
             path.filename().string());
    return false;
  }
  out.rgba.resize(byteCount);
  file.read(reinterpret_cast<char*>(out.rgba.data()), static_cast<std::streamsize>(byteCount));
  if (!file) {
    LOG_WARN("Water texture {} is truncated", path.filename().string());
    return false;
  }
  out.width = dims[0];
  out.height = dims[1];
  return true;
}

bool read_texture_asset(const std::filesystem::path& dir, const Entry& entry, TextureAsset& out) {
  out = {};

  const auto ddsPath = dir / (std::string(entry.stem) + ".dds");
  std::error_code existsError;
  const bool ddsExists = std::filesystem::exists(ddsPath, existsError);
  if (existsError) {
    LOG_ERROR("Could not inspect optional DDS texture {}: {}", ddsPath.string(),
              existsError.message());
    return false;
  }
  if (ddsExists) {
    std::string error;
    if (!game::load_dds_texture(ddsPath, out.dds, error)) {
      LOG_ERROR("DDS water texture {} is invalid: {}", ddsPath.string(), error);
      return false;
    }
    out.sourceFile = ddsPath.filename().string();
    return true;
  }

  const auto rawPath = dir / (std::string(entry.stem) + ".rgba");
  if (!read_irgb(rawPath, out.raw)) {
    LOG_ERROR("Required water texture missing or unreadable: {} (no DDS override was present)",
              rawPath.string());
    return false;
  }
  out.sourceFile = rawPath.filename().string();
  return true;
}

std::optional<unsigned> compressed_internal_format(game::DdsBlockFormat format) noexcept {
  switch (format) {
    case game::DdsBlockFormat::Bc1RgbUnorm:
      return game::gl::COMPRESSED_RGB_S3TC_DXT1_EXT;
    case game::DdsBlockFormat::Bc1RgbaUnorm:
      return game::gl::COMPRESSED_RGBA_S3TC_DXT1_EXT;
    case game::DdsBlockFormat::Bc1RgbaSrgb:
      return game::gl::COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT;
    case game::DdsBlockFormat::Bc3RgbaUnorm:
      return game::gl::COMPRESSED_RGBA_S3TC_DXT5_EXT;
    case game::DdsBlockFormat::Bc3RgbaSrgb:
      return game::gl::COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
    case game::DdsBlockFormat::Bc5RgUnorm:
      return game::gl::COMPRESSED_RG_RGTC2;
    case game::DdsBlockFormat::Bc7RgbaUnorm:
      return game::gl::COMPRESSED_RGBA_BPTC_UNORM;
    case game::DdsBlockFormat::Bc7RgbaSrgb:
      return game::gl::COMPRESSED_SRGB_ALPHA_BPTC_UNORM;
  }
  return std::nullopt;
}

int full_mip_count(std::uint32_t width, std::uint32_t height) noexcept {
  int count = 1;
  for (std::uint32_t size = (std::max)(width, height); size > 1; size >>= 1) ++count;
  return count;
}

bool upload_to_unit(const TextureAsset& asset, unsigned unit, unsigned& texture) {
  auto& gl = game::gl::get_gl_functions();
  if (!gl.textureUploadAvailable) {
    return false;
  }
  if (texture == 0) {
    gl.glGenTextures(1, &texture);
  }
  gl.glActiveTexture(game::gl::TEXTURE0 + unit);
  gl.glBindTexture(game::gl::TEXTURE_2D, texture);
  bool mipmapsReady = asset.compressed() && asset.dds.mipLevels.size() > 1;
  int highestMipLevel = 0;

  if (asset.compressed()) {
    const auto internalFormat = compressed_internal_format(asset.dds.format);
    if (!internalFormat || !gl.compressedTextureUploadAvailable) {
      LOG_ERROR(
          "DDS water texture {} ({}) cannot be uploaded: glCompressedTexImage2D is unavailable",
          asset.sourceFile, game::dds_block_format_name(asset.dds.format));
      return false;
    }
    for (std::size_t levelIndex = 0; levelIndex < asset.dds.mipLevels.size(); ++levelIndex) {
      const auto& level = asset.dds.mipLevels[levelIndex];
      if (level.dataSize > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        LOG_ERROR("DDS water texture {} has an oversized mip level", asset.sourceFile);
        return false;
      }
      gl.glCompressedTexImage2D(game::gl::TEXTURE_2D, static_cast<int>(levelIndex), *internalFormat,
                                static_cast<int>(level.width), static_cast<int>(level.height), 0,
                                static_cast<int>(level.dataSize),
                                asset.dds.payload.data() + level.dataOffset);
      if (!game::gl::check_error("DDS water texture mip upload")) {
        LOG_ERROR(
            "DDS water texture {} ({}) failed at mip level {}; the driver may not support "
            "that compressed format",
            asset.sourceFile, game::dds_block_format_name(asset.dds.format), levelIndex);
        return false;
      }
    }
    highestMipLevel = static_cast<int>(asset.dds.mipLevels.size()) - 1;
  } else {
    gl.glPixelStorei(game::gl::UNPACK_ALIGNMENT, 1);
    gl.glTexImage2D(game::gl::TEXTURE_2D, 0, game::gl::RGBA8, static_cast<int>(asset.raw.width),
                    static_cast<int>(asset.raw.height), 0, game::gl::RGBA, game::gl::UNSIGNED_BYTE,
                    asset.raw.rgba.data());
    if (!game::gl::check_error("water base texture upload")) return false;
  }

  if (!mipmapsReady && gl.glGenerateMipmap) {
    highestMipLevel = full_mip_count(asset.width(), asset.height()) - 1;
    gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MAX_LEVEL, highestMipLevel);
    gl.glGenerateMipmap(game::gl::TEXTURE_2D);
    mipmapsReady = game::gl::check_error("water texture mipmap generation");
    if (!mipmapsReady) highestMipLevel = 0;
  } else if (!mipmapsReady) {
    static bool warnedOnce = false;
    if (!warnedOnce) {
      warnedOnce = true;
      LOG_WARN("glGenerateMipmap is unavailable; water textures will use base-level filtering");
    }
  }
  gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MAX_LEVEL, highestMipLevel);
  gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MIN_FILTER,
                     mipmapsReady ? game::gl::LINEAR_MIPMAP_LINEAR : game::gl::LINEAR);
  gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MAG_FILTER, game::gl::LINEAR);
  gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_WRAP_S, game::gl::REPEAT);
  gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_WRAP_T, game::gl::REPEAT);
  return game::gl::check_error("water texture upload");
}

bool upload_all_to_current_context() {
  auto& gl = game::gl::get_gl_functions();
  const auto context = game::gl::current_context();
  if (!context || !gl.textureUploadAvailable || !g_assetsLoaded) return false;

  if (context != g_ownerContext) {
    g_ownerContext = context;
    g_uploadBlockedContext = nullptr;
    g_textures.fill(0);
    g_uploaded = false;
  }
  if (context == g_uploadBlockedContext) return false;

  core::GlStateGuard guard({game::texture_units::WaterNormal, game::texture_units::WaterDudv,
                            game::texture_units::WaterFoam});
  // Do not attribute an error left by preceding engine work to the first DDS
  // upload and then suppress retries for the lifetime of this GL context.
  game::gl::discard_errors();
  for (std::size_t index = 0; index < kEntries.size(); ++index) {
    if (!upload_to_unit(g_assets[index], kEntries[index].unit, g_textures[index])) {
      g_uploaded = false;
      g_uploadBlockedContext = context;
      return false;
    }
    LOG_DEBUG("Water texture uploaded: {} {}x{} (unit {}, storage={})", g_assets[index].sourceFile,
              g_assets[index].width(), g_assets[index].height(), kEntries[index].unit,
              g_assets[index].compressed() ? "DDS" : "IRGB");
  }
  g_uploadBlockedContext = nullptr;
  g_uploaded = true;
  return true;
}
}  // namespace

bool prepare_water_textures(const std::filesystem::path& dir) {
  try {
    std::lock_guard lock(g_textureMutex);
    if (g_assetsLoaded) return true;
    std::array<TextureAsset, kEntries.size()> decoded;
    for (std::size_t index = 0; index < kEntries.size(); ++index) {
      if (!read_texture_asset(dir, kEntries[index], decoded[index])) {
        g_assetsLoaded = false;
        return false;
      }
      LOG_INFO("Selected water texture asset: {} ({}x{}, {})", decoded[index].sourceFile,
               decoded[index].width(), decoded[index].height(),
               decoded[index].compressed() ? game::dds_block_format_name(decoded[index].dds.format)
                                           : "RGBA8");
    }
    g_assets = std::move(decoded);
    g_assetsLoaded = true;
    return true;
  } catch (const std::exception& e) {
    LOG_ERROR("Water texture preparation failed: {}", e.what());
    return false;
  } catch (...) {
    LOG_ERROR("Water texture preparation failed with an unknown exception");
    return false;
  }
}

bool upload_water_textures() {
  try {
    std::lock_guard lock(g_textureMutex);
    return upload_all_to_current_context();
  } catch (const std::exception& e) {
    LOG_ERROR("Water texture upload failed: {}", e.what());
    return false;
  } catch (...) {
    LOG_ERROR("Water texture upload failed with an unknown exception");
    return false;
  }
}

bool ensure_water_textures_bound() noexcept {
  try {
    std::lock_guard lock(g_textureMutex);
    const auto context = game::gl::current_context();
    if (!context || !g_assetsLoaded) return false;
    if (!g_uploaded || context != g_ownerContext ||
        std::any_of(g_textures.begin(), g_textures.end(),
                    [](unsigned texture) { return texture == 0; })) {
      if (!upload_all_to_current_context()) return false;
    }

    auto& gl = game::gl::get_gl_functions();
    int previousActiveTexture = -1;
    if (gl.glGetIntegerv) gl.glGetIntegerv(0x84E0 /*ACTIVE_TEXTURE*/, &previousActiveTexture);
    for (std::size_t index = 0; index < kEntries.size(); ++index) {
      gl.glActiveTexture(game::gl::TEXTURE0 + kEntries[index].unit);
      gl.glBindTexture(game::gl::TEXTURE_2D, g_textures[index]);
    }
    if (previousActiveTexture >= 0)
      gl.glActiveTexture(static_cast<unsigned>(previousActiveTexture));
    return true;
  } catch (...) {
    return false;
  }
}

void release_water_textures() noexcept {
  try {
    std::lock_guard lock(g_textureMutex);
    auto& gl = game::gl::get_gl_functions();
    if (g_ownerContext && game::gl::current_context() == g_ownerContext && gl.glDeleteTextures) {
      gl.glDeleteTextures(static_cast<int>(g_textures.size()), g_textures.data());
    }
    g_textures.fill(0);
    g_assets = {};
    g_assetsLoaded = false;
    g_uploaded = false;
    g_ownerContext = nullptr;
    g_uploadBlockedContext = nullptr;
  } catch (...) {
    // Explicit shutdown is best-effort and must never escape into EEex.
  }
}
}  // namespace iee::water
