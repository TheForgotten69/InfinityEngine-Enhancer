#pragma once
#include <cstdint>

#include "iee/core/config.h"


namespace iee::game {
    struct BuildManifest;

    struct DrawApi {
        using DrawBegin_t = void(*)(int);
        using DrawEnd_t = void(*)();
        using DrawPushState_t = void(*)();
        using DrawPopState_t = void(*)();
        using DrawTexCoord_t = void(*)(int, int);
        using DrawVertex_t = void(*)(int, int);
        using DrawBindTexture_t = void(*)(int);
        using DrawDisable_t = void(*)(int);
        using DrawColor_t = unsigned long(*)(unsigned long);
        using DrawColorTone_t = void(*)(int);
        using CRes_Demand_t = void*(*)(void *);

        DrawBegin_t DrawBegin{};
        DrawEnd_t DrawEnd{};
        DrawPushState_t DrawPushState{};
        DrawPopState_t DrawPopState{};
        DrawTexCoord_t DrawTexCoord{};
        DrawVertex_t DrawVertex{};
        DrawBindTexture_t DrawBindTexture{};
        DrawDisable_t DrawDisable{};
        DrawColor_t DrawColor{};
        DrawColorTone_t DrawColorTone{};
        CRes_Demand_t CRes_Demand{};
    };

    bool resolve_draw_api(DrawApi &out, std::uintptr_t renderTextureVA, const BuildManifest &manifest);

    bool ensure_texture_params(const core::EngineConfig &cfg, const DrawApi &api, int textureId);

    constexpr unsigned long BLACK_COLOR = 0xFF000000;
}
