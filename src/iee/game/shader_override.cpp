#include "shader_override.h"
#include <fstream>
#include <sstream>

namespace iee::game {
    std::string extract_shader_name(std::string_view source, std::string_view prefix) {
        const auto commentPos = source.find("// ");
        if (commentPos == std::string_view::npos) return {};
        const auto nameStart = commentPos + 3;
        const auto endPos = source.find(".glsl", nameStart);
        if (endPos == std::string_view::npos || endPos <= nameStart) return {};
        const auto name = source.substr(nameStart, endPos - nameStart);
        if (!prefix.empty() && name.rfind(prefix, 0) != 0) return {};
        return std::string(name);
    }

    std::size_t find_main_body_open(std::string_view source) {
        for (const std::string_view pattern : {"void main(", "void main ("}) {
            if (const auto pos = source.find(pattern); pos != std::string_view::npos) {
                return source.find('{', pos);
            }
        }
        return std::string_view::npos;
    }

    std::string make_magenta_variant(std::string_view source) {
        const auto bodyOpen = find_main_body_open(source);
        if (bodyOpen == std::string_view::npos) return {};
        std::string patched(source);
        constexpr std::string_view injection =
            "\n/* IEE_DEBUG_MAGENTA */\n"
            "gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0);\n"
            "return;\n";
        patched.insert(bodyOpen + 1, injection);
        return patched;
    }

    namespace {
        // Collect identifiers declared as "uniform ... NAME;" or "varying ... NAME;".
        std::vector<std::string> declared_interface_identifiers(std::string_view source) {
            std::vector<std::string> names;
            std::istringstream stream{std::string(source)};
            std::string line;
            while (std::getline(stream, line)) {
                std::istringstream words(line);
                std::string qualifier;
                words >> qualifier;
                if (qualifier != "uniform" && qualifier != "varying") continue;
                std::string token, last;
                while (words >> token) last = token;
                if (last.empty()) continue;
                if (last.back() == ';') last.pop_back();
                // strip array suffix e.g. name[4]
                if (const auto bracket = last.find('['); bracket != std::string::npos)
                    last.resize(bracket);
                if (!last.empty()) names.push_back(last);
            }
            return names;
        }

        // Returns true if `needle` appears in `haystack` as a complete GLSL identifier token
        // (not as a substring of a longer identifier). This prevents sTex from matching
        // sTex1, or uTc from matching uTcScale, when checking replacement sources.
        bool contains_as_identifier(std::string_view haystack, std::string_view needle) {
            const auto is_id_char = [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            };
            std::size_t pos = 0;
            while (true) {
                pos = haystack.find(needle, pos);
                if (pos == std::string_view::npos) return false;
                if (pos > 0 && is_id_char(haystack[pos - 1])) { pos += needle.size(); continue; }
                const std::size_t end = pos + needle.size();
                if (end < haystack.size() && is_id_char(haystack[end])) { pos += needle.size(); continue; }
                return true;
            }
        }
    }

    OverrideCheck check_interface_contract(std::string_view originalSource,
                                           std::string_view replacementSource) {
        OverrideCheck result{.ok = true};
        for (const auto &name : declared_interface_identifiers(originalSource)) {
            if (!contains_as_identifier(replacementSource, name)) {
                result.ok = false;
                result.missingIdentifiers.push_back(name);
            }
        }
        return result;
    }

    void ShaderOverrideRegistry::load_from_directory(const std::filesystem::path &dir) {
        entries_.clear();
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".glsl") continue;
            std::ifstream file(entry.path(), std::ios::binary);
            if (!file) continue;
            std::ostringstream contents;
            contents << file.rdbuf();
            entries_[entry.path().stem().string()] = contents.str();
        }
    }

    std::optional<std::string_view> ShaderOverrideRegistry::find(std::string_view shaderName) const {
        const auto it = entries_.find(std::string(shaderName));
        if (it == entries_.end()) return std::nullopt;
        return std::string_view{it->second};
    }
}
