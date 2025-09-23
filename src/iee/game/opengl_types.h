#pragma once
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

namespace iee::game::gl {

// region OpenGL Constants
constexpr unsigned TEXTURE_2D = 0x0DE1;
constexpr unsigned TEXTURE_WRAP_S = 0x2802;
constexpr unsigned TEXTURE_WRAP_T = 0x2803;
constexpr unsigned CLAMP_TO_EDGE = 0x812F;
constexpr unsigned TEXTURE_MIN_FILTER = 0x2801;
constexpr unsigned TEXTURE_MAG_FILTER = 0x2800;
constexpr unsigned LINEAR = 0x2601;
constexpr unsigned TEXTURE_BINDING_2D = 0x8069;
constexpr unsigned MAX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FF;
constexpr unsigned TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE;
constexpr unsigned TEXTURE_LOD_BIAS = 0x8501;

// OpenGL error codes
constexpr unsigned GL_NO_ERROR = 0x0000;
constexpr unsigned GL_INVALID_ENUM = 0x0500;
constexpr unsigned GL_INVALID_VALUE = 0x0501;
constexpr unsigned GL_INVALID_OPERATION = 0x0502;
constexpr unsigned GL_OUT_OF_MEMORY = 0x0505;
constexpr unsigned GL_INVALID_FRAMEBUFFER_OPERATION = 0x0506;
constexpr unsigned GL_CONTEXT_LOST = 0x0507;
// endregion

// region OpenGL Function Pointer Types
using PFN_glTexParameteri = void (APIENTRY*)(unsigned target, unsigned pname, int param);
using PFN_glTexParameterf = void (APIENTRY*)(unsigned target, unsigned pname, float param);
using PFN_glGetIntegerv = void (APIENTRY*)(unsigned pname, int* data);
using PFN_glGetTexParameteriv = void (APIENTRY*)(unsigned target, unsigned pname, int* params);
using PFN_glGetError = unsigned int (APIENTRY*)();
// endregion

// region Error Handling Utilities
const char* error_string(unsigned error_code) noexcept;
bool check_error(const char* operation) noexcept;
// endregion

// region Function Loading
struct OpenGLFunctions {
    PFN_glTexParameteri glTexParameteri{};
    PFN_glTexParameterf glTexParameterf{};
    PFN_glGetIntegerv glGetIntegerv{};
    PFN_glGetTexParameteriv glGetTexParameteriv{};
    PFN_glGetError glGetError{};

    bool valid{false};

    bool initialize() noexcept;
};

// Get the global OpenGL function table
OpenGLFunctions& get_gl_functions() noexcept;
// endregion

} // namespace iee::game::gl
