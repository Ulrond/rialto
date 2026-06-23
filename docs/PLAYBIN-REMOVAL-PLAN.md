# Migration Plan — Main-path `playbin` removal (explicit construction)

**TL;DR** — The MSE main path builds one GStreamer `playbin` and *reacts* to its autoplugging
through three signal callbacks (`source-setup` / `element-setup` / `deep-element-added`) to
detect, configure and cache the sinks/decoders playbin creates. This plan replaces playbin with
**explicit, deterministic pipeline construction** — Rialto builds `appsrc → parse → decoder →
(convert) → sink` per stream itself and asks `IPlatformBackend` for both sinks (extending
`createVideoSink` to the **ID-keyed** form). Delivered in **5 staged commits**, each
independently green on `servergstplayer` + `servermain`, behind an opt-in switch until the
explicit path is proven, so the 100s of MSE web apps never regress. **No code is changed by this
document — it is for review.**

_Status: PROPOSAL — awaiting review. Target branch: `feature/1-explicit-construction`._

---

## 1. Why

`playbin` autoplugs ~36 elements per pipeline, constructs decoders/sinks **lazily** (only on first
buffer), and hides them behind `playsink` + auto-sinks. The `experiments/playbin-vs-explicit`
nails measured the cost: 36 vs 4 elements, a lazy-construction deadlock under dual-decode, and a
155 ms vs 0.06 ms resume penalty. Explicit construction makes the graph deterministic, lets the
SoC backend own sink (and later decoder) selection, and removes the reactive
detect-and-reconfigure machinery.

## 2. Current shape (what we are replacing)

Signal-reactive, playbin owns the graph:

- `initMsePipeline` ([GstGenericPlayer.cpp:262](media/server/gstplayer/source/GstGenericPlayer.cpp#L262)):
  makes `playbin`, sets play-flags (`setPlaybinFlags`), `uri=rialto://`, tweaks `playsink`
  (`send-event-mode=0`), goes to READY.
- **`source-setup` → `SetupSource`**: stores the `rialtosrc` element only.
- **`element-setup` → `SetupElement`**: the heavy one — detects auto-video/auto-audio sinks
  (caches their real child via `child-added`), name-prefix-matches vendor sinks
  (`amlhalasink`/`brcmaudiosink`/`westerossink`/`omx`/`rialtotexttracksink`) to set properties,
  wires underflow + first-frame callbacks, and stores `m_context.videoSink`.
- **`deep-element-added` → `DeepElementAdded`**: tracks the audio decodebin internals (parser,
  decoder, typefind) and the audio playsink bin into `m_context.playbackGroup`.
- **`AttachSource`** ([AttachSource.cpp:48](media/server/gstplayer/source/tasks/generic/AttachSource.cpp#L48)):
  per source-type makes the appsrc (`audsrc`/`vidsrc`/`subsrc`), sets caps; subtitle path builds
  a `RialtoTextTrackSink` and assigns playbin's `text-sink`.
- **`getSink`** ([GstGenericPlayer.cpp:480](media/server/gstplayer/source/GstGenericPlayer.cpp#L480)):
  reads sinks back off playbin's `audio-sink`/`video-sink`/`text-sink` properties and unwraps
  auto-sinks via the cached children.
- `m_platformBackend` is injected but **unused** in this path (stored only).

## 3. Target shape (explicit construction)

Rialto builds the graph; nothing is autoplugged. Per attached source, in `FinishSetupSource` /
`AttachSource`:

```
appsrc(audsrc) → audioparse → audiodecoder → audioconvert/resample → audioSink   (from backend)
appsrc(vidsrc) → videoparse → videodecoder →                          videoSink   (from backend)
appsrc(subsrc) →                                                       RialtoTextTrackSink
```

added directly to a plain `GstPipeline` ("media_pipeline"). Key consequences:

- **Sinks come from `IPlatformBackend`**: `createAudioSink("audiosink")` (reuses the M1 ladder) and
  `createVideoSink(...)`. No more reading them back off playbin; `m_context.videoSink`/audio sink
  are set at construction.
- **Decoder/parser selection is explicit**, keyed off the source caps the client already provides
  to `AttachSource` (the codec is known). Vendor decoder selection is the SoC backend's concern
  in a later step (a `createVideoDecoder`/`createAudioDecoder` ABI addition is out of scope here;
  this plan uses explicit factory selection by caps, matching the nails' 4-element graph).
- **The 3 callbacks change role**: `source-setup` disappears (rialtosrc is built explicitly, no
  signal). `element-setup`/`deep-element-added` reactive detection is replaced by direct
  configuration at the point each element is created — the property-setting that
  `SetupElement`/`DeepElementAdded` did reactively now runs inline in construction. Underflow and
  first-frame callbacks are connected to the explicitly-created decoder/sink.
- `playsink` and auto-sink unwrapping (`getSinkChildIfAutoVideoSink/Audio`) are deleted —
  there are no auto-sinks.

## 4. ID-keyed `createVideoSink` (folded in, per decision)

Extend the platform-backend video-sink contract from a name-only call to a **resource-ID-keyed**
one, superseding the primary/secondary boolean:

- ABI (`IPlatformBackend.h`): `createVideoSink(const std::string &name, uint32_t videoId)` (or a
  small `VideoSinkConfig{ name; planeType; planeIndex }` struct to match
  `MediaSessionConfig.output { PlaneType, planeIndex }`). The reference backend sets
  `setWesterosSinkVideoID(videoId)` on the vendor sink; the Linux reference still returns
  `autovideosink`. Session 0 → Main (id 0), Session 1 → PiP (id 1).
- This is an **additive ABI change** while still pre-release; bump `kPlatformBackendAbiVersion`
  to 2 and update `PlatformBackendMock`.
- Rationale captured already in [IPlatformBackend.h](media/server/platform/interface/IPlatformBackend.h#L96):
  `setWesterossinkSecondaryVideo` is a capability **query**, not a sink-creation, so it never fit
  `createVideoSink` as-is.

## 5. Staged delivery (each commit independently green)

1. **ABI: ID-keyed `createVideoSink`** — extend the interface + `LinuxPlatformBackend` +
   `PlatformBackendMock`, bump ABI to v2, add backend tests. No engine behaviour change yet
   (still unused in MSE). Smallest, self-contained.
2. **Explicit construction behind an opt-in switch** — add an `initMsePipelineExplicit()` that
   builds the plain `GstPipeline` + per-stream chains using the backend sinks, selected by an
   env/config flag; `playbin` stays the default. New unit tests cover the explicit path; existing
   playbin tests untouched.
3. **Migrate the audio path** off the reactive callbacks for the explicit path. **Decoder
   selection: decodebin per stream** (decided) — `appsrc → decodebin → audioconvert →
   audioresample → audioSink(backend)`; decodebin autoplugs only the decoder, keeping a
   deterministic ~4-element container without a hand-maintained codec→factory map. Sub-commits:
   - **3a** — explicit audio chain builder on `GstGenericPlayer` (make the static elements + the
     backend `createAudioSink`, add to the pipeline, link `appsrc→decodebin` and the static tail,
     register the decodebin `pad-added` handler that links the decoder src pad to `audioconvert`);
     invoked from `AttachSource`'s explicit branch. Backend reached via `IGstGenericPlayerPrivate`
     (stays encapsulated in `GstGenericPlayer`, not the context).
   - **3b** — `FinishSetupSource` explicit branch: wire the appsrc data-flow callbacks
     (need/enough/seek) directly on the chain's appsrc, bypassing `rialtosrc`/`setupAndAddAppSrc`.
   - **3c** — relocate the audio-sink/decoder property-setting that `SetupElement`/`DeepElementAdded`
     did reactively (low-latency/sync, amlhalasink waits, underflow + first-frame callbacks) into
     the explicit builder.
   - **3d** — extend the parity fixture with audio behavioural cases (attach audio → appsrc + chain;
     play/pause; EOS); construct the Explicit param with a `PlatformBackendMock`. Each sub-commit
     keeps servergstplayer + servermain green.
   Keep video on the shared sink-config helpers until stage 4.
4. **Migrate the video + subtitle paths**; wire `createVideoSink(id)`; delete auto-sink unwrapping
   and `getSink`-via-playbin for the explicit path.
5. **Flip default to explicit, remove `playbin`** — delete `initMsePipeline` (playbin),
   `setPlaybinFlags`/`getGstPlayFlag`/`shouldEnableNativeAudio`, playsink tweaks, and the
   now-dead reactive branches; collapse the switch. Final test sweep.

## 6. Test strategy

- Unit tests are fully mocked (no real GStreamer), so each stage adds explicit-path expectations
  alongside the playbin ones; the playbin tests are deleted only in stage 5.
- `GstGenericPlayerTestCommon` gains an explicit-construction `gstPlayerWillBeCreatedExplicit()`
  helper mirroring the M1 web-audio rework (backend mock returns the sinks).
- Per-stage gate: `build_ut.py -s servergstplayer servermain` green; component tests
  (`build_ct.py`) at stages 2 and 5.
- The `playbin-vs-explicit` nails remain the behavioural reference for element counts / resume.

## 7. Risks & mitigations

- **MSE backward-compat** (100s of web apps): the opt-in switch (stage 2) keeps playbin default
  until the explicit path is proven on each platform; flip is per-platform, reversible.
- **Explicit decoder selection** is the deepest unknown — caps→factory mapping must cover every
  codec playbin handled. Mitigation: enumerate the codecs from the existing caps/`AttachSource`
  paths and the rialto-gstreamer test corpus before stage 3; fall back to `decodebin` for any
  uncovered codec rather than regress.
- **Underflow / first-frame / playback-group** semantics must be preserved for AAMP telemetry —
  re-wire the same signals onto the explicit decoder/sink and assert in unit tests.
