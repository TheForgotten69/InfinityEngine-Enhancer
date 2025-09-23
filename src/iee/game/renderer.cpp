#include "renderer.h"
#include "opengl_types.h"
#include "game_addrs.h"
#include "iee/core/config.h"
#include "iee/core/logger.h"
#include "iee/core/pattern_scanner.h"
#include <windows.h>
#include <algorithm>
#include <atomic>

namespace iee::game {
    bool resolve_draw_api(DrawApi &out, std::uintptr_t renderTextureVA) {
        if (!renderTextureVA) {
            LOG_ERROR("RenderTexture address is null");
            return false;
        }

        struct APIFunction {
            void **target;
            const char *name;
            size_t offset;
        };

        APIFunction functions[] = {
            {reinterpret_cast<void **>(&out.CRes_Demand), "CRes_Demand", GameOffsets::RT_CRES_DEMAND_OFFSET},
            {
                reinterpret_cast<void **>(&out.DrawBindTexture), "DrawBindTexture",
                GameOffsets::RT_DRAW_BIND_TEXTURE_OFFSET
            },
            {reinterpret_cast<void **>(&out.DrawDisable), "DrawDisable", GameOffsets::RT_DRAW_DISABLE_OFFSET},
            {reinterpret_cast<void **>(&out.DrawColor), "DrawColor", GameOffsets::RT_DRAW_COLOR_OFFSET},
            {reinterpret_cast<void **>(&out.DrawPushState), "DrawPushState", GameOffsets::RT_DRAW_PUSH_STATE_OFFSET},
            {reinterpret_cast<void **>(&out.DrawColorTone), "DrawColorTone", GameOffsets::RT_DRAW_COLOR_TONE_OFFSET},
            {reinterpret_cast<void **>(&out.DrawBegin), "DrawBegin", GameOffsets::RT_DRAW_BEGIN_OFFSET},
            {reinterpret_cast<void **>(&out.DrawTexCoord), "DrawTexCoord", GameOffsets::RT_DRAW_TEX_COORD_OFFSET},
            {reinterpret_cast<void **>(&out.DrawVertex), "DrawVertex", GameOffsets::RT_DRAW_VERTEX_OFFSET},
            {reinterpret_cast<void **>(&out.DrawEnd), "DrawEnd", GameOffsets::RT_DRAW_END_OFFSET},
            {reinterpret_cast<void **>(&out.DrawPopState), "DrawPopState", GameOffsets::RT_DRAW_POP_STATE_OFFSET}
        };

        bool allResolved = true;
        for (const auto &func: functions) {
            // Point to the instruction at renderTextureVA + func.offset
            auto *insn = reinterpret_cast<void *>(renderTextureVA + func.offset);
            // E8 xx xx xx xx => displacement starts at +1, total size 5
            if (void *addr = core::rel32_target(insn, 1, 5); !addr) {
                LOG_ERROR("Failed to resolve {} at offset 0x{:X}", func.name, func.offset);
                allResolved = false;
            } else {
                *func.target = addr;
                LOG_DEBUG("Resolved {}: 0x{:X}", func.name, reinterpret_cast<uintptr_t>(addr));
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
            if (!gl::check_error("glTexParameteri MIN_FILTER linear")) {
                if (logDetails)
                    LOG_INFO("  ✓ Linear filtering enabled");
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
