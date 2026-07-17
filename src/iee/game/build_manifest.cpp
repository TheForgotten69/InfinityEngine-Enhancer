#include "build_manifest.h"

#ifdef _WIN64
#include <windows.h>

#include <array>
#include <cwchar>
#include <limits>
#include <vector>
#endif

namespace iee::game {
namespace {
constexpr bool is_hex_char(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

constexpr bool is_wildcard_token(std::string_view token) noexcept {
  return token == "?" || token == "??";
}

constexpr bool is_hex_token(std::string_view token) noexcept {
  return token.size() == 2 && is_hex_char(token[0]) && is_hex_char(token[1]);
}

constexpr bool validate_pattern_format(std::string_view pattern) noexcept {
  if (pattern.empty()) {
    return false;
  }

  std::size_t tokenStart = 0;
  bool sawToken = false;
  while (tokenStart < pattern.size()) {
    while (tokenStart < pattern.size() && pattern[tokenStart] == ' ') {
      ++tokenStart;
    }
    if (tokenStart >= pattern.size()) {
      break;
    }

    std::size_t tokenEnd = tokenStart;
    while (tokenEnd < pattern.size() && pattern[tokenEnd] != ' ') {
      ++tokenEnd;
    }

    const auto token = pattern.substr(tokenStart, tokenEnd - tokenStart);
    if (!is_hex_token(token) && !is_wildcard_token(token)) {
      return false;
    }

    sawToken = true;
    tokenStart = tokenEnd;
  }

  return sawToken;
}

std::string normalize_product_name(std::string_view productName) {
  std::string normalized;
  normalized.reserve(productName.size());
  for (const unsigned char ch : productName) {
    if (ch >= 'A' && ch <= 'Z') {
      normalized.push_back(static_cast<char>(ch - 'A' + 'a'));
    } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      normalized.push_back(static_cast<char>(ch));
    }
  }
  return normalized;
}

#ifdef _WIN64
std::string wide_to_utf8(std::wstring_view value) {
  if (value.empty() || value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    return {};
  }
  const auto inputLength = static_cast<int>(value.size());
  const int outputLength =
      WideCharToMultiByte(CP_UTF8, 0, value.data(), inputLength, nullptr, 0, nullptr, nullptr);
  if (outputLength <= 0) return {};
  std::string output(static_cast<std::size_t>(outputLength), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, value.data(), inputLength, output.data(), outputLength,
                          nullptr, nullptr) != outputLength) {
    return {};
  }
  return output;
}

std::string read_product_name(const std::vector<std::byte>& versionData) {
  struct LanguageAndCodePage {
    WORD language;
    WORD codePage;
  };

  LanguageAndCodePage* translations = nullptr;
  UINT translationBytes = 0;
  std::array<LanguageAndCodePage, 3> fallbacks{{
      {0x0409, 0x04B0},
      {0x0409, 0x04E4},
      {0x0000, 0x04B0},
  }};
  const LanguageAndCodePage* candidates = fallbacks.data();
  std::size_t candidateCount = fallbacks.size();
  if (VerQueryValueW(versionData.data(), L"\\VarFileInfo\\Translation",
                     reinterpret_cast<void**>(&translations), &translationBytes) &&
      translations && translationBytes >= sizeof(LanguageAndCodePage)) {
    candidates = translations;
    candidateCount = translationBytes / sizeof(LanguageAndCodePage);
  }

  const auto readField = [&](const wchar_t* fieldName) -> std::string {
    for (std::size_t index = 0; index < candidateCount; ++index) {
      wchar_t query[96]{};
      if (swprintf_s(query, sizeof(query) / sizeof(query[0]),
                     L"\\StringFileInfo\\%04x%04x\\%ls",
                     static_cast<unsigned>(candidates[index].language),
                     static_cast<unsigned>(candidates[index].codePage), fieldName) <= 0) {
        continue;
      }
      wchar_t* value = nullptr;
      UINT valueChars = 0;
      if (!VerQueryValueW(versionData.data(), query, reinterpret_cast<void**>(&value),
                          &valueChars) ||
          !value || valueChars <= 1) {
        continue;
      }
      std::size_t length = valueChars;
      if (value[length - 1] == L'\0') --length;
      if (auto utf8 = wide_to_utf8(std::wstring_view(value, length)); !utf8.empty()) return utf8;
    }
    return {};
  };

  if (auto productName = readField(L"ProductName"); !productName.empty()) return productName;
  return readField(L"FileDescription");
}
#endif

constexpr BuildManifest kKnownBuilds[] = {
    {
        "BGEE 2.6.6.x",
        {"Baldur's Gate Enhanced Edition", "Baldur's Gate"},
        {2, 6, 6, ExecutableVersion::kAnyRevision},
        {
            "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 48 FD FF FF",
            "48 8B C4 44 89 48 20 48 83 EC 48 48 89 58 08 8B DA 48 89 68 10",
            // CGameObjectArray::GetShare (EEex InfinityLoader.db, version-
            // independent section). Verified unique on 2.6.6.0 at 0x276490.
            "48 C7 02 00 00 00 00 83 F9 FF",
        },
        {0x27E710, 0x4247E0, 0x276490},
        {0x100, 0x1DC, 0x14, 0x6590, 0x6598, 0x65F8},
        {{
            {"CRes_Demand", 0x36, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawBindTexture", 0x6E, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawDisable", 0x7F, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawColor", 0x89, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawPushState", 0x91, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawColorTone", 0xB6, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawBegin", 0xC0, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawTexCoord", 0xCD, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawVertex", 0xDB, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawEnd", 0x17A, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawPopState", 0x1AD, BranchInstructionKind::JmpRel32, 0xE9, 1, 5, true},
        }},
    },
    // Offline-validated 2026-07-16 (docs/validation/bgee-2.7.3-evidence.md):
    // both signatures match exactly once, all 11 callsites decode at the same
    // intra-function offsets, and CVidTile::pRes is read at RenderTexture+0x1D
    // as on 2.6.6. Only the function RVAs moved.
    {
        "BGEE 2.7.3.x",
        {"Baldur's Gate Enhanced Edition", "Baldur's Gate"},
        {2, 7, 3, ExecutableVersion::kAnyRevision},
        {
            "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 48 FD FF FF",
            "48 8B C4 44 89 48 20 48 83 EC 48 48 89 58 08 8B DA 48 89 68 10",
            // Same EEex version-independent pattern. Offline-verified on the
            // 2.7.3.0 binary: unique match, identical body shape, globals at
            // 0x68F8F4 (max index) / 0x68F910 (entry table).
            "48 C7 02 00 00 00 00 83 F9 FF",
        },
        {0x27EBD0, 0x4257C0, 0x276700},
        {0x100, 0x1DC, 0x14, 0x6590, 0x6598, 0x65F8},
        {{
            {"CRes_Demand", 0x36, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawBindTexture", 0x6E, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawDisable", 0x7F, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawColor", 0x89, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawPushState", 0x91, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawColorTone", 0xB6, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawBegin", 0xC0, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawTexCoord", 0xCD, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawVertex", 0xDB, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawEnd", 0x17A, BranchInstructionKind::CallRel32, 0xE8, 1, 5, true},
            {"DrawPopState", 0x1AD, BranchInstructionKind::JmpRel32, 0xE9, 1, 5, true},
        }},
    },
};

static_assert(validate_pattern_format(kKnownBuilds[0].patterns.loadArea),
              "LoadArea pattern format is invalid");
static_assert(validate_pattern_format(kKnownBuilds[0].patterns.renderTexture),
              "RenderTexture pattern format is invalid");
static_assert(validate_pattern_format(kKnownBuilds[0].patterns.objectArrayGetShare),
              "GetShare pattern format is invalid");
static_assert(kKnownBuilds[0].validate(), "Known build manifest is invalid");
static_assert(validate_pattern_format(kKnownBuilds[1].patterns.loadArea),
              "2.7.3 LoadArea pattern format is invalid");
static_assert(validate_pattern_format(kKnownBuilds[1].patterns.renderTexture),
              "2.7.3 RenderTexture pattern format is invalid");
static_assert(validate_pattern_format(kKnownBuilds[1].patterns.objectArrayGetShare),
              "2.7.3 GetShare pattern format is invalid");
static_assert(kKnownBuilds[1].validate(), "2.7.3 build manifest is invalid");
}  // namespace

const BuildManifest& current_manifest() noexcept { return kKnownBuilds[0]; }

std::optional<std::reference_wrapper<const BuildManifest>> find_manifest(
    std::string_view buildId) noexcept {
  for (const auto& manifest : kKnownBuilds) {
    if (manifest.buildId == buildId) {
      return manifest;
    }
  }
  return std::nullopt;
}

std::optional<std::reference_wrapper<const BuildManifest>> find_manifest_for_version(
    std::uint16_t major, std::uint16_t minor, std::uint16_t patch,
    std::uint16_t revision) noexcept {
  for (const auto& manifest : kKnownBuilds) {
    if (manifest.executableVersion.matches(major, minor, patch, revision)) {
      return manifest;
    }
  }
  return std::nullopt;
}

bool supports_product_name(const BuildManifest& manifest, std::string_view productName) {
  const auto normalizedCandidate = normalize_product_name(productName);
  if (normalizedCandidate.empty()) return false;
  for (const auto expected : manifest.supportedProductNames) {
    if (!expected.empty() && normalize_product_name(expected) == normalizedCandidate) return true;
  }
  return false;
}

const BuildManifest* detect_manifest(ExecutableVersion* detectedVersion,
                                     std::string* detectedProductName) noexcept {
  if (detectedVersion) *detectedVersion = {};
  if (detectedProductName) detectedProductName->clear();
#ifdef _WIN64
  try {
    wchar_t executablePath[MAX_PATH]{};
    const auto pathLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH) {
      return nullptr;
    }

    DWORD ignored = 0;
    const auto versionBytes = GetFileVersionInfoSizeW(executablePath, &ignored);
    if (versionBytes == 0) {
      return nullptr;
    }

    std::vector<std::byte> versionData(versionBytes);
    if (!GetFileVersionInfoW(executablePath, 0, versionBytes, versionData.data())) {
      return nullptr;
    }

    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedInfoBytes = 0;
    if (!VerQueryValueW(versionData.data(), L"\\", reinterpret_cast<void**>(&fixedInfo),
                        &fixedInfoBytes) ||
        !fixedInfo || fixedInfoBytes < sizeof(VS_FIXEDFILEINFO) ||
        fixedInfo->dwSignature != 0xFEEF04BD) {
      return nullptr;
    }

    const auto major = static_cast<std::uint16_t>(HIWORD(fixedInfo->dwFileVersionMS));
    const auto minor = static_cast<std::uint16_t>(LOWORD(fixedInfo->dwFileVersionMS));
    const auto patch = static_cast<std::uint16_t>(HIWORD(fixedInfo->dwFileVersionLS));
    const auto revision = static_cast<std::uint16_t>(LOWORD(fixedInfo->dwFileVersionLS));
    if (detectedVersion) *detectedVersion = {major, minor, patch, revision};
    const auto productName = read_product_name(versionData);
    if (detectedProductName) *detectedProductName = productName;
    if (const auto manifest = find_manifest_for_version(major, minor, patch, revision);
        manifest && supports_product_name(manifest->get(), productName)) {
      return &manifest->get();
    }
    return nullptr;
  } catch (...) {
    return nullptr;
  }
#else
  (void)detectedVersion;
  return nullptr;
#endif
}
}  // namespace iee::game
