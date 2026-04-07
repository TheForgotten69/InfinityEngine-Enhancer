# Sprite Shader Workspace

This directory holds repo-local sprite shader material.

- `runtime_baseline/`
  - Captured live shader sources from the target BGEE runtime.
- `v1/`
  - Repo-owned working copies for sprite-upscale experiments. `fpdraw.glsl` should stay baseline when validating the DLL-owned two-pass prototype.
- `easu_rcas/`
  - Reference FSR shader material kept for local adaptation.

The supported DLL path in this repository is still tile-focused. Files in this directory are for live GLSL replacement experiments against the game install and should not be treated as evidence that the DLL runtime path already supports shader interception.
