#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace iee::core {
    // PE module’s image in memory
    struct ModuleSpan {
        std::byte *base{};
        std::size_t size{};
    };

    // Get current process module info (base & size). Returns nullopt on failure.
    std::optional<ModuleSpan> get_module_span(void *module_handle);

    // Parse IDA-style pattern string: "48 8B ? ? ? 89 54 24 ?"
    // Produces a byte array and a mask where '?' are wildcards.
    bool parse_ida_pattern(std::string_view pattern,
                           std::vector<std::byte> &bytes,
                           std::vector<bool> &mask);

    // Find first occurrence of pattern in the given memory span.
    void *find_pattern(std::span<const std::byte> haystack,
                       std::span<const std::byte> needle,
                       const std::vector<bool> &mask);

    // Convenience: find in a module by pattern string. Returns nullptr if not found.
    void *find_first_in_module(void *module_handle, std::string_view ida_pattern);

    // Decode a relative 32-bit displacement target (e.g., call/jmp rel32).
    // addr = instruction address; disp_offset = offset of the displacement inside the instruction.
    // size = full instruction length in bytes.
    void *rel32_target(void *addr, std::size_t disp_offset, std::size_t size);

    // Decode a relative 32-bit displacement target after validating the leading opcode.
    void *rel32_target_checked(const void *addr,
                               std::uint8_t expected_opcode,
                               std::size_t disp_offset,
                               std::size_t size);

    // Best-effort readable check (Windows). Returns false if unreadable.
    bool is_readable(const void *p, std::size_t len);

    template<class T>
    bool safe_read(const void *p, T &out) {
        if (!is_readable(p, sizeof(T))) return false;
        out = *static_cast<const T *>(p);
        return true;
    }
}
