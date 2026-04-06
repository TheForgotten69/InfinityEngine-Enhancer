#pragma once

#include "iee/core/config.h"

namespace iee::game::shader_trace {
    bool install(const core::EngineConfig &cfg);

    void uninstall() noexcept;
}
