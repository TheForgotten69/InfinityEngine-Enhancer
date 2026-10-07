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
}  // namespace iee::game
