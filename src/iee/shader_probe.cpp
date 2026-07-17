#include "shader_probe.h"

#include <intrin.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/game/opengl_types.h"
#include "iee/game/renderer.h"
#include "iee/game/shader_override.h"
#include "iee/water_textures.h"
#include "shader_diagnostics.h"
#include "shader_uniform_bridge.h"

namespace iee::probe {
namespace {
constexpr unsigned SHADER_TYPE = 0x8B4F;
constexpr unsigned ATTACHED_SHADERS = 0x8B85;
constexpr unsigned COMPILE_STATUS = 0x8B81;
constexpr unsigned LINK_STATUS = 0x8B82;
constexpr unsigned INFO_LOG_LENGTH = 0x8B84;
constexpr unsigned VERTEX_SHADER = 0x8B31;
constexpr unsigned FRAGMENT_SHADER = 0x8B30;
constexpr unsigned SHADER_SOURCE_LENGTH = 0x8B88;

struct ShaderRecord {
  std::string sourcePreview;
  std::size_t sourceBytes{};
  bool compileLogged{};
  std::string shaderName;
};

struct ProgramRecord {
  bool linkLogged{};
  bool introspected{};
  std::unordered_set<std::uintptr_t> callerLogged;
};

// Hook signatures are aliases of the canonical OpenGL declarations.
using Fn_glShaderSource = game::gl::PFN_glShaderSource;
using Fn_glCompileShader = game::gl::PFN_glCompileShader;
using Fn_glDeleteShader = game::gl::PFN_glDeleteShader;
using Fn_glLinkProgram = game::gl::PFN_glLinkProgram;
using Fn_glUseProgram = game::gl::PFN_glUseProgram;
using Fn_glDeleteProgram = game::gl::PFN_glDeleteProgram;
using Fn_glShaderSourceARB = game::gl::PFN_glShaderSourceARB;
using Fn_glCompileShaderARB = game::gl::PFN_glCompileShaderARB;
using Fn_glLinkProgramARB = game::gl::PFN_glLinkProgramARB;
using Fn_glUseProgramObjectARB = game::gl::PFN_glUseProgramObjectARB;
using Fn_glDeleteObjectARB = game::gl::PFN_glDeleteObjectARB;
using Fn_glBindFramebuffer = game::gl::PFN_glBindFramebuffer;
using Fn_glDeleteTextures = game::gl::PFN_glDeleteTextures;

core::Hook<Fn_glShaderSource> g_glShaderSourceHook;
core::Hook<Fn_glCompileShader> g_glCompileShaderHook;
core::Hook<Fn_glDeleteShader> g_glDeleteShaderHook;
core::Hook<Fn_glLinkProgram> g_glLinkProgramHook;
core::Hook<Fn_glUseProgram> g_glUseProgramHook;
core::Hook<Fn_glDeleteProgram> g_glDeleteProgramHook;
core::Hook<Fn_glShaderSourceARB> g_glShaderSourceARBHook;
core::Hook<Fn_glCompileShaderARB> g_glCompileShaderARBHook;
core::Hook<Fn_glLinkProgramARB> g_glLinkProgramARBHook;
core::Hook<Fn_glUseProgramObjectARB> g_glUseProgramObjectARBHook;
core::Hook<Fn_glDeleteObjectARB> g_glDeleteObjectARBHook;
core::Hook<Fn_glBindFramebuffer> g_glBindFramebufferHook;
core::Hook<Fn_glDeleteTextures> g_glDeleteTexturesHook;

void finish_queued_probe_hooks() noexcept {
  g_glShaderSourceHook.finish_queued_enable();
  g_glCompileShaderHook.finish_queued_enable();
  g_glDeleteShaderHook.finish_queued_enable();
  g_glLinkProgramHook.finish_queued_enable();
  g_glUseProgramHook.finish_queued_enable();
  g_glDeleteProgramHook.finish_queued_enable();
  g_glDeleteTexturesHook.finish_queued_enable();
  g_glShaderSourceARBHook.finish_queued_enable();
  g_glCompileShaderARBHook.finish_queued_enable();
  g_glLinkProgramARBHook.finish_queued_enable();
  g_glUseProgramObjectARBHook.finish_queued_enable();
  g_glDeleteObjectARBHook.finish_queued_enable();
  g_glBindFramebufferHook.finish_queued_enable();
}

bool remove_probe_hooks() noexcept {
  bool removed = true;
  removed = g_glDeleteTexturesHook.remove() && removed;
  removed = g_glBindFramebufferHook.remove() && removed;
  removed = g_glDeleteObjectARBHook.remove() && removed;
  removed = g_glUseProgramObjectARBHook.remove() && removed;
  removed = g_glLinkProgramARBHook.remove() && removed;
  removed = g_glCompileShaderARBHook.remove() && removed;
  removed = g_glShaderSourceARBHook.remove() && removed;
  removed = g_glUseProgramHook.remove() && removed;
  removed = g_glDeleteProgramHook.remove() && removed;
  removed = g_glLinkProgramHook.remove() && removed;
  removed = g_glDeleteShaderHook.remove() && removed;
  removed = g_glCompileShaderHook.remove() && removed;
  removed = g_glShaderSourceHook.remove() && removed;
  return removed;
}

std::mutex g_probeMutex;
std::unordered_map<unsigned, ShaderRecord> g_shaderRecords;
std::unordered_map<unsigned, ProgramRecord> g_programRecords;
std::unordered_map<unsigned, std::shared_ptr<uniforms::Locations>> g_overriddenPrograms;
std::set<std::string> g_dumpedShaders;  // names already written to disk
bool g_shaderProbesInstalled = false;
bool g_waterOverrideActiveLogged = false;
bool g_waterOverrideMissingLogged = false;
bool g_uniformsInitialized = false;
std::atomic<HGLRC> g_programContext{nullptr};
std::atomic<HGLRC> g_hookContext{nullptr};
std::atomic<bool> g_contextRefreshPending{false};
core::EngineConfig g_cfg;
std::filesystem::path g_dumpDir;

// Set by install; consumed by the first frame tick (sweep runs at the
// frame boundary, never mid-draw).
std::atomic<bool> g_sweepPending{false};

bool ensure_program_context() {
  const auto context = game::gl::current_context();
  if (context == g_programContext.load(std::memory_order_acquire)) return false;

  std::lock_guard lock(g_probeMutex);
  if (context == g_programContext.load(std::memory_order_relaxed)) return false;

  // Shader and program names are scoped to a WGL context and may be reused by
  // a replacement context. Never carry classifications or uniform locations
  // across that boundary.
  g_shaderRecords.clear();
  g_programRecords.clear();
  g_overriddenPrograms.clear();
  g_waterOverrideActiveLogged = false;
  g_waterOverrideMissingLogged = false;
  g_programContext.store(context, std::memory_order_release);
  return true;
}

std::string sanitize_preview(std::string text) {
  for (char& ch : text) {
    if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
  }
  constexpr std::size_t kMaxPreviewBytes = 240;
  if (text.size() > kMaxPreviewBytes) {
    text.resize(kMaxPreviewBytes);
    text += "...";
  }
  return text;
}

// Joins ALL chunks into a single string, honouring lengths[i] >= 0.
std::string gather_full_source(int count, const char* const* strings, const int* lengths) {
  if (count <= 0 || !strings) return {};
  std::string combined;
  for (int i = 0; i < count; ++i) {
    const char* chunk = strings[i];
    if (!chunk) continue;
    std::size_t chunkSize = (lengths && lengths[i] >= 0) ? static_cast<std::size_t>(lengths[i])
                                                         : std::char_traits<char>::length(chunk);
    combined.append(chunk, chunkSize);
  }
  return combined;
}

std::string sanitized_preview_of(std::string_view source) {
  std::string s(source.substr(0, 256));
  return sanitize_preview(std::move(s));
}

const char* shader_type_name(int shaderType) noexcept {
  switch (shaderType) {
    case VERTEX_SHADER:
      return "vertex";
    case FRAGMENT_SHADER:
      return "fragment";
    default:
      return "unknown";
  }
}

std::string read_shader_log(const game::gl::OpenGLFunctions& gl, unsigned shader) {
  if (!gl.glGetShaderiv || !gl.glGetShaderInfoLog) return {};
  int infoLogLength = 0;
  gl.glGetShaderiv(shader, INFO_LOG_LENGTH, &infoLogLength);
  if (infoLogLength <= 1) return {};
  std::string infoLog(static_cast<std::size_t>(infoLogLength), '\0');
  int written = 0;
  gl.glGetShaderInfoLog(shader, infoLogLength, &written, infoLog.data());
  if (written > 0 && written < infoLogLength) infoLog.resize(static_cast<std::size_t>(written));
  return sanitize_preview(std::move(infoLog));
}

std::string read_program_log(const game::gl::OpenGLFunctions& gl, unsigned program) {
  if (!gl.glGetProgramiv || !gl.glGetProgramInfoLog) return {};
  int infoLogLength = 0;
  gl.glGetProgramiv(program, INFO_LOG_LENGTH, &infoLogLength);
  if (infoLogLength <= 1) return {};
  std::string infoLog(static_cast<std::size_t>(infoLogLength), '\0');
  int written = 0;
  gl.glGetProgramInfoLog(program, infoLogLength, &written, infoLog.data());
  if (written > 0 && written < infoLogLength) infoLog.resize(static_cast<std::size_t>(written));
  return sanitize_preview(std::move(infoLog));
}

std::string read_shader_source_preview(const game::gl::OpenGLFunctions& gl, unsigned shader) {
  if (!gl.glGetShaderSource) return {};
  constexpr int kMaxSourceBytes = 1024;
  std::array<char, kMaxSourceBytes> buffer{};
  int written = 0;
  gl.glGetShaderSource(shader, kMaxSourceBytes, &written, buffer.data());
  if (written <= 0) return {};
  return sanitize_preview(std::string(buffer.data(), static_cast<std::size_t>(written)));
}

// Un-truncated source prefix for content checks (the 240-byte sanitized
// preview is for logs only — uniform declarations sit past it when the
// engine prepends its header block; confirmed live 2026-06-11).
std::string read_shader_source_prefix(const game::gl::OpenGLFunctions& gl, unsigned shader,
                                      int maxBytes = 4096) {
  if (!gl.glGetShaderiv || !gl.glGetShaderSource) return {};
  int sourceLength = 0;
  gl.glGetShaderiv(shader, SHADER_SOURCE_LENGTH, &sourceLength);
  if (sourceLength <= 1) return {};
  const int cap = sourceLength < maxBytes ? sourceLength : maxBytes;
  std::string buffer(static_cast<std::size_t>(cap), '\0');
  int written = 0;
  gl.glGetShaderSource(shader, cap, &written, buffer.data());
  if (written <= 0) return {};
  buffer.resize(static_cast<std::size_t>(written));
  return buffer;
}

std::string_view get_gl_string(const game::gl::OpenGLFunctions& gl, unsigned name) noexcept {
  if (!gl.glGetString) return {};
  const auto* value = gl.glGetString(name);
  if (!value) return {};
  return reinterpret_cast<const char*>(value);
}

std::optional<int> infer_program_slot(std::string_view vertexShaderName,
                                      std::string_view fragmentShaderName) {
  if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpDraw") return 0;
  if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpTone") return 1;
  if (vertexShaderName == "vpBlit" && fragmentShaderName == "fpCatRom") return 2;
  if ((vertexShaderName == "vpYUV" || vertexShaderName == "vpDraw") &&
      fragmentShaderName == "fpYUV")
    return 3;
  if (vertexShaderName == "vpYUV" && fragmentShaderName == "fpYUVGRY") return 4;
  if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpSprite") return 5;
  if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpFONT") return 6;
  if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpSELECT") return 7;
  if (vertexShaderName == "vpDraw" && fragmentShaderName == "fpSEAM") return 8;
  return std::nullopt;
}

void submit_shader_source(unsigned shader, int count, const char* const* strings,
                          const int* lengths, Fn_glShaderSource forward, bool& forwarded) {
  ensure_program_context();
  const std::string fullSource = gather_full_source(count, strings, lengths);
  const auto name = game::extract_shader_name(fullSource, "");

  {
    std::lock_guard lock(g_probeMutex);
    auto& record = g_shaderRecords[shader];
    record.sourcePreview = sanitized_preview_of(fullSource);
    record.sourceBytes = fullSource.size();
    record.compileLogged = false;
    record.shaderName = name;
  }

  forwarded = true;
  forward(shader, count, strings, lengths);
}

void compile_shader(unsigned shader, Fn_glCompileShader compile, bool isArb) {
  compile(shader);

  const auto& gl = game::gl::get_gl_functions();
  int shaderType = 0;
  int compileStatus = 1;
  if (gl.glGetShaderiv) {
    gl.glGetShaderiv(shader, SHADER_TYPE, &shaderType);
    gl.glGetShaderiv(shader, COMPILE_STATUS, &compileStatus);
  }

  std::string preview;
  std::size_t sourceBytes = 0;
  bool shouldLog = false;
  {
    std::lock_guard lock(g_probeMutex);
    auto& record = g_shaderRecords[shader];
    shouldLog = !record.compileLogged;
    record.compileLogged = true;
    preview = record.sourcePreview;
    sourceBytes = record.sourceBytes;
  }

  if (!shouldLog) return;

  const auto infoLog = read_shader_log(gl, shader);
  if (compileStatus == 0) {
    LOG_WARN("GL shader compile{} failed: shader={}, type={}, bytes={}, preview={}, log={}",
             isArb ? " (ARB)" : "", shader, shader_type_name(shaderType), sourceBytes,
             preview.empty() ? "<empty>" : preview, infoLog.empty() ? "<empty>" : infoLog);
  } else {
    LOG_DEBUG("GL shader compile{}: shader={} type={} status=ok bytes={} preview={}",
              isArb ? " (ARB)" : "", shader, shader_type_name(shaderType), sourceBytes,
              preview.empty() ? "<empty>" : preview);
    if (!infoLog.empty()) {
      LOG_DEBUG("GL shader compile log{}: shader={} log={}", isArb ? " (ARB)" : "", shader,
                infoLog);
    }
  }
}

static void APIENTRY detour_glShaderSource(unsigned shader, int count, const char* const* strings,
                                           const int* lengths) noexcept {
  bool forwarded = false;
  try {
    submit_shader_source(shader, count, strings, lengths, g_glShaderSourceHook.original(),
                         forwarded);
  } catch (...) {
    if (!forwarded) {
      g_glShaderSourceHook.original()(shader, count, strings, lengths);
    }
  }
}

static void APIENTRY detour_glShaderSourceARB(unsigned shader, int count,
                                              const char* const* strings,
                                              const int* lengths) noexcept {
  bool forwarded = false;
  try {
    submit_shader_source(shader, count, strings, lengths, g_glShaderSourceARBHook.original(),
                         forwarded);
  } catch (...) {
    if (!forwarded) {
      g_glShaderSourceARBHook.original()(shader, count, strings, lengths);
    }
  }
}

static void APIENTRY detour_glCompileShader(unsigned shader) noexcept {
  try {
    compile_shader(shader, g_glCompileShaderHook.original(), false);
  } catch (...) {
    // The engine compile already ran; diagnostics are optional.
  }
}

static void APIENTRY detour_glCompileShaderARB(unsigned shader) noexcept {
  try {
    compile_shader(shader, g_glCompileShaderARBHook.original(), true);
  } catch (...) {
    // The engine compile already ran; diagnostics are optional.
  }
}

// Checks g_dumpedShaders under the lock, then releases before all GL/file work,
// then re-locks to insert the record — never holds g_probeMutex across GL calls.
void maybe_dump_engine_shader(const game::gl::OpenGLFunctions& gl, unsigned shader,
                              const std::string& name) {
  if (!g_cfg.dumpEngineShaders) return;
  if (name.empty()) return;

  bool alreadyDumped = false;
  {
    std::lock_guard lock(g_probeMutex);
    alreadyDumped = g_dumpedShaders.contains(name);
  }
  if (alreadyDumped) return;

  // GL work outside the lock
  if (gl.glGetShaderiv && gl.glGetShaderSource) {
    int srcLen = 0;
    gl.glGetShaderiv(shader, SHADER_SOURCE_LENGTH, &srcLen);
    if (srcLen > 1) {
      std::string fullSrc(static_cast<std::size_t>(srcLen), '\0');
      int written = 0;
      gl.glGetShaderSource(shader, srcLen, &written, fullSrc.data());
      if (written > 0) {
        fullSrc.resize(static_cast<std::size_t>(written));
        diagnostics::dump_shader_source(g_dumpDir, name, fullSrc);
      }
    }
  }

  std::lock_guard lock(g_probeMutex);
  g_dumpedShaders.insert(name);
}

// Introspect a program without holding g_probeMutex across OpenGL calls.
void link_program_introspect(unsigned program, bool isArb, bool logDetails = true) {
  ensure_program_context();
  const auto& gl = game::gl::get_gl_functions();
  if (!gl.glGetProgramiv || !gl.glGetAttachedShaders || !gl.glGetShaderiv) return;

  int attachedShaderCount = 0;
  gl.glGetProgramiv(program, ATTACHED_SHADERS, &attachedShaderCount);
  if (attachedShaderCount <= 0) return;

  std::vector<unsigned> shaders(static_cast<std::size_t>(attachedShaderCount), 0u);
  int actualShaderCount = 0;
  gl.glGetAttachedShaders(program, attachedShaderCount, &actualShaderCount, shaders.data());

  std::string vertexShaderName;
  std::string fragmentShaderName;
  bool anyOverride = false;

  for (int i = 0; i < actualShaderCount; ++i) {
    const unsigned s = shaders[static_cast<std::size_t>(i)];
    int shaderType = 0;
    gl.glGetShaderiv(s, SHADER_TYPE, &shaderType);

    // Determine name: prefer cached record, fall back to glGetShaderSource query
    std::string nameForShader;
    {
      std::lock_guard lock(g_probeMutex);
      auto it = g_shaderRecords.find(s);
      if (it != g_shaderRecords.end()) {
        nameForShader = it->second.shaderName;
      }
    }

    const auto preview = read_shader_source_preview(gl, s);
    // If we don't have a name from the record, try reading from the GL source preview
    if (nameForShader.empty()) {
      nameForShader = game::extract_shader_name(preview, shaderType == VERTEX_SHADER ? "vp" : "fp");
    }

    // Archival: fetch full source and write to disk (engine source only,
    // not our override). Must run AFTER the name fallback — swept programs
    // have no shader records, so the record name is always empty here.
    maybe_dump_engine_shader(gl, s, nameForShader);

    if (shaderType == VERTEX_SHADER)
      vertexShaderName = nameForShader;
    else if (shaderType == FRAGMENT_SHADER)
      fragmentShaderName = nameForShader;

    // Shaders declaring uIee* uniforms (our replacement sources delivered
    // through the game's override directory) get the uniform feed.
    if (read_shader_source_prefix(gl, s).find("uIee") != std::string::npos) anyOverride = true;

    if (logDetails) {
      LOG_DEBUG("GL program attached shader{}: program={} shader={} type={} preview={}",
                isArb ? " (ARB)" : "", program, s, shader_type_name(shaderType), preview);
    }
  }

  const auto inferredSlot = infer_program_slot(vertexShaderName, fragmentShaderName);
  bool logWaterOverrideActive = false;
  bool logWaterOverrideMissing = false;
  {
    std::lock_guard lock(g_probeMutex);
    g_programRecords[program].introspected = true;
    if (anyOverride) {
      g_overriddenPrograms.try_emplace(program, std::make_shared<uniforms::Locations>());
    } else {
      g_overriddenPrograms.erase(program);
    }

    // Validate what the engine actually linked. This covers its source
    // preamble, override discovery, driver compiler, and link path.
    if (fragmentShaderName == "fpSEAM" && g_cfg.enableWaterEffect) {
      if (anyOverride && !g_waterOverrideActiveLogged) {
        g_waterOverrideActiveLogged = true;
        logWaterOverrideActive = true;
      } else if (!anyOverride && !g_waterOverrideMissingLogged) {
        g_waterOverrideMissingLogged = true;
        logWaterOverrideMissing = true;
      }
    }
  }

  if (logWaterOverrideActive) {
    LOG_INFO("Water shader override is active in engine program {}", program);
  }
  if (logWaterOverrideMissing) {
    LOG_ERROR(
        "Engine fpSEAM program {} has no Infinity Engine Enhancer uniforms; the water "
        "override was not loaded or linked. Water animation is unavailable, but tile upscaling "
        "remains active. Check override/fpSEAM.glsl and the earlier shader compile/link logs.",
        program);
  }

  if (!logDetails) return;

  if (inferredSlot) {
    LOG_DEBUG("GL program slot inference{}: program={} slot={} vertex={} fragment={}",
              isArb ? " (ARB)" : "", program, *inferredSlot, vertexShaderName, fragmentShaderName);
  } else {
    LOG_DEBUG("GL program slot inference{}: program={} slot=<unknown> vertex={} fragment={}",
              isArb ? " (ARB)" : "", program,
              vertexShaderName.empty() ? "<unknown>" : vertexShaderName,
              fragmentShaderName.empty() ? "<unknown>" : fragmentShaderName);
  }
}
void link_program(unsigned program, Fn_glLinkProgram link, bool isArb) {
  link(program);
  ensure_program_context();

  const auto& gl = game::gl::get_gl_functions();
  int linkStatus = 1;
  if (gl.glGetProgramiv) {
    gl.glGetProgramiv(program, LINK_STATUS, &linkStatus);
  }

  bool shouldLog = false;
  {
    std::lock_guard lock(g_probeMutex);
    auto& record = g_programRecords[program];
    shouldLog = !record.linkLogged;
    record.linkLogged = true;
    record.introspected = false;
    g_overriddenPrograms.erase(program);
  }

  if (shouldLog || linkStatus == 0) {
    const auto infoLog = read_program_log(gl, program);
    if (linkStatus == 0) {
      LOG_WARN("GL program link{} failed: program={}, log={}", isArb ? " (ARB)" : "", program,
               infoLog.empty() ? "<empty>" : infoLog);
    } else {
      LOG_DEBUG("GL program link{}: program={} status=ok", isArb ? " (ARB)" : "", program);
      if (!infoLog.empty()) {
        LOG_DEBUG("GL program link log{}: program={} log={}", isArb ? " (ARB)" : "", program,
                  infoLog);
      }
    }
  }

  // Program identifiers may be reused or relinked with a different shader set.
  // Rebuild classification and uniform locations after every successful link.
  if (linkStatus != 0) {
    link_program_introspect(program, isArb, shouldLog);
  }
}

static void APIENTRY detour_glLinkProgram(unsigned program) noexcept {
  try {
    link_program(program, g_glLinkProgramHook.original(), false);
  } catch (...) {
    // The engine link already ran; introspection must not affect it.
  }
}

static void APIENTRY detour_glLinkProgramARB(unsigned program) noexcept {
  try {
    link_program(program, g_glLinkProgramARBHook.original(), true);
  } catch (...) {
    // The engine link already ran; introspection must not affect it.
  }
}

// Uniform state and GL feeding live in shader_uniform_bridge.cpp. The
// probe owns only program classification and location-cache lifetime.
void feed_uniforms_to_program(unsigned program) {
  std::shared_ptr<uniforms::Locations> locations;
  {
    std::lock_guard lock(g_probeMutex);
    auto it = g_overriddenPrograms.find(program);
    if (it == g_overriddenPrograms.end()) return;
    locations = it->second;
  }
  uniforms::feed(program, *locations);
}

void use_program(unsigned program, std::uintptr_t caller, bool isArb) {
  if (program == 0) return;
  ensure_program_context();

  bool shouldInspect = false;
  bool shouldLogCaller = false;
  std::shared_ptr<uniforms::Locations> locations;
  {
    std::lock_guard lock(g_probeMutex);
    auto& record = g_programRecords[program];
    shouldInspect = !record.introspected;
    if (!shouldInspect) {
      if (const auto it = g_overriddenPrograms.find(program); it != g_overriddenPrograms.end()) {
        locations = it->second;
      }
    }
    if (g_cfg.enableVerboseLogging) {
      shouldLogCaller = record.callerLogged.insert(caller).second;
    }
  }

  if (shouldLogCaller) {
    LOG_DEBUG("GL program bind{}: program={} {}", isArb ? " (ARB)" : "", program,
              diagnostics::caller_summary(caller));
  }
  if (shouldInspect) {
    // This also discovers programs linked before probe installation. Detailed
    // caller/symbol work remains strictly opt-in through verbose logging.
    link_program_introspect(program, isArb, g_cfg.enableVerboseLogging);
    feed_uniforms_to_program(program);
  } else if (locations) {
    uniforms::feed(program, *locations);
  }
}

static void APIENTRY detour_glUseProgram(unsigned program) noexcept {
  bool forwarded = false;
  try {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    forwarded = true;
    g_glUseProgramHook.original()(program);
    use_program(program, caller, false);
  } catch (...) {
    if (!forwarded) g_glUseProgramHook.original()(program);
  }
}

static void APIENTRY detour_glUseProgramObjectARB(unsigned program) noexcept {
  bool forwarded = false;
  try {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    forwarded = true;
    g_glUseProgramObjectARBHook.original()(program);
    use_program(program, caller, true);
  } catch (...) {
    if (!forwarded) g_glUseProgramObjectARBHook.original()(program);
  }
}

void forget_shader(unsigned shader) {
  std::lock_guard lock(g_probeMutex);
  g_shaderRecords.erase(shader);
}

void forget_program(unsigned program) {
  std::lock_guard lock(g_probeMutex);
  g_programRecords.erase(program);
  g_overriddenPrograms.erase(program);
}

static void APIENTRY detour_glDeleteShader(unsigned shader) noexcept {
  bool forwarded = false;
  try {
    forwarded = true;
    g_glDeleteShaderHook.original()(shader);
    forget_shader(shader);
  } catch (...) {
    if (!forwarded) g_glDeleteShaderHook.original()(shader);
  }
}

static void APIENTRY detour_glDeleteProgram(unsigned program) noexcept {
  bool forwarded = false;
  try {
    forwarded = true;
    g_glDeleteProgramHook.original()(program);
    forget_program(program);
  } catch (...) {
    if (!forwarded) g_glDeleteProgramHook.original()(program);
  }
}

static void APIENTRY detour_glDeleteTextures(int count, const unsigned* textures) noexcept {
  bool forwarded = false;
  try {
    forwarded = true;
    g_glDeleteTexturesHook.original()(count, textures);
    game::request_texture_configuration_cache_reset();
  } catch (...) {
    if (!forwarded) g_glDeleteTexturesHook.original()(count, textures);
  }
}

static void APIENTRY detour_glDeleteObjectARB(unsigned object) noexcept {
  bool forwarded = false;
  try {
    const auto& gl = game::gl::get_gl_functions();
    bool isProgram = gl.glIsProgram && gl.glIsProgram(object) != 0;
    if (!gl.glIsProgram) {
      std::lock_guard lock(g_probeMutex);
      isProgram = g_programRecords.contains(object);
    }

    forwarded = true;
    g_glDeleteObjectARBHook.original()(object);
    if (isProgram) {
      forget_program(object);
    } else {
      forget_shader(object);
    }
  } catch (...) {
    if (!forwarded) g_glDeleteObjectARBHook.original()(object);
  }
}

}  // anonymous namespace

ShaderRuntimeCapabilities detect_shader_runtime_capabilities() noexcept {
  const auto& gl = game::gl::get_gl_functions();
  ShaderRuntimeCapabilities caps{};
  caps.baseGlReady = gl.valid;
  caps.shaderObjectsAvailable = gl.shaderObjectsAvailable;
  caps.shaderIntrospectionAvailable = gl.shaderIntrospectionAvailable;
  caps.uniformApiAvailable = gl.uniformApiAvailable;
  caps.readyForSourcePatching =
      gl.shaderObjectsAvailable && gl.shaderIntrospectionAvailable && gl.uniformApiAvailable;
  caps.glVersion = get_gl_string(gl, game::gl::VERSION);
  caps.glslVersion = get_gl_string(gl, game::gl::SHADING_LANGUAGE_VERSION);
  caps.glVendor = get_gl_string(gl, game::gl::VENDOR);
  caps.glRenderer = get_gl_string(gl, game::gl::RENDERER);
  return caps;
}

void log_shader_runtime_capabilities() {
  const auto caps = detect_shader_runtime_capabilities();
  LOG_INFO("Shader runtime source patching: {}",
           caps.readyForSourcePatching ? "ready" : "not ready");
  LOG_DEBUG(
      "Shader runtime capabilities: base={}, objects={}, introspection={}, uniforms={}, "
      "ARB={}, GL='{}', GLSL='{}', vendor='{}', renderer='{}'",
      caps.baseGlReady, caps.shaderObjectsAvailable, caps.shaderIntrospectionAvailable,
      caps.uniformApiAvailable, game::gl::get_gl_functions().arbShaderObjectsAvailable,
      caps.glVersion, caps.glslVersion, caps.glVendor, caps.glRenderer);
}

namespace {
// Optional diagnostic probe for framebuffer ownership research.
std::set<unsigned long long> g_fboBindsLogged;

void APIENTRY detour_glBindFramebuffer(unsigned target, unsigned framebuffer) noexcept {
  bool forwarded = false;
  try {
    if (framebuffer != 0) {
      const auto key = (static_cast<unsigned long long>(target) << 32) | framebuffer;
      bool shouldLog = false;
      {
        std::lock_guard lock(g_probeMutex);
        shouldLog = g_fboBindsLogged.insert(key).second;
      }
      if (shouldLog) {
        LOG_WARN("Engine bound FBO {} on target 0x{:X}", framebuffer, target);
      }
    }
    g_glBindFramebufferHook.original()(target, framebuffer);
    forwarded = true;
  } catch (...) {
    if (!forwarded) g_glBindFramebufferHook.original()(target, framebuffer);
  }
}
}  // namespace

bool install_shader_probes(const core::EngineConfig& cfg) noexcept {
  try {
    std::lock_guard lock(g_probeMutex);
    if (g_shaderProbesInstalled) return true;

    g_cfg = cfg;

    // The water effect ships ON by default; the ini can disable it and
    // the F10 debug cycle (when enabled) still overrides at runtime.
    if (!g_uniformsInitialized) {
      uniforms::initialize(cfg.enableWaterEffect, cfg.enablePerformanceLogging);
      g_uniformsInitialized = true;
    }

    // Derive the DLL directory for the dump dir and shipped water textures
#ifdef _WIN64
    {
      wchar_t wpath[MAX_PATH]{};
      HMODULE selfModule = nullptr;
      GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&install_shader_probes), &selfModule);
      GetModuleFileNameW(selfModule ? selfModule : GetModuleHandleW(nullptr), wpath, MAX_PATH);
      const std::filesystem::path dllPath(wpath);
      const auto dllDir = dllPath.parent_path();

      g_dumpDir = dllDir / "iee-shader-dumps";

      // CPU assets were prepared during DLL initialization. Keep the GL
      // upload on this render thread while removing file I/O and decoding from
      // the first RenderTexture call.
      if (cfg.enableWaterEffect || cfg.enableDebugHotkeys) {
        (void)water::upload_water_textures();
      }
    }
#else
    {
      // macOS/Linux build path (host tests only — GL hooks never run)
      g_dumpDir = std::filesystem::current_path() / "iee-shader-dumps";
    }
#endif

    const auto& gl = game::gl::get_gl_functions();
    if (!gl.readyForSourcePatching && !gl.arbShaderObjectsAvailable &&
        !(gl.shaderObjectsAvailable && gl.shaderIntrospectionAvailable)) {
      return false;
    }

    try {
      if (gl.glShaderSource) {
        g_glShaderSourceHook.create(reinterpret_cast<void*>(gl.glShaderSource),
                                    reinterpret_cast<void*>(&detour_glShaderSource));
        g_glShaderSourceHook.queue_enable();
      }
      if (gl.glCompileShader) {
        g_glCompileShaderHook.create(reinterpret_cast<void*>(gl.glCompileShader),
                                     reinterpret_cast<void*>(&detour_glCompileShader));
        g_glCompileShaderHook.queue_enable();
      }
      if (gl.glDeleteShader) {
        g_glDeleteShaderHook.create(reinterpret_cast<void*>(gl.glDeleteShader),
                                    reinterpret_cast<void*>(&detour_glDeleteShader));
        g_glDeleteShaderHook.queue_enable();
      }
      if (gl.glLinkProgram) {
        g_glLinkProgramHook.create(reinterpret_cast<void*>(gl.glLinkProgram),
                                   reinterpret_cast<void*>(&detour_glLinkProgram));
        g_glLinkProgramHook.queue_enable();
      }
      if (gl.glUseProgram) {
        g_glUseProgramHook.create(reinterpret_cast<void*>(gl.glUseProgram),
                                  reinterpret_cast<void*>(&detour_glUseProgram));
        g_glUseProgramHook.queue_enable();
      }
      if (gl.glDeleteProgram) {
        g_glDeleteProgramHook.create(reinterpret_cast<void*>(gl.glDeleteProgram),
                                     reinterpret_cast<void*>(&detour_glDeleteProgram));
        g_glDeleteProgramHook.queue_enable();
      }
      if (gl.glDeleteTextures) {
        g_glDeleteTexturesHook.create(reinterpret_cast<void*>(gl.glDeleteTextures),
                                      reinterpret_cast<void*>(&detour_glDeleteTextures));
        g_glDeleteTexturesHook.queue_enable();
      }
      // Some ICDs return the same dispatch address for promoted ARB/core
      // pairs; a second MH_CreateHook on the same target would throw and
      // roll back everything. Hook ARB entry points only when distinct.
      if (gl.glShaderSourceARB && reinterpret_cast<void*>(gl.glShaderSourceARB) !=
                                      reinterpret_cast<void*>(gl.glShaderSource)) {
        g_glShaderSourceARBHook.create(reinterpret_cast<void*>(gl.glShaderSourceARB),
                                       reinterpret_cast<void*>(&detour_glShaderSourceARB));
        g_glShaderSourceARBHook.queue_enable();
      }
      if (gl.glCompileShaderARB && reinterpret_cast<void*>(gl.glCompileShaderARB) !=
                                       reinterpret_cast<void*>(gl.glCompileShader)) {
        g_glCompileShaderARBHook.create(reinterpret_cast<void*>(gl.glCompileShaderARB),
                                        reinterpret_cast<void*>(&detour_glCompileShaderARB));
        g_glCompileShaderARBHook.queue_enable();
      }
      if (gl.glLinkProgramARB && reinterpret_cast<void*>(gl.glLinkProgramARB) !=
                                     reinterpret_cast<void*>(gl.glLinkProgram)) {
        g_glLinkProgramARBHook.create(reinterpret_cast<void*>(gl.glLinkProgramARB),
                                      reinterpret_cast<void*>(&detour_glLinkProgramARB));
        g_glLinkProgramARBHook.queue_enable();
      }
      if (gl.glUseProgramObjectARB && reinterpret_cast<void*>(gl.glUseProgramObjectARB) !=
                                          reinterpret_cast<void*>(gl.glUseProgram)) {
        g_glUseProgramObjectARBHook.create(reinterpret_cast<void*>(gl.glUseProgramObjectARB),
                                           reinterpret_cast<void*>(&detour_glUseProgramObjectARB));
        g_glUseProgramObjectARBHook.queue_enable();
      }
      if (gl.glDeleteObjectARB &&
          reinterpret_cast<void*>(gl.glDeleteObjectARB) !=
              reinterpret_cast<void*>(gl.glDeleteShader) &&
          reinterpret_cast<void*>(gl.glDeleteObjectARB) !=
              reinterpret_cast<void*>(gl.glDeleteProgram)) {
        g_glDeleteObjectARBHook.create(reinterpret_cast<void*>(gl.glDeleteObjectARB),
                                       reinterpret_cast<void*>(&detour_glDeleteObjectARB));
        g_glDeleteObjectARBHook.queue_enable();
      }
      if (cfg.enableVerboseLogging && gl.glBindFramebuffer) {
        g_glBindFramebufferHook.create(reinterpret_cast<void*>(gl.glBindFramebuffer),
                                       reinterpret_cast<void*>(&detour_glBindFramebuffer));
        g_glBindFramebufferHook.queue_enable();
      }
      const auto applyStatus = MH_ApplyQueued();
      finish_queued_probe_hooks();
      if (applyStatus != MH_OK) {
        throw std::runtime_error("MH_ApplyQueued failed for GL shader probes");
      }
    } catch (const std::exception& e) {
      (void)MH_ApplyQueued();
      finish_queued_probe_hooks();
      LOG_WARN("Failed to install GL shader probes: {}", e.what());
      remove_probe_hooks();
      return false;
    } catch (...) {
      (void)MH_ApplyQueued();
      finish_queued_probe_hooks();
      LOG_WARN("Failed to install GL shader probes with an unknown exception");
      remove_probe_hooks();
      return false;
    }

    g_shaderProbesInstalled = true;
    const auto context = game::gl::current_context();
    g_hookContext.store(context, std::memory_order_release);
    g_programContext.store(context, std::memory_order_release);
    g_sweepPending.store(true, std::memory_order_relaxed);
    LOG_INFO("Installed GL shader probes");
    return true;
  } catch (const std::exception& e) {
    remove_probe_hooks();
    LOG_ERROR("GL shader probe initialization failed: {}", e.what());
    return false;
  } catch (...) {
    remove_probe_hooks();
    LOG_ERROR("GL shader probe initialization failed with an unknown exception");
    return false;
  }
}

void uninstall_shader_probes() noexcept {
  remove_probe_hooks();
  uniforms::reset();
  try {
    std::lock_guard lock(g_probeMutex);
    g_shaderRecords.clear();
    g_programRecords.clear();
    g_overriddenPrograms.clear();
    g_dumpedShaders.clear();
    g_fboBindsLogged.clear();
    g_waterOverrideActiveLogged = false;
    g_waterOverrideMissingLogged = false;
    g_programContext.store(nullptr, std::memory_order_release);
    g_hookContext.store(nullptr, std::memory_order_release);
    g_contextRefreshPending.store(false, std::memory_order_relaxed);
    g_shaderProbesInstalled = false;
    g_uniformsInitialized = false;
    g_sweepPending.store(false, std::memory_order_relaxed);
  } catch (...) {
    // Hooks are already gone; shutdown must not escape into the loader.
  }
}

void on_frame_tick(float secondsSinceStart) noexcept {
  try {
    uniforms::set_time(secondsSinceStart);

    if (g_contextRefreshPending.load(std::memory_order_acquire)) {
      if (!remove_probe_hooks()) return;
      if (!install_shader_probes(g_cfg)) return;
      g_contextRefreshPending.store(false, std::memory_order_release);
      LOG_INFO("Reinstalled GL shader probes for replacement context");
    }
    {
      std::lock_guard lock(g_probeMutex);
      if (!g_shaderProbesInstalled) return;
    }

    const bool hookContextChanged =
        game::gl::current_context() != g_hookContext.load(std::memory_order_acquire);
    const bool programContextChanged = ensure_program_context();
    if (hookContextChanged || programContextChanged) {
      {
        std::lock_guard lock(g_probeMutex);
        g_shaderProbesInstalled = false;
      }
      g_hookContext.store(nullptr, std::memory_order_release);
      g_contextRefreshPending.store(true, std::memory_order_release);
      game::request_texture_configuration_cache_reset();
      if (!remove_probe_hooks()) {
        LOG_WARN("Could not fully remove stale GL probes; retrying at the next frame boundary");
        return;
      }
      if (!install_shader_probes(g_cfg)) return;
      g_contextRefreshPending.store(false, std::memory_order_release);
      LOG_INFO("Reinstalled GL shader probes after WGL context replacement");
      g_sweepPending.store(true, std::memory_order_relaxed);
    }

    // Deferred program sweep: runs at the frame boundary (SDL swap detour)
    // instead of mid-draw inside RenderTexture — no open engine batch, clean
    // GL state for the introspection queries and dump file I/O.
    if (g_sweepPending.exchange(false, std::memory_order_relaxed)) {
      const auto& glSweep = game::gl::get_gl_functions();
      if (glSweep.glIsProgram) {
        int sweptCount = 0;
        for (unsigned id = 1; id <= 512; ++id) {
          if (glSweep.glIsProgram(id)) {
            link_program_introspect(id, false);
            ++sweptCount;
          }
        }
        LOG_DEBUG("Program sweep introspected {} pre-existing GL programs", sweptCount);
      }
    }

    // The bind-time feed misses programs the engine keeps bound across
    // frames; refresh the currently-bound program here (render thread,
    // context current — we are inside the SDL swap detour).
    const auto& gl = game::gl::get_gl_functions();
    if (gl.glGetIntegerv) {
      int currentProgram = 0;
      gl.glGetIntegerv(0x8B8D /*CURRENT_PROGRAM*/, &currentProgram);
      if (currentProgram > 0) {
        bool isOverridden = false;
        {
          std::lock_guard lock(g_probeMutex);
          isOverridden = g_overriddenPrograms.contains(static_cast<unsigned>(currentProgram));
        }
        if (isOverridden) {
          feed_uniforms_to_program(static_cast<unsigned>(currentProgram));
        }
      }
    }

    if (g_cfg.enablePerformanceLogging) {
      static std::uint32_t lastPerformanceLogTick = 0;
      const auto now = GetTickCount();
      if (lastPerformanceLogTick == 0) lastPerformanceLogTick = now;
      if (now - lastPerformanceLogTick >= 5000) {
        lastPerformanceLogTick = now;
        const auto stats = uniforms::take_performance_stats();
        static const long long frequency = [] {
          LARGE_INTEGER value{};
          return QueryPerformanceFrequency(&value) ? value.QuadPart : 0LL;
        }();
        const double ticksToMicroseconds =
            frequency > 0 ? 1'000'000.0 / static_cast<double>(frequency) : 0.0;
        const double averageMicroseconds = stats.calls > 0 ? static_cast<double>(stats.totalTicks) *
                                                                 ticksToMicroseconds /
                                                                 static_cast<double>(stats.calls)
                                                           : 0.0;
        LOG_INFO(
            "Shader uniform feed perf: calls={}, unchangedSkipped={}, textureBindPasses={}, "
            "avg={:.2f}us, max={:.2f}us over 5s",
            stats.calls, stats.skippedUnchanged, stats.textureBindPasses, averageMicroseconds,
            static_cast<double>(stats.maximumTicks) * ticksToMicroseconds);
      }
    }

    if (!g_cfg.enableDebugHotkeys) {
      return;
    }

    // F10: cycle the visual effect gate OFF(0) -> WATER(1) -> ALIGN(2) -> OFF.
    static bool f10WasDown = false;
    const bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10Down && !f10WasDown) {
      const float next = uniforms::cycle_debug_effect();

      // Snapshot the world-transform inputs so screenshots pair with the
      // exact values the shader saw (render thread; context current).
      int vp[4] = {0, 0, 0, 0};
      const auto& glState = game::gl::get_gl_functions();
      if (glState.glGetIntegerv) {
        glState.glGetIntegerv(0x0BA2 /*GL_VIEWPORT*/, vp);
      }
      const auto state = uniforms::snapshot();
      LOG_INFO(
          "Hotkey F10: override effect value {} (scroll=({}, {}), viewWorld={}x{}, viewport={}x{} "
          "at ({}, {}), world={}x{}, feeds={})",
          next, state.scrollX, state.scrollY, state.viewWorldWidth, state.viewWorldHeight, vp[2],
          vp[3], vp[0], vp[1], state.worldWidth, state.worldHeight, state.feedCount);
    }
    f10WasDown = f10Down;
  } catch (...) {
    // Frame presentation and the original swap must never depend on probes.
  }
}

void set_override_effect_enabled(bool enabled) noexcept { uniforms::set_effect_enabled(enabled); }

bool override_effect_enabled() noexcept { return uniforms::effect_enabled(); }

void set_area_world_size(float widthPx, float heightPx) noexcept {
  uniforms::set_world_size(widthPx, heightPx);
}

void set_area_water_tint(float r, float g, float b) noexcept { uniforms::set_water_tint(r, g, b); }

void set_area_effect_points(const float* xyzw, std::size_t count) noexcept {
  uniforms::set_effect_points(xyzw, count);
}

void set_area_view(float scrollX, float scrollY, float viewWorldW, float viewWorldH) noexcept {
  uniforms::set_view(scrollX, scrollY, viewWorldW, viewWorldH);
}

}  // namespace iee::probe
