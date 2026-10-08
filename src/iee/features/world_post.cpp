#include "iee/features/world_post.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
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
  int curve{-1};
};

// Blends the new fog image with the one shown last frame, looked up at the
// same place on the map.
struct FogTemporalProgram {
  unsigned id{};
  int origin{-1};
  int size{-1};
  int previousOrigin{-1};
  int previousSize{-1};
  int blend{-1};
};

struct SharpenProgram {
  unsigned id{};
  int sharpness{-1};
};

// One sprite atlas's upscaled copies: the edge-adaptive 2x image and the
// sharpened result the engine then draws from.
struct AtlasTargets {
  Target upscaled{};
  Target sharpened{};
};

struct ShimmerProgram {
  unsigned id{};
  int worldOrigin{-1};
  int worldSize{-1};
  int strength{-1};
  int time{-1};
};

struct FogProgram {
  unsigned id{};
  int worldOrigin{-1};
  int worldSize{-1};
  int drift{-1};
  int time{-1};
};

struct BlurProgram {
  unsigned id{};
  int step{-1};
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
  float bloomStrength{};
  float lightSpill{};
  float fogDrift{};
  float fogSmoothing{};
  float heatShimmer{};
  bool spriteUpscale{};
  float spriteSharpness{};
};

// Everything that lives and dies with one GL context.
struct Resources {
  HGLRC context{};
  game::Extent viewport{};
  unsigned vertexArray{};
  unsigned vertexBuffer{};
  CompositeProgram composite{};
  FogProgram fog{};
  FogTemporalProgram fogTemporal{};
  ShimmerProgram shimmer{};
  unsigned spriteUpscale{};
  SharpenProgram spriteSharpen{};
  std::array<AtlasTargets, 2> atlases{};
  std::array<bool, 2> atlasDrawnOnce{};
  BlurProgram blurDown{};
  BlurProgram blurUp{};
  Target fogCapture{};
  BlurChain fogChain{};
  // The fog as shown last frame and the one being built, swapped each frame.
  std::array<Target, 2> fogHistory{};
  int fogHistoryCurrent{};
  bool fogHistoryValid{};
  WorldView fogHistoryView{};
  std::chrono::steady_clock::time_point fogHistoryTime{};
  // The engine's additive (light-emitting) draws of this frame, on black.
  Target emissive{};
  Target emissiveHalf{};
  BlurChain bloomChain{};
  Target sceneCopy{};  // the world image, read back while it is redrawn rippled
  bool fogDrawnOnce{};
  bool bloomDrawnOnce{};
};

Settings g_settings;
Resources g_resources;
bool g_failed = false;
int g_engineDrawFramebuffer = 0;
int g_engineReadFramebuffer = 0;
float g_pixelsPerWorldPixel = 1.0f;
WorldView g_view{};
std::atomic<bool> g_logNextFrame{true};
std::atomic<bool> g_dropFogHistory{false};
// Frame bookkeeping for the emissive capture. The world pass runs from the
// frame boundary to the fog; additive draws queued later belong to the UI.
unsigned g_frame = 1;
unsigned g_emissiveFrame = 0;
game::WorldPassGate g_worldPass;
// Field counters for the per-area log line: frame boundaries seen and sprite
// atlases upscaled since the previous area frame.
int g_frameTicksSinceArea = 0;
int g_atlasUpscalesSinceArea = 0;
int g_emissiveCommands = 0;
int g_savedDrawFramebuffer = 0;
int g_savedReadFramebuffer = 0;

constexpr const char* kVertexSource = R"glsl(#version 330
layout(location = 0) in vec2 aPosition;
out vec2 vUv;
void main() {
  vUv = aPosition * 0.5 + 0.5;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)glsl";

// Smooth 2D value noise in [0, 1], shared by the shaders that animate. It is
// handed to the compiler as a second source string after the shader that
// declares `float noise(vec2 p);`.
constexpr const char* kValueNoiseSource = R"glsl(
float hash(vec2 p) {
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float noise(vec2 p) {
  vec2 cell = floor(p);
  vec2 f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(cell), hash(cell + vec2(1.0, 0.0)), f.x),
             mix(hash(cell + vec2(0.0, 1.0)), hash(cell + vec2(1.0, 1.0)), f.x), f.y);
}
)glsl";

// Writes the texture scaled by uScale; the caller's blend mode decides how it
// lands. uDither adds +-0.5/255 of ordered noise to hide banding in long dark
// gradients. With uCurve set the value is treated as light and mapped through
// 1 - exp(-x): faint light far from a source still registers, and light next
// to the source saturates instead of clipping.
constexpr const char* kCompositeSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform float uScale;
uniform float uDither;
uniform float uCurve;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec3 color = texture(uTexture, vUv).rgb * uScale;
  if (uCurve > 0.5) color = vec3(1.0) - exp(-color);
  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  fragColor = vec4(color + uDither * (noise - 0.5) / 255.0, 1.0);
}
)glsl";

// The engine recomputes visibility on its logic tick, so the fog would step
// while everything else moves smoothly. This moves the shown fog a fraction
// (uBlend) of the way to the new one each frame. The previous image is read
// at the same world position, so scrolling and zooming do not smear it;
// where that position was off screen the new image is used as is.
constexpr const char* kFogTemporalSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform sampler2D uHistory;
uniform vec2 uOrigin;
uniform vec2 uSize;
uniform vec2 uPreviousOrigin;
uniform vec2 uPreviousSize;
uniform float uBlend;
in vec2 vUv;
out vec4 fragColor;
void main() {
  vec3 current = texture(uTexture, vUv).rgb;
  vec2 world = uOrigin + vec2(vUv.x, 1.0 - vUv.y) * uSize;
  vec2 previous = (world - uPreviousOrigin) / uPreviousSize;
  previous.y = 1.0 - previous.y;
  float inside = step(0.0, previous.x) * step(previous.x, 1.0) * step(0.0, previous.y) *
                 step(previous.y, 1.0);
  vec3 history = texture(uHistory, previous).rgb;
  fragColor = vec4(mix(current, history, inside * (1.0 - uBlend)), 1.0);
}
)glsl";

// Multiplies the scene by the blurred fog capture (white = fully visible).
// The lookup is pushed around by slow noise anchored to the world, so the
// soft edge drifts like mist; where the capture is flat the push changes
// nothing, so explored and unexplored areas keep the engine's exact shade.
constexpr const char* kFogSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform vec2 uWorldOrigin;
uniform vec2 uWorldSize;
uniform float uDrift;
uniform float uTime;
in vec2 vUv;
out vec4 fragColor;
float noise(vec2 p);
void main() {
  vec2 uv = vUv;
  if (uDrift > 0.0 && uWorldSize.x > 0.0 && uWorldSize.y > 0.0) {
    vec2 world = uWorldOrigin + vec2(vUv.x, 1.0 - vUv.y) * uWorldSize;
    vec2 p = world / 96.0;
    vec2 push = vec2(noise(p + vec2(uTime * 0.07, uTime * 0.05)) +
                         0.5 * noise(p * 2.3 - vec2(uTime * 0.11, 0.0)),
                     noise(p + vec2(31.7, 17.3) - vec2(uTime * 0.06, uTime * 0.08)) +
                         0.5 * noise(p * 2.3 + vec2(5.1, uTime * 0.09))) / 1.5 - 0.5;
    uv += push * 2.0 * uDrift / uWorldSize;
  }
  vec3 visible = texture(uTexture, uv).rgb;
  float grain = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  fragColor = vec4(visible + (grain - 0.5) / 255.0, 1.0);
}
)glsl";

// FSR1 EASU (edge-adaptive spatial upsampling), after AMD's FidelityFX
// reference (MIT): a 12-tap kernel whose shape follows the local edge
// direction, so diagonal outlines come out smooth instead of stair-stepped.
// Runs on the sprite atlas at exactly 2x, texel to texel: output pixel
// gl_FragCoord maps to input position gl_FragCoord / 2. Alpha is filtered
// with the colour and counts towards the edge detection, because a sprite's
// outline is mostly an alpha edge.
constexpr const char* kSpriteUpscaleSource = R"glsl(#version 330
uniform sampler2D uTexture;
out vec4 fragColor;
ivec2 size;
vec4 tap(ivec2 p) {
  return texelFetch(uTexture, clamp(p, ivec2(0), size - 1), 0);
}
float luma(vec4 c) {
  return c.r * 0.5 + c.g + c.b * 0.5 + c.a;
}
void accumulate(inout vec4 colour, inout float weight, vec2 offset, vec2 dir, vec2 len, float lobe,
                float clip, vec4 c) {
  vec2 v = vec2(offset.x * dir.x + offset.y * dir.y, offset.x * -dir.y + offset.y * dir.x) * len;
  float d2 = min(dot(v, v), clip);
  float wB = 0.4 * d2 - 1.0;
  float wA = lobe * d2 - 1.0;
  wB *= wB;
  wA *= wA;
  wB = 1.5625 * wB - 0.5625;
  float w = wB * wA;
  colour += c * w;
  weight += w;
}
void edge(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD,
          float lE) {
  float lenX = max(abs(lD - lC), abs(lC - lB));
  float dirX = lD - lB;
  dir.x += dirX * w;
  lenX = clamp(abs(dirX) / max(lenX, 1e-5), 0.0, 1.0);
  len += lenX * lenX * w;
  float lenY = max(abs(lE - lC), abs(lC - lA));
  float dirY = lE - lA;
  dir.y += dirY * w;
  lenY = clamp(abs(dirY) / max(lenY, 1e-5), 0.0, 1.0);
  len += lenY * lenY * w;
}
void main() {
  size = textureSize(uTexture, 0);
  vec2 pp = gl_FragCoord.xy * 0.5 - 0.5;
  vec2 fp = floor(pp);
  pp -= fp;
  ivec2 ip = ivec2(fp);
  //    b c
  //  e f g h
  //  i j k l
  //    n o
  vec4 b = tap(ip + ivec2(0, -1));
  vec4 c = tap(ip + ivec2(1, -1));
  vec4 e = tap(ip + ivec2(-1, 0));
  vec4 f = tap(ip);
  vec4 g = tap(ip + ivec2(1, 0));
  vec4 h = tap(ip + ivec2(2, 0));
  vec4 i = tap(ip + ivec2(-1, 1));
  vec4 j = tap(ip + ivec2(0, 1));
  vec4 k = tap(ip + ivec2(1, 1));
  vec4 l = tap(ip + ivec2(2, 1));
  vec4 n = tap(ip + ivec2(0, 2));
  vec4 o = tap(ip + ivec2(1, 2));
  float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
  float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);
  vec2 dir = vec2(0.0);
  float len = 0.0;
  edge(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
  edge(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
  edge(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
  edge(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
  float dirR = dot(dir, dir);
  bool flat_ = dirR < 1.0 / 32768.0;
  dir = flat_ ? vec2(1.0, 0.0) : dir * inversesqrt(dirR);
  len = len * 0.5;
  len *= len;
  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
  float lobe = 0.5 + (0.21 - 0.5) * len;
  float clip = 1.0 / lobe;
  vec4 lowest = min(min(f, g), min(j, k));
  vec4 highest = max(max(f, g), max(j, k));
  vec4 colour = vec4(0.0);
  float weight = 0.0;
  accumulate(colour, weight, vec2(0.0, -1.0) - pp, dir, len2, lobe, clip, b);
  accumulate(colour, weight, vec2(1.0, -1.0) - pp, dir, len2, lobe, clip, c);
  accumulate(colour, weight, vec2(-1.0, 1.0) - pp, dir, len2, lobe, clip, i);
  accumulate(colour, weight, vec2(0.0, 1.0) - pp, dir, len2, lobe, clip, j);
  accumulate(colour, weight, vec2(0.0, 0.0) - pp, dir, len2, lobe, clip, f);
  accumulate(colour, weight, vec2(-1.0, 0.0) - pp, dir, len2, lobe, clip, e);
  accumulate(colour, weight, vec2(1.0, 1.0) - pp, dir, len2, lobe, clip, k);
  accumulate(colour, weight, vec2(2.0, 1.0) - pp, dir, len2, lobe, clip, l);
  accumulate(colour, weight, vec2(2.0, 0.0) - pp, dir, len2, lobe, clip, h);
  accumulate(colour, weight, vec2(1.0, 0.0) - pp, dir, len2, lobe, clip, g);
  accumulate(colour, weight, vec2(1.0, 2.0) - pp, dir, len2, lobe, clip, o);
  accumulate(colour, weight, vec2(0.0, 2.0) - pp, dir, len2, lobe, clip, n);
  fragColor = clamp(colour / weight, lowest, highest);
}
)glsl";

// FSR1 RCAS (robust contrast-adaptive sharpening), same source: sharpens as
// far as it can without pushing any channel outside its neighbourhood's range.
constexpr const char* kSpriteSharpenSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform float uSharpness;
out vec4 fragColor;
void main() {
  ivec2 size = textureSize(uTexture, 0);
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 b = texelFetch(uTexture, clamp(p + ivec2(0, -1), ivec2(0), size - 1), 0);
  vec4 d = texelFetch(uTexture, clamp(p + ivec2(-1, 0), ivec2(0), size - 1), 0);
  vec4 e = texelFetch(uTexture, p, 0);
  vec4 f = texelFetch(uTexture, clamp(p + ivec2(1, 0), ivec2(0), size - 1), 0);
  vec4 h = texelFetch(uTexture, clamp(p + ivec2(0, 1), ivec2(0), size - 1), 0);
  vec4 lowest = min(min(b, d), min(f, h));
  vec4 highest = max(max(b, d), max(f, h));
  vec4 hitMin = min(lowest, e) / max(4.0 * highest, vec4(1e-4));
  vec4 hitMax = (vec4(1.0) - max(highest, e)) / min(4.0 * lowest - 4.0, vec4(-1e-4));
  vec4 lobes = max(-hitMin, hitMax);
  float lobe = max(-0.1875, min(max(max(lobes.r, lobes.g), max(lobes.b, lobes.a)), 0.0)) *
               uSharpness;
  fragColor = clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 0.0, 1.0);
}
)glsl";

// Heat haze. Redraws the scene with its lookup rippled where the blurred
// emissive image (uMask) is warm, i.e. redder than it is blue, so fires and
// torches shimmer and cold or white magic does not. The mask is also read
// from below each pixel, because hot air rises above its source. The ripple
// is anchored to the world and flows upward.
constexpr const char* kShimmerSource = R"glsl(#version 330
uniform sampler2D uTexture;
uniform sampler2D uMask;
uniform vec2 uWorldOrigin;
uniform vec2 uWorldSize;
uniform float uStrength;
uniform float uTime;
in vec2 vUv;
out vec4 fragColor;
float noise(vec2 p);
float warmth(vec2 uv) {
  vec3 light = texture(uMask, uv).rgb;
  return light.r - light.b;
}
void main() {
  vec2 rise = vec2(0.0, 26.0 / uWorldSize.y);
  float heat = max(warmth(vUv), max(0.8 * warmth(vUv - rise), 0.5 * warmth(vUv - 2.0 * rise)));
  heat = clamp(heat * 4.0, 0.0, 1.0);
  vec2 world = uWorldOrigin + vec2(vUv.x, 1.0 - vUv.y) * uWorldSize;
  vec2 p = world / 9.0;
  vec2 ripple = vec2(noise(p + vec2(0.0, uTime * 2.2)), noise(p * 1.3 + vec2(7.0, uTime * 2.8))) - 0.5;
  vec2 offset = ripple * 2.0 * uStrength * heat / uWorldSize;
  fragColor = vec4(texture(uTexture, vUv + offset).rgb, 1.0);
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

void fail(const char* reason) noexcept {
  g_failed = true;
  try {
    LOG_WARN("Soft fog of war and bloom disabled for this session: {}", reason);
  } catch (...) {
  }
}

// `appended` is an optional second source string (shared helper functions).
unsigned compile_shader(unsigned type, const char* source,
                        const char* appended = nullptr) noexcept {
  const auto& fn = gl::get_gl_functions();
  const unsigned shader = fn.glCreateShader(type);
  if (!shader) return 0;
  const char* sources[2] = {source, appended};
  fn.glShaderSource(shader, appended ? 2 : 1, sources, nullptr);
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

unsigned build_program(const char* fragmentSource, const char* appended = nullptr) noexcept {
  const auto& fn = gl::get_gl_functions();
  const unsigned vertex = compile_shader(gl::VERTEX_SHADER, kVertexSource);
  const unsigned fragment = compile_shader(gl::FRAGMENT_SHADER, fragmentSource, appended);
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

// Wrapped hourly so the float keeps sub-frame precision.
float animation_seconds() noexcept {
  return static_cast<float>(GetTickCount64() % 3600000ULL) / 1000.0f;
}

void draw_composite(unsigned texture, float scale, float dither, bool lightCurve = false) noexcept {
  const auto& fn = gl::get_gl_functions();
  fn.glUseProgram(g_resources.composite.id);
  fn.glUniform1f(g_resources.composite.scale, scale);
  fn.glUniform1f(g_resources.composite.dither, dither);
  fn.glUniform1f(g_resources.composite.curve, lightCurve ? 1.0f : 0.0f);
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
  resources.fog.id = build_program(kFogSource, kValueNoiseSource);
  resources.fogTemporal.id = build_program(kFogTemporalSource);
  if (!resources.composite.id || !resources.blurDown.id || !resources.blurUp.id ||
      !resources.fog.id || !resources.fogTemporal.id) {
    return false;
  }
  resources.shimmer.id = build_program(kShimmerSource, kValueNoiseSource);
  resources.spriteUpscale = build_program(kSpriteUpscaleSource);
  resources.spriteSharpen.id = build_program(kSpriteSharpenSource);
  if (!resources.shimmer.id || !resources.spriteUpscale || !resources.spriteSharpen.id) {
    return false;
  }
  resources.spriteSharpen.sharpness =
      fn.glGetUniformLocation(resources.spriteSharpen.id, "uSharpness");
  auto& shimmer = resources.shimmer;
  shimmer.worldOrigin = fn.glGetUniformLocation(shimmer.id, "uWorldOrigin");
  shimmer.worldSize = fn.glGetUniformLocation(shimmer.id, "uWorldSize");
  shimmer.strength = fn.glGetUniformLocation(shimmer.id, "uStrength");
  shimmer.time = fn.glGetUniformLocation(shimmer.id, "uTime");
  fn.glUseProgram(shimmer.id);
  fn.glUniform1i(fn.glGetUniformLocation(shimmer.id, "uMask"), 1);
  auto& temporal = resources.fogTemporal;
  temporal.origin = fn.glGetUniformLocation(temporal.id, "uOrigin");
  temporal.size = fn.glGetUniformLocation(temporal.id, "uSize");
  temporal.previousOrigin = fn.glGetUniformLocation(temporal.id, "uPreviousOrigin");
  temporal.previousSize = fn.glGetUniformLocation(temporal.id, "uPreviousSize");
  temporal.blend = fn.glGetUniformLocation(temporal.id, "uBlend");
  fn.glUseProgram(temporal.id);
  fn.glUniform1i(fn.glGetUniformLocation(temporal.id, "uHistory"), 1);
  resources.composite.curve = fn.glGetUniformLocation(resources.composite.id, "uCurve");
  resources.fog.worldOrigin = fn.glGetUniformLocation(resources.fog.id, "uWorldOrigin");
  resources.fog.worldSize = fn.glGetUniformLocation(resources.fog.id, "uWorldSize");
  resources.fog.drift = fn.glGetUniformLocation(resources.fog.id, "uDrift");
  resources.fog.time = fn.glGetUniformLocation(resources.fog.id, "uTime");
  resources.composite.scale = fn.glGetUniformLocation(resources.composite.id, "uScale");
  resources.composite.dither = fn.glGetUniformLocation(resources.composite.id, "uDither");
  resources.blurDown.step = fn.glGetUniformLocation(resources.blurDown.id, "uStep");
  resources.blurUp.step = fn.glGetUniformLocation(resources.blurUp.id, "uStep");
  return true;
}

bool create_sized_resources(game::Extent viewport) noexcept {
  auto& resources = g_resources;
  const auto half = game::half_extent(viewport);
  if (!create_target(resources.fogCapture, viewport, gl::RGBA8, gl::UNSIGNED_BYTE)) return false;
  if (!create_chain(resources.fogChain, viewport)) return false;
  for (auto& history : resources.fogHistory) {
    if (!create_target(history, half, gl::RGBA16F, gl::HALF_FLOAT)) return false;
  }
  resources.fogHistoryValid = false;
  if (!create_target(resources.emissive, viewport, gl::RGBA8, gl::UNSIGNED_BYTE)) return false;
  if (!create_target(resources.emissiveHalf, half, gl::RGBA16F, gl::HALF_FLOAT)) return false;
  if (!create_target(resources.sceneCopy, viewport, gl::RGBA8, gl::UNSIGNED_BYTE)) return false;
  if (!create_chain(resources.bloomChain, half)) return false;
  resources.viewport = viewport;
  return true;
}

// Programs and the triangle, per GL context. Called inside the guards' scope:
// creation changes bindings.
bool ensure_shared_resources() noexcept {
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
  return true;
}

// Called inside the guards' scope: creation changes bindings.
bool ensure_resources(game::Extent viewport) noexcept {
  if (!ensure_shared_resources()) return false;
  if (!(viewport == g_resources.viewport)) {
    gl::discard_errors();
    if (!create_sized_resources(viewport) || !gl::check_error("world post targets")) return false;
    g_logNextFrame.store(true, std::memory_order_relaxed);
  }
  return true;
}

// Glow and light spill from the frame's emissive image. Runs before the fog
// so unexplored areas stay dark, and before the UI is drawn so the UI cannot
// glow. Requires set_pass_state().
void draw_bloom() noexcept {
  const auto& fn = gl::get_gl_functions();
  auto& resources = g_resources;
  if (g_emissiveFrame != g_frame) return;  // nothing emitted light this frame
  const auto half = resources.emissiveHalf.extent;
  const int planned = game::bloom_levels(half);
  if (planned <= 0) return;

  fn.glBindFramebuffer(gl::READ_FRAMEBUFFER, resources.emissive.framebuffer);
  fn.glBindFramebuffer(gl::DRAW_FRAMEBUFFER, resources.emissiveHalf.framebuffer);
  fn.glBlitFramebuffer(0, 0, resources.viewport.width, resources.viewport.height, 0, 0,
                       half.width, half.height, gl::COLOR_BUFFER_BIT, gl::LINEAR);
  const int levels =
      run_blur(resources.emissiveHalf, resources.bloomChain, {planned, 1.0f}, true);

  if (levels > 0 && g_settings.heatShimmer > 0.0f) {
    // Before the glow is added, so the haze bends the scene and not the halo.
    fn.glBindFramebuffer(gl::READ_FRAMEBUFFER, static_cast<unsigned>(g_engineDrawFramebuffer));
    fn.glBindFramebuffer(gl::DRAW_FRAMEBUFFER, resources.sceneCopy.framebuffer);
    fn.glBlitFramebuffer(0, 0, resources.viewport.width, resources.viewport.height, 0, 0,
                         resources.viewport.width, resources.viewport.height,
                         gl::COLOR_BUFFER_BIT, gl::NEAREST);
    bind_engine_framebuffer();
    fn.glViewport(0, 0, resources.viewport.width, resources.viewport.height);
    // Without the view transform, fall back to screen pixels as "world".
    const bool viewKnown = g_view.width > 0.0f && g_view.height > 0.0f;
    const auto& shimmer = resources.shimmer;
    const int mask = levels > 1 ? 1 : 0;
    fn.glUseProgram(shimmer.id);
    fn.glUniform2f(shimmer.worldOrigin, viewKnown ? g_view.scrollX : 0.0f,
                   viewKnown ? g_view.scrollY : 0.0f);
    fn.glUniform2f(shimmer.worldSize,
                   viewKnown ? g_view.width : static_cast<float>(resources.viewport.width),
                   viewKnown ? g_view.height : static_cast<float>(resources.viewport.height));
    fn.glUniform1f(shimmer.strength, g_settings.heatShimmer);
    fn.glUniform1f(shimmer.time, animation_seconds());
    fn.glActiveTexture(gl::TEXTURE0 + 1);
    fn.glBindTexture(gl::TEXTURE_2D,
                     resources.bloomChain.levels[static_cast<std::size_t>(mask)].texture);
    fn.glActiveTexture(gl::TEXTURE0);
    fn.glBindTexture(gl::TEXTURE_2D, resources.sceneCopy.texture);
    fn.glDrawArrays(gl::TRIANGLES, 0, 3);
  }

  bind_engine_framebuffer();
  fn.glViewport(0, 0, resources.viewport.width, resources.viewport.height);
  if (levels > 0) {
    // Each level adds its share on the way up; normalise so the strengths
    // mean the same thing at every resolution.
    const float perLevel = 1.0f / static_cast<float>(levels);
    const unsigned glow = resources.bloomChain.levels[0].texture;
    fn.glEnable(gl::BLEND);
    if (g_settings.lightSpill > 0.0f) {
      // framebuffer += framebuffer * light: surfaces near a light source get
      // brighter and take its colour; black stays black. The light comes
      // from a deep level of the chain, i.e. only the wide blurs, so its
      // reach does not depend on how sharp the source sprite is.
      const int wide = levels > 3 ? 2 : (levels > 1 ? 1 : 0);
      fn.glBlendFunc(gl::DST_COLOR, gl::ONE);
      draw_composite(resources.bloomChain.levels[static_cast<std::size_t>(wide)].texture,
                     g_settings.lightSpill * 3.0f * perLevel, 0.0f, true);
    }
    if (g_settings.bloomStrength > 0.0f) {
      fn.glBlendFunc(gl::ONE, gl::ONE);
      draw_composite(glow, g_settings.bloomStrength * perLevel, 0.0f);
    }
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
  g_settings = {cfg.softFogOfWar, cfg.bloom, cfg.softFogRadius, cfg.bloomStrength,
                cfg.lightSpill, cfg.softFogDrift, cfg.softFogSmoothing, cfg.heatShimmer,
                cfg.spriteUpscale, cfg.spriteSharpness};
}

bool world_post_active() noexcept {
  // Sprite upscaling needs the world flushed before the UI is queued, which
  // is what the fog detour does when this is true.
  return !g_failed && (g_settings.softFog || g_settings.bloom || g_settings.spriteUpscale);
}

void world_post_poll_hotkeys() noexcept {
  static bool fogKeyWasDown = false;
  static bool bloomKeyWasDown = false;
  static bool spriteKeyWasDown = false;
  const bool fogKeyDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
  const bool bloomKeyDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
  const bool spriteKeyDown = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
  const bool fogPressed = fogKeyDown && !fogKeyWasDown;
  const bool bloomPressed = bloomKeyDown && !bloomKeyWasDown;
  const bool spritePressed = spriteKeyDown && !spriteKeyWasDown;
  fogKeyWasDown = fogKeyDown;
  bloomKeyWasDown = bloomKeyDown;
  spriteKeyWasDown = spriteKeyDown;
  if (!fogPressed && !bloomPressed && !spritePressed) return;
  if (fogPressed) g_settings.softFog = !g_settings.softFog;
  if (bloomPressed) g_settings.bloom = !g_settings.bloom;
  if (spritePressed) g_settings.spriteUpscale = !g_settings.spriteUpscale;
  try {
    LOG_INFO("Hotkey: soft fog of war={}, bloom={}, sprite upscale={}", g_settings.softFog,
             g_settings.bloom, g_settings.spriteUpscale);
  } catch (...) {
  }
}

void world_post_on_frame() noexcept {
  ++g_frame;
  g_worldPass.on_frame();
  ++g_frameTicksSinceArea;
}

bool world_post_wants_atlas_upscale() noexcept {
  return g_worldPass.open() && g_settings.spriteUpscale && !g_failed;
}

unsigned world_post_upscale_atlas(int slot, unsigned sourceTexture, int width, int height,
                                  int rows) noexcept {
  const auto& fn = gl::get_gl_functions();
  if (!fn.postProcessAvailable || slot < 0 || slot >= 2 || !sourceTexture || width <= 0 ||
      height <= 0 || width > 4096 || height > 4096 || rows <= 0) {
    return 0;
  }
  int drawFramebuffer = 0;
  int readFramebuffer = 0;
  fn.glGetIntegerv(gl::DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
  fn.glGetIntegerv(gl::READ_FRAMEBUFFER_BINDING, &readFramebuffer);
  unsigned result = 0;
  {
    core::GlStateGuard textures({0});
    core::GlPassGuard pass;
    const game::Extent doubled{width * 2, height * 2};
    bool ready = ensure_shared_resources();
    if (ready) {
      auto& atlas = g_resources.atlases[static_cast<std::size_t>(slot)];
      if (!(atlas.sharpened.extent == doubled)) {
        gl::discard_errors();
        ready = create_target(atlas.upscaled, doubled, gl::RGBA8, gl::UNSIGNED_BYTE) &&
                create_target(atlas.sharpened, doubled, gl::RGBA8, gl::UNSIGNED_BYTE) &&
                gl::check_error("sprite atlas targets");
      }
      if (ready) {
        // Both shaders address texels by gl_FragCoord, so a viewport selects
        // which part of the copy is redrawn.
        const auto filter = [&](game::Region region) {
          fn.glViewport(region.x, region.y, region.width, region.height);
          fn.glBindFramebuffer(gl::FRAMEBUFFER, atlas.upscaled.framebuffer);
          fn.glUseProgram(g_resources.spriteUpscale);
          fn.glBindTexture(gl::TEXTURE_2D, sourceTexture);
          fn.glDrawArrays(gl::TRIANGLES, 0, 3);
          fn.glBindFramebuffer(gl::FRAMEBUFFER, atlas.sharpened.framebuffer);
          fn.glUseProgram(g_resources.spriteSharpen.id);
          fn.glUniform1f(g_resources.spriteSharpen.sharpness, g_settings.spriteSharpness);
          fn.glBindTexture(gl::TEXTURE_2D, atlas.upscaled.texture);
          fn.glDrawArrays(gl::TRIANGLES, 0, 3);
        };
        // Only the rows the engine just filled; two rows of margin for the kernel.
        const int outputRows = rows * 2 + 4 < doubled.height ? rows * 2 + 4 : doubled.height;
        set_pass_state();
        filter({0, 0, doubled.width, outputRows});
        // Untextured shapes drawn in this flush sample the last corner of
        // atlas 0, which those rows rarely reach: without it they come out
        // transparent.
        if (slot == 0 && outputRows < doubled.height) filter(game::untextured_corner(doubled));
        result = atlas.sharpened.texture;
        ++g_atlasUpscalesSinceArea;
        auto& drawnOnce = g_resources.atlasDrawnOnce[static_cast<std::size_t>(slot)];
        if (!drawnOnce) {
          drawnOnce = true;
          if (!gl::check_error("sprite atlas upscale")) {
            result = 0;
            ready = false;
          } else {
            try {
              LOG_INFO("World post: sprite atlas {} first upscaled ({}x{} -> {}x{}, {} rows)", slot,
                       width, height, doubled.width, doubled.height, rows);
            } catch (...) {
            }
          }
        }
      }
    }
    fn.glBindFramebuffer(gl::DRAW_FRAMEBUFFER, static_cast<unsigned>(drawFramebuffer));
    fn.glBindFramebuffer(gl::READ_FRAMEBUFFER, static_cast<unsigned>(readFramebuffer));
    if (!ready) fail("the sprite atlas upscale could not run");
  }
  return result;
}

bool world_post_wants_emissive() noexcept {
  return g_worldPass.open() && g_settings.bloom && !g_failed;
}

bool world_post_begin_emissive() noexcept {
  const auto& fn = gl::get_gl_functions();
  if (!fn.postProcessAvailable) return false;
  fn.glGetIntegerv(gl::DRAW_FRAMEBUFFER_BINDING, &g_savedDrawFramebuffer);
  fn.glGetIntegerv(gl::READ_FRAMEBUFFER_BINDING, &g_savedReadFramebuffer);
  int viewport[4]{};
  fn.glGetIntegerv(gl::VIEWPORT, viewport);
  if (viewport[0] != 0 || viewport[1] != 0 || viewport[2] <= 0 || viewport[3] <= 0) return false;
  const game::Extent extent{viewport[2], viewport[3]};
  const bool firstThisFrame = g_emissiveFrame != g_frame;
  if (firstThisFrame || !(extent == g_resources.viewport) ||
      gl::current_context() != g_resources.context) {
    core::GlStateGuard textures({0});
    core::GlPassGuard pass;
    const bool ready = ensure_resources(extent);
    if (ready) {
      fn.glBindFramebuffer(gl::FRAMEBUFFER, g_resources.emissive.framebuffer);
      fn.glDisable(gl::SCISSOR_TEST);
      fn.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      fn.glClear(gl::COLOR_BUFFER_BIT);
      g_emissiveCommands = 0;
    }
    fn.glBindFramebuffer(gl::DRAW_FRAMEBUFFER, static_cast<unsigned>(g_savedDrawFramebuffer));
    fn.glBindFramebuffer(gl::READ_FRAMEBUFFER, static_cast<unsigned>(g_savedReadFramebuffer));
    if (!ready) {
      fail("GL resources could not be created");
      return false;
    }
    g_emissiveFrame = g_frame;
  }
  // State is the engine's again; only the framebuffer differs for the replay.
  fn.glBindFramebuffer(gl::FRAMEBUFFER, g_resources.emissive.framebuffer);
  return true;
}

void world_post_end_emissive(int commands) noexcept {
  const auto& fn = gl::get_gl_functions();
  g_emissiveCommands += commands;
  fn.glBindFramebuffer(gl::DRAW_FRAMEBUFFER, static_cast<unsigned>(g_savedDrawFramebuffer));
  fn.glBindFramebuffer(gl::READ_FRAMEBUFFER, static_cast<unsigned>(g_savedReadFramebuffer));
}

bool world_post_before_fog(const WorldView& view) noexcept {
  g_view = view;
  const auto& fn = gl::get_gl_functions();
  g_worldPass.on_area_drawn();
  if (!fn.postProcessAvailable) {
    fail("the GL context lacks framebuffer or vertex-array support");
    return false;
  }
  fn.glGetIntegerv(gl::DRAW_FRAMEBUFFER_BINDING, &g_engineDrawFramebuffer);
  fn.glGetIntegerv(gl::READ_FRAMEBUFFER_BINDING, &g_engineReadFramebuffer);

  bool capturing = false;
  {
    core::GlStateGuard textures({0, 1});
    core::GlPassGuard pass;
    const int* viewport = pass.viewport();
    if (viewport[0] != 0 || viewport[1] != 0 || viewport[2] <= 0 || viewport[3] <= 0) {
      fail("the engine viewport is not a full-window rectangle at the origin");
    } else if (!ensure_resources({viewport[2], viewport[3]})) {
      fail("GL resources could not be created");
    } else {
      g_pixelsPerWorldPixel =
          game::pixels_per_world_pixel(static_cast<float>(viewport[2]), view.width);
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
              "bloom={} (strength {}, light spill {}, {} additive draws this frame), sprite "
              "upscale={} ({} atlas passes, {} frame boundaries since the last area frame), "
              "viewport {}x{}, {:.3f} px per world px, engine framebuffer draw={} read={}",
              g_settings.softFog, g_settings.fogRadius, plan.levels, plan.offset,
              g_settings.bloom, g_settings.bloomStrength, g_settings.lightSpill,
              g_emissiveFrame == g_frame ? g_emissiveCommands : 0, g_settings.spriteUpscale,
              g_atlasUpscalesSinceArea, g_frameTicksSinceArea, viewport[2], viewport[3],
              g_pixelsPerWorldPixel, g_engineDrawFramebuffer, g_engineReadFramebuffer);
        } catch (...) {
        }
      }
    }
    bind_engine_framebuffer();
  }
  g_frameTicksSinceArea = 0;
  g_atlasUpscalesSinceArea = 0;
  // The guards have restored the engine's state; hand it our target for the
  // fog draw only. Framebuffer bindings are not part of the engine's cache.
  if (capturing) fn.glBindFramebuffer(gl::FRAMEBUFFER, g_resources.fogCapture.framebuffer);
  return capturing;
}

void world_post_after_fog() noexcept {
  const auto& fn = gl::get_gl_functions();
  auto& resources = g_resources;
  core::GlStateGuard textures({0, 1});
  core::GlPassGuard pass;
  set_pass_state();
  const auto plan =
      game::blur_plan_for_radius(g_settings.fogRadius * g_pixelsPerWorldPixel, resources.viewport);
  const bool blurred = run_blur(resources.fogCapture, resources.fogChain, plan, false) > 0;
  unsigned fogTexture = resources.fogChain.levels[0].texture;
  const bool viewKnown = g_view.width > 0.0f && g_view.height > 0.0f;
  if (g_dropFogHistory.exchange(false, std::memory_order_relaxed)) {
    resources.fogHistoryValid = false;
  }
  if (blurred && viewKnown && g_settings.fogSmoothing > 0.0f) {
    const auto now = std::chrono::steady_clock::now();
    const float step = std::chrono::duration<float>(now - resources.fogHistoryTime).count();
    const float blend = resources.fogHistoryValid
                            ? game::temporal_blend(step, g_settings.fogSmoothing)
                            : 1.0f;
    const auto& shown = resources.fogHistory[static_cast<std::size_t>(resources.fogHistoryCurrent)];
    const int next = 1 - resources.fogHistoryCurrent;
    const auto& target = resources.fogHistory[static_cast<std::size_t>(next)];
    const auto& temporal = resources.fogTemporal;
    fn.glBindFramebuffer(gl::FRAMEBUFFER, target.framebuffer);
    fn.glViewport(0, 0, target.extent.width, target.extent.height);
    fn.glUseProgram(temporal.id);
    fn.glUniform2f(temporal.origin, g_view.scrollX, g_view.scrollY);
    fn.glUniform2f(temporal.size, g_view.width, g_view.height);
    fn.glUniform2f(temporal.previousOrigin, resources.fogHistoryView.scrollX,
                   resources.fogHistoryView.scrollY);
    // A zero size would divide by zero; with blend 1 the history is unused.
    fn.glUniform2f(temporal.previousSize,
                   resources.fogHistoryValid ? resources.fogHistoryView.width : 1.0f,
                   resources.fogHistoryValid ? resources.fogHistoryView.height : 1.0f);
    fn.glUniform1f(temporal.blend, blend);
    fn.glActiveTexture(gl::TEXTURE0 + 1);
    fn.glBindTexture(gl::TEXTURE_2D, shown.texture);
    fn.glActiveTexture(gl::TEXTURE0);
    fn.glBindTexture(gl::TEXTURE_2D, fogTexture);
    fn.glDrawArrays(gl::TRIANGLES, 0, 3);
    resources.fogHistoryCurrent = next;
    resources.fogHistoryValid = true;
    resources.fogHistoryView = g_view;
    resources.fogHistoryTime = now;
    fogTexture = target.texture;
  } else {
    resources.fogHistoryValid = false;
  }
  bind_engine_framebuffer();
  fn.glViewport(0, 0, resources.viewport.width, resources.viewport.height);
  // framebuffer = framebuffer * visible fraction
  fn.glEnable(gl::BLEND);
  fn.glBlendFunc(gl::DST_COLOR, gl::ZERO);
  if (blurred) {
    fn.glUseProgram(resources.fog.id);
    fn.glUniform2f(resources.fog.worldOrigin, g_view.scrollX, g_view.scrollY);
    fn.glUniform2f(resources.fog.worldSize, g_view.width, g_view.height);
    fn.glUniform1f(resources.fog.drift, g_settings.fogDrift);
    fn.glUniform1f(resources.fog.time, animation_seconds());
    fn.glBindTexture(gl::TEXTURE_2D, fogTexture);
    fn.glDrawArrays(gl::TRIANGLES, 0, 3);
  } else {
    // Unblurred capture: must reproduce the engine's fog exactly.
    draw_composite(resources.fogCapture.texture, 1.0f, 0.0f);
  }
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

void world_post_on_area_load() noexcept {
  g_logNextFrame.store(true, std::memory_order_relaxed);
  g_dropFogHistory.store(true, std::memory_order_relaxed);
}

void world_post_forget() noexcept { g_resources = {}; }
}  // namespace iee::features
