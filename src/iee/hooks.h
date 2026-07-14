#pragma once
#include "app_context.h"

namespace iee::hooks {
bool install_all(AppContext& ctx);

void uninstall_all() noexcept;

// Disables engine entry-point hooks without uninitializing MinHook. The
// caller must remove other MinHook-backed subsystems, then call
// uninstall_all().
void prepare_for_shutdown() noexcept;

bool is_active();
}  // namespace iee::hooks
