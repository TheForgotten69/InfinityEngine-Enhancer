#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace iee::game {

enum class DdsBlockFormat : std::uint8_t {
  Bc1RgbUnorm,
  Bc1RgbaUnorm,
  Bc1RgbaSrgb,
  Bc3RgbaUnorm,
  Bc3RgbaSrgb,
  Bc5RgUnorm,
  Bc7RgbaUnorm,
  Bc7RgbaSrgb,
};

struct DdsMipLevel {
  std::uint32_t width{};
  std::uint32_t height{};
  std::size_t dataOffset{};
  std::size_t dataSize{};
};

struct DdsTexture {
  std::uint32_t width{};
  std::uint32_t height{};
  DdsBlockFormat format{DdsBlockFormat::Bc1RgbUnorm};
  std::vector<DdsMipLevel> mipLevels;
  std::vector<std::byte> payload;

  [[nodiscard]] bool empty() const noexcept { return mipLevels.empty() || payload.empty(); }
};

[[nodiscard]] const char* dds_block_format_name(DdsBlockFormat format) noexcept;

// Parses a single 2D block-compressed DDS texture. Cubemaps, arrays, volume
// textures, and uncompressed formats are deliberately outside this loader.
[[nodiscard]] bool parse_dds_texture(std::span<const std::byte> bytes, DdsTexture& out,
                                     std::string& error);

// File wrapper with a bounded allocation. Parsing remains separately exposed
// so malformed-input tests do not depend on filesystem or OpenGL behavior.
[[nodiscard]] bool load_dds_texture(const std::filesystem::path& path, DdsTexture& out,
                                    std::string& error);

}  // namespace iee::game
