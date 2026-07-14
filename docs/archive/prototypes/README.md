# Archived Prototypes

These files preserve early experiments and are not compiled, packaged, tested, or supported.
They may contain stale offsets, obsolete APIs, duplicated types, and unsafe hooking assumptions.

- `legacy-renderer-hook.cpp` is the pre-modular renderer/hook experiment.
- `standalone-injector.cpp` is an abandoned launcher/injector; EEex is the supported loader.

Current behavior lives under `src/iee/`. Do not copy code from this directory into production
without re-validating it against the current build manifest and hooking rules.
