#include "iee/features/world_post.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>

#include "iee/core/config.h"
#include "iee/core/gl_pass_guard.h"
#include "iee/core/gl_state_guard.h"
#include "iee/core/logger.h"
#include "iee/game/opengl_types.h"
#include "iee/game/world_post_plan.h"

namespace iee::features {
namespace {
namespace gl = game::gl;

struct Target {
  unsigned texture{};
  unsigned framebuffer{};
  game::Extent extent{};
};

struct CompositeProgram {
  unsigned id{};
  int scale{-1};
  int dither{-1};
};

struct BlurProgram {
  unsigned id{};
  int step{-1};
};

struct BrightProgram {
  unsigned id{};
  int threshold{-1};
};

// Level i is the base image halved i + 1 times.
struct BlurChain {
  std::array<Target, game::kMaxBlurLevels> levels{};
  int count{};
};

struct Settings {
  bool softFog{};
  bool bloom{};
  float fogRadius{};
  float bloomThreshold{};
  float bloomStrength{};
};

// Everything that lives and dies with one GL context.
struct Resources {
  HGLRC context{};
  game::Extent viewport{};
  unsigned vertexArray{};
  unsigned vertexBuffer{};
  CompositeProgram composite{};
  BlurProgram blurDown{};
  BlurProgram blurUp{};
  BrightProgram bright{};
  Target fogCapture{};
  BlurChain fogChain{};
  Target bloomScene{};   // half-size copy of the world
  Target bloomBright{};  // its bright parts
  BlurChain bloomChain{};
  bool fogDrawnOnce{};
  bool bloomDrawnOnce{};
};

Settings g_settings;
Resources g_resources;
bool g_failed = false;
int g_engineDrawFramebuffer = 0;
int g_engineReadFramebuffer = 0;
float g_pixelsPerWorldPixel = 1.0f;
std::atomic<bool> g_logNextFrame{true};

constexpr const char* kVertexSource = R"glsl(#version 330
layout(location = 0) in vec2 aPosition;
out vec2 vUv;
void main() {
  vUv = aPosition * 0.5 + 0.5;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)glsl";

// Writes the texture scaled by uScale; the caller's blend mode decides whether
// that multiplies (fog) or adds (bloom). uDither adds +-0.5/255 of ordered
// noise to hide banding in long dark gradients.
constexpr const char* kCompositeSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform float uScale;
uniform float uDither;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec3 color = texture(uTexture, vUv).rgb * uScale;
  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  fragColor = vec4(color + uDither * (noise - 0.5) / 255.0, 1.0);
}
)glsl";

// Dual-filter blur (down then up through half-size levels). uStep is the
// sample spread in source UV units.
constexpr const char* kBlurDownSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform vec2 uStep;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec4 sum = texture(uTexture, vUv) * 4.0;
  sum += texture(uTexture, vUv - uStep);
  sum += texture(uTexture, vUv + uStep);
  sum += texture(uTexture, vUv + vec2(uStep.x, -uStep.y));
  sum += texture(uTexture, vUv - vec2(uStep.x, -uStep.y));
  fragColor = sum / 8.0;
}
)glsl";

constexpr const char* kBlurUpSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform vec2 uStep;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec4 sum = texture(uTexture, vUv + vec2(-uStep.x * 2.0, 0.0));
  sum += texture(uTexture, vUv + vec2(-uStep.x, uStep.y)) * 2.0;
  sum += texture(uTexture, vUv + vec2(0.0, uStep.y * 2.0));
  sum += texture(uTexture, vUv + vec2(uStep.x, uStep.y)) * 2.0;
  sum += texture(uTexture, vUv + vec2(uStep.x * 2.0, 0.0));
  sum += texture(uTexture, vUv + vec2(uStep.x, -uStep.y)) * 2.0;
  sum += texture(uTexture, vUv + vec2(0.0, -uStep.y * 2.0));
  sum += texture(uTexture, vUv + vec2(-uStep.x, -uStep.y)) * 2.0;
  fragColor = sum / 12.0;
}
)glsl";

// Keeps what is brighter than uThreshold at full strength, with a soft knee
// so the cut-off does not flicker. The image is 8-bit, so nothing is brighter
// than white: subtracting the threshold (as HDR bloom does) would leave at
// most a few percent to glow with.
constexpr const char* kBrightSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform float uThreshold;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec3 color = texture(uTexture, vUv).rgb;
  float level = max(color.r, max(color.g, color.b));
  const float knee = 0.1;
  float weight = smoothstep(uThreshold - knee, uThreshold + knee, level);
  fragColor = vec4(color * weight, 1.0);
}
)glsl";

void fail(const char* reason) noexcept {
  g_failed = true;
  try {
    LOG_WARN("Soft fog of war and bloom disabled for this session: {}", reason);
  } catch (...) {
  }
}

unsigned compile_shader(unsigned type, const char* source) noexcept {
  const auto& fn = gl::get_gl_functions();
  const unsigned shader = fn.glCreateShader(type);
  if (!shader) return 0;
  fn.glShaderSource(shader, 1, &source, nullptr);
  fn.glCompileShader(shader);
  int status = 0;
  fn.glGetShaderiv(shader, gl::COMPILE_STATUS, &status);
  if (!status) {
    char log[512]{};
    fn.glGetShaderInfoLog(shader, static_cast<int>(sizeof(log)) - 1, nullptr, log);
    try {
      LOG_WARN("World post shader compile failed: {}", log);
    } catch (...) {
    }
    fn.glDeleteShader(shader);
    return 0;
  }
  return shader;
}

unsigned build_program(const char* fragmentSource) noexcept {
  const auto& fn = gl::get_gl_functions();
  const unsigned vertex = compile_shader(gl::VERTEX_SHADER, kVertexSource);
  const unsigned fragment = compile_shader(gl::FRAGMENT_SHADER, fragmentSource);
  unsigned program = 0;
  if (vertex && fragment) {
    program = fn.glCreateProgram();
    fn.glAttachShader(program, vertex);
    fn.glAttachShader(program, fragment);
    fn.glLinkProgram(program);
    int status = 0;
    fn.glGetProgramiv(program, gl::LINK_STATUS, &status);
    if (!status) {
      char log[512]{};
      fn.glGetProgramInfoLog(program, static_cast<int>(sizeof(log)) - 1, nullptr, log);
      try {
        LOG_WARN("World post program link failed: {}", log);
      } catch (...) {
      }
      fn.glDeleteProgram(program);
      program = 0;
    }
  }
  if (vertex) fn.glDeleteShader(vertex);
  if (fragment) fn.glDeleteShader(fragment);
  if (program) {
    fn.glUseProgram(program);
    fn.glUniform1i(fn.glGetUniformLocation(program, "uTexture"), 0);
  }
  return program;
}

void destroy_target(Target& target) noexcept {
  const auto& fn = gl::get_gl_functions();
  if (target.framebuffer) fn.glDeleteFramebuffers(1, &target.framebuffer);
  if (target.texture) fn.glDeleteTextures(1, &target.texture);
  target = {};
}

// Leaves the new framebuffer and texture bound; the caller's guards restore.
bool create_target(Target& target, game::Extent extent, unsigned internalFormat,
                   unsigned type) noexcept {
  const auto& fn = gl::get_gl_functions();
  destroy_target(target);
  fn.glGenTextures(1, &target.texture);
  fn.glBindTexture(gl::TEXTURE_2D, target.texture);
  fn.glTexImage2D(gl::TEXTURE_2D, 0, static_cast<int>(internalFormat), extent.width,
                  extent.height, 0, gl::RGBA, type, nullptr);
  fn.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, static_cast<int>(gl::LINEAR));
  fn.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, static_cast<int>(gl::LINEAR));
  fn.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, static_cast<int>(gl::CLAMP_TO_EDGE));
  fn.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, static_cast<int>(gl::CLAMP_TO_EDGE));
  fn.glGenFramebuffers(1, &target.framebuffer);
  fn.glBindFramebuffer(gl::FRAMEBUFFER, target.framebuffer);
  fn.glFramebufferTexture2D(gl::FRAMEBUFFER, gl::COLOR_ATTACHMENT0, gl::TEXTURE_2D,
                            target.texture, 0);
  if (fn.glCheckFramebufferStatus(gl::FRAMEBUFFER) != gl::FRAMEBUFFER_COMPLETE) {
    destroy_target(target);
    return false;
  }
  target.extent = extent;
  return true;
}

void destroy_chain(BlurChain& chain) noexcept {
  for (auto& level : chain.levels) destroy_target(level);
  chain.count = 0;
}

bool create_chain(BlurChain& chain, game::Extent base) noexcept {
  destroy_chain(chain);
  const int count = game::available_blur_levels(base);
  game::Extent extent = base;
  for (int index = 0; index < count; ++index) {
    extent = game::half_extent(extent);
    if (!create_target(chain.levels[static_cast<std::size_t>(index)], extent, gl::RGBA16F,
                       gl::HALF_FLOAT)) {
      destroy_chain(chain);
      return false;
    }
  }
  chain.count = count;
  return true;
}

void bind_engine_framebuffer() noexcept {
  const auto& fn = gl::get_gl_functions();
  fn.glBindFramebuffer(gl::DRAW_FRAMEBUFFER, static_cast<unsigned>(g_engineDrawFramebuffer));
  fn.glBindFramebuffer(gl::READ_FRAMEBUFFER, static_cast<unsigned>(g_engineReadFramebuffer));
}

// State every pass of ours draws with. The guards in scope restore it.
void set_pass_state() noexcept {
  const auto& fn = gl::get_gl_functions();
  fn.glDisable(gl::SCISSOR_TEST);
  fn.glDisable(gl::DEPTH_TEST);
  fn.glDisable(gl::CULL_FACE);
  fn.glDisable(gl::STENCIL_TEST);
  fn.glDisable(gl::ALPHA_TEST);
  fn.glDisable(gl::BLEND);
  fn.glBindVertexArray(g_resources.vertexArray);
  fn.glActiveTexture(gl::TEXTURE0);
}

void draw_composite(unsigned texture, float scale, float dither) noexcept {
  const auto& fn = gl::get_gl_functions();
  fn.glUseProgram(g_resources.composite.id);
  fn.glUniform1f(g_resources.composite.scale, scale);
  fn.glUniform1f(g_resources.composite.dither, dither);
  fn.glBindTexture(gl::TEXTURE_2D, texture);
  fn.glDrawArrays(gl::TRIANGLES, 0, 3);
}

void blur_pass(const BlurProgram& program, const Target& source, const Target& destination,
               float stepTexels) noexcept {
  const auto& fn = gl::get_gl_functions();
  fn.glBindFramebuffer(gl::FRAMEBUFFER, destination.framebuffer);
  fn.glViewport(0, 0, destination.extent.width, destination.extent.height);
  fn.glUseProgram(program.id);
  fn.glUniform2f(program.step, stepTexels / static_cast<float>(source.extent.width),
                 stepTexels / static_cast<float>(source.extent.height));
  fn.glBindTexture(gl::TEXTURE_2D, source.texture);
  fn.glDrawArrays(gl::TRIANGLES, 0, 3);
}

// Blurs `source` into chain.levels[0]. With `accumulate`, each level keeps its
// own downsampled content and the wider blur is added on top (bloom); without
// it the result is a plain blur (fog). Returns the number of levels used.
// Requires set_pass_state(); leaves blending off.
int run_blur(const Target& source, BlurChain& chain, game::BlurPlan plan,
             bool accumulate) noexcept {
  const auto& fn = gl::get_gl_functions();
  const int levels = plan.levels < chain.count ? plan.levels : chain.count;
  if (levels <= 0) return 0;
  fn.glDisable(gl::BLEND);
  const Target* from = &source;
  for (int index = 0; index < levels; ++index) {
    const auto& to = chain.levels[static_cast<std::size_t>(index)];
    blur_pass(g_resources.blurDown, *from, to, plan.offset);
    from = &to;
  }
  if (accumulate) {
    fn.glEnable(gl::BLEND);
    fn.glBlendFunc(gl::ONE, gl::ONE);
  }
  for (int index = levels - 1; index > 0; --index) {
    blur_pass(g_resources.blurUp, chain.levels[static_cast<std::size_t>(index)],
              chain.levels[static_cast<std::size_t>(index - 1)], plan.offset * 0.5f);
  }
  fn.glDisable(gl::BLEND);
  return levels;
}

bool create_shared_resources() noexcept {
  const auto& fn = gl::get_gl_functions();
  static constexpr float kTriangle[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
  fn.glGenVertexArrays(1, &g_resources.vertexArray);
  fn.glGenBuffers(1, &g_resources.vertexBuffer);
  fn.glBindVertexArray(g_resources.vertexArray);
  fn.glBindBuffer(gl::ARRAY_BUFFER, g_resources.vertexBuffer);
  fn.glBufferData(gl::ARRAY_BUFFER, static_cast<std::ptrdiff_t>(sizeof(kTriangle)), kTriangle,
                  gl::STATIC_DRAW);
  fn.glEnableVertexAttribArray(0);
  fn.glVertexAttribPointer(0, 2, gl::FLOAT, 0, 0, nullptr);

  auto& resources = g_resources;
  resources.composite.id = build_program(kCompositeSource);
  resources.blurDown.id = build_program(kBlurDownSource);
  resources.blurUp.id = build_program(kBlurUpSource);
  resources.bright.id = build_program(kBrightSource);
  if (!resources.composite.id || !resources.blurDown.id || !resources.blurUp.id ||
      !resources.bright.id) {
    return false;
  }
  resources.composite.scale = fn.glGetUniformLocation(resources.composite.id, "uScale");
  resources.composite.dither = fn.glGetUniformLocation(resources.composite.id, "uDither");
  resources.blurDown.step = fn.glGetUniformLocation(resources.blurDown.id, "uStep");
  resources.blurUp.step = fn.glGetUniformLocation(resources.blurUp.id, "uStep");
  resources.bright.threshold = fn.glGetUniformLocation(resources.bright.id, "uThreshold");
  return true;
}

bool create_sized_resources(game::Extent viewport) noexcept {
  auto& resources = g_resources;
  const auto half = game::half_extent(viewport);
  if (!create_target(resources.fogCapture, viewport, gl::RGBA8, gl::UNSIGNED_BYTE)) return false;
  if (!create_chain(resources.fogChain, viewport)) return false;
  if (!create_target(resources.bloomScene, half, gl::RGBA16F, gl::HALF_FLOAT)) return false;
  if (!create_target(resources.bloomBright, half, gl::RGBA16F, gl::HALF_FLOAT)) return false;
  if (!create_chain(resources.bloomChain, half)) return false;
  resources.viewport = viewport;
  return true;
}

// Called inside the guards' scope: creation changes bindings.
bool ensure_resources(game::Extent viewport) noexcept {
  const auto context = gl::current_context();
  // Creation binds textures; keep that on unit 0, which the caller's guard restores.
  gl::get_gl_functions().glActiveTexture(gl::TEXTURE0);
  if (context != g_resources.context) {
    // The old context is gone and took its objects with it.
    g_resources = {};
    g_resources.context = context;
    gl::discard_errors();
    if (!create_shared_resources() || !gl::check_error("world post shared resources")) {
      return false;
    }
  }
  if (!(viewport == g_resources.viewport)) {
    gl::discard_errors();
    if (!create_sized_resources(viewport) || !gl::check_error("world post targets")) return false;
    g_logNextFrame.store(true, std::memory_order_relaxed);
  }
  return true;
}

// Adds a glow of the bright parts of the world image to the engine's
// framebuffer. Runs before the fog so unexplored areas stay dark, and before
// the UI is drawn so the UI cannot glow. Requires set_pass_state().
void draw_bloom() noexcept {
  const auto& fn = gl::get_gl_functions();
  auto& resources = g_resources;
  const auto half = resources.bloomScene.extent;
  const int planned = game::bloom_levels(half);
  if (planned <= 0 || !(g_settings.bloomStrength > 0.0f)) return;

  fn.glBindFramebuffer(gl::READ_FRAMEBUFFER, static_cast<unsigned>(g_engineDrawFramebuffer));
  fn.glBindFramebuffer(gl::DRAW_FRAMEBUFFER, resources.bloomScene.framebuffer);
  fn.glBlitFramebuffer(0, 0, resources.viewport.width, resources.viewport.height, 0, 0,
                       half.width, half.height, gl::COLOR_BUFFER_BIT, gl::LINEAR);

  fn.glBindFramebuffer(gl::FRAMEBUFFER, resources.bloomBright.framebuffer);
  fn.glViewport(0, 0, half.width, half.height);
  fn.glUseProgram(resources.bright.id);
  fn.glUniform1f(resources.bright.threshold, g_settings.bloomThreshold);
  fn.glBindTexture(gl::TEXTURE_2D, resources.bloomScene.texture);
  fn.glDrawArrays(gl::TRIANGLES, 0, 3);

  const int levels = run_blur(resources.bloomBright, resources.bloomChain, {planned, 1.0f}, true);

  bind_engine_framebuffer();
  fn.glViewport(0, 0, resources.viewport.width, resources.viewport.height);
  if (levels > 0) {
    fn.glEnable(gl::BLEND);
    fn.glBlendFunc(gl::ONE, gl::ONE);
    // Each level adds its share on the way up; normalise so strength means
    // the same thing at every resolution.
    draw_composite(resources.bloomChain.levels[0].texture,
                   g_settings.bloomStrength / static_cast<float>(levels), 0.0f);
    fn.glDisable(gl::BLEND);
  }

  if (!resources.bloomDrawnOnce) {
    resources.bloomDrawnOnce = true;
    if (!gl::check_error("world post bloom")) {
      fail("the bloom pass raised a GL error");
    } else {
      try {
        LOG_INFO("World post: first bloom drawn ({} levels)", levels);
      } catch (...) {
      }
    }
  }
}
}  // namespace

void world_post_configure(const core::EngineConfig& cfg) noexcept {
  g_settings = {cfg.softFogOfWar, cfg.bloom, cfg.softFogRadius, cfg.bloomThreshold,
                cfg.bloomStrength};
}

bool world_post_active() noexcept {
  return !g_failed && (g_settings.softFog || g_settings.bloom);
}

void world_post_poll_hotkeys() noexcept {
  static bool fogKeyWasDown = false;
  static bool bloomKeyWasDown = false;
  const bool fogKeyDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
  const bool bloomKeyDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
  const bool fogPressed = fogKeyDown && !fogKeyWasDown;
  const bool bloomPressed = bloomKeyDown && !bloomKeyWasDown;
  fogKeyWasDown = fogKeyDown;
  bloomKeyWasDown = bloomKeyDown;
  if (!fogPressed && !bloomPressed) return;
  if (fogPressed) g_settings.softFog = !g_settings.softFog;
  if (bloomPressed) g_settings.bloom = !g_settings.bloom;
  try {
    LOG_INFO("Hotkey: soft fog of war={}, bloom={}", g_settings.softFog, g_settings.bloom);
  } catch (...) {
  }
}

bool world_post_before_fog(float viewWorldWidth) noexcept {
  const auto& fn = gl::get_gl_functions();
  if (!fn.postProcessAvailable) {
    fail("the GL context lacks framebuffer or vertex-array support");
    return false;
  }
  fn.glGetIntegerv(gl::DRAW_FRAMEBUFFER_BINDING, &g_engineDrawFramebuffer);
  fn.glGetIntegerv(gl::READ_FRAMEBUFFER_BINDING, &g_engineReadFramebuffer);

  bool capturing = false;
  {
    core::GlStateGuard textures({0});
    core::GlPassGuard pass;
    const int* viewport = pass.viewport();
    if (viewport[0] != 0 || viewport[1] != 0 || viewport[2] <= 0 || viewport[3] <= 0) {
      fail("the engine viewport is not a full-window rectangle at the origin");
    } else if (!ensure_resources({viewport[2], viewport[3]})) {
      fail("GL resources could not be created");
    } else {
      g_pixelsPerWorldPixel =
          game::pixels_per_world_pixel(static_cast<float>(viewport[2]), viewWorldWidth);
      set_pass_state();
      if (g_settings.bloom) {
        draw_bloom();
        set_pass_state();
      }
      if (g_settings.softFog && !g_failed) {
        // White = "nothing hidden". The engine's alpha-blended black fog then
        // leaves, per pixel, the fraction of the scene that stays visible.
        fn.glBindFramebuffer(gl::FRAMEBUFFER, g_resources.fogCapture.framebuffer);
        fn.glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        fn.glClear(gl::COLOR_BUFFER_BIT);
        capturing = true;
      }
      if (g_logNextFrame.exchange(false, std::memory_order_relaxed)) {
        const auto plan = game::blur_plan_for_radius(
            g_settings.fogRadius * g_pixelsPerWorldPixel, g_resources.viewport);
        try {
          LOG_INFO(
              "World post: softFog={} (radius {} world px, blur levels={} offset={:.2f}), "
              "bloom={}, viewport {}x{}, {:.3f} px per world px, engine framebuffer draw={} "
              "read={}",
              g_settings.softFog, g_settings.fogRadius, plan.levels, plan.offset,
              g_settings.bloom, viewport[2], viewport[3], g_pixelsPerWorldPixel,
              g_engineDrawFramebuffer, g_engineReadFramebuffer);
        } catch (...) {
        }
      }
    }
    bind_engine_framebuffer();
  }
  // The guards have restored the engine's state; hand it our target for the
  // fog draw only. Framebuffer bindings are not part of the engine's cache.
  if (capturing) fn.glBindFramebuffer(gl::FRAMEBUFFER, g_resources.fogCapture.framebuffer);
  return capturing;
}

void world_post_after_fog() noexcept {
  const auto& fn = gl::get_gl_functions();
  auto& resources = g_resources;
  core::GlStateGuard textures({0});
  core::GlPassGuard pass;
  set_pass_state();
  const auto plan =
      game::blur_plan_for_radius(g_settings.fogRadius * g_pixelsPerWorldPixel, resources.viewport);
  const bool blurred = run_blur(resources.fogCapture, resources.fogChain, plan, false) > 0;
  bind_engine_framebuffer();
  fn.glViewport(0, 0, resources.viewport.width, resources.viewport.height);
  // framebuffer = framebuffer * visible fraction
  fn.glEnable(gl::BLEND);
  fn.glBlendFunc(gl::DST_COLOR, gl::ZERO);
  draw_composite(blurred ? resources.fogChain.levels[0].texture : resources.fogCapture.texture,
                 1.0f, blurred ? 1.0f : 0.0f);
  if (!resources.fogDrawnOnce) {
    resources.fogDrawnOnce = true;
    if (!gl::check_error("world post fog composite")) {
      fail("the fog composite raised a GL error");
    } else {
      try {
        LOG_INFO("World post: first fog composite drawn (blurred={})", blurred);
      } catch (...) {
      }
    }
  }
}

void world_post_on_area_load() noexcept { g_logNextFrame.store(true, std::memory_order_relaxed); }

void world_post_forget() noexcept { g_resources = {}; }
}  // namespace iee::features
