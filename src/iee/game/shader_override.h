#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace iee::game {
    // Extracts the engine's shader name from a source preview.
    // Engine convention: a comment line "// <name>.glsl" near the top.
    // `prefix` filters by kind ("fp" or "vp"); empty accepts both.
    [[nodiscard]] std::string extract_shader_name(std::string_view source, std::string_view prefix);

    struct OverrideCheck {
        bool ok{};
        std::vector<std::string> missingIdentifiers;
    };
    // Contract check: every uniform/varying identifier declared in the original
    // must appear in the replacement (the engine queries them by name; see spec §4.2).
    [[nodiscard]] OverrideCheck check_interface_contract(std::string_view originalSource,
                                                         std::string_view replacementSource);
}
