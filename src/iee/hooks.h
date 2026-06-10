#pragma once
#include "app_context.h"

namespace iee::hooks {
    bool install_all(AppContext &ctx);

    void uninstall_all();

    void prepare_for_shutdown() noexcept;

    bool is_active();
}
