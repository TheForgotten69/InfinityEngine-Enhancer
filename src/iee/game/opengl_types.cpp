#include "opengl_types.h"

#include <windows.h>

#include <atomic>
#include <mutex>

#include "iee/core/logger.h"

namespace iee::game::gl {
const char* error_string(unsigned error_code) noexcept {
  switch (error_code) {
    case GL_NO_ERROR:
      return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
      return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
      return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
      return "GL_INVALID_OPERATION";
    case GL_OUT_OF_MEMORY:
      return "GL_OUT_OF_MEMORY";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
      return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_CONTEXT_LOST:
      return "GL_CONTEXT_LOST";
    default:
      return "UNKNOWN_GL_ERROR";
  }
}

void discard_errors(unsigned maximumErrors) noexcept {
  const auto& gl = get_gl_functions();
  if (!gl.glGetError) return;
  for (unsigned errorCount = 0; errorCount < maximumErrors; ++errorCount) {
    if (gl.glGetError() == GL_NO_ERROR) return;
  }
}

bool check_error(const char* operation) noexcept {
  const auto& gl = get_gl_functions();
  if (!gl.glGetError) return false;

  if (unsigned error = gl.glGetError(); error != GL_NO_ERROR) {
    LOG_WARN("OpenGL error in {}: {} (0x{:X})", operation, error_string(error), error);
    return false;
  }
  return true;
}

// Load a GL1.1 core entry point from opengl32.dll directly.
// wglGetProcAddress returns NULL for GL1.1 core symbols, so we must use
// GetProcAddress on the opengl32.dll module handle instead.
static void* get_gl1_proc_address(HMODULE opengl32, const char* name) noexcept {
  if (!opengl32) return nullptr;
  return GetProcAddress(opengl32, name);
}

// Load an extension entry point.  Try wglGetProcAddress first (correct path
// for ARB/core-profile extensions); fall back to opengl32.dll GetProcAddress
// for any symbol that happens to be exported there (e.g. on some drivers).
static void* get_ext_proc_address(HMODULE opengl32, const char* name) noexcept {
  if (!opengl32) return nullptr;

  using PFN_wglGetProcAddress = void*(WINAPI*)(const char*);
  static auto wglGetProcAddr =
      reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(opengl32, "wglGetProcAddress"));

  if (wglGetProcAddr) {
    void* proc = wglGetProcAddr(name);
    // Some drivers return small sentinel values (1, 2, 3, -1) instead of
    // null for unavailable functions; calling through them crashes.
    const auto raw = reinterpret_cast<std::intptr_t>(proc);
    if (raw != 0 && raw != 1 && raw != 2 && raw != 3 && raw != -1) {
      return proc;
    }
  }
  return GetProcAddress(opengl32, name);
}

HGLRC current_context() noexcept {
  static HMODULE opengl32 = GetModuleHandleA("opengl32.dll");
  if (!opengl32) return nullptr;

  using PFN_wglGetCurrentContext = HGLRC(WINAPI*)();
  static auto wglGetCurrentContext =
      reinterpret_cast<PFN_wglGetCurrentContext>(GetProcAddress(opengl32, "wglGetCurrentContext"));

  return wglGetCurrentContext ? wglGetCurrentContext() : nullptr;
}

bool OpenGLFunctions::initialize() noexcept {
  static HMODULE opengl32 = GetModuleHandleA("opengl32.dll");

  // --- GL1.1 core: load via opengl32.dll GetProcAddress ---
  glTexParameteri =
      reinterpret_cast<PFN_glTexParameteri>(get_gl1_proc_address(opengl32, "glTexParameteri"));
  glTexParameterf =
      reinterpret_cast<PFN_glTexParameterf>(get_gl1_proc_address(opengl32, "glTexParameterf"));
  glGetIntegerv =
      reinterpret_cast<PFN_glGetIntegerv>(get_gl1_proc_address(opengl32, "glGetIntegerv"));
  glGetTexParameteriv = reinterpret_cast<PFN_glGetTexParameteriv>(
      get_gl1_proc_address(opengl32, "glGetTexParameteriv"));
  glGetError = reinterpret_cast<PFN_glGetError>(get_gl1_proc_address(opengl32, "glGetError"));
  glGenTextures =
      reinterpret_cast<PFN_glGenTextures>(get_gl1_proc_address(opengl32, "glGenTextures"));
  glBindTexture =
      reinterpret_cast<PFN_glBindTexture>(get_gl1_proc_address(opengl32, "glBindTexture"));
  glTexImage2D = reinterpret_cast<PFN_glTexImage2D>(get_gl1_proc_address(opengl32, "glTexImage2D"));
  glDeleteTextures =
      reinterpret_cast<PFN_glDeleteTextures>(get_gl1_proc_address(opengl32, "glDeleteTextures"));
  glPixelStorei =
      reinterpret_cast<PFN_glPixelStorei>(get_gl1_proc_address(opengl32, "glPixelStorei"));

  glGetString = reinterpret_cast<PFN_glGetString>(get_gl1_proc_address(opengl32, "glGetString"));

  // --- Extensions: load via wglGetProcAddress ---
  glCreateShader =
      reinterpret_cast<PFN_glCreateShader>(get_ext_proc_address(opengl32, "glCreateShader"));
  glShaderSource =
      reinterpret_cast<PFN_glShaderSource>(get_ext_proc_address(opengl32, "glShaderSource"));
  glCompileShader =
      reinterpret_cast<PFN_glCompileShader>(get_ext_proc_address(opengl32, "glCompileShader"));
  glDeleteShader =
      reinterpret_cast<PFN_glDeleteShader>(get_ext_proc_address(opengl32, "glDeleteShader"));
  glGetShaderiv =
      reinterpret_cast<PFN_glGetShaderiv>(get_ext_proc_address(opengl32, "glGetShaderiv"));
  glGetShaderInfoLog = reinterpret_cast<PFN_glGetShaderInfoLog>(
      get_ext_proc_address(opengl32, "glGetShaderInfoLog"));
  glCreateProgram =
      reinterpret_cast<PFN_glCreateProgram>(get_ext_proc_address(opengl32, "glCreateProgram"));
  glAttachShader =
      reinterpret_cast<PFN_glAttachShader>(get_ext_proc_address(opengl32, "glAttachShader"));
  glLinkProgram =
      reinterpret_cast<PFN_glLinkProgram>(get_ext_proc_address(opengl32, "glLinkProgram"));
  glUseProgram = reinterpret_cast<PFN_glUseProgram>(get_ext_proc_address(opengl32, "glUseProgram"));
  glDeleteProgram =
      reinterpret_cast<PFN_glDeleteProgram>(get_ext_proc_address(opengl32, "glDeleteProgram"));
  glGetProgramiv =
      reinterpret_cast<PFN_glGetProgramiv>(get_ext_proc_address(opengl32, "glGetProgramiv"));
  glGetProgramInfoLog = reinterpret_cast<PFN_glGetProgramInfoLog>(
      get_ext_proc_address(opengl32, "glGetProgramInfoLog"));
  glGetAttachedShaders = reinterpret_cast<PFN_glGetAttachedShaders>(
      get_ext_proc_address(opengl32, "glGetAttachedShaders"));
  glGetShaderSource =
      reinterpret_cast<PFN_glGetShaderSource>(get_ext_proc_address(opengl32, "glGetShaderSource"));
  glGetUniformLocation = reinterpret_cast<PFN_glGetUniformLocation>(
      get_ext_proc_address(opengl32, "glGetUniformLocation"));
  glUniform1f = reinterpret_cast<PFN_glUniform1f>(get_ext_proc_address(opengl32, "glUniform1f"));
  glUniform1i = reinterpret_cast<PFN_glUniform1i>(get_ext_proc_address(opengl32, "glUniform1i"));
  glUniform2f = reinterpret_cast<PFN_glUniform2f>(get_ext_proc_address(opengl32, "glUniform2f"));
  glUniform3f = reinterpret_cast<PFN_glUniform3f>(get_ext_proc_address(opengl32, "glUniform3f"));
  glActiveTexture =
      reinterpret_cast<PFN_glActiveTexture>(get_ext_proc_address(opengl32, "glActiveTexture"));
  glCompressedTexImage2D = reinterpret_cast<PFN_glCompressedTexImage2D>(
      get_ext_proc_address(opengl32, "glCompressedTexImage2D"));
  glGenerateMipmap =
      reinterpret_cast<PFN_glGenerateMipmap>(get_ext_proc_address(opengl32, "glGenerateMipmap"));
  glBindFramebuffer =
      reinterpret_cast<PFN_glBindFramebuffer>(get_ext_proc_address(opengl32, "glBindFramebuffer"));
  glIsProgram = reinterpret_cast<PFN_glIsProgram>(get_ext_proc_address(opengl32, "glIsProgram"));
  glShaderSourceARB =
      reinterpret_cast<PFN_glShaderSourceARB>(get_ext_proc_address(opengl32, "glShaderSourceARB"));
  glCompileShaderARB = reinterpret_cast<PFN_glCompileShaderARB>(
      get_ext_proc_address(opengl32, "glCompileShaderARB"));
  glLinkProgramARB =
      reinterpret_cast<PFN_glLinkProgramARB>(get_ext_proc_address(opengl32, "glLinkProgramARB"));
  glUseProgramObjectARB = reinterpret_cast<PFN_glUseProgramObjectARB>(
      get_ext_proc_address(opengl32, "glUseProgramObjectARB"));
  glDeleteObjectARB =
      reinterpret_cast<PFN_glDeleteObjectARB>(get_ext_proc_address(opengl32, "glDeleteObjectARB"));

  // --- Capability flags ---
  valid = (glTexParameteri != nullptr && glTexParameterf != nullptr && glGetIntegerv != nullptr &&
           glGetTexParameteriv != nullptr && glGetError != nullptr);

  shaderObjectsAvailable =
      glCreateShader != nullptr && glShaderSource != nullptr && glCompileShader != nullptr &&
      glDeleteShader != nullptr && glCreateProgram != nullptr && glAttachShader != nullptr &&
      glLinkProgram != nullptr && glUseProgram != nullptr && glDeleteProgram != nullptr;

  shaderIntrospectionAvailable = glGetShaderiv != nullptr && glGetShaderInfoLog != nullptr &&
                                 glGetProgramiv != nullptr && glGetProgramInfoLog != nullptr &&
                                 glGetAttachedShaders != nullptr && glGetShaderSource != nullptr;

  uniformApiAvailable = glGetUniformLocation != nullptr && glUniform1f != nullptr &&
                        glUniform1i != nullptr && glUniform2f != nullptr && glUniform3f != nullptr;

  readyForSourcePatching =
      shaderObjectsAvailable && shaderIntrospectionAvailable && uniformApiAvailable;

  arbShaderObjectsAvailable = glShaderSourceARB != nullptr && glCompileShaderARB != nullptr &&
                              glLinkProgramARB != nullptr && glUseProgramObjectARB != nullptr;

  textureUploadAvailable = valid && glGenTextures != nullptr && glBindTexture != nullptr &&
                           glTexImage2D != nullptr && glDeleteTextures != nullptr &&
                           glPixelStorei != nullptr && glActiveTexture != nullptr;
  compressedTextureUploadAvailable = textureUploadAvailable && glCompressedTexImage2D != nullptr;

  if (valid) {
    LOG_INFO(
        "OpenGL initialized (source patching={}, ARB shaders={}, texture upload={}, compressed "
        "texture upload={})",
        readyForSourcePatching ? "ready" : "partial",
        arbShaderObjectsAvailable ? "ready" : "partial",
        textureUploadAvailable ? "ready" : "partial",
        compressedTextureUploadAvailable ? "ready" : "partial");
    LOG_DEBUG(
        "OpenGL entry points: glGetString={}, glTexParameteri={}, glTexParameterf={}, "
        "glGetIntegerv={}, glGetTexParameteriv={}, glGetError={}, glGenTextures={}, "
        "glBindTexture={}, glTexImage2D={}, glDeleteTextures={}, glPixelStorei={}, "
        "glCompressedTexImage2D={}, glGenerateMipmap={}",
        glGetString != nullptr, glTexParameteri != nullptr, glTexParameterf != nullptr,
        glGetIntegerv != nullptr, glGetTexParameteriv != nullptr, glGetError != nullptr,
        glGenTextures != nullptr, glBindTexture != nullptr, glTexImage2D != nullptr,
        glDeleteTextures != nullptr, glPixelStorei != nullptr, glCompressedTexImage2D != nullptr,
        glGenerateMipmap != nullptr);
  } else {
    LOG_ERROR("Failed to initialize some OpenGL functions");
  }

  return valid;
}

OpenGLFunctions& get_gl_functions() noexcept {
  static OpenGLFunctions instance;
  static std::mutex mutex;
  static std::atomic<HGLRC> lastContext{nullptr};
  static std::atomic<bool> ready{false};

  const auto context = current_context();
  if (ready.load(std::memory_order_acquire) &&
      context == lastContext.load(std::memory_order_relaxed)) {
    return instance;
  }

  // Context initialization/replacement is the slow path. The steady-state
  // render thread avoids taking this mutex on every uniform or texture feed.
  std::lock_guard lock(mutex);
  if (!ready.load(std::memory_order_relaxed) ||
      context != lastContext.load(std::memory_order_relaxed)) {
    instance.initialize();
    lastContext.store(context, std::memory_order_relaxed);
    ready.store(instance.valid, std::memory_order_release);
  }

  return instance;
}

}  // namespace iee::game::gl
