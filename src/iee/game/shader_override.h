#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iee::game {
    // Extracts the engine's shader name from a source preview.
    // Engine convention: a comment line "// <name>.glsl" near the top.
    // `prefix` filters by kind ("fp" or "vp"); empty accepts both.
    [[nodiscard]] std::string extract_shader_name(std::string_view source, std::string_view prefix);

    // Finds the offset of '{' opening main()'s body, or npos.
    [[nodiscard]] std::size_t find_main_body_open(std::string_view source);

    // Builds the magenta debug variant of a fragment source (inserts an early
    // return of vec4(1,0,1,1) at the top of main). Returns empty if not patchable.
    [[nodiscard]] std::string make_magenta_variant(std::string_view source);

    struct OverrideCheck {
        bool ok{};
        std::vector<std::string> missingIdentifiers;
    };
    // Contract check: every uniform/varying identifier declared in the original
    // must appear in the replacement (the engine queries them by name; see spec §4.2).
    [[nodiscard]] OverrideCheck check_interface_contract(std::string_view originalSource,
                                                         std::string_view replacementSource);

    class ShaderOverrideRegistry {
    public:
        // Loads every "<name>.glsl" in dir (non-recursive). Missing dir => empty registry.
        void load_from_directory(const std::filesystem::path &dir);
        [[nodiscard]] std::optional<std::string_view> find(std::string_view shaderName) const;
        [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    private:
        std::unordered_map<std::string, std::string> entries_; // name -> source
    };
}
