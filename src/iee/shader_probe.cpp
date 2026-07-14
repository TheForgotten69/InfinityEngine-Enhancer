#include "shader_probe.h"

#include <intrin.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "iee/core/hooking.h"
#include "iee/core/logger.h"
#include "iee/game/opengl_types.h"
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

void remove_probe_hooks() noexcept {
  (void)g_glBindFramebufferHook.remove();
  (void)g_glDeleteObjectARBHook.remove();
  (void)g_glUseProgramObjectARBHook.remove();
  (void)g_glLinkProgramARBHook.remove();
  (void)g_glCompileShaderARBHook.remove();
  (void)g_glShaderSourceARBHook.remove();
  (void)g_glUseProgramHook.remove();
  (void)g_glDeleteProgramHook.remove();
  (void)g_glLinkProgramHook.remove();
  (void)g_glDeleteShaderHook.remove();
  (void)g_glCompileShaderHook.remove();
  (void)g_glShaderSourceHook.remove();
}

std::mutex g_probeMutex;
std::unordered_map<unsigned, ShaderRecord> g_shaderRecords;
std::unordered_map<unsigned, ProgramRecord> g_programRecords;
std::unordered_map<unsigned, uniforms::Locations> g_overriddenPrograms;
std::set<std::string> g_dumpedShaders;  // names already written to disk
bool g_shaderProbesInstalled = false;
core::EngineConfig g_cfg;
std::filesystem::path g_dumpDir;

// Set by install; consumed by the first frame tick (sweep runs at the
// frame boundary, never mid-draw).
std::atomic<bool> g_sweepPending{false};

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
  {
    std::lock_guard lock(g_probeMutex);
    if (anyOverride) {
      g_overriddenPrograms.try_emplace(program, uniforms::Locations{});
    } else {
      g_overriddenPrograms.erase(program);
    }
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
  uniforms::Locations locations{};
  {
    std::lock_guard lock(g_probeMutex);
    auto it = g_overriddenPrograms.find(program);
    if (it == g_overriddenPrograms.end()) return;
    locations = it->second;
  }

  uniforms::feed(program, locations);

  std::lock_guard lock(g_probeMutex);
  if (auto it = g_overriddenPrograms.find(program); it != g_overriddenPrograms.end()) {
    it->second = locations;
  }
}

void use_program(unsigned program, std::uintptr_t caller, bool isArb) {
  bool shouldInspect = false;
  {
    std::lock_guard lock(g_probeMutex);
    auto& record = g_programRecords[program];
    shouldInspect = record.callerLogged.insert(caller).second;
  }

  if (shouldInspect) {
    LOG_DEBUG("GL program bind{}: program={} {}", isArb ? " (ARB)" : "", program,
              diagnostics::caller_summary(caller));
    if (program != 0) {
      // This also discovers programs linked before probe installation.
      link_program_introspect(program, isArb);
    }
  }

  if (program != 0) {
    feed_uniforms_to_program(program);
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
    uniforms::initialize(cfg.enableWaterEffect);

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

      // Water textures (render thread, context current here).
      (void)water::load_water_textures(dllDir / "iee-textures");
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
        g_glShaderSourceHook.enable();
      }
      if (gl.glCompileShader) {
        g_glCompileShaderHook.create(reinterpret_cast<void*>(gl.glCompileShader),
                                     reinterpret_cast<void*>(&detour_glCompileShader));
        g_glCompileShaderHook.enable();
      }
      if (gl.glDeleteShader) {
        g_glDeleteShaderHook.create(reinterpret_cast<void*>(gl.glDeleteShader),
                                    reinterpret_cast<void*>(&detour_glDeleteShader));
        g_glDeleteShaderHook.enable();
      }
      if (gl.glLinkProgram) {
        g_glLinkProgramHook.create(reinterpret_cast<void*>(gl.glLinkProgram),
                                   reinterpret_cast<void*>(&detour_glLinkProgram));
        g_glLinkProgramHook.enable();
      }
      if (gl.glUseProgram) {
        g_glUseProgramHook.create(reinterpret_cast<void*>(gl.glUseProgram),
                                  reinterpret_cast<void*>(&detour_glUseProgram));
        g_glUseProgramHook.enable();
      }
      if (gl.glDeleteProgram) {
        g_glDeleteProgramHook.create(reinterpret_cast<void*>(gl.glDeleteProgram),
                                     reinterpret_cast<void*>(&detour_glDeleteProgram));
        g_glDeleteProgramHook.enable();
      }
      // Some ICDs return the same dispatch address for promoted ARB/core
      // pairs; a second MH_CreateHook on the same target would throw and
      // roll back everything. Hook ARB entry points only when distinct.
      if (gl.glShaderSourceARB && reinterpret_cast<void*>(gl.glShaderSourceARB) !=
                                      reinterpret_cast<void*>(gl.glShaderSource)) {
        g_glShaderSourceARBHook.create(reinterpret_cast<void*>(gl.glShaderSourceARB),
                                       reinterpret_cast<void*>(&detour_glShaderSourceARB));
        g_glShaderSourceARBHook.enable();
      }
      if (gl.glCompileShaderARB && reinterpret_cast<void*>(gl.glCompileShaderARB) !=
                                       reinterpret_cast<void*>(gl.glCompileShader)) {
        g_glCompileShaderARBHook.create(reinterpret_cast<void*>(gl.glCompileShaderARB),
                                        reinterpret_cast<void*>(&detour_glCompileShaderARB));
        g_glCompileShaderARBHook.enable();
      }
      if (gl.glLinkProgramARB && reinterpret_cast<void*>(gl.glLinkProgramARB) !=
                                     reinterpret_cast<void*>(gl.glLinkProgram)) {
        g_glLinkProgramARBHook.create(reinterpret_cast<void*>(gl.glLinkProgramARB),
                                      reinterpret_cast<void*>(&detour_glLinkProgramARB));
        g_glLinkProgramARBHook.enable();
      }
      if (gl.glUseProgramObjectARB && reinterpret_cast<void*>(gl.glUseProgramObjectARB) !=
                                          reinterpret_cast<void*>(gl.glUseProgram)) {
        g_glUseProgramObjectARBHook.create(reinterpret_cast<void*>(gl.glUseProgramObjectARB),
                                           reinterpret_cast<void*>(&detour_glUseProgramObjectARB));
        g_glUseProgramObjectARBHook.enable();
      }
      if (gl.glDeleteObjectARB &&
          reinterpret_cast<void*>(gl.glDeleteObjectARB) !=
              reinterpret_cast<void*>(gl.glDeleteShader) &&
          reinterpret_cast<void*>(gl.glDeleteObjectARB) !=
              reinterpret_cast<void*>(gl.glDeleteProgram)) {
        g_glDeleteObjectARBHook.create(reinterpret_cast<void*>(gl.glDeleteObjectARB),
                                       reinterpret_cast<void*>(&detour_glDeleteObjectARB));
        g_glDeleteObjectARBHook.enable();
      }
      if (cfg.enableVerboseLogging && gl.glBindFramebuffer) {
        g_glBindFramebufferHook.create(reinterpret_cast<void*>(gl.glBindFramebuffer),
                                       reinterpret_cast<void*>(&detour_glBindFramebuffer));
        g_glBindFramebufferHook.enable();
      }
    } catch (const std::exception& e) {
      LOG_WARN("Failed to install GL shader probes: {}", e.what());
      remove_probe_hooks();
      return false;
    } catch (...) {
      LOG_WARN("Failed to install GL shader probes with an unknown exception");
      remove_probe_hooks();
      return false;
    }

    g_shaderProbesInstalled = true;
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
    g_shaderProbesInstalled = false;
    g_sweepPending.store(false, std::memory_order_relaxed);
  } catch (...) {
    // Hooks are already gone; shutdown must not escape into the loader.
  }
}

void on_frame_tick(float secondsSinceStart) noexcept {
  try {
    uniforms::set_time(secondsSinceStart);

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

void set_area_view(float scrollX, float scrollY, float viewWorldW, float viewWorldH) noexcept {
  uniforms::set_view(scrollX, scrollY, viewWorldW, viewWorldH);
}

}  // namespace iee::probe
