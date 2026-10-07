#pragma once
#include <cstddef>
#include <filesystem>

namespace iee::core {
struct EngineConfig {
  bool enableAnisotropicFiltering = false;
  float maxAnisotropy = 8.0f;
  float lodBias = -0.25f;
  bool smoothSpriteMovement = false;
  bool interpolateAnimations = false;
  bool softFogOfWar = false;
  float softFogRadius = 24.0f;  // world pixels; 0 = capture without blur
  bool bloom = false;
  float bloomStrength = 0.35f;
  // How strongly light-emitting effects brighten what is around them; 0 = off.
  float lightSpill = 1.5f;

  bool dumpEngineShaders = false;
  bool enableDebugHotkeys = false;
  bool enableWaterEffect = true;
  bool enablePointEffects = true;

  bool enableAreaAnimationScan = true;

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
