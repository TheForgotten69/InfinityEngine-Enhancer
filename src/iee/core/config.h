#pragma once
#include <cstddef>
#include <filesystem>

namespace iee::core {
struct EngineConfig {
  bool enableAnisotropicFiltering = false;
  float maxAnisotropy = 8.0f;
  float lodBias = -0.25f;

  bool dumpEngineShaders = false;
  bool enableDebugHotkeys = false;
  bool enableWaterEffect = true;

  bool enableVerboseLogging = false;
  bool enablePerformanceLogging = false;
};

struct ConfigLoadDiagnostics {
  bool fileExisted{};
  bool loadSucceeded{};
  bool defaultFileWritten{};
  std::size_t malformedLines{};
  std::size_t invalidValues{};
};

class ConfigManager {
 public:
  static std::filesystem::path config_path();

  static bool load(const std::filesystem::path& path, EngineConfig& out,
                   ConfigLoadDiagnostics* diagnostics = nullptr);

  static bool save(const std::filesystem::path& path, const EngineConfig& cfg);

  static EngineConfig load_or_default(ConfigLoadDiagnostics* diagnostics = nullptr);
};
}  // namespace iee::core
