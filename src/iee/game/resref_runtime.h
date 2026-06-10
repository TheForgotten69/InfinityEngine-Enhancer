#pragma once

#include <array>
#include <string_view>

namespace iee::game {
    using ResrefBuffer = std::array<char, 9>;

    [[nodiscard]] bool read_runtime_resref(const void *value, ResrefBuffer &out) noexcept;

    [[nodiscard]] std::string_view resref_view(const ResrefBuffer &value) noexcept;
}
