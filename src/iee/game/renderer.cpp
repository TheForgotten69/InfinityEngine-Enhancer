#include "renderer.h"

#include <windows.h>

#include <algorithm>
#include <atomic>

#include "build_manifest.h"
#include "iee/core/config.h"
#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"
#include "opengl_types.h"

namespace iee::game {
namespace {
const char* instruction_kind_name(BranchInstructionKind kind) noexcept {
  switch (kind) {
    case BranchInstructionKind::CallRel32:
      return "call";
    case BranchInstructionKind::JmpRel32:
      return "jmp";
  }
  return "unknown";
}

bool is_address_in_module(const core::ModuleSpan& module, const void* address) noexcept {
  const auto value = reinterpret_cast<std::uintptr_t>(address);
  const auto start = reinterpret_cast<std::uintptr_t>(module.base);
  const auto end = start + module.size;
  return value >= start && value < end;
}
}  // namespace

bool resolve_draw_api(DrawApi& out, std::uintptr_t renderTextureVA, const BuildManifest& manifest) {
  if (!renderTextureVA) {
    LOG_ERROR("RenderTexture address is null");
    return false;
  }

  out = {};

  struct APIFunction {
    void** target;
    const BranchInstructionDesc* desc;
  };

  const APIFunction functions[] = {
      {reinterpret_cast<void**>(&out.CRes_Demand), &manifest.renderTextureCallsites[0]},
      {reinterpret_cast<void**>(&out.DrawBindTexture), &manifest.renderTextureCallsites[1]},
      {reinterpret_cast<void**>(&out.DrawDisable), &manifest.renderTextureCallsites[2]},
      {reinterpret_cast<void**>(&out.DrawColor), &manifest.renderTextureCallsites[3]},
      {reinterpret_cast<void**>(&out.DrawPushState), &manifest.renderTextureCallsites[4]},
      {reinterpret_cast<void**>(&out.DrawColorTone), &manifest.renderTextureCallsites[5]},
      {reinterpret_cast<void**>(&out.DrawBegin), &manifest.renderTextureCallsites[6]},
      {reinterpret_cast<void**>(&out.DrawTexCoord), &manifest.renderTextureCallsites[7]},
      {reinterpret_cast<void**>(&out.DrawVertex), &manifest.renderTextureCallsites[8]},
      {reinterpret_cast<void**>(&out.DrawEnd), &manifest.renderTextureCallsites[9]},
      {reinterpret_cast<void**>(&out.DrawPopState), &manifest.renderTextureCallsites[10]},
  };

  const auto moduleInfo = core::get_module_span(nullptr);
  bool allResolved = true;
  for (const auto& func : functions) {
    const auto* desc = func.desc;
    const auto* insn = reinterpret_cast<const void*>(renderTextureVA + desc->offset);

    void* addr = core::rel32_target_checked(insn, desc->opcode, desc->displacementOffset,
                                            desc->instructionSize);
    if (!addr) {
      const auto* messageLevel = desc->required ? "Failed" : "Skipped";
      if (desc->required) {
        LOG_ERROR("{} to resolve {} at offset 0x{:X} (expected {} opcode 0x{:02X})", messageLevel,
                  desc->name, desc->offset, instruction_kind_name(desc->kind), desc->opcode);
        allResolved = false;
      } else {
        LOG_WARN("{} optional {} at offset 0x{:X} (expected {} opcode 0x{:02X})", messageLevel,
                 desc->name, desc->offset, instruction_kind_name(desc->kind), desc->opcode);
      }
    } else if (moduleInfo && !is_address_in_module(*moduleInfo, addr)) {
      if (desc->required) {
        LOG_ERROR("Resolved {} outside the game module: 0x{:X}", desc->name,
                  reinterpret_cast<uintptr_t>(addr));
        allResolved = false;
      } else {
        LOG_WARN("Resolved optional {} outside the game module: 0x{:X}", desc->name,
                 reinterpret_cast<uintptr_t>(addr));
      }
    } else {
      *func.target = addr;
      LOG_DEBUG("Resolved {} via {} at 0x{:X}: 0x{:X}", desc->name,
                instruction_kind_name(desc->kind), renderTextureVA + desc->offset,
                reinterpret_cast<uintptr_t>(addr));
    }
  }

  if (allResolved) {
    LOG_INFO("Successfully resolved all draw API functions from RenderTexture at 0x{:X}",
             renderTextureVA);
  } else {
    LOG_WARN("Some draw API functions failed to resolve");
  }

  return allResolved;
}

bool configure_bound_texture(const core::EngineConfig& cfg, int sourceTextureId) {
  auto& gl = gl::get_gl_functions();
  if (!gl.valid || sourceTextureId <= 0) {
    return false;
  }

  static std::atomic<unsigned> configurationCount{0};
  const auto callCount = configurationCount.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool logDetails = callCount <= 3 || cfg.enableVerboseLogging;

  // Discard errors from engine work before attributing a failure to this operation.
  for (int errorCount = 0; errorCount < 16 && gl.glGetError() != gl::GL_NO_ERROR; ++errorCount) {
  }

  const auto checkError = [&](const char* operation) {
    const unsigned error = gl.glGetError();
    if (error == gl::GL_NO_ERROR) return true;
    LOG_WARN("OpenGL error while {}: {} (0x{:X})", operation, ::iee::game::gl::error_string(error),
             error);
    return false;
  };

  int boundTexture = 0;
  gl.glGetIntegerv(gl::TEXTURE_BINDING_2D, &boundTexture);
  if (!checkError("querying GL_TEXTURE_BINDING_2D") || boundTexture == 0) {
    LOG_WARN("No GL texture is bound for source texture {}", sourceTextureId);
    return false;
  }

  const auto setInteger = [&](unsigned parameter, int value, const char* operation) {
    gl.glTexParameteri(gl::TEXTURE_2D, parameter, value);
    return checkError(operation);
  };
  if (!setInteger(gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE, "setting texture wrap S") ||
      !setInteger(gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE, "setting texture wrap T") ||
      !setInteger(gl::TEXTURE_MAG_FILTER, gl::LINEAR, "setting texture mag filter") ||
      !setInteger(gl::TEXTURE_MIN_FILTER, gl::LINEAR, "setting texture min filter")) {
    return false;
  }

  if (cfg.enableAnisotropicFiltering) {
    int maximumAnisotropy = 0;
    gl.glGetIntegerv(gl::MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximumAnisotropy);
    if (!checkError("querying maximum anisotropy") || maximumAnisotropy < 1) {
      LOG_WARN("Anisotropic filtering was requested but is unavailable");
      return false;
    }
    const float anisotropy = std::min(cfg.maxAnisotropy, static_cast<float>(maximumAnisotropy));
    gl.glTexParameterf(gl::TEXTURE_2D, gl::TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);
    if (!checkError("setting texture anisotropy")) return false;
  }

  gl.glTexParameterf(gl::TEXTURE_2D, gl::TEXTURE_LOD_BIAS, cfg.lodBias);
  if (!checkError("setting texture LOD bias")) return false;

  if (logDetails) {
    LOG_DEBUG("Configured source texture {} (GL name {}, anisotropy={}, LOD bias={:.2f})",
              sourceTextureId, boundTexture,
              cfg.enableAnisotropicFiltering ? cfg.maxAnisotropy : 1.0f, cfg.lodBias);
  }
  return true;
}
}  // namespace iee::game
