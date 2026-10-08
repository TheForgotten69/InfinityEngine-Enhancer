#include "iee/game/draw_queue.h"

#include <cstring>

namespace iee::game {
bool is_additive(const DrawQueueLayout& layout, std::uint32_t state) noexcept {
  if (((state >> layout.blendEnableBit) & 1u) == 0) return false;
  return ((state >> layout.blendDstShift) & 0xFu) == layout.blendFactorOne;
}

std::size_t collect_additive(const DrawQueueLayout& layout, const DrawCommand* commands,
                             int count, std::span<DrawCommand> out) noexcept {
  if (!commands || count <= 0 || static_cast<std::uint32_t>(count) > layout.maxCommands) return 0;
  std::size_t collected = 0;
  for (int index = 0; index < count && collected < out.size(); ++index) {
    const auto& command = commands[index];
    if (command.primCount <= 0 || !is_additive(layout, command.state)) continue;
    out[collected++] = command;
  }
  return collected;
}

std::uintptr_t draw_queue_count_address(const DrawQueueLayout& layout,
                                        std::uintptr_t drawFlush) noexcept {
  if (!drawFlush || !layout.valid()) return 0;
  const auto* instruction = reinterpret_cast<const std::uint8_t*>(drawFlush + layout.countCompareOffset);
  // cmp dword ptr [rip + disp32], r14d
  if (instruction[0] != 0x44 || instruction[1] != 0x39 || instruction[2] != 0x35) return 0;
  std::int32_t displacement = 0;
  std::memcpy(&displacement, instruction + 3, sizeof(displacement));
  const auto next = drawFlush + layout.countCompareOffset + 7;
  return next + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(displacement));
}

std::uintptr_t draw_queue_commands_address(const DrawQueueLayout& layout,
                                           std::uintptr_t countAddress) noexcept {
  if (!countAddress || !layout.valid()) return 0;
  return countAddress - layout.commandsBeforeCount;
}

namespace {
template <typename T>
T read_at(std::uintptr_t address) noexcept {
  T value{};
  std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
  return value;
}
}  // namespace

int atlas_slot_for_upload(const SpriteAtlasLayout& layout, std::uintptr_t countAddress,
                          const void* pixels) noexcept {
  if (!countAddress || !pixels || !layout.valid()) return -1;
  const auto state = read_at<std::uint32_t>(countAddress - layout.userStateBeforeCount);
  const auto selected = (state >> layout.textureShift) & layout.textureMask;
  for (std::size_t slot = 0; slot < layout.atlasCount; ++slot) {
    const auto atlas = countAddress + layout.atlasAfterCount + slot * layout.atlasStride;
    if (read_at<std::uintptr_t>(atlas + layout.texelsOffset) !=
        reinterpret_cast<std::uintptr_t>(pixels)) {
      continue;
    }
    return read_at<std::uint32_t>(atlas + layout.textureIndexOffset) == selected
               ? static_cast<int>(slot)
               : -1;
  }
  return -1;
}

SpriteAtlasInfo atlas_info(const SpriteAtlasLayout& layout, std::uintptr_t countAddress,
                           int slot) noexcept {
  if (!countAddress || !layout.valid() || slot < 0 ||
      static_cast<std::size_t>(slot) >= layout.atlasCount) {
    return {};
  }
  const auto atlas =
      countAddress + layout.atlasAfterCount + static_cast<std::size_t>(slot) * layout.atlasStride;
  const auto index = read_at<std::uint32_t>(atlas + layout.textureIndexOffset) & layout.textureMask;
  return {read_at<std::int32_t>(atlas + layout.widthOffset),
          read_at<std::int32_t>(atlas + layout.heightOffset),
          reinterpret_cast<std::uint32_t*>(countAddress - layout.texturesBeforeCount +
                                           index * layout.textureEntrySize)};
}
}  // namespace iee::game
