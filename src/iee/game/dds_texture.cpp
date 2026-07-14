#include "dds_texture.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <utility>

namespace iee::game {
namespace {
constexpr std::size_t kLegacyHeaderBytes = 128;
constexpr std::size_t kDx10HeaderBytes = 20;
constexpr std::size_t kMaximumFileBytes = 128 * 1024 * 1024;
constexpr std::uint32_t kMaximumDimension = 8192;

constexpr std::uint32_t kPixelFormatAlphaPixels = 0x1;
constexpr std::uint32_t kPixelFormatFourCc = 0x4;
constexpr std::uint32_t kCaps2CubemapMask = 0xFE00;
constexpr std::uint32_t kCaps2Volume = 0x200000;
constexpr std::uint32_t kDx10Texture2D = 3;
constexpr std::uint32_t kDx10TextureCube = 0x4;

constexpr std::uint32_t four_cc(char a, char b, char c, char d) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
}

constexpr std::uint32_t kFourCcDxt1 = four_cc('D', 'X', 'T', '1');
constexpr std::uint32_t kFourCcDxt5 = four_cc('D', 'X', 'T', '5');
constexpr std::uint32_t kFourCcDx10 = four_cc('D', 'X', '1', '0');
constexpr std::uint32_t kFourCcAti2 = four_cc('A', 'T', 'I', '2');
constexpr std::uint32_t kFourCcBc5u = four_cc('B', 'C', '5', 'U');

// DXGI_FORMAT values used by the DDS DX10 extension header.
constexpr std::uint32_t kDxgiBc1Unorm = 71;
constexpr std::uint32_t kDxgiBc1UnormSrgb = 72;
constexpr std::uint32_t kDxgiBc3Unorm = 77;
constexpr std::uint32_t kDxgiBc3UnormSrgb = 78;
constexpr std::uint32_t kDxgiBc5Unorm = 83;
constexpr std::uint32_t kDxgiBc7Unorm = 98;
constexpr std::uint32_t kDxgiBc7UnormSrgb = 99;

bool read_u32(std::span<const std::byte> bytes, std::size_t offset, std::uint32_t& out) noexcept {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(out)) return false;
  out = std::to_integer<std::uint32_t>(bytes[offset]) |
        (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8) |
        (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16) |
        (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24);
  return true;
}

std::uint32_t full_mip_count(std::uint32_t width, std::uint32_t height) noexcept {
  std::uint32_t count = 1;
  for (std::uint32_t size = (std::max)(width, height); size > 1; size >>= 1) ++count;
  return count;
}

std::size_t block_bytes(DdsBlockFormat format) noexcept {
  return format == DdsBlockFormat::Bc1RgbUnorm || format == DdsBlockFormat::Bc1RgbaUnorm ||
                 format == DdsBlockFormat::Bc1RgbaSrgb
             ? 8
             : 16;
}

bool resolve_format(std::uint32_t pixelFormatFlags, std::uint32_t formatCode,
                    std::span<const std::byte> bytes, DdsBlockFormat& format,
                    std::size_t& dataOffset, std::string& error) {
  if ((pixelFormatFlags & kPixelFormatFourCc) == 0) {
    error = "DDS texture is not block-compressed";
    return false;
  }

  if (formatCode == kFourCcDxt1) {
    format = (pixelFormatFlags & kPixelFormatAlphaPixels) != 0 ? DdsBlockFormat::Bc1RgbaUnorm
                                                               : DdsBlockFormat::Bc1RgbUnorm;
    return true;
  }
  if (formatCode == kFourCcDxt5) {
    format = DdsBlockFormat::Bc3RgbaUnorm;
    return true;
  }
  if (formatCode == kFourCcAti2 || formatCode == kFourCcBc5u) {
    format = DdsBlockFormat::Bc5RgUnorm;
    return true;
  }
  if (formatCode != kFourCcDx10) {
    error = "DDS uses an unsupported legacy FourCC (expected DXT1, DXT5, ATI2/BC5U, or DX10)";
    return false;
  }

  if (bytes.size() < kLegacyHeaderBytes + kDx10HeaderBytes) {
    error = "DDS DX10 header is truncated";
    return false;
  }

  std::uint32_t dxgiFormat = 0;
  std::uint32_t resourceDimension = 0;
  std::uint32_t miscellaneousFlags = 0;
  std::uint32_t arraySize = 0;
  if (!read_u32(bytes, 128, dxgiFormat) || !read_u32(bytes, 132, resourceDimension) ||
      !read_u32(bytes, 136, miscellaneousFlags) || !read_u32(bytes, 140, arraySize)) {
    error = "DDS DX10 header is unreadable";
    return false;
  }
  if (resourceDimension != kDx10Texture2D || arraySize != 1 ||
      (miscellaneousFlags & kDx10TextureCube) != 0) {
    error = "DDS must contain exactly one non-cubemap 2D texture";
    return false;
  }

  switch (dxgiFormat) {
    case kDxgiBc1Unorm:
      format = DdsBlockFormat::Bc1RgbaUnorm;
      break;
    case kDxgiBc1UnormSrgb:
      format = DdsBlockFormat::Bc1RgbaSrgb;
      break;
    case kDxgiBc3Unorm:
      format = DdsBlockFormat::Bc3RgbaUnorm;
      break;
    case kDxgiBc3UnormSrgb:
      format = DdsBlockFormat::Bc3RgbaSrgb;
      break;
    case kDxgiBc5Unorm:
      format = DdsBlockFormat::Bc5RgUnorm;
      break;
    case kDxgiBc7Unorm:
      format = DdsBlockFormat::Bc7RgbaUnorm;
      break;
    case kDxgiBc7UnormSrgb:
      format = DdsBlockFormat::Bc7RgbaSrgb;
      break;
    default:
      error = "DDS DX10 header uses an unsupported DXGI format";
      return false;
  }

  dataOffset += kDx10HeaderBytes;
  return true;
}
}  // namespace

const char* dds_block_format_name(DdsBlockFormat format) noexcept {
  switch (format) {
    case DdsBlockFormat::Bc1RgbUnorm:
      return "BC1 RGB";
    case DdsBlockFormat::Bc1RgbaUnorm:
      return "BC1 RGBA";
    case DdsBlockFormat::Bc1RgbaSrgb:
      return "BC1 sRGB";
    case DdsBlockFormat::Bc3RgbaUnorm:
      return "BC3 RGBA";
    case DdsBlockFormat::Bc3RgbaSrgb:
      return "BC3 sRGB";
    case DdsBlockFormat::Bc5RgUnorm:
      return "BC5 RG";
    case DdsBlockFormat::Bc7RgbaUnorm:
      return "BC7 RGBA";
    case DdsBlockFormat::Bc7RgbaSrgb:
      return "BC7 sRGB";
  }
  return "unknown";
}

bool parse_dds_texture(std::span<const std::byte> bytes, DdsTexture& out, std::string& error) {
  out = {};
  error.clear();

  try {
    if (bytes.size() < kLegacyHeaderBytes || std::memcmp(bytes.data(), "DDS ", 4) != 0) {
      error = "DDS magic/header is missing or truncated";
      return false;
    }

    std::uint32_t headerSize = 0;
    std::uint32_t height = 0;
    std::uint32_t width = 0;
    std::uint32_t depth = 0;
    std::uint32_t mipCount = 0;
    std::uint32_t pixelFormatSize = 0;
    std::uint32_t pixelFormatFlags = 0;
    std::uint32_t formatCode = 0;
    std::uint32_t caps2 = 0;
    if (!read_u32(bytes, 4, headerSize) || !read_u32(bytes, 12, height) ||
        !read_u32(bytes, 16, width) || !read_u32(bytes, 24, depth) ||
        !read_u32(bytes, 28, mipCount) || !read_u32(bytes, 76, pixelFormatSize) ||
        !read_u32(bytes, 80, pixelFormatFlags) || !read_u32(bytes, 84, formatCode) ||
        !read_u32(bytes, 112, caps2)) {
      error = "DDS header fields are truncated";
      return false;
    }

    if (headerSize != 124 || pixelFormatSize != 32) {
      error = "DDS header or pixel-format structure has an invalid size";
      return false;
    }
    if (width == 0 || height == 0 || width > kMaximumDimension || height > kMaximumDimension) {
      error = "DDS dimensions are zero or exceed 8192x8192";
      return false;
    }
    if (depth > 1 || (caps2 & (kCaps2CubemapMask | kCaps2Volume)) != 0) {
      error = "DDS cubemap and volume textures are unsupported";
      return false;
    }

    std::size_t dataOffset = kLegacyHeaderBytes;
    DdsBlockFormat format{};
    if (!resolve_format(pixelFormatFlags, formatCode, bytes, format, dataOffset, error)) {
      return false;
    }

    if (mipCount == 0) mipCount = 1;
    if (mipCount > full_mip_count(width, height)) {
      error = "DDS declares more mip levels than its dimensions permit";
      return false;
    }

    std::vector<DdsMipLevel> mipLevels;
    mipLevels.reserve(mipCount);
    std::size_t cursor = dataOffset;
    std::uint32_t mipWidth = width;
    std::uint32_t mipHeight = height;
    const auto bytesPerBlock = block_bytes(format);
    for (std::uint32_t level = 0; level < mipCount; ++level) {
      const auto blocksAcross = static_cast<std::size_t>((mipWidth + 3) / 4);
      const auto blocksDown = static_cast<std::size_t>((mipHeight + 3) / 4);
      if (blocksAcross > (std::numeric_limits<std::size_t>::max)() / blocksDown ||
          blocksAcross * blocksDown > (std::numeric_limits<std::size_t>::max)() / bytesPerBlock) {
        error = "DDS mip size overflows addressable memory";
        return false;
      }
      const auto levelBytes = blocksAcross * blocksDown * bytesPerBlock;
      if (cursor > bytes.size() || levelBytes > bytes.size() - cursor) {
        error = "DDS compressed mip payload is truncated";
        return false;
      }
      mipLevels.push_back({
          .width = mipWidth,
          .height = mipHeight,
          .dataOffset = cursor - dataOffset,
          .dataSize = levelBytes,
      });
      cursor += levelBytes;
      mipWidth = (std::max)(std::uint32_t{1}, mipWidth / 2);
      mipHeight = (std::max)(std::uint32_t{1}, mipHeight / 2);
    }

    out.width = width;
    out.height = height;
    out.format = format;
    out.mipLevels = std::move(mipLevels);
    out.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(cursor));
    return true;
  } catch (const std::exception& exception) {
    out = {};
    error = std::string("DDS parsing failed: ") + exception.what();
    return false;
  } catch (...) {
    out = {};
    error = "DDS parsing failed with an unknown exception";
    return false;
  }
}

bool load_dds_texture(const std::filesystem::path& path, DdsTexture& out, std::string& error) {
  out = {};
  error.clear();

  try {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
      error = "file could not be opened";
      return false;
    }
    const auto endPosition = file.tellg();
    if (endPosition <= 0) {
      error = "file is empty or unreadable";
      return false;
    }
    const auto fileBytes = static_cast<std::streamoff>(endPosition);
    if (fileBytes <= 0 || static_cast<std::uintmax_t>(fileBytes) > kMaximumFileBytes) {
      error = "file is empty or exceeds the 128 MiB safety limit";
      return false;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(fileBytes));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
      error = "file could not be read completely";
      return false;
    }
    return parse_dds_texture(bytes, out, error);
  } catch (const std::exception& exception) {
    out = {};
    error = std::string("file loading failed: ") + exception.what();
    return false;
  } catch (...) {
    out = {};
    error = "file loading failed with an unknown exception";
    return false;
  }
}

}  // namespace iee::game
