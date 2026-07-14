# Threading and Failure Boundaries

The engine is effectively single-threaded for OpenGL rendering, but area loading and explicit
shutdown are treated as separate producers so ownership remains clear.

## Ownership

- `LoadArea` parses WED data and publishes immutable CPU snapshots. It never creates, binds, or
  deletes OpenGL objects.
- `DrawColorTone(Seam)` is the world-pass publication point. It resolves the active area, publishes
  the view transform once per frame, and flushes pending area textures while a GL context is current.
- The swap hook advances the frame counter and refreshes time-dependent uniforms.
- Safe-read region results are cached only for that frame epoch; `LoadArea`
  advances the epoch before touching a replacement object graph.
- Shader/program records are protected by `g_probeMutex`. Steady-state OpenGL introspection and
  shader-dump I/O copy the required record state and release that mutex first. One-time probe
  installation is serialized before those records become visible.
- Area snapshot publication is protected by `g_areaGpuMutex`; GL upload uses a copied
  `shared_ptr<const AreaGpuSnapshot>` after releasing the mutex.
- Uniform inputs are independent relaxed atomics. They are a latest-value snapshot, not a
  transaction; the render thread is the only consumer that calls OpenGL.
- `AppContext::activeArea`, `infGame`, `wed`, and the hook-active flag are atomic because load and
  render callbacks may observe them at different engine boundaries.

## Hook and Exception Rules

No C++ exception may cross an engine, OpenGL, SDL, GDI, EEex, or DLL export boundary. Detours catch
all exceptions only at those ABI boundaries, preserve the original engine call exactly once, and
treat diagnostics as optional. Internal helpers use ordinary typed errors or return values and do
not silently convert a failed OpenGL operation into success.

`ShutdownBindings` must run before unloading the DLL. It first disables engine entry-point hooks,
then removes frame and OpenGL probes, removes the engine hooks, and only then uninitializes MinHook
and clears shared state. `DllMain` never performs MinHook, logger, or OpenGL teardown while the
Windows loader lock is held.

## Runtime Invariants

1. Only a thread with the owning WGL context may touch enhancer GL objects.
2. A program is reclassified after every successful link and forgotten on deletion.
3. A recreated WGL context invalidates enhancer texture names, hook entry
   points, program classifications, and uniform locations; probes are
   reinstalled against the replacement context before processing programs.
4. A new area publishes a no-liquid generation before parsing, so stale masks cannot cross an area
   transition.
5. If the frame hook is unavailable, view publication remains correct and is
   time-coalesced across Seam calls instead of using the frame epoch.
