#include "config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <optional>
#include <stdexcept>

#ifdef _WIN64
#include <windows.h>
#endif

namespace iee::core {
static std::string trim(std::string s) {
  auto issp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c) { return !issp(c); }));
  s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c) { return !issp(c); }).base(),
          s.end());
  return s;
}

static bool ensure_parent_dir(const std::filesystem::path& p) {
  std::error_code ec;
  const auto d = p.parent_path();
  if (d.empty() || std::filesystem::exists(d)) return true;
  return std::filesystem::create_directories(d, ec) && !ec;
}

static bool iequals(std::string a, std::string b) {
  const auto lower = [](unsigned char c) { return static_cast<char>(::tolower(c)); };
  std::transform(a.begin(), a.end(), a.begin(), lower);
  std::transform(b.begin(), b.end(), b.begin(), lower);
  return a == b;
}

static std::optional<bool> parse_bool(const std::string& s) {
  if (iequals(s, "1") || iequals(s, "true") || iequals(s, "yes") || iequals(s, "on")) return true;
  if (iequals(s, "0") || iequals(s, "false") || iequals(s, "no") || iequals(s, "off")) return false;
  return std::nullopt;
}

static std::optional<float> parse_float(const std::string& s) {
  try {
    std::size_t parsedBytes = 0;
    const float value = std::stof(s, &parsedBytes);
    return parsedBytes == s.size() && std::isfinite(value) ? std::optional<float>{value}
                                                           : std::nullopt;
  } catch (const std::invalid_argument&) {
    return std::nullopt;
  } catch (const std::out_of_range&) {
    return std::nullopt;
  }
}

static void normalize(EngineConfig& cfg) noexcept {
  if (!std::isfinite(cfg.maxAnisotropy)) cfg.maxAnisotropy = 8.0f;
  if (!std::isfinite(cfg.lodBias)) cfg.lodBias = -0.25f;
  cfg.maxAnisotropy = std::clamp(cfg.maxAnisotropy, 1.0f, 64.0f);
  cfg.lodBias = std::clamp(cfg.lodBias, -4.0f, 4.0f);
}

static void apply_kv(EngineConfig& cfg, const std::string& section, const std::string& key,
                     const std::string& val, ConfigLoadDiagnostics* diagnostics) {
  const auto assign_bool = [&](bool& target) {
    if (const auto parsed = parse_bool(val)) {
      target = *parsed;
    } else if (diagnostics) {
      ++diagnostics->invalidValues;
    }
  };
  const auto assign_float = [&](float& target) {
    if (const auto parsed = parse_float(val)) {
      target = *parsed;
    } else if (diagnostics) {
      ++diagnostics->invalidValues;
    }
  };

  // [Core]
  if (iequals(section, "core")) {
    if (iequals(key, "VerboseLogs"))
      assign_bool(cfg.enableVerboseLogging);
    else if (iequals(key, "PerformanceLogs"))
      assign_bool(cfg.enablePerformanceLogging);
    return;
  }

  // [Rendering]
  if (iequals(section, "rendering")) {
    if (iequals(key, "EnableAnisotropicFiltering"))
      assign_bool(cfg.enableAnisotropicFiltering);
    else if (iequals(key, "MaxAnisotropy"))
      assign_float(cfg.maxAnisotropy);
    else if (iequals(key, "LODBias"))
      assign_float(cfg.lodBias);
    return;
  }

  // [Shaders]
  if (iequals(section, "shaders")) {
    if (iequals(key, "DumpEngineShaders"))
      assign_bool(cfg.dumpEngineShaders);
    else if (iequals(key, "EnableDebugHotkeys"))
      assign_bool(cfg.enableDebugHotkeys);
    else if (iequals(key, "EnableWaterEffect"))
      assign_bool(cfg.enableWaterEffect);
    return;
  }
}

static void write_section(std::ofstream& f, const char* name) { f << "\n[" << name << "]\n"; }

static void write_bool(std::ofstream& f, const char* key, bool v) {
  f << key << " = " << (v ? "true" : "false") << "\n";
}

std::filesystem::path ConfigManager::config_path() {
#ifdef _WIN64
  wchar_t wpath[MAX_PATH]{};
  GetModuleFileNameW(GetModuleHandleW(nullptr), wpath, MAX_PATH);
  std::filesystem::path p(wpath);
  p = p.remove_filename() / "InfinityEngine-Enhancer.ini";
  return p;
#else
  return std::filesystem::current_path() / "InfinityEngine-Enhancer.ini";
#endif
}

bool ConfigManager::load(const std::filesystem::path& path, EngineConfig& out,
                         ConfigLoadDiagnostics* diagnostics) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }

  EngineConfig cfg = out;
  std::string section;
  std::string line;

  while (std::getline(in, line)) {
    auto raw = trim(line);
    if (raw.empty() || raw[0] == ';' || raw[0] == '#') continue;

    if (raw.front() == '[' && raw.back() == ']') {
      section = trim(raw.substr(1, raw.size() - 2));
      continue;
    }

    auto eq = raw.find('=');
    if (eq == std::string::npos) {
      if (diagnostics) ++diagnostics->malformedLines;
      continue;
    }

    auto key = trim(raw.substr(0, eq));
    auto val = trim(raw.substr(eq + 1));
    apply_kv(cfg, section, key, val, diagnostics);
  }

  normalize(cfg);
  out = cfg;
  return true;
}

bool ConfigManager::save(const std::filesystem::path& path, const EngineConfig& cfg) {
  if (!ensure_parent_dir(path)) return false;

  std::ofstream f(path, std::ios::trunc);
  if (!f) return false;

  f << "; InfinityEngine-Enhancer configuration\n";
  f << "; Lines starting with ';' or '#' are comments.\n";
  f << "; Hook addresses are resolved from the validated build manifest.\n\n";

  write_section(f, "Core");
  write_bool(f, "VerboseLogs", cfg.enableVerboseLogging);
  write_bool(f, "PerformanceLogs", cfg.enablePerformanceLogging);

  write_section(f, "Rendering");
  write_bool(f, "EnableAnisotropicFiltering", cfg.enableAnisotropicFiltering);
  f << "MaxAnisotropy = " << cfg.maxAnisotropy << "\n";
  f << "LODBias = " << cfg.lodBias << "\n";

  write_section(f, "Shaders");
  write_bool(f, "DumpEngineShaders", cfg.dumpEngineShaders);
  write_bool(f, "EnableDebugHotkeys", cfg.enableDebugHotkeys);
  write_bool(f, "EnableWaterEffect", cfg.enableWaterEffect);

  return true;
}

EngineConfig ConfigManager::load_or_default(ConfigLoadDiagnostics* diagnostics) {
  if (diagnostics) *diagnostics = {};
  EngineConfig cfg{};
  const auto path = config_path();
  std::error_code existsError;
  const bool exists = std::filesystem::exists(path, existsError);
  if (diagnostics) diagnostics->fileExisted = exists && !existsError;
  if (existsError || !exists) {
    const bool written = !existsError && save(path, cfg);
    if (diagnostics) diagnostics->defaultFileWritten = written;
    return cfg;
  }
  const bool loaded = load(path, cfg, diagnostics);
  if (diagnostics) diagnostics->loadSucceeded = loaded;
  return cfg;
}
}  // namespace iee::core
