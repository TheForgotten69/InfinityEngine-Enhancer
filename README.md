# InfinityEngine-Enhancer

`InfinityEngine-Enhancer` is an EEex-loaded Windows DLL that extends Infinity Engine tile rendering without changing ARE/WED coordinates. The validated target is BGEE `2.6.6.x`; newer executables, including BGEE 2.7, are detected and rejected until their manifests are validated.

## What It Does

The first supported feature is higher-resolution TIS/PVRZ tile rendering, with an optional WED-masked water shader. Scale detection is header-first:

- `TIS header + 0x14` is the authored tile dimension
- explicit `64`, `128`, `256`, and `512` dimensions map to `1x`, `2x`, `4x`, and `8x`

Some standard tilesets legitimately have no live header pointer. In those cases the runtime classifies scale from the PVR entry table before considering the old UV / texture-id heuristic fallback.

Runtime tileset decisions and configured GL textures are cached only for the
current area, with fixed capacities, so visiting more maps does not accumulate
cache state. Set `PerformanceLogs = true` in the `[Core]` section to emit
five-second CPU timing windows (including per-frame p95), shader-feed work,
safe-read cache, and GL texture-configuration counters.

## Requirements

- Windows game runtime
- Baldur's Gate: Enhanced Edition
- EEex
- OpenGL-capable renderer

## Installation

1. Install [EEex](https://github.com/Bubb13/EEex).
2. Download the release bundle from [Releases](../../releases).
3. Copy `InfinityEngine-Enhancer.dll` and `iee-textures/` into the game root.
4. Copy the contents of `override/` (the EEex loader script and shader) into the game's `override` directory.
5. Optionally copy [InfinityEngine-Enhancer.sample.ini](tools/InfinityEngine-Enhancer.sample.ini) to the game root as `InfinityEngine-Enhancer.ini` and customize it.

The runtime writes `InfinityEngine-Enhancer.ini` and `InfinityEngine-Enhancer.log` next to the game executable.
Optional BC1, BC3, BC5, or BC7 DDS water-texture overrides can be placed in
`iee-textures/`; supported layouts and format guidance are documented in
[`assets/game-textures/README.md`](assets/game-textures/README.md).

## Development

Use WSL for analysis and host-side tests. The actual DLL build is Windows-only.

- WSL host tests:
  `cmake -S . -B cmake-build-debug -DBUILD_TESTING=ON`
  `cmake --build cmake-build-debug --target iee_tests`
  `ctest --test-dir cmake-build-debug --output-on-failure`
- Windows DLL build:
  `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DIEE_BUILD_WINDOWS_DLL=ON -DBUILD_TESTING=ON`
  `cmake --build build --config Release --target release_bundle`

`cmake --install build --config Release --prefix <directory>` produces the
same game-root layout as `release_bundle`.

C++ changes follow the repository's Google-derived [`.clang-format`](.clang-format).

GitHub Dependabot maintains workflow actions. Commit-pinned CMake
`FetchContent` dependencies are described by [`renovate.json`](renovate.json);
install the Renovate GitHub App for the repository to enable those update PRs
while keeping builds reproducibly pinned to full commit SHAs.

## Docs

- [docs/architecture.md](docs/architecture.md)
- [docs/threading-model.md](docs/threading-model.md)
- [docs/reverse-engineering.md](docs/reverse-engineering.md)
- [docs/build-manifests.md](docs/build-manifests.md)
- [docs/new-build-validation.md](docs/new-build-validation.md)
- [docs/tile-upscale.md](docs/tile-upscale.md)
- [docs/superpowers/specs/2026-06-10-graphics-enhancement-roadmap-design.md](docs/superpowers/specs/2026-06-10-graphics-enhancement-roadmap-design.md) — graphics enhancement roadmap
- [CLAUDE.md](CLAUDE.md)

## Notes

- Unsupported experiments live under [docs/archive/prototypes](docs/archive/prototypes/README.md).
- Build-specific offsets and callsites live in the manifest layer under [src/iee/game/build_manifest.cpp](src/iee/game/build_manifest.cpp).

## License

See [LICENSE](LICENSE).
