#pragma once
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
};

class ConfigManager {
 public:
  static std::filesystem::path config_path();

  static bool load(const std::filesystem::path& path, EngineConfig& out);

  static bool save(const std::filesystem::path& path, const EngineConfig& cfg);

  static EngineConfig load_or_default();
};
}  // namespace iee::core
