#include "pattern_scanner.h"
#include "logger.h"

#ifdef _WIN64
#include <windows.h>
#include <psapi.h>
#endif

#include <charconv>

namespace iee::core {
    std::optional<ModuleSpan> get_module_span(void *module_handle) {
#ifdef _WIN64
        if (!module_handle) module_handle = GetModuleHandleW(nullptr);
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(),
                                  static_cast<HMODULE>(module_handle), &mi, sizeof(mi)))
            return std::nullopt;
        return ModuleSpan{
            static_cast<std::byte *>(mi.lpBaseOfDll),
            static_cast<std::size_t>(mi.SizeOfImage)
        };
#else
        (void) module_handle;
        return std::nullopt;
#endif
    }

    static inline bool is_hex_digit(char c) {
        return (c >= '0' && c <= '9') ||
               (c >= 'A' && c <= 'F') ||
               (c >= 'a' && c <= 'f');
    }

    bool parse_ida_pattern(std::string_view pattern,
                           std::vector<std::byte> &bytes,
                           std::vector<bool> &mask) {
        bytes.clear();
        mask.clear();
        for (size_t i = 0; i < pattern.size();) {
            while (i < pattern.size() && pattern[i] == ' ') ++i;
            if (i >= pattern.size()) break;

            if (pattern[i] == '?') {
                // support '?' or '??'
                ++i;
                if (i < pattern.size() && pattern[i] == '?') ++i;
                bytes.push_back(std::byte{0});
                mask.push_back(false);
            } else {
                if (i + 1 >= pattern.size() || !is_hex_digit(pattern[i]) || !is_hex_digit(pattern[i + 1]))
                    return false;
                unsigned int value = 0;
                auto res = std::from_chars(&pattern[i], &pattern[i] + 2, value, 16);
                if (res.ec != std::errc{}) return false;
                bytes.push_back(static_cast<std::byte>(value));
                mask.push_back(true);
                i += 2;
            }
            if (i < pattern.size() && pattern[i] == ' ') ++i;
        }
        return !bytes.empty() && bytes.size() == mask.size();
    }

    void *find_pattern(std::span<const std::byte> haystack,
                       std::span<const std::byte> needle,
                       const std::vector<bool> &mask) {
        if (needle.empty() || needle.size() != mask.size()) return nullptr;
        const auto n = needle.size();
        const auto end = haystack.size() < n ? 0 : haystack.size() - n + 1;

        for (size_t i = 0; i < end; ++i) {
            bool match = true;
            for (size_t j = 0; j < n; ++j) {
                if (!mask[j]) continue;
                if (haystack[i + j] != needle[j]) {
                    match = false;
                    break;
                }
            }
            if (match) return const_cast<std::byte *>(&haystack[i]);
        }
        return nullptr;
    }

    void *find_first_in_module(void *module_handle, std::string_view ida_pattern) {
        auto mod = get_module_span(module_handle);
        if (!mod) return nullptr;

        std::vector<std::byte> bytes;
        std::vector<bool> mask;
        if (!parse_ida_pattern(ida_pattern, bytes, mask)) {
            LOG_ERROR("parse_ida_pattern failed for '{}'", ida_pattern);
            return nullptr;
        }
        auto *found = find_pattern(
            std::span<const std::byte>(mod->base, mod->size),
            std::span<const std::byte>(bytes.data(), bytes.size()),
            mask);
        return found;
    }

    void *rel32_target(void *addr, std::size_t disp_offset, std::size_t size) {
        auto *p = static_cast<std::byte *>(addr);
        auto *disp_ptr = reinterpret_cast<int32_t *>(p + disp_offset);
        int32_t disp = *disp_ptr;
        std::byte *next = p + size;
        return next + disp;
    }

    bool is_readable(const void *p, std::size_t len) {
#ifdef _WIN64
        MEMORY_BASIC_INFORMATION mbi{};
        auto *cur = reinterpret_cast<const std::byte *>(p);
        const std::byte *end = cur + len;

        while (cur < end) {
            if (!VirtualQuery(cur, &mbi, sizeof(mbi))) return false;
            const auto state_ok = (mbi.State == MEM_COMMIT);
            const auto protect = mbi.Protect & 0xFF;
            const bool readable =
                    state_ok && !(protect & PAGE_NOACCESS) && !(protect & PAGE_GUARD);
            if (!readable) return false;

            auto *region_end = static_cast<const std::byte *>(mbi.BaseAddress) + mbi.RegionSize;
            if (region_end <= cur) return false;
            cur = region_end;
        }
        return true;
#else
        (void) p; (void) len; return true;
#endif
    }
}
