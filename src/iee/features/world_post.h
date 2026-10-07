#pragma once

namespace iee::core {
struct EngineConfig;
}

namespace iee::features {
// World post passes: soft fog of war, and glow plus light spill from the
// engine's light-emitting draws, run from the
// CInfinity::RenderFog detour. The engine queues the whole frame and only
// draws in DrawFlush_GL, so the caller flushes that queue around these calls:
//
//   flush();                                  // world is now in the framebuffer
//   bool capturing = world_post_before_fog(); // glow + light spill; fog target bound if true
//   original RenderFog                        // queues the fog triangles
//   if (capturing) { flush(); world_post_after_fog(); }
//
// Render thread only, except world_post_on_area_load.

void world_post_configure(const core::EngineConfig& cfg) noexcept;
// True when at least one effect is switched on and no pass has failed.
bool world_post_active() noexcept;
// Debug A/B: F8 toggles soft fog of war, F9 toggles bloom. Call once per
// frame, only when debug hotkeys are enabled.
void world_post_poll_hotkeys() noexcept;
// Frame boundary: the next world pass starts.
void world_post_on_frame() noexcept;
// Bloom is fed by the engine's own additive draws (fires, spell effects,
// glows). The DrawFlush_GL detour replays them into a black target:
//
//   if (world_post_wants_emissive()) {
//     collect the queue's additive commands; original flush;
//     if (any && world_post_begin_emissive()) {   // our target is bound
//       put them back in the queue; original flush;
//       world_post_end_emissive(count);           // engine framebuffer rebound
//     }
//   }
bool world_post_wants_emissive() noexcept;
bool world_post_begin_emissive() noexcept;
void world_post_end_emissive(int commands) noexcept;
// viewWorldWidth: world pixels visible across the view (0 when unknown).
bool world_post_before_fog(float viewWorldWidth) noexcept;
void world_post_after_fog() noexcept;
// Any thread. The next frame logs what engaged.
void world_post_on_area_load() noexcept;
// Drops every GL handle without touching GL. For shutdown paths.
void world_post_forget() noexcept;
}  // namespace iee::features
