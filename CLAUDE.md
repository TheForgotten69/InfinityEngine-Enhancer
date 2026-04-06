# CLAUDE

This repository keeps its operational instructions in [AGENTS.md](AGENTS.md).

Use that file as the primary source for environment expectations, build commands, runtime facts, and reverse-engineering guardrails.

Important caveats to keep in mind:

- `CResTileSet::h` is optional in this build. A null header does not imply missing deterministic metadata.
- When the header is absent, the PVR entry table in `pData` is the expected deterministic fallback.
- Runtime questions must be answered with DLL-side logging, not with Ghidra alone.
- The repo is structured to be data-driven across builds, but manifest selection is still pinned to the validated BGEE target until explicit build probing is implemented.
- Sprite-upscale v1 is shader-file-first and limited to `fpsprite.glsl` and `fpselect.glsl`; keep `fpseam`, terrain, UI, and tile work out of scope.
- Do not assume true two-pass FSR is available for sprites. The working v1 path is a one-pass shader experiment unless a sprite-local intermediate pass is proven from live runtime evidence.
