#pragma once

namespace iee::game::texture_units {

// Shared texture-unit contract for IEE-owned shader resources. Keep this list
// centralized as new effects are added so independent features cannot silently
// bind over one another.
inline constexpr unsigned AreaMask = 2;
inline constexpr unsigned WaterNormal = 3;
inline constexpr unsigned WaterDudv = 4;
inline constexpr unsigned WaterFoam = 5;
inline constexpr unsigned EffectsNoise = 6;

}  // namespace iee::game::texture_units
