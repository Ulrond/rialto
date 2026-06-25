<!--
Copyright 2026 RDK Management. Licensed under the Apache License, Version 2.0.
-->
# media/server/platform — SoC platform backend

Isolates SoC-specific media element selection behind a **frozen, versioned ABI**
(`IPlatformBackend`), so the vendor layer can be upgraded without rebuilding or
re-certifying the core (Phase 1 — see `Ulrond/rialto#1`).

The engine names **no SoC**: it asks the backend to make the platform's sinks
(`createAudioSink` / `createVideoSink`). At startup `PlatformBackendLoader` discovers and
`dlopen`s a per-SoC backend object; when none is present it uses the built-in reference
backend (`LinuxPlatformBackend` → `autoaudiosink` / `autovideosink`) in-process, so
Rialto-for-Linux always plays with zero configuration.

Phase 1 keeps GStreamer as the engine (sinks are `GstElement*`); the engine-neutral
generalisation is Phase 2 (`Ulrond/rialto#2`).

## Vendor backend `.so` contract

A per-SoC backend is a shared object implementing `IPlatformBackend` and exporting the
three versioned `extern "C"` entrypoints from `interface/IPlatformBackend.h`:

| Entrypoint | Signature |
| --- | --- |
| `rialtoPlatformBackendAbiVersion` | `uint32_t (void)` — returns the ABI version the object was built against |
| `rialtoCreatePlatformBackend` | `IPlatformBackend *(const PlatformHostContext *host)` — constructs the backend from the host's gst/glib wrappers |
| `rialtoDestroyPlatformBackend` | `void (IPlatformBackend *backend)` — destroys an instance from `rialtoCreatePlatformBackend` |

- **Filename:** `librialtoplatform-<soc>.so` (e.g. `librialtoplatform-amlogic.so`).
- **ABI version:** `rialtoPlatformBackendAbiVersion()` MUST equal the core's
  `kPlatformBackendAbiVersion`. The loader refuses any object reporting a different version
  (logs the file and both versions, `dlclose`s without creating a backend) and falls back
  to the reference backend. Bumps are additive; old backends are not re-certified.
- **Sinks:** `createAudioSink` returns the vendor audio sink (e.g. `amlhalasink`,
  `rtkaudiosink`); `createVideoSink(name, videoId)` returns the plane-bound vendor video
  sink (e.g. `westerossink` via `setWesterosSinkVideoID(videoId)`; `videoId` 0 = Main,
  1 = PiP). Returned elements carry a floating ref for the caller to add to the pipeline.

## Discovery and lifetime

The loader resolves exactly one external backend per process, in order:

1. **`RIALTO_PLATFORM_BACKEND`** — absolute path to a specific `.so` (bring-up / override).
2. A single **`librialtoplatform-*.so`** in **`RIALTO_PLATFORM_DIR`**, else the compile-time
   default directory (`-DRIALTO_PLATFORM_BACKEND_DIR`, default `/usr/lib/rialto/platform`).
3. The built-in **reference backend** (no `dlopen`).

A loaded backend is owned by a `std::shared_ptr<IPlatformBackend>` whose deleter calls
`rialtoDestroyPlatformBackend` then `dlclose`, so the object stays mapped exactly as long as
the backend instance lives. The reference fallback uses a plain deleter (no `dlclose`).

## Files

- `interface/IPlatformBackend.h` — the versioned ABI + the `extern "C"` loader entrypoints.
- `interface/IPlatformBackendLoader.h`, `include/PlatformBackendLoader.h`,
  `source/PlatformBackendLoader.cpp` — discovery, `dlopen`, version-check, fallback.
- `include/LinuxPlatformBackend.h`, `source/LinuxPlatformBackend.cpp` — the reference
  backend and the playback proof on `NATIVE_BUILD` (Rialto for Linux).

## Build

```bash
cmake -S media/server/platform -B media/server/platform/build && \
cmake --build media/server/platform/build      # librialtoplatform-linux.so
```

The reference backend is also compiled into the core as the in-process fallback; the
standalone build above produces it as a `librialtoplatform-linux.so` a device may ship as
its vendor object via the override path.
