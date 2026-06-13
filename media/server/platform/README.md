<!--
Copyright 2026 RDK Management. Licensed under the Apache License, Version 2.0.
-->
# media/server/platform — SoC platform backend

Isolates SoC-specific media element selection behind a **frozen, versioned ABI**
(`IPlatformBackend`), so the vendor layer can be upgraded without rebuilding or
re-certifying the core (Phase 1 — see `Ulrond/rialto#1`).

Today the engine inlines a registry-probe ladder — `amlhalasink` / `rtkaudiosink`
for audio, `westerossink` for video, with `autoaudiosink` / `autovideosink` as the
Linux fallback. Phase 1 moves that selection here: the engine asks the backend to
make the platform's sinks and carries **no SoC names**.

## Status / plan

- [x] `interface/IPlatformBackend.h` — the versioned ABI + `extern "C"` loader.
- [ ] `include/`+`source/` `LinuxPlatformBackend` — `autoaudiosink` / `autovideosink`;
      the reference backend and the **playback proof on NATIVE_BUILD** (Rialto for
      Linux).
- [ ] Move the `amlhalasink`/`rtkaudiosink`/`westerossink` ladders out of
      `GstWebAudioPlayer` / `GstGenericPlayer` into device backends.
- [ ] Convert the player factory's link-time `make_shared` into a `dlopen` loader
      that resolves the `extern "C"` entrypoints and version-checks the backend.
- [ ] CMake: build `librialto-platform-linux.so`; wire `add_subdirectory(platform)`.

Phase 1 keeps GStreamer as the engine (sinks are `GstElement*`); the engine-neutral
generalisation is Phase 2 (`Ulrond/rialto#2`).
