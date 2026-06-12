#pragma once
#include "app_context.h"

namespace iee::hooks {
    bool install_all(AppContext &ctx);

    void uninstall_all();

    void prepare_for_shutdown() noexcept;

    bool is_active();

    // Called by the frame hook every frame (render thread): publishes the
    // current view transform to the shader uniform feed. Must not depend on
    // the tile-render hook, which disables itself on non-upscaled areas.
    void publish_view_state() noexcept;
}
