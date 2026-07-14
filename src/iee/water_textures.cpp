#include "water_textures.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <utility>
#include <vector>

#include "iee/core/gl_state_guard.h"
#include "iee/core/logger.h"
#include "iee/game/opengl_types.h"

namespace iee::water {
namespace {
struct RawImage {
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<std::uint8_t> rgba;
};

struct Entry {
  const char* file;
  unsigned unit;
};

constexpr std::array<Entry, 3> kEntries{{
    {"iee_water_normal.rgba", 3},
    {"iee_water_dudv.rgba", 4},
    {"iee_water_foam.rgba", 5},
}};

std::mutex g_textureMutex;
std::array<RawImage, kEntries.size()> g_images;
std::array<unsigned, kEntries.size()> g_textures{};
HGLRC g_ownerContext{};
bool g_assetsLoaded{};
bool g_uploaded{};

// Reads the authoring-time blob format: "IRGB" magic, u32 width,
// u32 height (little endian), then width*height*4 bytes of RGBA.
bool read_irgb(const std::filesystem::path& path, RawImage& out) {
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
  const std::size_t byteCount = std::size_t{dims[0]} * dims[1] * 4;
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

bool upload_to_unit(const RawImage& image, unsigned unit, unsigned& texture) {
  auto& gl = game::gl::get_gl_functions();
  if (!gl.textureUploadAvailable) {
    return false;
  }
  if (texture == 0) {
    gl.glGenTextures(1, &texture);
  }
  gl.glActiveTexture(game::gl::TEXTURE0 + unit);
  gl.glBindTexture(game::gl::TEXTURE_2D, texture);
  gl.glPixelStorei(game::gl::UNPACK_ALIGNMENT, 1);
  gl.glTexImage2D(game::gl::TEXTURE_2D, 0, game::gl::RGBA8, static_cast<int>(image.width),
                  static_cast<int>(image.height), 0, game::gl::RGBA, game::gl::UNSIGNED_BYTE,
                  image.rgba.data());
  gl.glTexParameteri(game::gl::TEXTURE_2D, game::gl::TEXTURE_MIN_FILTER, game::gl::LINEAR);
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
    g_textures.fill(0);
    g_uploaded = false;
  }

  core::GlStateGuard guard({3, 4, 5});
  for (std::size_t index = 0; index < kEntries.size(); ++index) {
    if (!upload_to_unit(g_images[index], kEntries[index].unit, g_textures[index])) {
      g_uploaded = false;
      return false;
    }
    LOG_DEBUG("Water texture uploaded: {} {}x{} (unit {})", kEntries[index].file,
              g_images[index].width, g_images[index].height, kEntries[index].unit);
  }
  g_uploaded = true;
  return true;
}
}  // namespace

bool load_water_textures(const std::filesystem::path& dir) {
  try {
    std::lock_guard lock(g_textureMutex);
    if (g_assetsLoaded) {
      return upload_all_to_current_context();
    }
    std::array<RawImage, kEntries.size()> decoded;
    for (std::size_t index = 0; index < kEntries.size(); ++index) {
      const auto path = dir / kEntries[index].file;
      if (!read_irgb(path, decoded[index])) {
        LOG_ERROR("Required water texture missing or unreadable: {}", path.string());
        g_assetsLoaded = false;
        return false;
      }
    }
    g_images = std::move(decoded);
    g_assetsLoaded = true;
    return upload_all_to_current_context();
  } catch (const std::exception& e) {
    LOG_ERROR("Water texture initialization failed: {}", e.what());
    return false;
  } catch (...) {
    LOG_ERROR("Water texture initialization failed with an unknown exception");
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
    g_images = {};
    g_assetsLoaded = false;
    g_uploaded = false;
    g_ownerContext = nullptr;
  } catch (...) {
    // Explicit shutdown is best-effort and must never escape into EEex.
  }
}
}  // namespace iee::water
