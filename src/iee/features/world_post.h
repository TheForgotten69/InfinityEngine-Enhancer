#pragma once

namespace iee::core {
struct EngineConfig;
}

namespace iee::features {
// The part of the world the view shows, in world pixels. All zero when unknown.
struct WorldView {
  float scrollX{};
  float scrollY{};
  float width{};
  float height{};
};

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
// Debug A/B: F7 toggles sprite upscaling, F8 soft fog of war, F9 bloom. Call once per
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
// Sprite smoothing. The engine composites sprites on the CPU into a streaming
// atlas and uploads it at the start of a flush. Right after that upload, in
// the world pass, the atlas is upscaled 2x with FSR1 (edge-adaptive upsample,
// then sharpen); the caller makes the engine draw from the returned texture
// for the rest of that flush and puts the engine's own back afterwards.
// Normalised texture coordinates are unchanged by the larger texture.
bool world_post_wants_atlas_upscale() noexcept;
// Returns the GL name of the upscaled texture, or 0 to leave the atlas alone.
// `rows` is how many atlas rows the engine just uploaded.
unsigned world_post_upscale_atlas(int slot, unsigned sourceTexture, int width, int height,
                                  int rows) noexcept;
bool world_post_before_fog(const WorldView& view) noexcept;
void world_post_after_fog() noexcept;
// Any thread. The next frame logs what engaged.
void world_post_on_area_load() noexcept;
// Drops every GL handle without touching GL. For shutdown paths.
void world_post_forget() noexcept;
}  // namespace iee::features
