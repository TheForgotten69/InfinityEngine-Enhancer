#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "iee/game/build_manifest.h"

namespace iee::game {
// The GL backend queues every draw as one of these and submits the queue in
// DrawFlush_GL. Layout verified against DrawEnd_GL / DrawFlush_GL (2.7.3).
struct DrawCommand {
  std::uint32_t state{};
  std::int32_t primStart{};
  // -1 marks a clear command; it carries no vertices.
  std::int32_t primCount{};
};
static_assert(sizeof(DrawCommand) == 12, "DrawCommand must match the engine's 12-byte command");

// True when the command adds its colour to what is already drawn (destination
// factor GL_ONE). The engine draws its light-emitting art this way: fires,
// spell effects, glows.
bool is_additive(const DrawQueueLayout& layout, std::uint32_t state) noexcept;

// Copies the additive, drawable commands of a queue into `out`, in order.
// Returns how many were copied; 0 for an implausible queue.
std::size_t collect_additive(const DrawQueueLayout& layout, const DrawCommand* commands,
                             int count, std::span<DrawCommand> out) noexcept;

// Address of the queue's command count, read from the rip-relative compare at
// the start of DrawFlush_GL. 0 when the bytes there are not that instruction.
std::uintptr_t draw_queue_count_address(const DrawQueueLayout& layout,
                                        std::uintptr_t drawFlush) noexcept;
std::uintptr_t draw_queue_commands_address(const DrawQueueLayout& layout,
                                           std::uintptr_t countAddress) noexcept;

// Which sprite atlas the engine is uploading, given the pixel buffer passed
// to its upload: the buffer must be that atlas's own, and the texture the
// engine has selected must be that atlas's texture. -1 for any other upload.
int atlas_slot_for_upload(const SpriteAtlasLayout& layout, std::uintptr_t countAddress,
                          const void* pixels) noexcept;

struct SpriteAtlasInfo {
  int width{};
  int height{};
  // The atlas's GL texture name inside the engine's texture table; the engine
  // binds whatever is stored here. Null for an invalid slot.
  std::uint32_t* textureName{};
};
SpriteAtlasInfo atlas_info(const SpriteAtlasLayout& layout, std::uintptr_t countAddress,
                           int slot) noexcept;
}  // namespace iee::game
