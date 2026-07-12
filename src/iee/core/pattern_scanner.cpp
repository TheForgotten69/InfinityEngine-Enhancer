#include "pattern_scanner.h"
#include "logger.h"

#ifdef _WIN64
#include <windows.h>
#include <psapi.h>
#endif

#include <algorithm>
#include <charconv>
#include <limits>

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
                if (i + 1 >= pattern.size() || !is_hex_digit(pattern[i]) || !is_hex_digit(pattern[i + 1])) return false;
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

    void *find_pattern(std::span<const std::byte> haystack, std::span<const std::byte> needle,
                       const std::vector<bool> &mask) {
        return find_pattern_unique(haystack, needle, mask).address;
    }

    PatternMatchResult find_pattern_unique(std::span<const std::byte> haystack, std::span<const std::byte> needle,
                                           const std::vector<bool> &mask) {
        PatternMatchResult result{};
        if (needle.empty() || needle.size() != mask.size()) return result;
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
            if (match) {
                if (!result.address) {
                    result.address = const_cast<std::byte *>(&haystack[i]);
                    result.count = 1;
                } else {
                    result.count = 2;
                    return result;
                }
            }
        }
        return result;
    }

#ifdef _WIN64
    namespace {
        PatternMatchResult find_in_executable_sections(const ModuleSpan &module, std::span<const std::byte> needle,
                                                       const std::vector<bool> &mask) {
            PatternMatchResult result{};

            IMAGE_DOS_HEADER dos{};
            if (!safe_read(module.base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
                return result;
            }

            const auto ntOffset = static_cast<std::size_t>(dos.e_lfanew);
            IMAGE_NT_HEADERS64 nt{};
            if (ntOffset > module.size || module.size - ntOffset < sizeof(nt) ||
                !safe_read(module.base + ntOffset, nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
                return result;
            }

            const auto sectionTableOffset =
                ntOffset + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
            for (std::size_t index = 0; index < nt.FileHeader.NumberOfSections; ++index) {
                const auto sectionOffset = sectionTableOffset + index * sizeof(IMAGE_SECTION_HEADER);
                IMAGE_SECTION_HEADER section{};
                if (sectionOffset > module.size || module.size - sectionOffset < sizeof(section) ||
                    !safe_read(module.base + sectionOffset, section)) {
                    return {};
                }
                if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;

                const auto virtualAddress = static_cast<std::size_t>(section.VirtualAddress);
                if (virtualAddress >= module.size) continue;
                const auto declaredSize = std::max(static_cast<std::size_t>(section.Misc.VirtualSize),
                                                   static_cast<std::size_t>(section.SizeOfRawData));
                const auto sectionSize = std::min(declaredSize, module.size - virtualAddress);
                if (sectionSize < needle.size() || !is_readable(module.base + virtualAddress, sectionSize)) continue;

                const auto sectionResult = find_pattern_unique(
                    std::span<const std::byte>(module.base + virtualAddress, sectionSize), needle, mask);
                if (sectionResult.count == 0) continue;
                if (result.count != 0 || sectionResult.count > 1) {
                    result.count = 2;
                    return result;
                }
                result = sectionResult;
            }
            return result;
        }
    } // namespace
#endif

    void *find_first_in_module(void *module_handle, std::string_view ida_pattern) {
        auto mod = get_module_span(module_handle);
        if (!mod) return nullptr;

        std::vector<std::byte> bytes;
        std::vector<bool> mask;
        if (!parse_ida_pattern(ida_pattern, bytes, mask)) {
            LOG_ERROR("parse_ida_pattern failed for '{}'", ida_pattern);
            return nullptr;
        }
#ifdef _WIN64
        return find_in_executable_sections(*mod, std::span<const std::byte>(bytes.data(), bytes.size()), mask).address;
#else
        return nullptr;
#endif
    }

    void *find_unique_in_module(void *module_handle, std::string_view ida_pattern, std::size_t *matchCount) {
        if (matchCount) *matchCount = 0;
        const auto mod = get_module_span(module_handle);
        if (!mod) return nullptr;

        std::vector<std::byte> bytes;
        std::vector<bool> mask;
        if (!parse_ida_pattern(ida_pattern, bytes, mask)) {
            LOG_ERROR("parse_ida_pattern failed for '{}'", ida_pattern);
            return nullptr;
        }

#ifdef _WIN64
        const auto result =
            find_in_executable_sections(*mod, std::span<const std::byte>(bytes.data(), bytes.size()), mask);
#else
        const PatternMatchResult result{};
#endif
        if (matchCount) *matchCount = result.count;
        return result.unique() ? result.address : nullptr;
    }

    void *rel32_target(void *addr, std::size_t disp_offset, std::size_t size) {
        auto *p = static_cast<std::byte *>(addr);
        auto *disp_ptr = reinterpret_cast<int32_t *>(p + disp_offset);
        int32_t disp = *disp_ptr;
        std::byte *next = p + size;
        return next + disp;
    }

    void *rel32_target_checked(const void *addr, std::uint8_t expected_opcode, std::size_t disp_offset,
                               std::size_t size) {
        std::uint8_t opcode = 0;
        if (!safe_read(addr, opcode) || opcode != expected_opcode) {
            return nullptr;
        }

        int32_t displacement = 0;
        const auto *p = static_cast<const std::byte *>(addr);
        if (!safe_read(p + disp_offset, displacement)) {
            return nullptr;
        }

        const auto *next = p + size;
        return const_cast<std::byte *>(next + displacement);
    }

    bool is_readable(const void *p, std::size_t len) {
#ifdef _WIN64
        if (!p) return false;
        if (len == 0) return true;

        MEMORY_BASIC_INFORMATION mbi{};
        auto cur = reinterpret_cast<std::uintptr_t>(p);
        if (len > std::numeric_limits<std::uintptr_t>::max() - cur) return false;
        const auto end = cur + len;

        while (cur < end) {
            if (!VirtualQuery(reinterpret_cast<const void *>(cur), &mbi, sizeof(mbi))) return false;
            if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0) return false;

            const auto protection = mbi.Protect & 0xFF;
            const bool readable = protection == PAGE_READONLY ||
                                  protection == PAGE_READWRITE ||
                                  protection == PAGE_WRITECOPY ||
                                  protection == PAGE_EXECUTE_READ ||
                                  protection == PAGE_EXECUTE_READWRITE ||
                                  protection == PAGE_EXECUTE_WRITECOPY;
            if (!readable) return false;

            const auto regionBase = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            if (mbi.RegionSize > std::numeric_limits<std::uintptr_t>::max() - regionBase) return false;
            const auto region_end = regionBase + mbi.RegionSize;
            if (region_end <= cur) return false;
            cur = region_end;
        }
        return true;
#else
        (void) p; (void) len; return true;
#endif
    }
}
