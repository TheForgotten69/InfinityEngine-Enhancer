#pragma once

#include "iee/core/config.h"

namespace iee::game {
    struct DrawApi;

    namespace shader_trace {
        bool install(const core::EngineConfig &cfg);

        bool attach_draw_api(const DrawApi &draw);

        void reset_runtime_capture(const char *reason = nullptr) noexcept;

        void uninstall() noexcept;
    }
}
