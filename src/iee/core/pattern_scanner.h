#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
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

    struct PatternMatchResult {
        void *address{};
        std::size_t count{};

        [[nodiscard]] bool unique() const noexcept { return address != nullptr && count == 1; }
    };

    // Finds every occurrence up to the point where ambiguity is established.
    // count is 0, 1, or 2 (2 means "two or more").
    [[nodiscard]] PatternMatchResult find_pattern_unique(std::span<const std::byte> haystack,
                                                         std::span<const std::byte> needle,
                                                         const std::vector<bool> &mask);

    // Convenience: find in a module by pattern string. Returns nullptr if not found.
    void *find_first_in_module(void *module_handle, std::string_view ida_pattern);

    // Safe hook-resolution variant: returns an address only when the pattern has
    // exactly one match in the module image. matchCount is saturated at 2.
    void *find_unique_in_module(void *module_handle,
                                std::string_view ida_pattern,
                                std::size_t *matchCount = nullptr);

    // Core (host-testable) of the detour-tolerant match: true when every
    // non-wildcard needle byte from `prologue_bytes` onward equals the
    // corresponding haystack byte and at least `min_tail` such bytes were
    // verified. `haystack` must be at least needle-length.
    [[nodiscard]] bool matches_past_prologue(std::span<const std::byte> haystack,
                                             std::span<const std::byte> needle,
                                             const std::vector<bool> &mask,
                                             std::size_t prologue_bytes, std::size_t min_tail);

    // Confirms `ida_pattern` at a specific module RVA while tolerating a detour
    // that another component may have written over the function prologue: the
    // first `prologue_bytes` are ignored, every non-wildcard byte after them must
    // match, and at least `min_tail` non-wildcard tail bytes must be verified so
    // a short pattern cannot pass on prologue alone. Intended only for a target
    // whose build identity is already positively confirmed. Returns the RVA's
    // absolute address on success, nullptr otherwise.
    void *confirm_pattern_with_patched_prologue(void *module_handle, std::uintptr_t rva,
                                                std::string_view ida_pattern,
                                                std::size_t prologue_bytes = 16,
                                                std::size_t min_tail = 4);

    // Decode a relative 32-bit displacement target (e.g., call/jmp rel32).
    // addr = instruction address; disp_offset = offset of the displacement inside the instruction.
    // size = full instruction length in bytes.
    void *rel32_target(void *addr, std::size_t disp_offset, std::size_t size);

    // Decode a relative 32-bit displacement target after validating the leading opcode.
    void *rel32_target_checked(const void *addr,
                               std::uint8_t expected_opcode,
                               std::size_t disp_offset,
                               std::size_t size);

    // Best-effort readable-page check (Windows). This does not prove that an
    // engine object is still alive or unchanged; callers must validate object
    // identity, bounds, and generation where those facts matter.
    bool is_readable(const void *p, std::size_t len);

    // Readability results are cached only within an engine frame/area epoch.
    // Advance at boundaries where resource mappings may change.
    void advance_readability_cache_epoch() noexcept;

    struct ReadabilityStats {
        std::uint64_t cacheHits{};
        std::uint64_t virtualQueries{};
    };

    void set_readability_stats_enabled(bool enabled) noexcept;
    [[nodiscard]] ReadabilityStats take_readability_stats() noexcept;

    template<class T>
    bool safe_read(const void *p, T &out) {
        static_assert(std::is_trivially_copyable_v<T>, "safe_read requires a trivially-copyable type");
        if (!is_readable(p, sizeof(T))) return false;
        std::memcpy(&out, p, sizeof(T));
        return true;
    }
}
