# Per-SoC platform backend shims

The ABI-conversion layer: one loadable `librialtoplatform-<soc>.so` per platform, each implementing the
versioned `IPlatformBackend` ABI the Rialto server `dlopen`s. This is the in-tree prototype; the
relocation target is `comcast-sky/rialto-platform-backends`, built against the published
`rialto-platform-abi` artifact.

## Layout

```
common/    GenericGstBackend.{h,cpp}  — the SoC-free implementation of all 11 ABI methods
           SocProfile.h               — the per-SoC delta descriptor
           PlatformBackendEntry.h     — RIALTO_DEFINE_PLATFORM_BACKEND(profile): the extern "C" loader ABI
           BackendLog.h               — dependency-free logging for the standalone .so
linux/  amlogic/  broadcom/  mediatek/  realtek/   — one SocProfile + the entry macro per platform
test/      abi_smoke.cpp              — dlopen + ABI-version check, as the loader does
```

Linux is just another SoC: its profile (auto-sinks, video-master, no SoC audio path) sits beside the
others with no privileged status.

## What lives where

`GenericGstBackend` does the shim's three jobs:

1. **Marshal** the neutral ABI types (`AudioCodecSwitchContext`, `EaseType`) to the vendor-facing wrapper
   types (`AudioAttributesPrivate`, `rgu_Ease`).
2. **Delegate** the live-graph audio ops — `audioFade`, `processAudioGap`, `switchAudioCodec`,
   `isAudioFadeSupported` — through the injected `IRdkGstreamerUtilsWrapper` to the device's
   `rdk_gstreamer_utils_<soc>.so`. The SoC-specific audio behaviour comes from the vendor lib the box
   already ships; the shim names no SoC.
3. **Implement** the methods the wrapper has no equivalent for — `buildAudioPath` / `buildVideoPath`
   (topology), `getSupportedProperties` (capability authority), `isVideoMaster`, `createAudioSink`,
   `applyPlaybackRate`.

Everything a platform actually varies is captured in its `SocProfile`: audio/video sink element names,
topology kind (split decode vs fused sink), master role, whether it has a SoC audio path, and whether
video binds a plane.

## Build & test

```
./test.sh
```

Run it from anywhere. If you are already inside `rialto-build:local` it builds and tests directly;
otherwise it relaunches itself in that image via `sc` and runs the same steps. It:

1. builds all five `.so` and dlopens each to confirm it reports the expected ABI version with the loader
   entrypoints resolved (`test/abi_smoke.cpp`);
2. runs an **end-to-end playback proof** (`test/playback_proof.cpp`): the real `PlatformBackendLoader`
   dlopens a shim (production discovery + ABI gate + destroy/dlclose), hands it real gst/glib wrappers,
   and lets `GenericGstBackend` build and drive an actual Opus/Ogg decode pipeline to EOS.

The proof runs against a **fakesink fixture backend** (`test/TestProfileFakesink.cpp`), not one of the five
shipped shims: the mocked-unit image has core+base GStreamer but no vendor/autodetect sinks, so the leaf is
`fakesink` while the loader + `GenericGstBackend` path is exactly a shipped SoC's. Real-sink playback
(`autovideosink`, `amlhalasink`, …) belongs on a target with those plugins.

## Status

`linux/` and `amlogic/` carry real deltas. `broadcom/`, `mediatek/`, and `realtek/` are ABI-conformant
stubs — their `SocProfile` fields marked `TBD` are placeholders to confirm against the vendor lib before
certification (issue #13 Slice E).

## Extension points

`SocProfile` is pure data. A genuine behavioural delta that cannot be expressed as data — a westeros plane
binding (`setWesterosSinkVideoID`) or a `SegmentToSinkPad` playback-rate — is added as a hook on
`SocProfile` (a function pointer) or by subclassing `GenericGstBackend`, only when a SoC actually needs it.
Today both are recorded as intent (`bindsVideoPlane`, `RateStrategy`) and handled generically.
