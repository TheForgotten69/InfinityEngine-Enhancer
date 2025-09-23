#pragma once
#include "app_context.h"

namespace iee::hooks {
    bool install_all(AppContext &ctx);

    void uninstall_all();

    bool is_active();
}
