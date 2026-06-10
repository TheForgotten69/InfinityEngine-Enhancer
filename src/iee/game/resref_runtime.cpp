#include "resref_runtime.h"

#include "iee/core/pattern_scanner.h"

#include <algorithm>
#include <cctype>

namespace iee::game {
    namespace {
        bool is_resref_char(char c) noexcept {
            const auto uc = static_cast<unsigned char>(c);
            return std::isalnum(uc) != 0 || c == '_' || c == '-';
        }

        bool read_direct_resref(const char *ptr, ResrefBuffer &out) noexcept {
            if (!ptr) {
                return false;
            }

            out.fill('\0');
            bool haveAny = false;
            for (std::size_t i = 0; i < 8; ++i) {
                char c = '\0';
                if (!core::safe_read(ptr + i, c)) {
                    return false;
                }
                if (c == '\0') {
                    return haveAny;
                }
                if (!is_resref_char(c)) {
                    return false;
                }

                out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                haveAny = true;
            }

            return haveAny;
        }
    }

    bool read_runtime_resref(const void *value, ResrefBuffer &out) noexcept {
        out.fill('\0');
        if (!value) {
            return false;
        }

        if (read_direct_resref(static_cast<const char *>(value), out)) {
            return true;
        }

        const char *indirect = nullptr;
        if (!core::safe_read(value, indirect) || !indirect) {
            out.fill('\0');
            return false;
        }

        if (read_direct_resref(indirect, out)) {
            return true;
        }

        out.fill('\0');
        return false;
    }

    std::string_view resref_view(const ResrefBuffer &value) noexcept {
        const auto end = std::find(value.begin(), value.end(), '\0');
        return std::string_view(value.data(), static_cast<std::size_t>(std::distance(value.begin(), end)));
    }
}
