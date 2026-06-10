# CLAUDE

This repository keeps its operational instructions in [AGENTS.md](AGENTS.md).

Use that file as the primary source for environment expectations, build commands, runtime facts, and reverse-engineering guardrails.

Important caveats to keep in mind:

- `CResTileSet::h` is optional in this build. A null header does not imply missing deterministic metadata.
- When the header is absent, the PVR entry table in `pData` is the expected deterministic fallback.
- Runtime questions must be answered with DLL-side logging, not with Ghidra alone.
- The repo is structured to be data-driven across builds, but manifest selection is still pinned to the validated BGEE target until explicit build probing is implemented.
- The engine does zero lighting work; day/night is an authored-asset swap. Do not propose "lighting" features without reading the roadmap spec first.
- Never detour `CVisibilityMap::BltFogOWar3d` (confirmed crasher).
- Rendering feature ideas are pre-evaluated in `docs/superpowers/specs/2026-06-10-graphics-enhancement-roadmap-design.md` — check its idea ledger (future/dropped/rejected) before re-proposing.
