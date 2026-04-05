# InfinityEngine-Enhancer

`InfinityEngine-Enhancer` is an EEex-loaded Windows DLL that extends Infinity Engine tile rendering without changing ARE/WED coordinates. The current supported target is BGEE `2.6.6.x`.

## What It Does

The first supported feature is 4x TIS/PVRZ tile upscaling. Scale detection is header-first:

- `TIS header + 0x14 == 0x40` means standard tiles
- `TIS header + 0x14 == 0x100` means authored 4x tiles

Some standard tilesets legitimately have no live header pointer. In those cases the runtime classifies scale from the PVR entry table before considering the old UV / texture-id heuristic fallback.

## Requirements

- Windows game runtime
- Baldur's Gate: Enhanced Edition
- EEex
- OpenGL-capable renderer

## Installation

1. Install [EEex](https://github.com/Bubb13/EEex).
2. Download the release bundle from [Releases](../../releases).
3. Copy `InfinityEngine-Enhancer.dll` into the game root.
4. Copy [M_IEEE.lua](tools/M_IEEE.lua) into the game's `override` directory.
5. Optionally start from [InfinityEngine-Enhancer.sample.ini](tools/InfinityEngine-Enhancer.sample.ini).

The runtime writes `InfinityEngine-Enhancer.ini` and `InfinityEngine-Enhancer.log` next to the game executable.

## Development

Use WSL for analysis and host-side tests. The actual DLL build is Windows-only.

- WSL host tests:
  `cmake -S . -B cmake-build-debug -DBUILD_TESTING=ON`
  `cmake --build cmake-build-debug --target iee_tests`
  `ctest --test-dir cmake-build-debug --output-on-failure`
- Windows DLL build:
  `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DIEE_BUILD_WINDOWS_DLL=ON -DBUILD_TESTING=ON`
  `cmake --build build --config Release --target release_bundle`

## Docs

- [docs/architecture.md](docs/architecture.md)
- [docs/reverse-engineering.md](docs/reverse-engineering.md)
- [docs/build-manifests.md](docs/build-manifests.md)
- [docs/tile-upscale.md](docs/tile-upscale.md)
- [AGENTS.md](AGENTS.md)

## Notes

- [shaders/InfinityEngine-Enhancer.cpp](shaders/InfinityEngine-Enhancer.cpp) is archival research code, not the supported runtime path.
- Build-specific offsets and callsites live in the manifest layer under [src/iee/game/build_manifest.cpp](src/iee/game/build_manifest.cpp).

## License

See [LICENSE](LICENSE).
