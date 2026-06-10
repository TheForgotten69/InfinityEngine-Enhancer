#include "renderer.h"
#include "build_manifest.h"
#include "opengl_types.h"
#include "iee/core/config.h"
#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"
#include <windows.h>
#include <algorithm>
#include <atomic>

namespace iee::game {
    namespace {
        const char *instruction_kind_name(BranchInstructionKind kind) noexcept {
            switch (kind) {
                case BranchInstructionKind::CallRel32:
                    return "call";
                case BranchInstructionKind::JmpRel32:
                    return "jmp";
            }
            return "unknown";
        }

        bool is_address_in_module(const core::ModuleSpan &module, const void *address) noexcept {
            const auto value = reinterpret_cast<std::uintptr_t>(address);
            const auto start = reinterpret_cast<std::uintptr_t>(module.base);
            const auto end = start + module.size;
            return value >= start && value < end;
        }
    }

    bool resolve_draw_api(DrawApi &out, std::uintptr_t renderTextureVA, const BuildManifest &manifest) {
        if (!renderTextureVA) {
            LOG_ERROR("RenderTexture address is null");
            return false;
        }

        out = {};

        struct APIFunction {
            void **target;
            const BranchInstructionDesc *desc;
        };

        const APIFunction functions[] = {
            {reinterpret_cast<void **>(&out.CRes_Demand), &manifest.renderTextureCallsites[0]},
            {reinterpret_cast<void **>(&out.DrawBindTexture), &manifest.renderTextureCallsites[1]},
            {reinterpret_cast<void **>(&out.DrawDisable), &manifest.renderTextureCallsites[2]},
            {reinterpret_cast<void **>(&out.DrawColor), &manifest.renderTextureCallsites[3]},
            {reinterpret_cast<void **>(&out.DrawPushState), &manifest.renderTextureCallsites[4]},
            {reinterpret_cast<void **>(&out.DrawColorTone), &manifest.renderTextureCallsites[5]},
            {reinterpret_cast<void **>(&out.DrawBegin), &manifest.renderTextureCallsites[6]},
            {reinterpret_cast<void **>(&out.DrawTexCoord), &manifest.renderTextureCallsites[7]},
            {reinterpret_cast<void **>(&out.DrawVertex), &manifest.renderTextureCallsites[8]},
            {reinterpret_cast<void **>(&out.DrawEnd), &manifest.renderTextureCallsites[9]},
            {reinterpret_cast<void **>(&out.DrawPopState), &manifest.renderTextureCallsites[10]},
        };

        const auto moduleInfo = core::get_module_span(nullptr);
        bool allResolved = true;
        for (const auto &func: functions) {
            const auto *desc = func.desc;
            const auto *insn = reinterpret_cast<const void *>(renderTextureVA + desc->offset);

            void *addr = core::rel32_target_checked(insn,
                                                    desc->opcode,
                                                    desc->displacementOffset,
                                                    desc->instructionSize);
            if (!addr) {
                const auto *messageLevel = desc->required ? "Failed" : "Skipped";
                if (desc->required) {
                    LOG_ERROR("{} to resolve {} at offset 0x{:X} (expected {} opcode 0x{:02X})",
                              messageLevel,
                              desc->name,
                              desc->offset,
                              instruction_kind_name(desc->kind),
                              desc->opcode);
                    allResolved = false;
                } else {
                    LOG_WARN("{} optional {} at offset 0x{:X} (expected {} opcode 0x{:02X})",
                             messageLevel,
                             desc->name,
                             desc->offset,
                             instruction_kind_name(desc->kind),
                             desc->opcode);
                }
            } else if (moduleInfo && !is_address_in_module(*moduleInfo, addr)) {
                if (desc->required) {
                    LOG_ERROR("Resolved {} outside the game module: 0x{:X}",
                              desc->name,
                              reinterpret_cast<uintptr_t>(addr));
                    allResolved = false;
                } else {
                    LOG_WARN("Resolved optional {} outside the game module: 0x{:X}",
                             desc->name,
                             reinterpret_cast<uintptr_t>(addr));
                }
            } else {
                *func.target = addr;
                LOG_DEBUG("Resolved {} via {} at 0x{:X}: 0x{:X}",
                          desc->name,
                          instruction_kind_name(desc->kind),
                          renderTextureVA + desc->offset,
                          reinterpret_cast<uintptr_t>(addr));
            }
        }

        if (allResolved) {
            LOG_INFO("Successfully resolved all draw API functions from RenderTexture at 0x{:X}", renderTextureVA);
        } else {
            LOG_WARN("Some draw API functions failed to resolve");
        }

        return allResolved;
    }

    bool ensure_texture_params(const core::EngineConfig &cfg, const DrawApi &api, int textureId) {
        auto &gl = gl::get_gl_functions();
        if (!gl.valid) {
            return false;
        }

        // Short-circuit for invalid texture IDs
        if (textureId == 0 || textureId == -1) {
            return true; // Not an error, just nothing to configure
        }

        try {
            static std::atomic enhancementCount{0};
            const int callCount = enhancementCount.fetch_add(1);
            const bool logDetails = (callCount < 3) || cfg.enableVerboseLogging;

            if (logDetails) {
                LOG_DEBUG("Applying GL texture enhancements (call #{})", callCount + 1);
            }

            // Check if we have a valid texture bound
            if (gl.glGetIntegerv) {
                int boundTexture = 0;
                gl.glGetIntegerv(gl::TEXTURE_BINDING_2D, &boundTexture);
                if (!gl::check_error("glGetIntegerv TEXTURE_BINDING_2D")) {
                    if (logDetails)
                        LOG_WARN("  ! Cannot query bound texture - GL context may be invalid");
                    return true;
                }
                if (boundTexture == 0) {
                    if (logDetails)
                        LOG_WARN("  ! No texture bound - skipping GL parameter setup");
                    return true;
                }
                if (logDetails)
                    LOG_DEBUG("  Configuring texture ID: {}", boundTexture);
            }

            // Set wrapping and basic filtering
            gl.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
            if (!gl::check_error("glTexParameteri WRAP_S")) {
                LOG_WARN("  ! Failed to set texture wrap S - may not be a valid texture");
                return true;
            }

            gl.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
            if (!gl::check_error("glTexParameteri WRAP_T")) {
                LOG_WARN("  ! Failed to set texture wrap T");
                return true;
            }

            gl.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
            if (!gl::check_error("glTexParameteri MAG_FILTER")) {
                LOG_WARN("  ! Failed to set mag filter");
                return true;
            }

            // Always use linear filtering
            gl.glTexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
            if (gl::check_error("glTexParameteri MIN_FILTER linear")) {
                if (logDetails)
                    LOG_INFO("  ✓ Linear filtering enabled");
            } else {
                LOG_WARN("  ! Failed to set min filter");
                return true;
            }

            // Apply anisotropic filtering if configured
            if (cfg.enableAnisotropicFiltering && gl.glTexParameterf && gl.glGetIntegerv) {
                int maxAniso = 0;
                gl.glGetIntegerv(gl::MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
                if (gl::check_error("glGetIntegerv MAX_ANISOTROPY") && maxAniso > 0) {
                    float want = std::min(cfg.maxAnisotropy, (float) maxAniso);
                    gl.glTexParameterf(gl::TEXTURE_2D, gl::TEXTURE_MAX_ANISOTROPY_EXT, want);
                    if (gl::check_error("glTexParameterf ANISOTROPY")) {
                        if (logDetails)
                            LOG_INFO("  ✓ Anisotropic filtering: {:.1f}x (max: {})", want, maxAniso);

                        if (gl.glGetTexParameteriv) {
                            int actualAniso = 0;
                            gl.glGetTexParameteriv(gl::TEXTURE_2D, gl::TEXTURE_MAX_ANISOTROPY_EXT, &actualAniso);
                            if (gl::check_error("glGetTexParameteriv ANISOTROPY verify")) {
                                if (logDetails)
                                    LOG_INFO("  ✓ Verified anisotropy set to: {}", actualAniso);
                            }
                        }
                    } else {
                        LOG_ERROR("  ✗ Anisotropic filtering failed to apply");
                    }
                } else {
                    if (logDetails)
                        LOG_WARN("  ! Anisotropic filtering not supported (max: {})", maxAniso);
                }
            }

            // LOD bias
            if (gl.glTexParameterf) {
                gl.glTexParameterf(gl::TEXTURE_2D, gl::TEXTURE_LOD_BIAS, cfg.lodBias);
                if (gl::check_error("glTexParameterf LOD_BIAS")) {
                    if (logDetails)
                        LOG_INFO("  ✓ LOD bias set to: {:.2f}", cfg.lodBias);
                } else {
                    LOG_ERROR("  ✗ LOD bias failed to apply");
                }
            }

            if (logDetails) {
                LOG_DEBUG("GL texture enhancement complete for texture ID");
            }

            return true;
        } catch (...) {
            LOG_ERROR("Exception during GL texture parameter setup");
            return false;
        }
    }
}
