#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <memory>
#include <thread>
#include <unordered_set>
#include <cmath>

#include "MinHook.h"
#include "../src/iee/core/Config.h"
#include "Logger.h"
#include "PatternScanner.h"
#include <winnt.h>


struct TILE_CODE;
//region Global Constants
static EngineConfig g_config;
// Atomic counters for thread safety
static std::atomic<int> g_lastTexId{-1};
// HD renderer state management
static bool g_isRenderHookActive = false;
// Per-area scaling factor detection
static std::atomic<int> g_currentAreaScaleFactor{1};
static std::atomic<bool> g_scaleDetected{false};
static std::atomic<int> g_detectionCount{0};
static constexpr int DRAW_TRIANGLES = 2;
static constexpr int DRAW_TRIANGLE_STRIP = 3;
static constexpr int DRAW_TEXTURE_2D = 1;
static constexpr int DRAW_BLEND = 4;
static constexpr int DRAW_SRC_ALPHA = 5;
static constexpr int DRAW_ONE_MINUS_SRC_ALPHA = 6;
// endregion

// region Game Struct
struct GameAddresses {
    uintptr_t LoadArea;
    uintptr_t RenderTexture;
    uintptr_t RenderFog;
    bool initialized;
};
using PFN_GetTileCode = void(*)(void* /*this*/, short /*tileIndex*/, TILE_CODE* /*out*/);
static PFN_GetTileCode g_GetTileCode = nullptr;



static GameAddresses g_addresses = {0};

// TIS Resource structures
#pragma pack(push, 1)
struct CResRef {
    char m_resRef[8]{};

    CResRef() {
        memset(m_resRef, 0, 8);
    }

    explicit CResRef(const char *name) {
        memset(m_resRef, 0, 8);
        if (name) {
            strncpy_s(m_resRef, 8, name, _TRUNCATE);
        }
    }
};

struct CResTIS_Header {
    char signature[4]; // Offset 0x00
    char version[4]; // Offset 0x04
    uint32_t tileCount; // Offset 0x08
    uint32_t tileDataBlockLen; // Offset 0x0C
    uint32_t headerSizeOffset; // Offset 0x10
    uint32_t headerSize; // Offset 0x14
    uint32_t tileDimension; // Offset 0x18 <-- This is what we need!
};
#pragma pack(pop)

// Function pointer for the engine's resource loader
// Confirmed offsets for CGameArea structure
static constexpr uintptr_t X64_OFFSET_CGAMEAREA_CINFINITY = 0x5c8;
static constexpr uintptr_t X64_OFFSET_CINFINITY_MPTIS = 0x8;

#pragma pack(push,1)
struct CResTile {
    void *tis;
    int32_t tileIndex;
};

struct CResTIS_MIN {
    uint8_t pad40[0x40];
    void *tileTable; // +0x40 -> PVRZTileEntry[]
    uint32_t tileDataBlockLen; // +0x48 == 12
    uint32_t tileDimension; // +0x4C == 64 or 256
};

struct PVRZTileEntry {
    int32_t page, u, v;
};
#pragma pack(pop)
// endregion

// region Game Functions
// draw API (resolved from RenderTexture)
using DrawBegin_t = void(*)(int);
using DrawEnd_t = void(*)();
using DrawPushState_t = void(*)();
using DrawPopState_t = void(*)();
using DrawTexCoord_t = void(*)(int, int);
using DrawVertex_t = void(*)(int, int);
using DrawBindTexture_t = void(*)(int);
using DrawDisable_t = void(*)(int);
using DrawEnable_t = void(*)(int);
using DrawBlendFunc_t = void(*)(int, int);
using DrawColor_t = unsigned long(*)(unsigned long);
using DrawColorTone_t = void(*)(int);
using CRes_Demand_t = void*(*)(void *);
using Fn_LoadArea = void* (*)(void *, void *, unsigned char, unsigned char, unsigned char);
using Fn_RenderTexture = void (*)(void *, int, void *, int, int, unsigned long);
using Fn_RenderFog = void (*)(void *, void *, void *); // CInfinity*, CVidMode*, CVisibilityMap*
using Fn_DrawColorTone = void(*)(int);

static DrawBegin_t DrawBegin{};
static DrawEnd_t DrawEnd{};
static DrawPushState_t DrawPushState{};
static DrawPopState_t DrawPopState{};
static DrawTexCoord_t DrawTexCoord{};
static DrawVertex_t DrawVertex{};
static DrawBindTexture_t DrawBindTexture{};
static DrawDisable_t DrawDisable{};
static DrawEnable_t DrawEnable{};
static DrawBlendFunc_t DrawBlendFunc{};
static DrawColor_t DrawColor{};
static DrawColorTone_t DrawColorTone{};
static CRes_Demand_t CRes_Demand{};
static Fn_LoadArea oLoadArea{};
static Fn_RenderTexture oRenderTexture{};
static Fn_DrawColorTone oDrawColorTone = nullptr;
// endregion

// region helpers
static inline uintptr_t BASE() { return (uintptr_t) GetModuleHandleW(nullptr); }

// endregion

#include <winnt.h>

// returns function start for any RIP inside it (or 0 on failure)
static uintptr_t GetFuncStartFromUnwind(uintptr_t inner) {
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64) inner, &imageBase, nullptr);
    if (!rf) return 0;
    return (uintptr_t) (imageBase + rf->BeginAddress);
}



static bool ScanForGameFunctions() {
    // Performance optimization: only scan once
    static std::once_flag scanFlag;
    static bool scanResult = false;

    std::call_once(scanFlag, []() {
        ModuleInfo mod = PatternScanner::GetModuleInfo();
        if (!mod.base || !mod.size) {
            LOG_ERROR("Failed to get module information");
            scanResult = false;
            return;
        }

        LOG_INFO("Scanning module: Base=0x{:X}, Size=0x{:X}", mod.base, mod.size);

        bool success = false;

        if (g_config.enableSignatureScanning) {
            LOG_INFO("Signature scanning enabled - using enhanced PolyHook + fallback patterns");

            // Real patterns extracted from actual running BGEE executable
            // LoadArea function: 40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 48 FD FF FF
            const char *loadAreaPattern =
                    "\x40\x55\x53\x56\x57\x41\x54\x41\x55\x41\x56\x41\x57\x48\x8D\xAC\x24\x48\xFD\xFF\xFF";
            const char *loadAreaMask = "xxxxxxxxxxxxxxxxxxxxx"; // All bytes exact match

            // RenderTexture function: 48 8B C4 44 89 48 20 48 83 EC 48 48 89 58 08 8B DA 48 89 68 10
            const char *renderTexturePattern =
                    "\x48\x8B\xC4\x44\x89\x48\x20\x48\x83\xEC\x48\x48\x89\x58\x08\x8B\xDA\x48\x89\x68\x10";
            const char *renderTextureMask = "xxxxxxxxxxxxxxxxxxxxx"; // All bytes exact match

            LOG_DEBUG("Searching for LoadArea pattern...");
            g_addresses.LoadArea = PatternScanner::FindPattern(loadAreaPattern, loadAreaMask);

            LOG_DEBUG("Searching for RenderTexture pattern...");
            g_addresses.RenderTexture = PatternScanner::FindPattern(renderTexturePattern, renderTextureMask);

            // Skip dangerous pattern scanning for RenderFog - use known RVA only
            LOG_DEBUG("Using known RVA for RenderFog (pattern scanning disabled for safety)");
            g_addresses.RenderFog = mod.base + g_config.fallbackRenderFogRVA;

            success = g_addresses.LoadArea && g_addresses.RenderTexture;

            if (success) {
                LOG_INFO("Pattern scanning SUCCESS:");
                uintptr_t foundLoadAreaRVA = g_addresses.LoadArea - mod.base;
                uintptr_t foundRenderTextureRVA = g_addresses.RenderTexture - mod.base;
                uintptr_t foundRenderFogRVA = g_addresses.RenderFog - mod.base;

                LOG_INFO("  LoadArea: 0x{:X} (RVA: 0x{:X})", g_addresses.LoadArea, foundLoadAreaRVA);
                LOG_INFO("  RenderTexture: 0x{:X} (RVA: 0x{:X})", g_addresses.RenderTexture, foundRenderTextureRVA);
                LOG_INFO("  RenderFog: 0x{:X} (RVA: 0x{:X})", g_addresses.RenderFog, foundRenderFogRVA);

                // Verify against expected RVAs
                if (foundLoadAreaRVA == g_config.fallbackLoadAreaRVA) {
                    LOG_INFO("  ✓ LoadArea RVA matches expected 0x{:X}", g_config.fallbackLoadAreaRVA);
                } else {
                    LOG_WARN("  ⚠ LoadArea RVA differs from expected 0x{:X}", g_config.fallbackLoadAreaRVA);
                }

                if (foundRenderTextureRVA == g_config.fallbackRenderTextureRVA) {
                    LOG_INFO("  ✓ RenderTexture RVA matches expected 0x{:X}", g_config.fallbackRenderTextureRVA);
                } else {
                    LOG_WARN("  ⚠ RenderTexture RVA differs from expected 0x{:X}", g_config.fallbackRenderTextureRVA);
                }

                if (foundRenderFogRVA == g_config.fallbackRenderFogRVA) {
                    LOG_INFO("  ✓ RenderFog RVA matches expected 0x{:X}", g_config.fallbackRenderFogRVA);
                } else {
                    LOG_WARN("  ⚠ RenderFog RVA differs from expected 0x{:X}", g_config.fallbackRenderFogRVA);
                }

                g_addresses.initialized = true;
            } else {
                LOG_WARN("Pattern scanning failed - functions may have changed");
                if (!g_addresses.LoadArea)
                    LOG_WARN("  LoadArea pattern not found");
                if (!g_addresses.RenderTexture)
                    LOG_WARN("  RenderTexture pattern not found");
                // RenderFog handled separately above

                LOG_INFO("Dumping actual bytes for analysis:");
                // Dump bytes at the known working addresses to see what's really there
                PatternScanner::DumpBytesAtAddress(mod.base + g_config.fallbackLoadAreaRVA, "LoadArea (fallback RVA)");
                PatternScanner::DumpBytesAtAddress(mod.base + g_config.fallbackRenderTextureRVA,
                                                   "RenderTexture (fallback RVA)");
            }
        } else {
            LOG_INFO("Signature scanning disabled - using known RVAs");
        }

        if (!success) {
            LOG_INFO("Using configured fallback RVAs (known working addresses)");
            g_addresses.LoadArea = mod.base + g_config.fallbackLoadAreaRVA;
            g_addresses.RenderTexture = mod.base + g_config.fallbackRenderTextureRVA;
            g_addresses.RenderFog = mod.base + g_config.fallbackRenderFogRVA;
            g_addresses.initialized = true;

            LOG_INFO("Fallback addresses:");
            LOG_INFO("  LoadArea: 0x{:X} (RVA: 0x{:X})", g_addresses.LoadArea, g_config.fallbackLoadAreaRVA);
            LOG_INFO("  RenderTexture: 0x{:X} (RVA: 0x{:X})", g_addresses.RenderTexture,
                     g_config.fallbackRenderTextureRVA);
            LOG_INFO("  RenderFog: 0x{:X} (RVA: 0x{:X})", g_addresses.RenderFog, g_config.fallbackRenderFogRVA);
        }


        scanResult = g_addresses.initialized;
    });

    return scanResult;
}

static bool ResolveDrawAPI() {
    if (!g_addresses.initialized) {
        LOG_ERROR("Game addresses not initialized before ResolveDrawAPI");
        return false;
    }

    const uintptr_t rt = g_addresses.RenderTexture;
    if (!rt) {
        LOG_ERROR("RenderTexture address is null");
        return false;
    }

    // Resolve all function pointers with error checking
    struct APIFunction {
        void **target;
        const char *name;
        size_t offset;
    };

    APIFunction functions[] = {
        {reinterpret_cast<void **>(&CRes_Demand), "CRes_Demand", 0x36},
        {reinterpret_cast<void **>(&DrawBindTexture), "DrawBindTexture", 0x6E},
        {reinterpret_cast<void **>(&DrawDisable), "DrawDisable", 0x7F},
        {reinterpret_cast<void **>(&DrawEnable), "DrawEnable", 0x85}, // Estimated offset near DrawDisable
        {reinterpret_cast<void **>(&DrawBlendFunc), "DrawBlendFunc", 0x87}, // Estimated offset
        {reinterpret_cast<void **>(&DrawColor), "DrawColor", 0x89},
        {reinterpret_cast<void **>(&DrawPushState), "DrawPushState", 0x91},
        {reinterpret_cast<void **>(&DrawColorTone), "DrawColorTone", 0xB6},
        {reinterpret_cast<void **>(&DrawBegin), "DrawBegin", 0xC0},
        {reinterpret_cast<void **>(&DrawTexCoord), "DrawTexCoord", 0xCD},
        {reinterpret_cast<void **>(&DrawVertex), "DrawVertex", 0xDB},
        {reinterpret_cast<void **>(&DrawEnd), "DrawEnd", 0x17A},
        {reinterpret_cast<void **>(&DrawPopState), "DrawPopState", 0x1AD}
    };

    bool allResolved = true;
    for (const auto &func: functions) {
        uintptr_t addr = PatternScanner::AbsFromRel32(rt + func.offset);
        if (!addr) {
            // Special handling for DrawEnable and DrawBlendFunc - they're optional for fog
            if (strcmp(func.name, "DrawEnable") == 0 || strcmp(func.name, "DrawBlendFunc") == 0) {
                LOG_WARN("Failed to resolve {} at offset 0x{:X} - fog effects may be limited", func.name, func.offset);
                *func.target = nullptr; // Will be checked before use
            } else {
                LOG_ERROR("Failed to resolve {} at offset 0x{:X}", func.name, func.offset);
                allResolved = false;
            }
        } else {
            *func.target = (void *) addr;
            if (g_config.enableVerboseLogging) {
                LOG_DEBUG("Resolved {}: 0x{:X}", func.name, addr);
            }
        }
    }

    if (allResolved) {
        LOG_INFO("Successfully resolved all draw API functions from RenderTexture at 0x{:X}", rt);
    } else {
        LOG_WARN("Some draw API functions failed to resolve");
    }

    return allResolved;
}

static void *GL_GetProc(const char *name) {
    // 1) try extension loader
    static auto p_wglGetProcAddress =
            reinterpret_cast<PROC (WINAPI*)(LPCSTR)>(
                GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetProcAddress"));
    if (p_wglGetProcAddress) {
        if (PROC p = p_wglGetProcAddress(name)) return (void *) p;
    }
    // 2) fall back to core 1.1 exports
    return (void *) GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name);
}

// Shader/Program tracking for debugging
struct ShaderInfo {
    unsigned int programId;
    std::string name;
    int lastTone;
};

static std::vector<ShaderInfo> g_shaderHistory;
static std::mutex g_shaderMutex;

// GL error checking helper
static const char *GLErrorToString(unsigned int error) {
    switch (error) {
        case 0x0500: return "GL_INVALID_ENUM";
        case 0x0501: return "GL_INVALID_VALUE";
        case 0x0502: return "GL_INVALID_OPERATION";
        case 0x0505: return "GL_OUT_OF_MEMORY";
        case 0x0506: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        case 0x0507: return "GL_CONTEXT_LOST";
        default: return "UNKNOWN_GL_ERROR";
    }
}

using PFN_glGetIntegerv  = void (APIENTRY*)(unsigned, int*);
using PFN_glBindTexture  = void (APIENTRY*)(unsigned, unsigned);
static PFN_glGetIntegerv  fog_glGetIntegerv  = (PFN_glGetIntegerv) GL_GetProc("glGetIntegerv");
static PFN_glBindTexture  fog_glBindTexture  = (PFN_glBindTexture) GL_GetProc("glBindTexture");
static constexpr unsigned GL_TEXTURE_2D_ENUM = 0x0DE1;
static constexpr unsigned GL_TEXTURE_BINDING_2D_ENUM = 0x8069;

static bool CheckGLError(const char *operation) {
    using PFN_glGetError = unsigned int (APIENTRY*)();
    static auto glGetError = (PFN_glGetError) GL_GetProc("glGetError");

    if (!glGetError) return true; // Can't check, assume success

    unsigned int error = glGetError();
    if (error != 0) {
        LOG_ERROR("GL ERROR after {}: 0x{:x} ({})", operation, error, GLErrorToString(error));
        return false;
    }
    return true;
}

// --- GetTileCode binding (use your Ghidra RVA) ---
static bool BindGetTileCodeFromGhidra() {
    // RVA from your decompilation: 0x140256398 => RVA 0x256398
    uintptr_t cand = BASE() + 0x256398;

    // Snap to the real start using unwind info (works on all x64 builds)
    if (uintptr_t start = GetFuncStartFromUnwind(cand)) {
        cand = start;
    }

    // Light sanity: most x64 prologues start 0x40/0x48/0x55 (REX, MOV RSP, push rbp etc.)
    uint8_t b0 = 0x00;
    __try { b0 = *(uint8_t*)cand; }
    __except(EXCEPTION_EXECUTE_HANDLER) { b0 = 0; }

    if (!(b0 == 0x40 || b0 == 0x48 || b0 == 0x55)) {
        LOG_WARN("GetTileCode prologue looks odd at 0x{:X} (first byte {:02X})", cand, b0);
    }

    g_GetTileCode = reinterpret_cast<PFN_GetTileCode>(cand);
    LOG_INFO("GetTileCode bound at 0x{:X} (RVA 0x{:X})", cand, cand - BASE());
    return g_GetTileCode != nullptr;
}



static bool EnsureNiceSamplerForCurrentGLTex() {
    using PFN_glTexParameteri = void (APIENTRY*)(unsigned, unsigned, int);
    using PFN_glTexParameterf = void (APIENTRY*)(unsigned, unsigned, float);
    using PFN_glGenerateMipmap = void (APIENTRY*)(unsigned);
    using PFN_glGetIntegerv = void (APIENTRY*)(unsigned, int *);
    using PFN_glGetTexParameteriv = void (APIENTRY*)(unsigned, unsigned, int *);

    // Thread-safe initialization of GL function pointers
    static std::once_flag glFuncsInitialized;
    static PFN_glTexParameteri glTexParameteri = nullptr;
    static PFN_glTexParameterf glTexParameterf = nullptr;
    static PFN_glGenerateMipmap glGenerateMipmap = nullptr;
    static PFN_glGetIntegerv glGetIntegerv = nullptr;
    static PFN_glGetTexParameteriv glGetTexParameteriv = nullptr;
    static bool glFuncsValid = false;
    static bool diagLogged = false;

    std::call_once(glFuncsInitialized, []() {
        glTexParameteri = (PFN_glTexParameteri) GL_GetProc("glTexParameteri");
        glTexParameterf = (PFN_glTexParameterf) GL_GetProc("glTexParameterf");
        glGenerateMipmap = (PFN_glGenerateMipmap) GL_GetProc("glGenerateMipmap");
        glGetIntegerv = (PFN_glGetIntegerv) GL_GetProc("glGetIntegerv");
        glGetTexParameteriv = (PFN_glGetTexParameteriv) GL_GetProc("glGetTexParameteriv");

        glFuncsValid = (glTexParameteri != nullptr);

        LOG_INFO("GL Function availability:");
        LOG_INFO("  glTexParameteri: {}", glTexParameteri ? "OK" : "MISSING");
        LOG_INFO("  glTexParameterf: {}", glTexParameterf ? "OK" : "MISSING");
        LOG_INFO("  glGenerateMipmap: {}", glGenerateMipmap ? "OK" : "MISSING");
        LOG_INFO("  glGetIntegerv: {}", glGetIntegerv ? "OK" : "MISSING");
        LOG_INFO("  glGetTexParameteriv: {}", glGetTexParameteriv ? "OK" : "MISSING");

        if (!glFuncsValid) {
            LOG_ERROR("Essential GL functions not available");
        }
    });

    if (!glFuncsValid) {
        return false; // Essential functions not available
    }

    // Apply texture parameters safely with verification
    try {
        static std::atomic<int> enhancementCount{0};
        int callCount = enhancementCount.fetch_add(1);
        bool logDetails = (callCount < 3) || g_config.enableVerboseLogging;

        if (logDetails) {
            LOG_DEBUG("Applying GL texture enhancements (call #{})", callCount + 1);
        }

        // Check if we have a valid texture bound
        using PFN_glGetIntegerv = void (APIENTRY*)(unsigned int, int *);
        static auto glGetIntegerv_local = (PFN_glGetIntegerv) GL_GetProc("glGetIntegerv");

        if (glGetIntegerv_local) {
            int boundTexture = 0;
            glGetIntegerv_local(0x8069/*GL_TEXTURE_BINDING_2D*/, &boundTexture);
            if (!CheckGLError("glGetIntegerv TEXTURE_BINDING_2D")) {
                if (logDetails)
                    LOG_WARN("  ! Cannot query bound texture - GL context may be invalid");
                return true; // Not our fault, context is probably bad
            }
            if (boundTexture == 0) {
                if (logDetails)
                    LOG_WARN("  ! No texture bound - skipping GL parameter setup");
                return true; // Not an error, just nothing to configure
            }
            if (logDetails)
                LOG_DEBUG("  Configuring texture ID: {}", boundTexture);
        } else {
            if (logDetails)
                LOG_WARN("  ! Cannot query bound texture - glGetIntegerv unavailable");
            // Continue anyway, maybe the texture is bound and we just can't check
        }

        // Set wrapping and basic filtering
        glTexParameteri(0x0DE1/*GL_TEXTURE_2D*/, 0x2802/*GL_TEXTURE_WRAP_S*/, 0x812F/*GL_CLAMP_TO_EDGE*/);
        if (!CheckGLError("glTexParameteri WRAP_S")) {
            LOG_WARN("  ! Failed to set texture wrap S - may not be a valid texture");
            return true; // Don't fail completely
        }

        glTexParameteri(0x0DE1, 0x2803/*GL_TEXTURE_WRAP_T*/, 0x812F);
        if (!CheckGLError("glTexParameteri WRAP_T")) {
            LOG_WARN("  ! Failed to set texture wrap T");
            return true;
        }

        glTexParameteri(0x0DE1, 0x2800/*GL_TEXTURE_MAG_FILTER*/, 0x2601/*GL_LINEAR*/);
        if (!CheckGLError("glTexParameteri MAG_FILTER")) {
            LOG_WARN("  ! Failed to set mag filter");
            return true;
        }

        // Configure min filter based on trilinear setting
        if (g_config.enableTrilinearFiltering) {
            glTexParameteri(0x0DE1, 0x2801/*GL_TEXTURE_MIN_FILTER*/, 0x2703/*GL_LINEAR_MIPMAP_LINEAR*/);
            if (CheckGLError("glTexParameteri MIN_FILTER trilinear")) {
                if (logDetails)
                    LOG_INFO("  ✓ Trilinear filtering enabled");
            } else {
                LOG_WARN("  ! Trilinear filtering failed - falling back to linear");
                glTexParameteri(0x0DE1, 0x2801/*GL_TEXTURE_MIN_FILTER*/, 0x2601/*GL_LINEAR*/);
                CheckGLError("glTexParameteri MIN_FILTER linear fallback");
            }
        } else {
            glTexParameteri(0x0DE1, 0x2801/*GL_TEXTURE_MIN_FILTER*/, 0x2601/*GL_LINEAR*/);
            if (CheckGLError("glTexParameteri MIN_FILTER linear")) {
                if (logDetails)
                    LOG_INFO("  ✓ Linear filtering enabled");
            }
        }

        // Generate mipmaps if configured
        if (g_config.enableMipmaps && glGenerateMipmap) {
            // glGenerateMipmap(0x0DE1/*GL_TEXTURE_2D*/);
            if (CheckGLError("glGenerateMipmap")) {
                if (logDetails)
                    LOG_INFO("  ✓ Mipmaps generated successfully");
            } else {
                LOG_WARN("  ! Mipmap generation failed - continuing without mipmaps");
                // Don't fail completely - mipmaps are an enhancement, not essential
            }
        }

        // Apply anisotropic filtering if configured
        if (g_config.enableAnisotropicFiltering && glTexParameterf && glGetIntegerv) {
            int maxAniso = 0;
            glGetIntegerv(0x84FF/*GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT*/, &maxAniso);
            if (CheckGLError("glGetIntegerv MAX_ANISOTROPY") && maxAniso > 0) {
                float want = std::min(g_config.maxAnisotropy, (float) maxAniso);
                glTexParameterf(0x0DE1, 0x84FE/*GL_TEXTURE_MAX_ANISOTROPY_EXT*/, want);
                if (CheckGLError("glTexParameterf ANISOTROPY")) {
                    if (logDetails)
                        LOG_INFO("  ✓ Anisotropic filtering: {:.1f}x (max: {})", want, maxAniso);

                    // Verify it was actually set
                    if (glGetTexParameteriv) {
                        int actualAniso = 0;
                        glGetTexParameteriv(0x0DE1, 0x84FE, &actualAniso);
                        if (CheckGLError("glGetTexParameteriv ANISOTROPY verify")) {
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

        // Apply LOD bias
        if (glTexParameterf) {
            constexpr unsigned GL_TEXTURE_LOD_BIAS = 0x8501;
            glTexParameterf(0x0DE1/*GL_TEXTURE_2D*/, GL_TEXTURE_LOD_BIAS, g_config.lodBias);
            if (CheckGLError("glTexParameterf LOD_BIAS")) {
                if (logDetails)
                    LOG_INFO("  ✓ LOD bias set to: {:.2f}", g_config.lodBias);
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


static void Hook_DrawColorTone(int mode) {
    oDrawColorTone(mode);
}

static bool GetTileInfo(void *pVidTile, CResTIS_MIN **outTis, int *outIdx, const PVRZTileEntry **outTbl) {
    if (!pVidTile || !outTis || !outIdx || !outTbl) {
        LOG_ERROR("GetTileInfo - Null parameters");
        return false;
    }

    *outTis = nullptr;
    *outIdx = -1;
    *outTbl = nullptr;

    // Basic sanity check on the read address (game objects are typically in heap, not module memory)
    uintptr_t readAddr = (uintptr_t) pVidTile + 0x100;
    if (readAddr < 0x1000) {
        LOG_ERROR("GetTileInfo - Suspicious read address 0x{:X}", readAddr);
        return false;
    }

    auto pRes = *reinterpret_cast<CResTile **>(readAddr);
    if (!pRes) {
        // Don't spam logs for common case
        return false;
    }

    if (!pRes->tis) {
        // Don't spam logs for common case
        return false;
    }

    // Safely call CRes_Demand if available
    if (CRes_Demand) {
        try {
            CRes_Demand(pRes->tis);
        } catch (...) {
            LOG_ERROR("Exception in CRes_Demand call");
            return false;
        }
    }

    auto tis = (CResTIS_MIN *) pRes->tis;
    if (!tis) {
        LOG_WARN("GetTileInfo - tis is null after demand");
        return false;
    }

    // Validate TIS structure - tileDataBlockLen is dynamic based on map size
    if (tis->tileDataBlockLen == 0) {
        LOG_WARN("GetTileInfo - Empty tileDataBlockLen");
        return false;
    }

    if (!tis->tileTable) {
        LOG_WARN("GetTileInfo - tileTable is null");
        return false;
    }

    *outTis = tis;
    *outIdx = pRes->tileIndex;
    *outTbl = static_cast<const PVRZTileEntry *>(tis->tileTable);

    // Reduced verbosity - only log first few successful calls
    if (g_config.enableVerboseLogging) {
        static std::atomic<int> successCount{0};
        int count = successCount.fetch_add(1);
        if (count < 5) {
            LOG_DEBUG("GetTileInfo success - tileIndex: {}", pRes->tileIndex);
        }
    }

    return true;
}


// region Rendering helpers
// globals
static std::unordered_set<int> g_texConfigured; // per-texture one-time setup
static int g_lastTone = -1; // avoid per-tile tone swaps
static void *g_lastTis = nullptr; // avoid per-tile demands

// region Enhanced Fog of War System
// Compatibility helper for older C++ standards
#ifndef __cpp_lib_clamp
template<typename T>
constexpr const T &clamp(const T &v, const T &lo, const T &hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}
#else
using std::clamp;
#endif

// Data structures matching the decompiled CInfinity and CVisibilityMap
struct CRect {
    int left, top, right, bottom;
};

struct CInfinity_FOG {
    char padding1[0x50];
    int nOffsetX; // +0x50
    int nOffsetY; // +0x54
    int nTilesX; // +0x58
    int nTilesY; // +0x5C
    char padding2[0x18];
    CRect rViewPort; // +0x78 (left at +0x78, top at +0x7C)
    char padding3[0x30];
    int nVisibleTilesX; // +0xA8
    int nVisibleTilesY; // +0xAC
    char padding4[0x8];
    int nCurrentTileX; // +0xB8
    int nCurrentTileY; // +0xBC
};

struct TILE_CODE {
    uint8_t tileNW;
    uint8_t tileNE;
    uint8_t tileSW;
    uint8_t tileSE;
};

// Enhanced fog globals (per-area)
static int g_fogTexture = 0;
static int g_fogWidth = 0, g_fogHeight = 0; // supersampled dimensions
static std::vector<uint8_t> g_fogMask; // 0=visible, 1=hidden
static std::vector<uint16_t> g_fogDist; // squared distance transform
static std::vector<uint8_t> g_fogAlpha; // final alpha values 0-255
static bool g_fogDirty = true; // needs texture update
static int g_lastAreaTilesX = 0, g_lastAreaTilesY = 0; // detect area changes
static std::atomic<int> g_fogHookCallCount{0}; // track hook calls for debugging
static bool g_firstUploadPending = false; // force upload after texture creation
// globals you already added earlier
using PFN_glGetIntegerv = void (APIENTRY*)(unsigned, int*);
using PFN_glBindTexture = void (APIENTRY*)(unsigned, unsigned);

// Direct OpenGL function pointers for normalized UV drawing
using PFN_glBegin       = void (APIENTRY*)(unsigned);
using PFN_glEnd         = void (APIENTRY*)();
using PFN_glTexCoord2f  = void (APIENTRY*)(float, float);
using PFN_glVertex2i    = void (APIENTRY*)(int, int);
using PFN_glEnable_gl   = void (APIENTRY*)(unsigned);
using PFN_glDisable_gl  = void (APIENTRY*)(unsigned);
using PFN_glBlendFunc_gl= void (APIENTRY*)(unsigned, unsigned);
using PFN_glColor4ub    = void (APIENTRY*)(unsigned char, unsigned char, unsigned char, unsigned char);

// GL function pointers for shader-free immediate mode rendering
using PFN_glUseProgram = void (APIENTRY*)(unsigned);
using PFN_glIsEnabled  = unsigned char (APIENTRY*)(unsigned);
using PFN_glDepthMask  = void (APIENTRY*)(unsigned char);
using PFN_glTexEnvi    = void (APIENTRY*)(unsigned, unsigned, int);
using PFN_glGetBooleanv = void (APIENTRY*)(unsigned, unsigned char*);
using PFN_glColorMask   = void (APIENTRY*)(unsigned char,unsigned char,unsigned char,unsigned char);

static auto glUseProgram_ = (PFN_glUseProgram)GL_GetProc("glUseProgram");
static auto glIsEnabled_  = (PFN_glIsEnabled) GL_GetProc("glIsEnabled");
static auto glDepthMask_  = (PFN_glDepthMask) GL_GetProc("glDepthMask");
static auto glTexEnvi_    = (PFN_glTexEnvi)   GL_GetProc("glTexEnvi");
static auto glGetBooleanv_ = (PFN_glGetBooleanv)GL_GetProc("glGetBooleanv");
static auto glColorMask_   = (PFN_glColorMask)  GL_GetProc("glColorMask");

// Static GL function pointers for direct OpenGL rendering
static auto glBegin_      = (PFN_glBegin)      GL_GetProc("glBegin");
static auto glEnd_        = (PFN_glEnd)        GL_GetProc("glEnd");
static auto glTexCoord2f_ = (PFN_glTexCoord2f) GL_GetProc("glTexCoord2f");
static auto glVertex2i_   = (PFN_glVertex2i)   GL_GetProc("glVertex2i");
static auto glEnable_     = (PFN_glEnable_gl)  GL_GetProc("glEnable");
static auto glDisable_    = (PFN_glDisable_gl) GL_GetProc("glDisable");
static auto glBlendFunc_  = (PFN_glBlendFunc_gl)GL_GetProc("glBlendFunc");
static auto glColor4ub_   = (PFN_glColor4ub)   GL_GetProc("glColor4ub");

// OpenGL constants
static constexpr unsigned GL_TRIANGLE_STRIP = 0x0005;
static constexpr unsigned GL_BLEND_ENUM     = 0x0BE2;
static constexpr unsigned GL_TEXTURE_2D_ENUM_GL = 0x0DE1;
static constexpr unsigned GL_SRC_ALPHA      = 0x0302;
static constexpr unsigned GL_ONE_MINUS_SRC_ALPHA = 0x0303;


// New normalized UV fog drawing function
static void DrawFogQuad_GL_Normalized(int x0,int y0,int x1,int y1, int u0,int v0,int u1,int v1) {
    if (!fog_glBindTexture || !fog_glGetIntegerv ||
        !glBegin_ || !glEnd_ || !glTexCoord2f_ || !glVertex2i_ ||
        !glEnable_ || !glBlendFunc_ || !glColor4ub_) {
        LOG_ERROR("DrawFogQuad_GL_Normalized: Required GL functions not available");
        return;
    }

    // Save & bind
    int prevTex = 0;
    __try { fog_glGetIntegerv(GL_TEXTURE_BINDING_2D_ENUM, &prevTex); } __except(EXCEPTION_EXECUTE_HANDLER) { prevTex = 0; }
    __try { fog_glBindTexture(GL_TEXTURE_2D_ENUM_GL, (unsigned)g_fogTexture); } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("Failed to bind fog texture for normalized drawing");
        return;
    }

    // State (we're inside DrawPushState/DrawPopState already)
    __try {
        if (glEnable_) {
            glEnable_(GL_TEXTURE_2D_ENUM_GL);
            glEnable_(GL_BLEND_ENUM);
        }
        if (glBlendFunc_) glBlendFunc_(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Use magenta for debugging - change to white later
        if (glColor4ub_) glColor4ub_(255, 0, 255, 160); // Semi-transparent magenta
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("Failed to set GL state for fog rendering");
        fog_glBindTexture(GL_TEXTURE_2D_ENUM_GL, (unsigned)prevTex);
        return;
    }

    // Convert to normalized UVs
    const float s0 = (float)u0 / (float)g_fogWidth;
    const float t0 = (float)v0 / (float)g_fogHeight;
    const float s1 = (float)u1 / (float)g_fogWidth;
    const float t1 = (float)v1 / (float)g_fogHeight;

    LOG_INFO("DrawFogQuad_GL_Normalized: Drawing quad {}x{} -> {}x{}, UVs ({:.3f},{:.3f}) -> ({:.3f},{:.3f})",
             x0, y0, x1, y1, s0, t0, s1, t1);

    __try {
        glBegin_(GL_TRIANGLE_STRIP);
            glTexCoord2f_(s0, t0); glVertex2i_(x0, y0);
            glTexCoord2f_(s0, t1); glVertex2i_(x0, y1);
            glTexCoord2f_(s1, t0); glVertex2i_(x1, y0);
            glTexCoord2f_(s1, t1); glVertex2i_(x1, y1);
        glEnd_();
        LOG_INFO("DrawFogQuad_GL_Normalized: Successfully drew normalized fog quad");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("Exception during normalized GL fog drawing, code: 0x{:X}", GetExceptionCode());
    }



    // Restore previous binding
    __try { fog_glBindTexture(GL_TEXTURE_2D_ENUM_GL, (unsigned)prevTex); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void BindFogTex_SaveRestoreAndDrawQuad(int x0,int y0,int x1,int y1,int u0,int v0,int u1,int v1){
    if (!fog_glBindTexture || !fog_glGetIntegerv) {
        LOG_ERROR("BindFogTex_SaveRestoreAndDrawQuad: GL functions not available");
        return;
    }

    // Validate all required draw functions are available
    if (!DrawBegin || !DrawVertex || !DrawEnd || !DrawTexCoord) {
        LOG_ERROR("BindFogTex_SaveRestoreAndDrawQuad: Draw functions not available (Begin:{}, Vertex:{}, End:{}, TexCoord:{})",
                  (void*)DrawBegin, (void*)DrawVertex, (void*)DrawEnd, (void*)DrawTexCoord);
        return;
    }

    int prevTex = 0;
    __try { fog_glGetIntegerv(GL_TEXTURE_BINDING_2D_ENUM, &prevTex); } __except(EXCEPTION_EXECUTE_HANDLER){ prevTex = 0; }
    __try { fog_glBindTexture(GL_TEXTURE_2D_ENUM, (unsigned)g_fogTexture); } __except(EXCEPTION_EXECUTE_HANDLER){
        LOG_ERROR("BindFogTex_SaveRestoreAndDrawQuad: Failed to bind fog texture");
        return;
    }

    __try {
        DrawBegin(DRAW_TRIANGLE_STRIP);
        DrawTexCoord(u0, v0); DrawVertex(x0, y0);
        DrawTexCoord(u0, v1); DrawVertex(x0, y1);
        DrawTexCoord(u1, v0); DrawVertex(x1, y0);
        DrawTexCoord(u1, v1); DrawVertex(x1, y1);
        DrawEnd();
        LOG_INFO("BindFogTex_SaveRestoreAndDrawQuad: Successfully drew fog quad");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("BindFogTex_SaveRestoreAndDrawQuad: Exception during draw operations, code: 0x{:X}", GetExceptionCode());
    }

    __try { fog_glBindTexture(GL_TEXTURE_2D_ENUM, (unsigned)prevTex); } __except(EXCEPTION_EXECUTE_HANDLER){}
}





// Distance Transform Algorithm (Felzenszwalb & Huttenlocher)
static void edt_1d(const uint8_t *f, int n, uint16_t *d) {
    std::vector<int> v(n); // parabola vertices
    std::vector<float> z(n + 1); // intersection points
    int k = 0;
    v[0] = 0;
    z[0] = -1e6f;
    z[1] = 1e6f;

    for (int q = 1; q < n; q++) {
        while (k >= 0) {
            float s = ((f[q] + q * q) - (f[v[k]] + v[k] * v[k])) / (2.0f * q - 2.0f * v[k]);
            if (s <= z[k]) {
                k--;
            } else {
                break;
            }
        }
        k++;
        v[k] = q;
        z[k] = ((f[q] + q * q) - (f[v[k - 1]] + v[k - 1] * v[k - 1])) / (2.0f * q - 2.0f * v[k - 1]);
        z[k + 1] = 1e6f;
    }

    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < q) k++;
        d[q] = (q - v[k]) * (q - v[k]) + f[v[k]];
    }
}

// 1D squared distance transform on uint16_t
static void edt_1d_u16(const uint16_t *f, int n, uint16_t *d) {
    static thread_local std::vector<int> v; // vertex locations
    static thread_local std::vector<float> z; // intersections

    if ((int) v.size() < n) v.resize(n);
    if ((int) z.size() < n + 1) z.resize(n + 1);

    int k = 0;
    v[0] = 0;
    z[0] = -1e20f;
    z[1] = 1e20f;

    auto F = [&](int i) { return (int) f[i]; };

    for (int q = 1; q < n; ++q) {
        float s;
        while (true) {
            s = ((F(q) + q * q) - (F(v[k]) + v[k] * v[k])) / float(2 * q - 2 * v[k]);
            if (s > z[k]) break;
            if (--k < 0) {
                k = 0;
                break;
            }
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e20f;
    }

    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        int dx = q - v[k];
        int val = dx * dx + F(v[k]);
        d[q] = (uint16_t) std::min(val, 0xFFFF);
    }
}

static void edt_2d_u16(const uint8_t *mask, uint16_t *dist, int W, int H) {
    static thread_local std::vector<uint16_t> tmp;
    static thread_local std::vector<uint16_t> line;
    if ((int) tmp.size() < W * H) tmp.resize(W * H);
    if ((int) line.size() < std::max(W, H)) line.resize(std::max(W, H));

    const uint16_t INF = 0x3FFF;

    // rows
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x)
            line[x] = mask[y * W + x] ? INF : 0; // hidden=INF, visible=0
        edt_1d_u16(line.data(), W, &tmp[y * W]);
    }

    // cols
    for (int x = 0; x < W; ++x) {
        for (int y = 0; y < H; ++y) line[y] = tmp[y * W + x];
        edt_1d_u16(line.data(), H, line.data());
        for (int y = 0; y < H; ++y) dist[y * W + x] = line[y];
    }
}


static void InitializeFogTexture(int tilesX, int tilesY) {
    // 2x supersampling (each tile -> 2x2 texels for 4 quadrants)
    g_fogWidth = tilesX * 2;
    g_fogHeight = tilesY * 2;

    size_t totalPixels = g_fogWidth * g_fogHeight;
    g_fogMask.assign(totalPixels, 1); // start fully hidden
    g_fogDist.assign(totalPixels, 0);
    g_fogAlpha.assign(totalPixels, 255); // start fully opaque

    // Create OpenGL texture
    using PFN_glGenTextures = void (APIENTRY*)(int, unsigned int *);
    using PFN_glBindTexture = void (APIENTRY*)(unsigned int, unsigned int);
    using PFN_glTexImage2D = void (APIENTRY*)(unsigned int, int, int, int, int, int, unsigned int, unsigned int,
                                              const void *);
    using PFN_glTexParameteri = void (APIENTRY*)(unsigned int, unsigned int, int);

    static auto glGenTextures = (PFN_glGenTextures) GL_GetProc("glGenTextures");
    static auto glBindTexture = (PFN_glBindTexture) GL_GetProc("glBindTexture");
    static auto glTexImage2D = (PFN_glTexImage2D) GL_GetProc("glTexImage2D");
    static auto glTexParameteri = (PFN_glTexParameteri) GL_GetProc("glTexParameteri");

    if (glGenTextures && glBindTexture && glTexImage2D && glTexParameteri) {
        unsigned int texId;
        glGenTextures(1, &texId);
        g_fogTexture = (int) texId;

        glBindTexture(0x0DE1/*GL_TEXTURE_2D*/, texId);
        glTexParameteri(0x0DE1, 0x2802/*GL_TEXTURE_WRAP_S*/, 0x812F/*GL_CLAMP_TO_EDGE*/);
        glTexParameteri(0x0DE1, 0x2803/*GL_TEXTURE_WRAP_T*/, 0x812F/*GL_CLAMP_TO_EDGE*/);
        glTexParameteri(0x0DE1, 0x2800/*GL_TEXTURE_MAG_FILTER*/, 0x2601/*GL_LINEAR*/);
        glTexParameteri(0x0DE1, 0x2801/*GL_TEXTURE_MIN_FILTER*/, 0x2601/*GL_LINEAR*/);

        // Initialize with transparent black RGBA texture
        std::vector<uint32_t> initData(totalPixels, 0x00000000); // transparent black
        glTexImage2D(0x0DE1, 0, 0x1908, g_fogWidth, g_fogHeight, 0, 0x1908, 0x1401, initData.data());

        g_firstUploadPending = true; // force first upload even if no changes detected

        LOG_INFO("Enhanced fog texture created: {}x{} ({}x{} tiles)", g_fogWidth, g_fogHeight, tilesX, tilesY);
    } else {
        LOG_ERROR("Failed to get OpenGL functions for fog texture creation");
        g_fogTexture = 0;
    }

    g_fogDirty = true;
}



static void ProcessFogDistanceTransform() {
    if (!g_fogDirty && !g_firstUploadPending) return;
    edt_2d_u16(g_fogMask.data(), g_fogDist.data(), g_fogWidth, g_fogHeight);
    const float r0 = 2.0f, r1 = 8.0f;
    for (size_t i = 0; i < g_fogAlpha.size(); ++i) {
        float d = std::sqrt((float) g_fogDist[i]);
        float t = clamp((d - r0) / (r1 - r0), 0.0f, 1.0f);
        float a = t * t * (3.0f - 2.0f * t);
        g_fogAlpha[i] = (uint8_t) (a * 255 + 0.5f);
    }
    // NOTE: do NOT set g_fogDirty = false here.
}

static void UploadFogTexture() {
    LOG_INFO("UploadFogTexture called: fogTexture=0x{:X}, fogDirty={}, firstUpload={}",
             g_fogTexture, g_fogDirty, g_firstUploadPending);

    if (!g_fogTexture) return;

    // TEMP TEST: Force upload to verify drawing pipeline works
    static int forceUploadCount = 0;
    bool forceUpload = (forceUploadCount < 5); // Force first 5 uploads for testing
    if (forceUpload) {
        forceUploadCount++;
        LOG_INFO("TEMP: Forcing upload #{} to test drawing pipeline", forceUploadCount);
    }

    if (!g_fogDirty && !g_firstUploadPending && !forceUpload) {
        LOG_WARN("UploadFogTexture: Skipping upload - texture:0x{:X}, dirty:false", g_fogTexture);
        return;
    }

    using PFN_glBindTexture = void (APIENTRY*)(unsigned int, unsigned int);
    using PFN_glTexSubImage2D = void (APIENTRY*)(unsigned int, int, int, int, int, int, unsigned int, unsigned int,
                                                 const void *);

    static auto glBindTexture = (PFN_glBindTexture) GL_GetProc("glBindTexture");
    static auto glTexSubImage2D = (PFN_glTexSubImage2D) GL_GetProc("glTexSubImage2D");

    std::vector<uint8_t> rgba(g_fogWidth * g_fogHeight * 4);

    // Debug: Count non-zero alpha values to see what we're working with
    int visibleCount = 0, foggedCount = 0;
    for (size_t i = 0; i < g_fogAlpha.size(); ++i) {
        if (g_fogAlpha[i] == 0) visibleCount++;
        else foggedCount++;

        rgba[i*4 + 0] = 0;   // R - black fog
        rgba[i*4 + 1] = 0;   // G - black fog
        rgba[i*4 + 2] = 0;   // B - black fog
        rgba[i*4 + 3] = g_fogAlpha[i];  // A - fog alpha controls visibility
    }
    LOG_INFO("UploadFogTexture: {}x{} texture, visible pixels: {}, fogged pixels: {}",
             g_fogWidth, g_fogHeight, visibleCount, foggedCount);

    glBindTexture(0x0DE1, g_fogTexture);
    glTexSubImage2D(0x0DE1, 0, 0, 0, g_fogWidth, g_fogHeight,
                    0x1908/*GL_RGBA*/, 0x1401/*UNSIGNED_BYTE*/, rgba.data());

    g_fogDirty = false;
    g_firstUploadPending = false;
}
using PFN_wglGetCurrentContext = HGLRC (WINAPI*)(void);
static PFN_wglGetCurrentContext fog_wglGetCurrentContext =
    (PFN_wglGetCurrentContext)GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetCurrentContext");



static bool ReadVisMapWH(void* pVis, int& W, int& H) {
    if (!pVis) return false;
    auto p = (const uint8_t*)pVis;
    // Suspected layout: width @ +0x0C, height @ +0x0E (uint16)
    uint16_t w = 0, h = 0;
    __try {
        w = *(const uint16_t*)(p + 0x0C);
        h = *(const uint16_t*)(p + 0x0E);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // sanity: visible maps are smallish (<= 8192) and even (because 2 subtile columns per tile)
    if (w < 4 || h < 4 || w > 8192 || h > 8192) return false;
    W = (int)w; H = (int)h;
    return true;
}

// optional debug flag (or wire to INI)
static std::atomic<bool> g_fogSimpleSmokeTest{false}; // smoke test passed, now do real fog
static void RenderEnhancedFogQuad(void* pInfinity) {
    auto* inf = (CInfinity_FOG*)pInfinity;
    if (!g_fogTexture || !fog_glBindTexture || !fog_glGetIntegerv) return;

    // Save current GL binding, bind our fog tex, draw, then restore.
    int prevTex = 0;
    __try { fog_glGetIntegerv(GL_TEXTURE_BINDING_2D_ENUM, &prevTex); }
    __except(EXCEPTION_EXECUTE_HANDLER) { prevTex = 0; }

    __try { fog_glBindTexture(GL_TEXTURE_2D_ENUM, (unsigned)g_fogTexture); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return; }

    // Screen rect
    int offX = inf->nOffsetX ? (inf->nOffsetX - 0x40) : 0;
    int offY = inf->nOffsetY ? (inf->nOffsetY - 0x40) : 0;
    int x0 = inf->nCurrentTileX * 64 + offX;
    int y0 = inf->nCurrentTileY * 64 + offY;
    int x1 = x0 + inf->nVisibleTilesX * 64;
    int y1 = y0 + inf->nVisibleTilesY * 64;

    // UVs in supersampled texels
    const int ss = 2;
    int u0 = inf->nCurrentTileX * ss;
    int v0 = inf->nCurrentTileY * ss;
    int u1 = u0 + inf->nVisibleTilesX * ss;
    int v1 = v0 + inf->nVisibleTilesY * ss;

    // Draw via engine’s immediate-like wrappers (they don’t re-bind unless we call DrawBindTexture)
    DrawBegin(DRAW_TRIANGLE_STRIP);
    DrawTexCoord(u0, v0); DrawVertex(x0, y0);
    DrawTexCoord(u0, v1); DrawVertex(x0, y1);
    DrawTexCoord(u1, v0); DrawVertex(x1, y0);
    DrawTexCoord(u1, v1); DrawVertex(x1, y1);
    DrawEnd();

    // Restore previous texture binding
    __try { fog_glBindTexture(GL_TEXTURE_2D_ENUM, (unsigned)prevTex); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// Enhanced fog rendering hook - replaces CInfinity::RenderFog
static Fn_RenderFog oRenderFog = nullptr;
static bool UpdateFogFromVisibility(void* pInfinity, void* pVisibilityMap) {
    (void)pInfinity; // avoid touching it until we really need to

    LOG_INFO("UpdateFogFromVisibility: Starting visibility map processing");

    int Wsub=0, Hsub=0; // subtiles
    __try {
        if (!ReadVisMapWH(pVisibilityMap, Wsub, Hsub)) {
            LOG_WARN("Visibility map W/H invalid; skipping enhanced fog this frame");
            return false;
        }
        LOG_INFO("UpdateFogFromVisibility: Read visibility map dimensions: {}x{} subtiles", Wsub, Hsub);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("Exception in ReadVisMapWH, code: 0x{:X}", GetExceptionCode());
        return false;
    }

    const int tilesX = Wsub / 2;
    const int tilesY = Hsub / 2;
    LOG_INFO("UpdateFogFromVisibility: Calculated tile dimensions: {}x{} tiles", tilesX, tilesY);

    // (Re)create fog texture if dims changed
    if (tilesX != g_lastAreaTilesX || tilesY != g_lastAreaTilesY) {
        LOG_INFO("UpdateFogFromVisibility: Area dimensions changed, reinitializing fog texture");
        g_lastAreaTilesX = tilesX;
        g_lastAreaTilesY = tilesY;

        __try {
            InitializeFogTexture(tilesX, tilesY);
            LOG_INFO("UpdateFogFromVisibility: Fog texture initialized");
            if (!g_fogTexture) {
                LOG_ERROR("UpdateFogFromVisibility: Failed to create fog texture");
                return false;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("Exception in InitializeFogTexture, code: 0x{:X}", GetExceptionCode());
            return false;
        }
    }

    if (!g_GetTileCode) {
        LOG_ERROR("UpdateFogFromVisibility: g_GetTileCode is null");
        return false; // you already bind this at 0x140256380
    }

    LOG_INFO("UpdateFogFromVisibility: Processing tiles for fog mask");
    bool anyChange = false;
    int visibleTiles = 0, foggedTiles = 0;
    __try {
        for (int ty=0; ty<tilesY; ++ty) {
            for (int tx=0; tx<tilesX; ++tx) {
                const short tileIndex = (short)(ty * tilesX + tx); // guaranteed in-range
                TILE_CODE code{};

                __try {
                    g_GetTileCode(pVisibilityMap, tileIndex, &code);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    LOG_ERROR("Exception in g_GetTileCode for tile {}x{}, code: 0x{:X}", tx, ty, GetExceptionCode());
                    continue; // skip this tile
                }

                // map 4 quadrants → 2x2 supersample texels
                const int sx = tx * 2, sy = ty * 2;
                const uint8_t nw = (code.tileNW & 0xF) ? 0 : 255;  // 0=transparent, 255=opaque fog
                const uint8_t ne = (code.tileNE & 0xF) ? 0 : 255;  // 0=transparent, 255=opaque fog
                const uint8_t sw = (code.tileSW & 0xF) ? 0 : 255;  // 0=transparent, 255=opaque fog
                const uint8_t se = (code.tileSE & 0xF) ? 0 : 255;  // 0=transparent, 255=opaque fog

                // Debug: Log first few tiles to understand the visibility data
                if (tx < 3 && ty < 3) {
                    LOG_INFO("Tile {}x{}: code {{NW:0x{:X}, NE:0x{:X}, SW:0x{:X}, SE:0x{:X}}} -> fog {{NW:{}, NE:{}, SW:{}, SE:{}}}",
                             tx, ty, code.tileNW, code.tileNE, code.tileSW, code.tileSE, nw, ne, sw, se);
                }

                // Count tile types
                if (nw || ne || sw || se) foggedTiles++;
                else visibleTiles++;

                // Bounds check before writing to fog mask
                size_t idx;
                idx = (sy+0)*g_fogWidth + (sx+0);
                if (idx < g_fogMask.size() && g_fogMask[idx] != nw) { g_fogMask[idx]=nw; anyChange=true; }

                idx = (sy+0)*g_fogWidth + (sx+1);
                if (idx < g_fogMask.size() && g_fogMask[idx] != ne) { g_fogMask[idx]=ne; anyChange=true; }

                idx = (sy+1)*g_fogWidth + (sx+0);
                if (idx < g_fogMask.size() && g_fogMask[idx] != sw) { g_fogMask[idx]=sw; anyChange=true; }

                idx = (sy+1)*g_fogWidth + (sx+1);
                if (idx < g_fogMask.size() && g_fogMask[idx] != se) { g_fogMask[idx]=se; anyChange=true; }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("Exception in fog mask processing loop, code: 0x{:X}", GetExceptionCode());
        return false;
    }

    if (anyChange) {
        g_fogDirty = true;
        LOG_INFO("UpdateFogFromVisibility: Fog mask updated, marked as dirty");
    } else {
        LOG_INFO("UpdateFogFromVisibility: No changes to fog mask");
    }

    LOG_INFO("UpdateFogFromVisibility: Completed successfully - visible tiles: {}, fogged tiles: {}, total: {}",
             visibleTiles, foggedTiles, tilesX * tilesY);
    return true;
}

static bool IsValidModuleAddress(void* addr) {
    uintptr_t ptr = (uintptr_t)addr;
    uintptr_t base = (uintptr_t)BASE();
    return ptr >= base && ptr < (base + 0x3522000); // Module size from logs
}

static void DrawMagentaSmokeQuad() {
    // Validate all required draw functions are available
    if (!DrawBegin || !DrawVertex || !DrawEnd) {
        LOG_ERROR("DrawMagentaSmokeQuad: Critical draw functions not available (Begin:{}, Vertex:{}, End:{})",
                  (void*)DrawBegin, (void*)DrawVertex, (void*)DrawEnd);
        return;
    }

    // Only call functions with valid addresses
    if (DrawEnable && IsValidModuleAddress(DrawEnable)) {
        DrawEnable(DRAW_BLEND);
    } else {
        LOG_WARN("Skipping DrawEnable - invalid address: {}", (void*)DrawEnable);
    }

    if (DrawDisable && IsValidModuleAddress(DrawDisable)) {
        DrawDisable(DRAW_TEXTURE_2D);
    } else {
        LOG_WARN("Skipping DrawDisable - invalid address: {}", (void*)DrawDisable);
    }

    if (DrawBlendFunc && IsValidModuleAddress(DrawBlendFunc)) {
        DrawBlendFunc(DRAW_SRC_ALPHA, DRAW_ONE_MINUS_SRC_ALPHA);
    } else {
        LOG_WARN("Skipping DrawBlendFunc - invalid address: {}", (void*)DrawBlendFunc);
    }

    if (DrawColor && IsValidModuleAddress(DrawColor)) {
        DrawColor(0x40FF00FF); // very transparent magenta (0x40 = 25% alpha)
    } else {
        LOG_WARN("Skipping DrawColor - invalid address: {}", (void*)DrawColor);
    }

    // Tiny 32x32 overlay near top-left corner; very small and safe
    const int x0=10,y0=10,x1=42,y1=42;

    __try {
        DrawBegin(DRAW_TRIANGLE_STRIP);
        DrawVertex(x0,y0);
        DrawVertex(x0,y1);
        DrawVertex(x1,y0);
        DrawVertex(x1,y1);
        DrawEnd();
        LOG_INFO("DrawMagentaSmokeQuad: Successfully drew smoke test quad");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("DrawMagentaSmokeQuad: Exception during draw operations, code: 0x{:X}", GetExceptionCode());
    }
}

using PFN_glMatrixMode   = void (APIENTRY*)(unsigned);
using PFN_glPushMatrix   = void (APIENTRY*)();
using PFN_glPopMatrix    = void (APIENTRY*)();
using PFN_glLoadIdentity = void (APIENTRY*)();
using PFN_glOrtho        = void (APIENTRY*)(double,double,double,double,double,double);

static auto glMatrixMode_   = (PFN_glMatrixMode)   GL_GetProc("glMatrixMode");
static auto glPushMatrix_   = (PFN_glPushMatrix)   GL_GetProc("glPushMatrix");
static auto glPopMatrix_    = (PFN_glPopMatrix)    GL_GetProc("glPopMatrix");
static auto glLoadIdentity_ = (PFN_glLoadIdentity) GL_GetProc("glLoadIdentity");
static auto glOrtho_        = (PFN_glOrtho)        GL_GetProc("glOrtho");

static bool g_fogUploadedOnce = false;


// Enhanced fog rendering hook — *replaces* CInfinity::RenderFog
// === replaces CInfinity::RenderFog ===
// ─────────────────────────────────────────────────────────────────────────────
// Exact re-implementation of CInfinity::RenderFog (triangles per tile, no tex)
// ─────────────────────────────────────────────────────────────────────────────
// Simple stock-like fog renderer that uses only your own structs + Draw* API.
// No CInfinity/CVidMode/CVisibilityMap types, no __thiscall, x64-safe.
static void RenderFog_StockLike(void* pInfinity, void* /*pVidMode*/, void* pVisibility)
{
    auto* self = reinterpret_cast<CInfinity_FOG*>(pInfinity);
    if (!self || !pVisibility || !g_GetTileCode) return;

    constexpr int kTilePx = 64;
    const int vpL = self->rViewPort.left;
    const int vpT = self->rViewPort.top;

    // Match your Hook_RenderFog base offset logic (observed in engine)
    const int baseX = (self->nOffsetX == 0) ? vpL : (self->nOffsetX - kTilePx + vpL);
    const int baseY = (self->nOffsetY == 0) ? vpT : (self->nOffsetY - kTilePx + vpT);

    // Clamp the visible window
    const int startX = std::max(0, self->nCurrentTileX);
    const int startY = std::max(0, self->nCurrentTileY);
    const int endX   = std::min(self->nTilesX, self->nCurrentTileX + self->nVisibleTilesX);
    const int endY   = std::min(self->nTilesY, self->nCurrentTileY + self->nVisibleTilesY);
    if (endX <= startX || endY <= startY) return;

    // Draw state: solid color quads, alpha blended, no texturing
    if (DrawPushState) DrawPushState();
    if (DrawDisable)   DrawDisable(DRAW_TEXTURE_2D);
    if (DrawEnable)    DrawEnable(DRAW_BLEND);
    if (DrawBlendFunc) DrawBlendFunc(DRAW_SRC_ALPHA, DRAW_ONE_MINUS_SRC_ALPHA);

    // Slightly transparent black fog (tweak alpha to taste)
    if (DrawColor) DrawColor(0x99000000); // ~60% alpha black

    TILE_CODE code{};
    for (int ty = startY; ty < endY; ++ty) {
        for (int tx = startX; tx < endX; ++tx) {
            const short tileIndex = static_cast<short>(ty * self->nTilesX + tx);
            __try { g_GetTileCode(pVisibility, tileIndex, &code); }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }

            // Screen-space top-left of this tile
            const int sx = (tx - self->nCurrentTileX) * kTilePx + baseX;
            const int sy = (ty - self->nCurrentTileY) * kTilePx + baseY;

            // Each quadrant is 32x32 inside the 64x64 tile
            auto drawQuad = [&](int x0,int y0,int x1,int y1){
                if (!DrawBegin || !DrawVertex || !DrawEnd) return;
                DrawBegin(DRAW_TRIANGLE_STRIP);
                    DrawVertex(x0, y0);
                    DrawVertex(x0, y1);
                    DrawVertex(x1, y0);
                    DrawVertex(x1, y1);
                DrawEnd();
            };

            // In your mask logic: (code & 0xF) != 0 => visible, 0 => fogged.
            const bool nwFog = ( (code.tileNW & 0x0F) == 0 );
            const bool neFog = ( (code.tileNE & 0x0F) == 0 );
            const bool swFog = ( (code.tileSW & 0x0F) == 0 );
            const bool seFog = ( (code.tileSE & 0x0F) == 0 );

            if (nwFog) drawQuad(sx +  0, sy +  0, sx + 32, sy + 32);
            if (neFog) drawQuad(sx + 32, sy +  0, sx + 64, sy + 32);
            if (swFog) drawQuad(sx +  0, sy + 32, sx + 32, sy + 64);
            if (seFog) drawQuad(sx + 32, sy + 32, sx + 64, sy + 64);
        }
    }

    if (DrawPopState) DrawPopState();
}


static void Hook_RenderFog(void* thisPtr, void* /*pVidMode*/, void* pVisibilityMap)
{
    // Bail to vanilla if disabled or bad args
    if (!g_config.enableEnhancedFog || !thisPtr || !pVisibilityMap) {
        RenderFog_StockLike(thisPtr, nullptr, pVisibilityMap);
        return;
    }

    auto* inf = (CInfinity_FOG*)thisPtr;

    // 1) Build/refresh mask & texture when area dims change
    if (!UpdateFogFromVisibility(thisPtr, pVisibilityMap)) {
        if (oRenderFog) oRenderFog(thisPtr, nullptr, pVisibilityMap);
        return;
    }

    // Turn mask → alpha
    ProcessFogDistanceTransform();

    // IMPORTANT: do NOT force firstUpload every frame.
    // InitializeFogTexture() already set g_firstUploadPending = true on creation.
    UploadFogTexture();

    if (!g_fogTexture || g_fogWidth <= 0 || g_fogHeight <= 0)
        return;

    // 2) Mirror the engine’s tile-window clamp (min/max)
    const int tilesX = g_lastAreaTilesX;
    const int tilesY = g_lastAreaTilesY;
    const int curX   = inf->nCurrentTileX;
    const int curY   = inf->nCurrentTileY;
    const int visX   = inf->nVisibleTilesX;
    const int visY   = inf->nVisibleTilesY;

    const int startX = std::max(0, curX);
    const int startY = std::max(0, curY);
    const int endX   = std::min(tilesX, curX + visX);
    const int endY   = std::min(tilesY, curY + visY);
    if (endX <= startX || endY <= startY) return;

    // Viewport base like the stock function
    const int vpL = inf->rViewPort.left;
    const int vpT = inf->rViewPort.top;
    const int baseX = (inf->nOffsetX == 0) ? vpL : (inf->nOffsetX - 0x40 + vpL);
    const int baseY = (inf->nOffsetY == 0) ? vpT : (inf->nOffsetY - 0x40 + vpT);

    // Screen-space quad (64 px per tile)
    const int x0 = (startX - curX) * 64 + baseX;
    const int y0 = (startY - curY) * 64 + baseY;
    const int x1 = x0 + (endX - startX) * 64;
    const int y1 = y0 + (endY - startY) * 64;

    // Fog tex coords in supersampled texels (2x per tile)
    const int ss = 2;
    const int u0i = startX * ss;
    const int v0i = startY * ss;
    const int u1i = endX   * ss;
    const int v1i = endY   * ss;

    // Normalized UVs for GL
    const float s0 = float(u0i) / float(g_fogWidth);
    const float t0 = float(v0i) / float(g_fogHeight);
    const float s1 = float(u1i) / float(g_fogWidth);
    const float t1 = float(v1i) / float(g_fogHeight);

    // 3) Draw — keep it contained and restore state
    if (DrawPushState) DrawPushState();

    // Save minimal GL state we touch
    int prevProg = 0, prevTex = 0, vp[4] = {0,0,0,0};
    unsigned char prevMask[4] = {1,1,1,1};
    const bool haveGetInt  = (fog_glGetIntegerv != nullptr);
    const bool haveBindTex = (fog_glBindTexture  != nullptr);

    if (haveGetInt) {
        fog_glGetIntegerv(0x8B8D/*GL_CURRENT_PROGRAM*/, &prevProg);
        fog_glGetIntegerv(GL_TEXTURE_BINDING_2D_ENUM,    &prevTex);
        fog_glGetIntegerv(0x0BA2/*GL_VIEWPORT*/,         vp); // x,y,w,h
    }
    if (glGetBooleanv_) glGetBooleanv_(0x0C23/*GL_COLOR_WRITEMASK*/, prevMask);

    // Kill state that blocks overlays
    const bool scissorOn = (glIsEnabled_ && glIsEnabled_(0x0C11/*GL_SCISSOR_TEST*/));
    const bool stencilOn = (glIsEnabled_ && glIsEnabled_(0x0B90/*GL_STENCIL_TEST*/));
    const bool alphaOn   = (glIsEnabled_ && glIsEnabled_(0x0BC0/*GL_ALPHA_TEST*/));
    if (glDisable_) {
        if (scissorOn) glDisable_(0x0C11/*GL_SCISSOR_TEST*/);
        if (stencilOn) glDisable_(0x0B90/*GL_STENCIL_TEST*/);
        if (alphaOn)   glDisable_(0x0BC0/*GL_ALPHA_TEST*/);
    }

    // Use fixed pipeline, full color writes
    if (glUseProgram_) glUseProgram_(0);
    if (glColorMask_)  glColorMask_(1,1,1,1);

    // Pixel-space ortho (y-down)
    if (glMatrixMode_ && glPushMatrix_ && glLoadIdentity_ && glOrtho_) {
        glMatrixMode_(0x1701/*GL_PROJECTION*/); glPushMatrix_(); glLoadIdentity_();
        glOrtho_(0.0, (double)vp[2], (double)vp[3], 0.0, -1.0, 1.0);
        glMatrixMode_(0x1700/*GL_MODELVIEW*/);  glPushMatrix_(); glLoadIdentity_();
    }

    // Depth off for overlay
    const bool depthOn = (glIsEnabled_ && glIsEnabled_(0x0B71/*GL_DEPTH_TEST*/));
    if (depthOn && glDisable_) glDisable_(0x0B71/*GL_DEPTH_TEST*/);
    if (glDepthMask_) glDepthMask_(0);

    // Blend + texture
    if (glEnable_) {
        glEnable_(GL_TEXTURE_2D_ENUM_GL);
        glEnable_(GL_BLEND_ENUM);
    }
    if (glBlendFunc_) glBlendFunc_(0x0302/*SRC_ALPHA*/, 0x0303/*ONE_MINUS_SRC_ALPHA*/);

    // Optional but safe: REPLACE to ignore vertex color on fixed pipeline
    if (glTexEnvi_) glTexEnvi_(0x2300/*GL_TEXTURE_ENV*/, 0x2200/*GL_TEXTURE_ENV_MODE*/, 0x1E01/*GL_REPLACE*/);

    if (haveBindTex) fog_glBindTexture(GL_TEXTURE_2D_ENUM_GL, (unsigned)g_fogTexture);

    // Color white so texture alpha does the darkening
    if (glColor4ub_) glColor4ub_(255, 0, 255, 160);

    // Draw the quad
    if (glBegin_ && glTexCoord2f_ && glVertex2i_ && glEnd_) {
        glBegin_(0x0005/*GL_TRIANGLE_STRIP*/);
            glTexCoord2f_(s0, t0); glVertex2i_(x0, y0);
            glTexCoord2f_(s0, t1); glVertex2i_(x0, y1);
            glTexCoord2f_(s1, t0); glVertex2i_(x1, y0);
            glTexCoord2f_(s1, t1); glVertex2i_(x1, y1);
        glEnd_();
    }

    // Restore GL & engine state
    if (haveBindTex) fog_glBindTexture(GL_TEXTURE_2D_ENUM_GL, (unsigned)prevTex);
    if (glDepthMask_) glDepthMask_(1);
    if (depthOn && glEnable_) glEnable_(0x0B71/*GL_DEPTH_TEST*/);

    if (glMatrixMode_ && glPopMatrix_) {
        glMatrixMode_(0x1700/*GL_MODELVIEW*/);  glPopMatrix_();
        glMatrixMode_(0x1701/*GL_PROJECTION*/); glPopMatrix_();
    }
    if (glUseProgram_) glUseProgram_(prevProg);
    if (glColorMask_)  glColorMask_(prevMask[0], prevMask[1], prevMask[2], prevMask[3]);
    if (glEnable_) {
        if (scissorOn) glEnable_(0x0C11/*GL_SCISSOR_TEST*/);
        if (stencilOn) glEnable_(0x0B90/*GL_STENCIL_TEST*/);
        if (alphaOn)   glEnable_(0x0BC0/*GL_ALPHA_TEST*/);
    }

    if (DrawPopState) DrawPopState();
}


// endregion Enhanced Fog of War System

// Optional toggle: default to passthrough (fast). Set true to use your custom draw.
static bool g_useCustomDraw = false; // or plumb from g_config.forceCustomDraw
constexpr unsigned GL_TEXTURE_2D = 0x0DE1;
constexpr unsigned GL_TEXTURE_WRAP_S = 0x2802;
constexpr unsigned GL_TEXTURE_WRAP_T = 0x2803;
constexpr unsigned GL_CLAMP_TO_EDGE = 0x812F;
constexpr unsigned GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr unsigned GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr int GL_LINEAR = 0x2601;

inline void ConfigureTextureIfFirstTime(int texId) {
    if (texId <= 0) return;
    if (g_texConfigured.find(texId) != g_texConfigured.end()) return;

    using PFN_glTexParameteri = void (APIENTRY*)(unsigned, unsigned, int);
    static PFN_glTexParameteri glTexParameteri =
            (PFN_glTexParameteri) GL_GetProc("glTexParameteri");
    if (!glTexParameteri) {
        g_texConfigured.insert(texId);
        return;
    }

    // Engine will have it bound before drawing; if not, DrawBindTexture(texId) just before.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    g_texConfigured.insert(texId);
}

inline void SetTone(const int tone) {
    if (tone == g_lastTone) return;
    g_lastTone = tone;
    if (DrawColorTone) DrawColorTone(tone);
}

inline void DemandIfNew(void *tis) {
    if (tis && tis != g_lastTis && CRes_Demand) {
        CRes_Demand(tis);
        g_lastTis = tis;
    }
}

// endregion

// RenderTexture replacement: keep 64× vertices; enlarge UV span to tileDimension.
// If table u,v are still 64-based, we detect and scale once.
static void Hook_RenderTexture(void *thisPtr, int texId, void *, int x, int y, unsigned long flags) {
    CResTIS_MIN *tis = nullptr;
    int idx = -1;
    const PVRZTileEntry *tbl = nullptr;
    // If we can't resolve tile info, just leave immediately through the original
    if (!GetTileInfo(thisPtr, &tis, &idx, &tbl)) {
        oRenderTexture(thisPtr, texId, nullptr, x, y, flags);
        return;
    }

    const PVRZTileEntry &e = tbl[idx];

    // Use consistent scaling factor for entire area
    int U0 = e.u, V0 = e.v;

    // Detect HD content and update area-wide scale factor
    if (!g_scaleDetected.load() && g_detectionCount.load() < 10) {
        g_detectionCount.fetch_add(1);

        if (e.u > 1024 || e.v > 1024 || texId > 10000) {
            // HD content detected - set area-wide 4x scaling
            g_currentAreaScaleFactor.store(4);
            g_scaleDetected.store(true);
            LOG_INFO("HD area detected! Setting 4x scale factor for entire area (texId: {}, UV: ({},{}))", texId, e.u,
                     e.v);
        } else if (g_detectionCount.load() == 10) {
            // Sampled enough tiles, assume standard content
            g_currentAreaScaleFactor.store(1);
            g_scaleDetected.store(true);

            // DISABLE the hook for standard areas to avoid performance overhead
            MH_DisableHook((LPVOID) (BASE() + g_config.fallbackRenderTextureRVA));
            g_isRenderHookActive = false;
            LOG_INFO("Standard area detected - DISABLING RenderTexture hook for performance");
        }
    }

    // Apply consistent scaling across entire area
    int scaleFactor = g_currentAreaScaleFactor.load();
    int DU = 64 * scaleFactor, DV = 64 * scaleFactor;

    unsigned long savedColor = 0;
    if (texId == 0) {
        if (DrawDisable) DrawDisable(1);
        if (DrawColor) savedColor = DrawColor(0xFF000000);
    } else if (texId != -1) DrawBindTexture(texId);

    // Only apply texture enhancements to area tiles (detected by successful GetTileInfo)
    // Additional safety: only enhance textures that look like area tiles (higher IDs)
    // UI textures typically have lower IDs and should not be enhanced
    int currentLastTex = g_lastTexId.load();
    if (texId != currentLastTex && texId > 100) {
        // Conservative threshold - UI textures are usually < 100
        if (EnsureNiceSamplerForCurrentGLTex()) {
            g_lastTexId.store(texId);
            LOG_DEBUG("Enhanced tile texture {}", texId);
        } else {
            LOG_WARN("Failed to configure GL texture parameters for tile texture {}", texId);
        }
    }

    DrawPushState();

    // Enhanced tone detection with proper fallback to tone=8
    // tone=0 means no shader bound, so we always default to tone=8 (fpseam.glsl)
    int tone = 8; // Default to fpseam.glsl for enhanced rendering

    // Check if grey tone is specifically requested via flags
    if ((flags >> 19) & 1) {
        tone = 1; // Use fpgrey.glsl for greyscale mode
    }

    // Check the "linear tiles" switch in TIS structure
    // tis + 0x1DC holds the "linear tiles" switch in this build (your disasm shows it)
    if (*(int *) ((uintptr_t) tis + 0x1DC) != 0) {
        tone = 8; // Force fpseam.glsl for linear tiles
    }

    // Final safety check - never allow tone=0 (no shader)
    if (tone == 0) {
        tone = 8; // Use fpseam.glsl as fallback
    }

    if (DrawColorTone) DrawColorTone(tone);

    DrawBegin(DRAW_TRIANGLES);

    // Keep the 64×64 screen quad (lighting/scissor correctness)
    const int X0 = x, Y0 = y;
    const int X1 = x + 64, Y1 = y + 64;

    // tri 1
    DrawTexCoord(U0, V0);
    DrawVertex(X0, Y0);
    DrawTexCoord(U0, V0 + DV);
    DrawVertex(X0, Y1);
    DrawTexCoord(U0 + DU, V0);
    DrawVertex(X1, Y0);

    // tri 2
    DrawTexCoord(U0 + DU, V0);
    DrawVertex(X1, Y0);
    DrawTexCoord(U0, V0 + DV);
    DrawVertex(X0, Y1);
    DrawTexCoord(U0 + DU, V0 + DV);
    DrawVertex(X1, Y1);

    DrawEnd();
    if (texId == 0 && DrawColor) DrawColor(savedColor);
    DrawPopState();
}

static void *Hook_LoadArea(void *thisPtr, void *pAreaNameString, unsigned char a2, unsigned char a3, unsigned char a4) {
    LOG_DEBUG("LoadArea called - resetting scale detection for new area");

    // DO NOT clear g_texConfigured - this breaks UI textures that were already properly configured
    // Only reset tile-specific state variables
    g_lastTone = -1;
    g_lastTis = nullptr;
    g_lastTexId.store(-1); // Reset texture tracking to avoid cross-area state pollution

    // Reset scale detection for new area
    g_currentAreaScaleFactor.store(1);
    g_scaleDetected.store(false);
    g_detectionCount.store(0);

    // Reset enhanced fog state for new area
    g_lastAreaTilesX = 0;
    g_lastAreaTilesY = 0;
    g_fogDirty = true;

    // Re-enable RenderTexture hook for new area detection
    if (!g_isRenderHookActive) {
        MH_EnableHook((LPVOID) (BASE() + g_config.fallbackRenderTextureRVA));
        g_isRenderHookActive = true;
        LOG_DEBUG("Re-enabled RenderTexture hook for new area detection");
    }

    return oLoadArea(thisPtr, pAreaNameString, a2, a3, a4);;
}

static DWORD WINAPI InitThread(LPVOID) {
    // Initialize enhanced logging system first
    Logger::Initialize();

    LOG_INFO("Infinity Engine Enhancer initializing...");

    // Initialize pattern scanner
    if (!PatternScanner::Initialize()) {
        LOG_ERROR("Failed to initialize PatternScanner");
        return 1;
    }

    // First, load configuration
    std::string configPath = ConfigManager::GetConfigPath();
    if (ConfigManager::LoadConfig(configPath, g_config)) {
        LOG_INFO("Configuration loaded from: {}", configPath.c_str());
    } else {
        LOG_WARN("Failed to load config, using defaults");
    }

    // Log configuration summary
    LogF("=== INFINITY ENGINE ENHANCER DIAGNOSTICS ===");
    LogF("UV Mode: %s (%d)", g_config.uvMode == UvMode::A_HD_DU256_Base ? "HD 256x256" : "Vanilla 64x64",
         (int) g_config.uvMode);
    LogF("Texture Enhancements:");
    LogF("  Trilinear filtering: %s", g_config.enableTrilinearFiltering ? "ENABLED" : "disabled");
    LOG_INFO("  Anisotropic filtering: {}", g_config.enableAnisotropicFiltering ? "ENABLED" : "disabled");
    if (g_config.enableAnisotropicFiltering) {
        LOG_INFO("    Max anisotropy: {:.1f}x", g_config.maxAnisotropy);
    }
    LOG_INFO("  Mipmaps: {}", g_config.enableMipmaps ? "ENABLED" : "disabled");
    LOG_INFO("  LOD bias: {:.2f}", g_config.lodBias);
    LOG_INFO("Engine Settings:");
    LOG_INFO("  Signature scanning: {}", g_config.enableSignatureScanning ? "ENABLED" : "disabled");
    LOG_INFO("  Verbose logging: {}", g_config.enableVerboseLogging ? "ENABLED" : "disabled");
    LOG_INFO("============================================");

    // Scan for game functions
    if (!ScanForGameFunctions()) {
        LOG_ERROR("Critical error: Failed to locate game functions");
        return 1;
    }

    LOG_INFO("Base=0x{:X}  LoadArea=0x{:X}  RenderTexture=0x{:X}  RenderFog=0x{:X}", BASE(),
             g_addresses.LoadArea, g_addresses.RenderTexture, g_addresses.RenderFog);
    uintptr_t inner = g_addresses.RenderFog;
    uintptr_t start = GetFuncStartFromUnwind(inner);
    if (start) { LOG_INFO("RenderFog inner 0x{:X} -> start 0x{:X}", inner, start); g_addresses.RenderFog = start; }
    else { LOG_ERROR("Failed to locate RenderFog start via unwind near 0x{:X}", inner); }

    // Bind GetTileCode from Ghidra RVA
    if (!BindGetTileCodeFromGhidra()) {
        LOG_ERROR("GetTileCode bind failed; disabling enhanced fog to stay safe");
        g_config.enableEnhancedFog = false;
    }
    if (start) {
        LOG_INFO("RenderFog inner 0x{:X} -> start 0x{:X}", inner, start);
        g_addresses.RenderFog = start;
    } else {
        LOG_ERROR("Failed to locate RenderFog start via unwind near 0x{:X}", inner);
    }

    if (!ResolveDrawAPI()) {
        LOG_ERROR("Failed to resolve draw API functions");
        return 1;
    }

    // Initialize MinHook
    MH_STATUS ms = MH_Initialize();
    LOG_INFO("MH_Initialize -> {}", MH_StatusToString(ms));
    if (ms != MH_OK && ms != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("Failed to initialize MinHook");
        return 1;
    }

    // Create hooks using dynamically resolved addresses
    if (DrawColorTone) {
        MH_CreateHook((LPVOID) DrawColorTone, Hook_DrawColorTone, (LPVOID *) &oDrawColorTone);
        MH_EnableHook((LPVOID) DrawColorTone);
    }

    ms = MH_CreateHook(reinterpret_cast<LPVOID>(g_addresses.LoadArea), (LPVOID) &Hook_LoadArea,
                       reinterpret_cast<LPVOID *>(&oLoadArea));
    LOG_INFO("MH_CreateHook(LoadArea) -> {}", MH_StatusToString(ms));

    ms = MH_CreateHook(reinterpret_cast<LPVOID>(g_addresses.RenderTexture), (LPVOID) &Hook_RenderTexture,
                       reinterpret_cast<LPVOID *>(&oRenderTexture));
    LOG_INFO("MH_CreateHook(RenderTexture) -> {}", MH_StatusToString(ms));

    // Add prologue sanity check before hooking RenderFog
    uint8_t prolog[4];
    memcpy(prolog, (void *) g_addresses.RenderFog, 4);

    // x64 prolog is usually 0x40/0x48- prefixed or a PUSH RBP/...
    bool looksLikeProlog = (prolog[0] == 0x40 || prolog[0] == 0x48 || prolog[0] == 0x55);

    if (!looksLikeProlog) {
        LOG_ERROR("RenderFog pattern resolved to a non-prologue address: 0x{:X} (bytes: {:02X} {:02X} {:02X} {:02X})",
                  g_addresses.RenderFog, prolog[0], prolog[1], prolog[2], prolog[3]);
        LOG_ERROR("SKIPPING RenderFog hook to prevent crash");
        g_addresses.RenderFog = 0; // Mark as invalid
    } else {
        LOG_INFO("RenderFog prologue check passed (bytes: {:02X} {:02X} {:02X} {:02X})",
                 prolog[0], prolog[1], prolog[2], prolog[3]);

        // Enhanced fog of war hook
        ms = MH_CreateHook(reinterpret_cast<LPVOID>(g_addresses.RenderFog), (LPVOID) &Hook_RenderFog,
                           reinterpret_cast<LPVOID *>(&oRenderFog));
        LOG_INFO("MH_CreateHook(RenderFog) -> {}", MH_StatusToString(ms));
    }

    ms = MH_EnableHook(reinterpret_cast<LPVOID>(g_addresses.LoadArea));
    LOG_INFO("MH_EnableHook(LoadArea) -> {}", MH_StatusToString(ms));

    ms = MH_EnableHook(reinterpret_cast<LPVOID>(g_addresses.RenderTexture));
    LOG_INFO("MH_EnableHook(RenderTexture) -> {}", MH_StatusToString(ms));

    if (g_config.enableEnhancedFog && g_addresses.RenderFog != 0) {
        ms = MH_EnableHook(reinterpret_cast<LPVOID>(g_addresses.RenderFog));
        LOG_INFO("MH_EnableHook(RenderFog) -> {}", MH_StatusToString(ms));
        LOG_INFO("Enhanced fog of war system ENABLED - will create smooth fog edges");
        LOG_INFO("RenderFog hook installed at: 0x{:X}", g_addresses.RenderFog);
    } else {
        if (!g_config.enableEnhancedFog) {
            LOG_INFO("Enhanced fog disabled in config - RenderFog hook not enabled");
        } else {
            LOG_INFO("Enhanced fog disabled - RenderFog address validation failed");
        }
    }
    LOG_INFO("RenderTexture hook enabled - will detect upscaled textures automatically");
    LOG_INFO("Install complete.");
    LOG_INFO("======================");

    return 0;
}

static void CleanupHooks() {
    LOG_INFO("Cleaning up hooks...");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    LOG_INFO("Cleanup complete.");
    Logger::Shutdown();
}

// EEex integration
extern "C" __declspec(dllexport) void __stdcall InitBindings(void *argSharedState) {
    LOG_INFO("InitBindings called by EEex framework");
    std::string configPath = ConfigManager::GetConfigPath();
    if (ConfigManager::LoadConfig(configPath, g_config)) {
        LOG_INFO("Configuration loaded from: {}", configPath.c_str());
    } else {
        LOG_WARN("Failed to load config, using defaults");
    }

    if (HANDLE th = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) {
        CloseHandle(th);
        LOG_INFO("InitBindings complete - texture enhancement started");
    } else {
        LOG_ERROR("Failed to create initialization thread");
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    switch (r) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(h);
            LOG_INFO("DLL_PROCESS_ATTACH - waiting for EEex InitBindings call");
            break;

        case DLL_PROCESS_DETACH:
            CleanupHooks();
            break;
    }
    return TRUE;
}
