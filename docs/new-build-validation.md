# Validating A New Game Build

## Purpose

This is the minimum runbook for adding a game executable version to the
supported manifest set. A matching byte sequence alone is not sufficient:
hooks run inside the game process, so every target, callsite, and object offset
must be independently validated. Unknown builds remain fail-closed until the
whole checklist passes.

## Evidence To Capture

Keep the following with the change or in a linked validation note:

- game, store/distribution, executable path, fixed file version, and SHA-256;
- image base and the candidate RVAs for `CInfGame::LoadArea` and
  `CVidTile::RenderTexture`;
- unique executable-section signatures for both targets;
- bytes and decoded instruction for every render callsite;
- validated offsets for `CVidTile::pRes`, the TIS linear flag, the TIS header
  dimension, and all active-area fields in `RuntimeOffsets`;
- one clean-start log and the smoke-test results below.

Do not copy offsets from a nearby version without revalidating them. Avoid
signatures that include relocations or addresses, and include enough stable
instructions to produce exactly one executable-section match.

## Offline Validation

1. Read the executable fixed version and compute its SHA-256.
2. Locate both hook functions in a disassembler and record their RVAs.
3. Build candidate IDA-style patterns from stable prologue/body instructions.
4. Run the project's scanner against the executable image. Each required
   pattern must have exactly one executable-section match at the expected
   function.
5. Decode all 11 `RenderTexture` descriptors. Confirm opcode, displacement
   location, instruction length, resolved target, and semantic callee. In
   particular, retain the `DrawPopState` tail `JMP` distinction if present.
6. Revalidate runtime layouts using cross-references and live inspection. The
   loaded-area flag, WED pointer, tileset fields, and active-area array/master
   pointers must point to readable objects with plausible values.

## Manifest And Tests

1. Add a distinct `BuildManifest` entry with the exact fixed version.
2. Add tests for manifest validation, lookup by id, selection by version, and
   rejection of adjacent unknown versions.
3. Add or update fixtures for any layout or file-format difference; do not put
   version-specific branching into rendering features.
4. Run host tests with warnings-as-errors and sanitizers:

   ```text
   cmake -S . -B build -DBUILD_TESTING=ON \
     -DIEE_WARNINGS_AS_ERRORS=ON -DIEE_ENABLE_SANITIZERS=ON
   cmake --build build --target iee_tests --parallel
   ctest --test-dir build --output-on-failure
   ```

5. Run the Windows Release build and native tests:

   ```text
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
     -DIEE_BUILD_WINDOWS_DLL=ON -DBUILD_TESTING=ON \
     -DIEE_WARNINGS_AS_ERRORS=ON
   cmake --build build --config Release --parallel
   cmake --build build --config Release --target release_bundle
   ctest --test-dir build -C Release --output-on-failure
   ```

## In-Game Gates

Use a clean installation first, with verbose and performance logging enabled.
Exercise at least one standard tileset, every supported authored scale present
in the target content, one water area, an area transition, resize/fullscreen
transition, save/load, and shutdown. Confirm:

- the intended manifest is selected and both scanner match counts are one;
- all draw callsites resolve inside the game module;
- standard areas delegate correctly and authored tiles preserve 64x64 world
  geometry while sampling the expected span;
- WED masks, tint sampling, and water motion stay world-aligned while scrolling
  and zooming;
- context replacement logs a successful probe reinstall and does not retain
  stale GL object names;
- no recurring GL error, hook retry, failed texture upload, or malformed config
  warning appears;
- performance windows have plausible sample counts and no unexplained p95/max
  regression against the currently supported build;
- `ShutdownBindings` runs before unload and the game exits cleanly.

Only after those gates pass should the README and manifest documentation claim
support for the new executable version.
