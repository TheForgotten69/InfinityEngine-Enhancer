#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace iee::probe::diagnostics {

[[nodiscard]] std::string caller_summary(std::uintptr_t caller);
void dump_shader_source(const std::filesystem::path& directory, std::string_view name,
                        std::string_view source);

}  // namespace iee::probe::diagnostics
