#pragma once
#include "app_context.h"

namespace iee::hooks {
bool install_all(AppContext& ctx);

void uninstall_all() noexcept;

// Disables engine entry-point hooks without uninitializing MinHook. The
// caller must remove other MinHook-backed subsystems, then call
// uninstall_all().
void prepare_for_shutdown() noexcept;

// Render-thread retry point used by the frame boundary. Probe recovery must
// not depend on successful tile decoding or a later RenderTexture call.
void retry_shader_probe_install() noexcept;

bool is_active();
}  // namespace iee::hooks
