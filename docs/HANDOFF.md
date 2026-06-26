# Session Handoff — Rialto SoC-isolation + Transformation

**TL;DR** — Paste this file's contents into the first message of a new Claude Code session
to continue the Rialto transformation work. It captures the state that is *not* recoverable
from the repo alone: the two-project map, what's done, and what's pending. This is a **living
document** — ask me to "update the handoff" whenever the state moves and I'll refresh it.

_Last updated: 2026-06-26 (#6 dlopen-loader **implemented + merged to master `637a11de` and PUSHED to origin/Ulrond**; issue #6 still OPEN — merge subject `(#6)` lacked a closing keyword, needs manual close like #5;
two new OpenSpec changes written + validated — `rialto-conformance-suite` and `complete-soc-platform-isolation`;
architecture diagrams added; SoC migration scoped + classified)_

---

## Session update — 2026-06-26

**#6 dlopen platform-backend loader — DONE, merged to `master`, PUSHED to origin/Ulrond.**
Branch `feature/6-dlopen-platform-backend`; squashed commit `857722a7` + `--no-ff` merge `637a11de` on
`master`, now **in sync with `origin/master`** (push confirmed 2026-06-26). **Issue #6 CLOSED** (manually,
2026-06-26 — the merge subject `(#6)` carried no closing keyword, same as #5). `PlatformBackendLoader` (discovery
`RIALTO_PLATFORM_BACKEND` → single `librialtoplatform-*.so` in `RIALTO_PLATFORM_DIR`/compile-time default →
in-core reference fallback; real `dlopen`+`dlsym` of the 3 entrypoints; version-check vs
`kPlatformBackendAbiVersion`; `shared_ptr` deleter = destroy+`dlclose`). `GstGenericPlayer::create` **and**
`GstWebAudioPlayer::create` now acquire via the loader; `LinuxPlatformBackend` is auto-sinks only (SoC ladder
removed). New `PlatformBackendLoaderTest` drives **real dlopen** against CMake-built fixture `.so`s
(match / ABI-mismatch / missing-symbol). Suites green: **servergstplayer 614/614, servermain 471/471**.
OpenSpec change `dlopen-per-soc-platform-backend` archived.

**Two new OpenSpec changes written + validated (`openspec/changes/`, not yet applied):**
- **`rialto-conformance-suite`** — a **compliance** suite (new public repo `Ulrond/rialto-conformance`) that
  proves every Rialto **external** interface/feature is met. CONTENT-DRIVEN (real streams via MSE + native),
  control plane OPTIONAL. ut-core (`VARIANT=CPP`, L1-L4) + python_raft, x86-first (unblocked by #6). Key
  decision (user-driven): **mocked gtests are NOT ingested** — they're the white-box dev inner loop; compliance
  = real external tests only. Capabilities: `conformance-harness`, `interface-conformance-levels`,
  `content-driven-conformance`, `versioned-abi-certification`.
- **`complete-soc-platform-isolation`** — finish what #6 started: move the *remaining* genuine SoC code behind
  the seam (additive `IPlatformBackend` v2→v3) + author per-SoC shim `.so`s, **stability-gated** (no
  amlogic/realtek/broadcom regression). Capabilities: `soc-concern-coverage`, `per-soc-backend-packages`,
  `platform-stability-guarantee`. **NOT yet implemented** (next = concern #1 `isVideoMaster`).

**Architecture model nailed down (terminology matters):**
- **Two EXTERNAL surfaces** (northbound, app's choice, both supported): MSE `rialtomse*sink` (app keeps its own
  GStreamer pipeline) **and** native `canCreateSession`/`IMediaSession` (app passes data + calls native). The
  only "surfaces" external users see.
- **Two INTERNAL plug points** (southbound SoC lower layer, *not* "surfaces"): `IPlatformBackend` `.so`
  (element create/config/caps/quirks) and `rdk-gstreamer-utils` (live-graph ops via `RdkGstreamerUtilsWrapper`).
  Goal: collapse to one seam; AIDL HAL subsumes both eventually.
- **Vendor SoC media code does NOT change.** Sinks + `rdk-gstreamer-utils-<platform>` (already versioned HAL)
  are untouched; the work is upper-layer + a thin per-SoC shim. Shim placement (Decision 7): per-platform repos
  mirroring `rdk-gstreamer-utils-<platform>`, co-located per SoC — never the core, never the vendor sink repo;
  for internal platforms we branch the per-platform HAL repo to **add** the shim, not fix the sink.

**SoC inventory classified (smaller than a grep suggests).** Genuine migrations: `isVideoMaster`
(`GstCapabilities`) + playback-rate (`SetPlaybackRate`). **Stay in core** (false positives — generic code with
vendor-named comments): `pushAdditionalSegmentIfRequired` (unconditional) + `enable-rate-correction`
(property-probed). Sink creation already done (#6).

**Cloned references** (in `external/`, gitignored): `rdk-gstreamer-utils-raspberrypi` (public plug-point-2
reference, `rdk_gstreamer_utils_soc.cpp` + `halif-versions.h`) and `westeros` (`westerossink` at
`v4l2/westeros-sink`). Vendor sinks `amlhalasink`/`rtkaudiosink`/`brcmaudiosink` are **proprietary, not public**
— and not needed (we reuse name+property contracts).

**Docs added** (`external/rialto/docs/`, untracked working docs): `ARCHITECTURE-rialto-overview.md` (5 sections)
+ `diagram-05..10` (`.svg`/`.png`): high-level arch, dlopen layering, dlopen load-decision, requirements
playbin-vs-explicit, SoC plug points, SoC migration map. `HAL-INTEGRATION-TEST-PLAN.md` renamed →
`RIALTO-INTEGRATION-TEST-PLAN.md`.

**SoC isolation (concluding the transformation) — IN PROGRESS on `feature/1-soc-platform-isolation`**
(branched off `master`, under Milestone #1; docs committed here `059ae98c`):
- **Inventory + classify DONE** (task 1). Genuine seam migrations: **#1 video-master** (`GstCapabilities`)
  + **#2 playback-rate** (`SetPlaybackRate`). Stay-in-core (relabel only): position quirk
  (`pushAdditionalSegmentIfRequired`) + decoder rate-correction (`enable-rate-correction`, property-probed).
  rdk-gstreamer-utils convergence (task 4): codec-switch `amlhalasink` branch (`GstGenericPlayer.cpp:1914`).
- **Concern #1 video-master — LANDED (`7bb20bb8`).** `IPlatformBackend` grown to **ABI v3** (additive
  `isVideoMaster()`, `kPlatformBackendAbiVersion`→3); `LinuxPlatformBackend::isVideoMaster()`=true; mock +
  loader fixture gain the method; `GstCapabilities` acquires a backend via `PlatformBackendLoader` (ctor param
  = test seam) and delegates `isVideoMaster()`, deleting the inline `amlhalasink` registry probe. Tests inject
  `PlatformBackendMock`. servergstplayer 614/614, servermain 471/471.
- **Concern #2 playback-rate — LANDED (`577088ef`).** v3 gains `applyPlaybackRate(pipeline,rate)`; Linux ref
  sends the custom-instant-rate-change event downstream (sink-pad new-segment variant → per-SoC `.so`).
  `GstGenericPlayer` exposes `applyPlaybackRate(rate)` on `IGstGenericPlayerPrivate` and delegates to the
  backend; `SetPlaybackRate` takes the player (not gst/glib wrappers), keeps its pipeline-state gating, deletes
  the inline `gStrHasPrefix(...,"amlhalasink")` dispatch + audio-sink lookup. 6 dispatch tests → 2 (success/
  failure); +Linux `applyPlaybackRate`/`isVideoMaster` cases. servergstplayer 613/613, servermain 471/471.
- **Relabels + grep-clean — LANDED (`1bdea813`).** Stay-in-core generic logic stripped of SoC names: position
  quirk (`amlogic devices`→`some platforms`), rate-correction log (`broadcom decoder`→`the decoder`),
  web-audio comment (dropped the amlhalasink/rtkaudiosink/autoaudiosink example list). **Engine grep-clean**
  now: the ONLY SoC name left in engine code is the live-graph audio **codec switch** (`switchAudioCodec`,
  `GstGenericPlayer.cpp:1918`), intentionally retained — it's the **rdk-gstreamer-utils convergence (task 4)**,
  owned by `RdkGstreamerUtilsWrapper::performAudioTrackCodecChannelSwitch` (not reimplementable), coordinates
  with Beej; marked with an orienting note. Platform-seam interface docs name vendors as legitimate contract
  examples.

**Engine-side transformation is COMPLETE** (the two genuine seam migrations + relabels; core names no SoC bar
the documented codec-switch). **Remaining = the "testing it all" phase** (out of this core branch):
- **Task 4** — rdk-gstreamer-utils convergence (codec switch + audio fade/easing), with Beej's
  `RdkGstreamerUtilsWrapper` work ([[beej-rdkgstreamerutils-soc-alignment]]).
- **Task 5** — author per-SoC `.so`s (`librialtoplatform-amlogic/-realtek/-broadcom`) in per-platform repos
  (Decision 7: never the core), carrying the migrated vendor logic (amlhalasink video-master=false + sink-pad
  new-segment rate, etc.).
- **Task 6** — certify each `.so` against the `rialto-conformance` v3 suite; retire inline paths per the
  stability gate. Then the external-interface suites (`rialto-conformance-suite`).

**Open / next:** merge `feature/1-soc-platform-isolation` → `master` (local; hold push per the fork-push gate),
then start the testing phase.

---

## Two projects (keep them straight)

- **DESIGN / SPEC project** (canonical, corp-synced to Confluence):
  `comcast-sky/dual-decode-programme` = `Presentations/Netflix-NRDP9`. All LLDs / specs live here.
  **Lives on hpz4 only** (`/home/gweatherup/git/fast/sky/Presentations/Netflix-NRDP9`) — **NOT copied
  to ceres** as of the 2026-06-22 relocation; rsync it over if LLD/spec work is needed on ceres.
- **CODE / TEST project** (testbench, this repo): **on ceres** at
  `/home/gew04/git/fast/sky/pipewire_example` (relocated from hpz4 `/home/gweatherup/...` on 2026-06-22;
  full parity restored — main repo + every `external/` clone HEAD-matched to hpz4 and in sync).
  Branch: `feature/13-binder-hal-bridge`.
  The Rialto fork is cloned at `external/rialto` (gitignored plain clone). Remotes: `origin` =
  Ulrond/rialto (push+fetch), `upstream` = rdkcentral/rialto (**fetch only — push URL disabled**, see
  [[rialto-fork-commit-identity]]). Commit identity pinned to Ulrond (local git config); never
  gerald.weatherup@sky.uk.
  Branch state on the fork: PR Ulrond/rialto#3 (stage 3 + M1 + stage-1, 14 commits, all Ulrond) was
  **merged** (rebase). **Stage 4 + stage 5 + #8 are now MERGED to local `master`** via a `--no-ff` merge
  commit (`6ab0d3dd`, authored Ulrond) — `master` is **ahead of `origin/master` by 11** (10 staged
  commits + the merge commit) and **NOT pushed** (push waits for explicit go). The stack was fully linear,
  all Ulrond, no conflicts. **Working branch stays `feature/8-explicit-av-appsrc-callbacks`** (tip
  `bfe9d7ec`, now an ancestor of master), with `feature/5-flip-default-delete-playbin` (`ac00245f`, #5c)
  and the stage-4 branch likewise merged in. A pristine-upstream **git worktree** sits at
  `external/rialto-upstream`
  (detached at `upstream/master`, rdkcentral/rialto) — the regression oracle / true-default baseline
  for the `GST_DEBUG_DUMP_DOT_DIR` cross-check (confirmed playbin-only: no `initMsePipelineExplicit`).
  Working docs now live in `external/rialto/docs/` (this file, the plans).
  **Build host: ceres** (80 cores, `ssh ceres` on home LAN) — relocated from hpz4 (4 cores) for faster
  builds; run `claude` natively on ceres. Builds run in a docker via `sc` (see the Build/test section).
  Roadmap issues on the fork: #4 stage 4 (done), #5 stage 5 (done via 5c), #8 video/subtitle appsrc wiring
  (done, `bfe9d7ec`) — all three **merged to local master (`6ab0d3dd`), will auto-close on push**; still
  open: #6 dlopen per-SoC loader, #7 RAFT+ut_core (all under #1 Milestone 1).

Design originates in the spec project; code/evidence ("nails") live in `pipewire_example`
and flow **back** into the LLDs.

## The mission

**The transformation comes first.** The near-term work is the Rialto transformation: the
versioned-ABI SoC seam and the two coexisting northbound interfaces. PipeWire is **not
confirmed** — it is a *possible* future swap, not a committed phase. GStreamer stays the engine.

1. SoC specifics (BCM/RTK/Amlogic/MTK) sit behind a **versioned ABI** loaded as a per-SoC
   `.so` — **no `#ifdef BCM/RTK`**. Upgrading the SoC layer must **not** force re-test of all
   platforms (upper layers binary-compatible across a stable ABI).
2. Two northbound interfaces **coexist**: the GStreamer-sink idiom (`rialtomse*sink` — for
   100s of MSE web apps, stays 100% backward-compatible) **and** a new
   `canCreateSession()` / `IMediaSession` path (Netflix / YouTube / Cobalt).
3. GStreamer stays the engine. **PipeWire is not confirmed** — a swappable "core" remains a
   *candidate* future direction, contingent on the transformation landing first; do not treat
   it as a planned phase.
4. Migration is **per-platform, not a flip**. Old functions get Doxygen `@deprecated` / `@see`
   and are rewritten internally to delegate to the new path (facade), e.g.
   `createMediaPipeline` → `IMediaSession`. SoCs also move to AIDL at different paces
   (late-2026); once Rialto core↔AIDL lands, the per-SoC `.so` can drop out.

## What's DONE (committed, green)

- **IPlatformBackend versioned `extern "C"` ABI seam** proven end-to-end through the real
  Rialto build (662 tests green, no regression). Steps 1 + 2a committed.
  Files in `external/rialto`:
  - `media/server/platform/interface/IPlatformBackend.h` (versioned ABI, ABI v1)
  - `media/server/platform/{include,source}/LinuxPlatformBackend.{h,cpp}`
    (reference backend: `createAudioSink`→autoaudiosink, `createVideoSink`→autovideosink)
  - `media/server/gstplayer/{include,source}/GstGenericPlayer.{h,cpp}`
    (ctor takes `IPlatformBackend` as last param; factory builds backend via
    `PlatformHostContext{gstWrapper,glibWrapper}`)
  - `media/server/gstplayer/CMakeLists.txt` (added includes + `LinuxPlatformBackend.cpp`)
  - `tests/unittests/media/server/mocks/PlatformBackendMock.h`
- **Empirical evidence harness (6 "nails")**:
  `pipewire_example/experiments/playbin-vs-explicit/` (`playbin_vs_explicit.c`, `run.sh`,
  `RESULTS.md`) — 36 vs 4 elements, `nvh264dec` rank, 72-element dual-decode collision,
  decoder-absent-at-construction, 155 ms vs 0.06 ms resume.
- **LLD updates**: `LLD-rialto-transformation.md` §4.1 (versioned-ABI fold + resource-ID-keyed
  sink note + `@deprecated`/`@see` facade). Constraints LLD split into argument-only +
  explicit-construction-model doc; 6 measurement nails folded in.
- **M1 — web-audio backend routing (Step 2b), committed green** (`feature/1-explicit-construction`).
  `GstWebAudioPlayer` no longer names SoC sinks: it asks the injected `IPlatformBackend` for the
  audio sink and its factory builds the backend via the loader ABI (same pattern as
  `GstGenericPlayer`). The amlhalasink/rtkaudiosink registry-probe ladder + vendor config moved
  **down** into `LinuxPlatformBackend::createAudioSink`, tagged transitional `→ per-SoC .so` (no
  vendor-hardware regression in the interim). Tests inject `PlatformBackendMock`; 3 platform
  variants collapsed to 1; SoC sink-failure cases relocated to **7 new `LinuxPlatformBackendTest`**
  cases. servergstplayer 664/664, servermain 471/471. (commit `gstplayer: route web-audio …`)
- **M1 commit:** `f5fbf45c`.
- **Playbin-removal: plan + first commits** (`feature/1-explicit-construction`). `PLAYBIN-REMOVAL-PLAN.md`
  written (5 staged commits, opt-in switch, backend sinks incl. ID-keyed video). Landed so far:
  **(1)** ID-keyed `createVideoSink` (ABI **v2**) — backend takes a video/plane ID via
  `setWesterosSinkVideoID`, superseding the primary/secondary boolean; Linux ref is plane-agnostic.
  Commit `056fdb3e`.
  **(2)** playbin/explicit **parity fixture** — `TEST_P` over `{Playbin, Explicit}`
  (`genericPlayer/ParityTest.cpp`) asserting observable behaviour independent of construction path.
  Commit `3771fde3`.
  **(3)** **explicit MSE construction behind opt-in switch** — `initMsePipeline` dispatches on
  `RIALTO_EXPLICIT_PIPELINE` (default playbin); legacy body → `initMsePipelinePlaybin`,
  `initMsePipelineExplicit` builds a plain `GstPipeline` (no autoplug/signals/flags/playsink/uri).
  Parity fixture now runs **both** params live. servergstplayer 666/666, servermain 471/471.
  Commit `bad30d45`.
  **Stage 3 (audio), decision locked: decodebin per stream.** Build `appsrc → decodebin →
  audioconvert → audioresample → audioSink(backend)` in the explicit attach path. Sub-commits in
  `PLAYBIN-REMOVAL-PLAN.md` §5:
  - **3a — LANDED (`3f4123e0`).** `buildAudioChain()` on the player, exposed via
    `IGstGenericPlayerPrivate` (backend stays encapsulated in `GstGenericPlayer`); builds
    `appsrc → decodebin → audioconvert → audioresample → audioSink(backend)`, links the static tail,
    registers decodebin `pad-added` → audioconvert. Invoked from `AttachSource::addSource` on the
    explicit branch only, gated by new `GenericPlayerContext::isExplicitConstruction` (set in
    `initMsePipelineExplicit`). New tests: `shouldBuildExplicitAudioChain` (player-level wrapper
    sequence, `PlatformBackendMock` injected into the private-test SUT) +
    `shouldBuildExplicitAudioChainWhenExplicitConstruction` (AttachSource wiring). servergstplayer
    668/668, servermain 471/471.
  - **3b — LANDED (`6558b176`).** `FinishSetupSource` branches on `isExplicitConstruction`: on the
    explicit path it configures the audio chain's appsrc and wires its need/enough/seek callbacks
    directly (`configureExplicitAppSrc`), bypassing `GstSrc::setupAndAddAppSrc`/`allAppSrcsAdded` and
    the rialtosrc bin. The task now takes the gst/glib wrappers (injected by the factory, like
    `AttachSource`); userData stays the player so the `schedule*` callbacks are unchanged. New tests:
    `shouldFinishSetupSourceExplicit` + explicit need/seek-data cases. servergstplayer 671/671,
    servermain 471/471.
  - **3c — LANDED (`545e4f32`).** Relocated the `isAudioSink` branch of `SetupElement` (low-latency,
    sync) into the explicit builder: `buildAudioChain` stores the backend audio sink in
    `m_context.audioSink` (an extra ref, released by `termPipeline`) and applies the pending
    audio-sink properties inline. `getSink` gained an explicit-construction branch returning the
    stored audio sink directly (no playbin to read it off), so the audio-sink setters reach it. New
    test `shouldGetExplicitAudioSink`; `shouldBuildExplicitAudioChain` asserts the store. servergstplayer
    672/672, servermain 471/471.
  - **3c-decoder — LANDED (`b5d8a221`).** The audio-decoder properties (`sync-off`/`stream-sync-mode`/
    `buffering-limit`/`enable-rate-correction`) now reach the decoder on the explicit path. The decoder
    is autoplugged async inside `decodebin`, so a new `SetupAudioDecoder` task applies the four via the
    existing setters; `getDecoder(AUDIO)` reaches it through `gstBinIterateRecurse`. The decodebin
    pad-added handler enqueues the task on the worker thread (decoder analogue of the 3c sink store).
    `setEnableRateCorrection()` added as the getDecoder-based, live-only player method (the only one of
    the four not already a setter), mirroring the inline `SetupElement` block; playbin path untouched.
    New tests: 4 `setEnableRateCorrection` cases + 2 `SetupAudioDecoder` task cases + 1 factory case.
    servergstplayer 679/679, servermain 471/471.
  - **3d — LANDED (`247f6d7f`). Stage 3 COMPLETE.** Execution-level parity cases: attach-audio, play,
    pause and EOS now run the *real* task through the *real* player against the mocked GStreamer
    wrappers, so the same spec exercises both construction paths end-to-end (the attach case is the
    first test to drive attach → real `buildAudioChain`, sourcing the explicit sink from the
    `PlatformBackend`). Construction/teardown keep the mocked task factory (shared helpers unchanged);
    each case makes the factory return a real task for just the call under test, which the synchronous
    worker mock executes on enqueue. Per-mode arrange encodes the legitimate divergence (playbin
    autoplugs lazily vs explicit builds the chain now); the asserted end-state is the same.
    servergstplayer 687/687, servermain 471/471. Vendor-sink props (`wait-video`/`disable-xrun`/`async`)
    still belong in the per-SoC backend, not the builder (stage 4+).
  - **NEXT — stage 4** (video+subtitle, wire `createVideoSink(id)` into the explicit video path) then
    stage 5 (flip default, delete playbin). **Merge checkpoint: stage 3 done → merge
    `feature/1-explicit-construction` → `master` now** (all behind the default-off
    `RIALTO_EXPLICIT_PIPELINE` switch, so behaviorally inert).
- **Data-path direction of travel** (design note, not code): `external/rialto-data-path-direction.md`.
  Reviewed Beej's "Rialto CPU concerns" (`~bsh35`) — **correct** (4 CPU copies, verified against
  `GstGenericPlayer.cpp:1285` `gstBufferFill`). Direction = **(A) zero-copy front end** (custom GST
  allocator over SHM + server-side buffer wrapping → 3 of 4 copies gone) **+ (B) fused secure decode**
  (our HAL: decoder `open(secure)` + decoder-bound `IAVBuffer` secureHeap pool → the OCDM↔decoder
  hand-off disappears; encrypted path → **1 copy + fused decode**). (A) saves CPU memcpys; (B) saves
  secure-domain hand-offs. Composes with explicit construction. → fold into dual-decode/HAL LLD.
- **Beej's `RdkGstreamerUtilsWrapper` SoC-audio work — reviewed 2026-06-24, aligned in approach.** Beej is
  independently moving SoC-specific audio knowledge out of `GstGenericPlayer` into rialto's
  `firebolt::rialto::wrappers::RdkGstreamerUtilsWrapper` (new methods `configureAudioSink`,
  `configureAudioDecoder`, `applyPlaybackRateChange`, `getWebAudioSinkKind`, `shouldIgnoreCapabilityFactory`,
  and an amlogic codec-switch branch). Confirmed with user: **NOT a competing-home conflict** with our
  `IPlatformBackend` seam — the hard-coded vendor-name string compares are the same transitional scaffolding
  we have in `LinuxPlatformBackend::createAudioSink`; the per-SoC `.so` split (#6) is a known requirement
  Beej just hasn't added yet (he's "behind the curve"). Both efforts converge on the SoC `.so`. Real review
  gaps (his snapshot was interface+impl only, files were on `gmac:~/Downloads/beej`): (1) won't compile —
  concrete `wrappers/include/RdkGstreamerUtilsWrapper.h` + the test mock still need the 5 new decls;
  (2) missing `<gst/base/gstbasesink.h>` for `GST_BASE_SINK_PAD`; (3) latent pre-existing bug — amlogic
  codec-switch path `(void)`-discards caller `ret`/`status` outputs. (Memory: [[beej-rdkgstreamerutils-soc-alignment]].)

## What's PENDING (next, in order)

- **Playbin removal — COMPLETE (3 → 5c) + follow-up bug #8 fixed.** Default is explicit; the playbin
  construction path, the `RIALTO_EXPLICIT_PIPELINE` switch, and the `isExplicitConstruction` flag are all
  gone. `#5c` = **`ac00245f`** on `feature/5-flip-default-delete-playbin`; `#8` = **`bfe9d7ec`** on
  `feature/8-explicit-av-appsrc-callbacks` (stacked on feature/5). The stack is now **MERGED to local
  master (`6ab0d3dd`)**; two things still open — the **push to origin** and a small **residual dead-code
  cleanup** (now scoped as an OpenSpec proposal) — detailed in the residual / Merge-checkpoint sub-bullets
  below. **Stage 3 MERGED** (PR #3, rebase): 3a/3b/3c/3c-decoder/3d.
  **Stage 4 COMPLETE** (4a/4b/4c/4d). Landed-stage history (3 → 5b) follows for recovery — Stage 4
  sub-commits (mirror stage 3; see `PLAYBIN-REMOVAL-PLAN.md` §5 stage 4):
  - **4a — LANDED.** Explicit video chain builder `buildVideoChain` (appsrc → decodebin → backend
    videoSink), wiring `IPlatformBackend::createVideoSink(name, videoId)` (videoId 0=primary/1=secondary,
    derived in the ctor and stored in `m_context.videoId`; the explicit path skips the playbin
    westerossink-secondary setup). `getSink(VIDEO)` explicit branch returns the stored sink; invoked
    from `AttachSource`'s explicit VIDEO branch. New tests: shouldBuildExplicitVideoChain,
    shouldGetExplicitVideoSink, shouldBuildExplicitVideoChainWhenExplicitConstruction.
  - **4b — LANDED.** Explicit subtitle path. `AttachSource` gained a `buildSubtitleChain` helper that
    builds `appsrc → RialtoTextTrackSink` directly (the text-track-sink factory is injected into
    `AttachSource`, not the backend, so — unlike the audio/video chains delegated to the player — the
    chain is built in the task): create the sink via the factory, `gstBinAdd` the appsrc + sink, link
    them, store the sink in `m_context.subtitleSink` with an extra ref released by `termPipeline`. The
    playbin `text-sink` property assignment in `addSource` is now guarded `!isExplicitConstruction` (the
    explicit `GstPipeline` has no `text-sink`); the existing subtitle-sink setters/getters reach the
    stored sink unchanged. New test: `shouldBuildExplicitSubtitleChainWhenExplicitConstruction`
    (+ `shouldBuildExplicitSubtitleChainOnAttach` helper). servergstplayer 691/691, servermain 471/471.
  - **4c — LANDED (`ed0e416c`).** Relocated the reactive video-sink/parser property-setting into the
    explicit path (analogue of 3c sink + 3c-decoder). The video sink is created synchronously, so
    `buildVideoChain` applies the four pending video-sink properties inline after storing
    `m_context.videoSink` (geometry → `setVideoSinkRectangle`, immediate-output → `setImmediateOutput`,
    render-frame → `setRenderFrame`, show-window → `setShowVideoWindow`), each guarded by its pending
    state exactly as the `isVideoSink` branch of `SetupElement`. The video parser is autoplugged async
    inside decodebin, so a new `SetupVideoParser` task applies `stream-sync-mode` via the existing
    `setStreamSyncMode(VIDEO)` (reaches the parser through `getParser(VIDEO)`/`gstBinIterateRecurse`);
    `videoDecodebinPadAdded` enqueues it on the worker thread (`scheduleSetupVideoParser`). No new
    player/mock method needed (the setter already exists). Playbin path untouched. New tests:
    `SetupVideoParserTest` (apply + no-pipeline) + `ShouldCreateSetupVideoParser` factory case.
    servergstplayer 694/694, servermain 471/471.
  - **4d — LANDED (`5ed7425a`). Stage 4 COMPLETE.** Execution-level video + subtitle parity cases,
    completing the per-mode coverage begun for audio in 3d. `AttachVideoSourceBuildsExpectedGraph`
    (first to drive attach → real `buildVideoChain`, sourcing the explicit video sink from the
    `PlatformBackend` keyed by video id 0), `SetEosSignalsVideoAppSrc`, and
    `AttachSubtitleSourceBuildsExpectedGraph` (attach → real `buildSubtitleChain`) run the real task
    through the real player against the mocked wrappers. Per-mode arrange encodes the legitimate
    divergence (playbin autoplugs lazily + assigns playbin's `text-sink`; explicit builds the decodebin
    chain / `appsrc → RialtoTextTrackSink` now); asserted end-state is the same. The parity helpers are
    kept byte-consistent with the committed 3d audio siblings. servergstplayer 700/700, servermain
    471/471.
  - **Stage 5 — IN PROGRESS on `feature/5-flip-default-delete-playbin` (off the stage-4 tip).** Scoping
    found the flip is NOT pure deletion: two telemetry/feature behaviours rode on playbin-only signals
    and must reach parity on the explicit path BEFORE flipping the default, or the production path
    regresses (PLAYBIN-REMOVAL-PLAN.md §7).
    - **5a — LANDED (`7d86d4d6`). Underflow + first-video-frame telemetry on the explicit path.** The
      underflow/first-frame callbacks AAMP relies on were connected only by `SetupElement` (playbin
      `element-setup` signal), so explicit had none. New `connectStreamSignals(element, type)` scans an
      element for the underflow (audio/video) + first-frame (video) signal and connects the matching
      callback, skipping the isDecoder/isSink/isAudio/isVideo probing (role + type already known on the
      explicit path). Sinks wired synchronously in `buildAudioChain`/`buildVideoChain`; autoplugged
      decoders wired via new `IGstGenericPlayerPrivate::connectDecoderSignals(type)` called from
      `SetupAudioDecoder`/`SetupVideoParser`. servergstplayer 705/705, servermain 471/471.
    - **5a-switch — LANDED (`df30cb97`).** Seamless **mid-stream audio codec switching** on the explicit
      path. `reattachSource` → `performAudioTrackCodecChannelSwitch(&m_context.playbackGroup,…)` /
      `switchAudioCodec` reads the audio decoder/parser/typefind/decodebin/pipeline off
      `GenericPlayerContext::playbackGroup`, which on the playbin path is populated reactively by
      `DeepElementAdded`/`UpdatePlaybackGroup` off playbin signals — empty on the explicit path, so the
      switch would fail/degrade once the default flips.
      **Decision (settled, not a real fork):** the external rdk-gstreamer-utils
      `performAudioTrackCodecChannelSwitch` takes the whole `playbackGroup` and cannot be reimplemented, so
      the group is **populated on the explicit path** (re-implementing the swap is not viable for the vendor
      SoCs). Consumers (`reattachSource`, `getSink`) then need no rewrite; both the amlhalasink (halt/switch/
      resume) and rdk-utils paths operate on current elements.
      **Implementation (two writes):**
      - `buildAudioChain` stores the **stable handles** owned at construction: `m_gstPipeline` = pipeline,
        `m_curAudioDecodeBin` = the `auddecodebin`, and `m_curAudioPlaysinkBin` = the backend audio sink
        (the explicit topology has no playsink wrapper; the sink is the audio output branch that
        haltAudioPlayback/resumeAudioPlayback gate — extra ref released by the existing termPipeline unref).
      - `reattachSource` (explicit branch, before the swap) refreshes the **per-switch handles** from the
        live graph via `getDecoder(AUDIO)`/`getParser(AUDIO)`/new decodebin-scoped `getAudioTypefind()` —
        refreshing per switch is robust to `switchAudioCodec` nulling and recreating decoder/parser across
        successive switches. New private methods: `getAudioTypefind()`, `updateAudioPlaybackGroupHandles()`.
      Tests: `shouldRefreshPlaybackGroupHandlesOnExplicitReattach`, `shouldBuildExplicitAudioChain` extended
      to assert the stable stores, parity audio arrange updated for the 2nd sink ref. servergstplayer
      706/706, servermain 471/471. **Stage 5 is now unblocked → 5b (flip default) is the next item.**
    - **5b — LANDED (`159b9653`).** Flipped the production default to explicit construction in
      `initMsePipeline`. **Refinement vs the original plan:** the `RIALTO_EXPLICIT_PIPELINE` switch is
      *retained as an opt-OUT* (`=0` → legacy playbin) rather than removed here — removing it outright
      would break the entire playbin test corpus (the bulk of the generic-player unit tests construct
      via `gstPlayerWillBeCreated`/`expectMakePlaybin` and rely on the playbin default; only ParityTest
      sets the env). Keeping the opt-out gives the §7 per-platform fallback and keeps the playbin path +
      tests exercised until they are deleted together in 5c/5d (switch removal moves to 5c with playbin
      deletion). Tests select their path deterministically by setenv on the construction expectation
      (`expectMakePlaybin` → `=0`, `expectMakePipeline` → `=1`). servergstplayer 706/706, servermain 471/471.
    - **5c — LANDED (`ac00245f`). Playbin construction + flag removed; tests collapsed.** Deleted
      `initMsePipelinePlaybin`, `setPlaybinFlags`/`getGstPlayFlag`/`shouldEnableNativeAudio`,
      `setWesterossinkSecondaryVideo`/`setErmContext`, the playbin `source-setup`/`element-setup`/
      `deep-element-added` callbacks, the `RIALTO_EXPLICIT_PIPELINE` switch, and the
      `isExplicitConstruction` flag. Collapsed all per-source playbin data branches (AttachSource subtitle
      `text-sink`, FinishSetupSource rialtosrc) and the parity suite `{Playbin,Explicit}` → explicit-only;
      the 6 audio-reattach tests now expect the unconditional playback-group refresh. servergstplayer
      674/674, servermain 471/471. (Folded the plan's 5d test-collapse into this commit.)
    - **#8 — LANDED (`bfe9d7ec`).** `FinishSetupSource` now iterates every `streamInfo` entry instead of
      audio-only, so video/subtitle appsrcs get their need-data/seek callbacks (they stalled on the
      explicit/default path — pre-existing gap from stage 4). New video/subtitle `FinishSetupSourceTest`
      cases. servergstplayer 677/677, servermain 471/471. `Fixes #8` (auto-closes on merge).
    - **RESIDUAL CLEANUP — DONE (`b6d4a0c4`), merged + pushed.** Implemented via the OpenSpec change
      `playbin-removal-residual-cleanup` (the first OpenSpec trial). Removed the auto-sink *unwrap* in
      `getSink` + the `getSinkChildIfAuto*` helpers, the four `add/removeAuto*SinkChild` methods (impl +
      `IGstGenericPlayerPrivate` + mock), the `autoVideoChildSink`/`autoAudioChildSink` members, and the
      three dead reactive tasks `SetupElement`/`SetupSource`/`DeepElementAdded` (sources, factory methods,
      tests, ~80 dead `GenericTasksTestsBase` helpers, CMake). **Partial by design:** the bare
      `gObjectGet("audio-sink"/"video-sink"/"text-sink")` property read in `getSink` is **retained** — the
      server component suite still drives it and can't migrate until the harness has a platform backend (#6);
      full deletion is tracked in **#9**. Implementation note: the tests-first sink migration turned out a
      **no-op** for `SetPlaybackRate` (it reads `audio-sink` directly, not via `getSink`); the real test work
      was dropping the orphaned `gTypeName` unwrap expectation from `expectGetAVSink` + the reattach cases and
      deleting the auto-sink-specific tests. Unit suites green: servergstplayer **611/611**, servermain
      **471/471**.
    - **MERGE CHECKPOINT — DONE + PUSHED.** `master` (cleanup `b6d4a0c4` + `--no-ff` merge `f9d14b04`, on top
      of `6ab0d3dd`) **pushed to `origin` (Ulrond) on 2026-06-25**. Auto-closed **#4** and **#8**; **#5**
      closed manually (its `#5c` commit carried no closing keyword). Commit identity pinned to Ulrond.
- **dlopen loader (#6) — SPECCED via OpenSpec, ready to implement.** Change `dlopen-per-soc-platform-backend`
  (4/4 artifacts, validates) lifts the transitional amlhalasink/rtkaudiosink ladder out of
  `LinuxPlatformBackend::createAudioSink` into a per-SoC `.so`; the reference backend then keeps only
  autoaudiosink/autovideosink. Design: loader discovery (`RIALTO_PLATFORM_BACKEND` override → `RIALTO_PLATFORM_DIR`
  → built-in fallback), ABI version-check against `kPlatformBackendAbiVersion` (refuse+fallback on mismatch),
  `shared_ptr` deleter that `rialtoDestroyPlatformBackend`+`dlclose`, and an **injectable loader seam**.
  Today the engine calls `rialtoCreatePlatformBackend` directly (static link, `GstGenericPlayer.cpp:133`);
  the loader replaces that. The injectable backend is the hook **#9** consumes to give the component harness a
  sink-storing backend. Lives at `pipewire_example/openspec/changes/dlopen-per-soc-platform-backend/`.
  **Next: `/opsx:apply dlopen-per-soc-platform-backend`.**
- **python_raft + ut_core** (`github.com/rdkcentral/`) — the test-harness move, for **integrated-HAL
  testing via the control plane** (not just wrapping unit tests). Goal: stand up the real binder/AIDL
  HAL services (`pipewire_example/hal`, brought up by `hal/scripts/services_up.sh`) and have raft
  drive the **control plane already defined in the AIDL** — the `I<X>Controller` / `I<X>Manager` /
  `I<X>EventListener` / `I<X>ControllerListener` interfaces (e.g. `IAudioSinkController`) — to
  **trigger HAL stimulus** and observe responses + metrics deterministically. ut_core wraps the test
  structure/reporting and the existing gtest suites (rialto 339 unit incl. 76 gstplayer + component;
  rialto-gstreamer 21; rialto-ocdm 10) for per-platform conformance. The mocked gtest suites (incl.
  the parity fixture) stay the fast inner loop; raft+ut_core are the outer HW-in-the-loop loop.
  **Plan written: `RIALTO-INTEGRATION-TEST-PLAN.md`** (next to this file). Organising principle =
  **versioned conformance against the `IPlatformBackend`/AIDL ABI**, with two versioning axes:
  **pinned ut-core** (ingested at a fixed version → consistent harness/reporting/config) running an
  **ABI-versioned conformance suite** (content tied to `kPlatformBackendAbiVersion`). Key decisions:
  the existing ~1100 gtest tests join ut-core with **no conversion** — a `UtCoreGTestBase` + a
  gtest/gmock→ut-core `TestEventListener`, re-parent ~4 shared fixtures (that listener = the scoped
  **gmock-in-ut-core ticket**); certification = backend declares ABI vN via
  `rialtoPlatformBackendAbiVersion()` → passing the vN suite certifies it, **additive** bumps, old
  backends not re-certified (makes "upgrade SoC layer without re-test" enforceable). Coverage matrix
  (31 props / ~10 codecs / 39 ops × path) is the versioned requirement record.
- **getSink-fallback finish + component-suite migration (#9)** — the deferred tail of the residual cleanup.
  Delete the `gObjectGet("audio-sink"/...)` property read from `getSink` (so the stored sink is the *exclusive*
  source) **and** migrate the server component harness off the playbin/source-setup model so it builds the
  explicit chain. Blocked on **#6** (the injectable backend the harness needs). Restores the full
  `explicit-pipeline-sink-access` spec requirement (currently relaxed to an interim contract).

## Conformance suite — hard requirements (#7 / #9)

The conformance suite (ut-core + python-raft) is a **version-independent, platform-agnostic oracle**, not a
fixture welded to one build:

- **Independent of Rialto / version-independent.** A separate suite you point at *any* build — old Rialto, an
  older checkout, or latest — and the *same* tests give the *same* results. Any diff is a real bug/regression
  or intended new behaviour. It is the migration's parity / non-regression oracle.
- **Platform-agnostic.** python-raft selects the target, so the suite runs across SoC backends (reference
  autosink + per-SoC vendor `.so` over the versioned `IPlatformBackend` ABI) by **building/selecting**, never
  by editing tests. ut-core = case/assertion structure + reporting; raft = multi-platform / HW-in-the-loop.
- **Covers both contracts.** The **MSE interface** (properties & caps) **and** **Rialto native** — *all*
  functions, existing **and** new.

**Design rule that makes this possible: bind to the stable external contract, never to internal wiring.** The
enablers already exist — the injectable `IPlatformBackend` seam (#6) swaps implementation without touching
tests; the versioned ABI lets old/new run side-by-side.

**Concrete counter-example / motivation:** the current server component suite (`tests/componenttests/server`,
`build_ct.py -s server`) is **red — 12 tests** (`MediaPipelineTest.*`, `AudioSourceSwitchTest`,
`DualVideoPlaybackTest`, `EncryptedPlaybackTest`, `FlushTest`, `FirstFrameNotificationTest`). Verified
**pre-existing from 5b/5c**, not the residual cleanup: identical 12 failures at `bfe9d7ec` (pre-cleanup) and at
current master — zero regressions. Root cause = the harness (`MediaPipelineTest`) is hard-wired to the old
playbin/source-setup GStreamer mocks and was never migrated to the explicit path; post-5c the pipeline is never
built (no backend → `buildAudioChain` bails → "Could not find any Sink" → teardown SIGSEGV). #9 re-cuts it
against the injectable backend (#6) — the same move that gives the suite its old/new portability.

## Local working docs (in `external/`, gitignored — NOT in any repo)

- `external/rialto/docs/HANDOFF.md` — this file. (Working docs were moved under `docs/`.)
- `external/rialto/docs/PLAYBIN-REMOVAL-PLAN.md` — the 5-stage plan; stage-3 sub-commits + decodebin decision.
- `external/rialto/docs/RIALTO-INTEGRATION-TEST-PLAN.md` — versioned-conformance test plan (ut-core + raft;
  pinned-harness + ABI-versioned-contract axes; no-conversion gtest ingest; coverage matrix).
- `pipewire_example/experiments/playbin-vs-explicit/RESULTS.md` — the evidence report, committed on
  `feature/13-binder-hal-bridge` (`1549409`, `40148ac`). Now an A-vs-B report (current playbin vs new
  decodebin path; hand-built floor demoted to a reference), with measured lifecycle / scaling / memory.
  Key conclusion: element count is a graph-complexity metric, **not** a resource metric — construction
  time, startup and memory are all decoder-bound and similar for both paths; the explicit path's real
  wins are determinism, reconfigure speed, and held-configured resume (avoiding the rebuild). The
  harness gained `run_lifecycle` / `run_memory` and per-path scaling timing; `external/rialto-upstream`
  is the cross-check baseline.
- `external/rialto-data-path-direction.md` — zero-copy + fused-secure-decode direction of travel.
- `external/links.md` — link index + the corp-sync access recipe.
- `external/rialto-cpu-concerns.md`, `external/beej-current-rialto-model.png`, `external/beej-optimised.png`
  — Beej's fetched page + diagrams.
- Confluence access (Sky `stb-confluence`): `corp-sync login --source stb-confluence --skip-browser`
  (re-extracts local browser cookies → authenticates), then
  `corp-sync fetch "<url>" --source stb-confluence --refresh -o … --proxy`.

## Conventions (from CLAUDE.md — must follow)

> These come from the **user-global** `~/.claude/CLAUDE.md` (on hpz4: `/home/gweatherup/.claude/CLAUDE.md`).
> **Synced to ceres on 2026-06-22** (`/home/gew04/.claude/CLAUDE.md`, md5 `b4bfe577…`, identical to hpz4) —
> a fresh ceres session auto-loads these conventions.

- New files in **rdkcentral** repos: Apache 2.0, copyright holder **"RDK Management"** (NOT Comcast).
  Existing files: keep their header as-is.
- No `Co-Authored-By` / no "Generated with Claude Code" anywhere.
- TodoWrite ON for any multi-step task. Issue first, then branch `feature/{issue}-{synopsis}`.
- `corp-sync` is a pipx CLI on PATH — never vendor it; never corp-sync without explicit go-ahead.
- TL;DR / exec summary at top of every doc. Docs: destination, not journey.

## Build / test commands (in `external/rialto`)

```
build_ut.py -s <suite> -cov                          # unit tests (green)
./build_ut_docker.sh -s servergstplayer servermain   # ceres docker wrapper for the unit suites
build_ct.py -s server                                # component tests — KNOWN RED (12, pre-existing 5c; #9)
sc docker run -l -t local rialto-build -- env PROFILER_ENABLED=true python3 build_ct.py -s server  # ceres
```

**On ceres (the build host) — builds run in a docker via `sc`** (ceres host lacks the rialto deps and you
are not an admin/no sudo; the deps live in a local docker image, and docker — and `sc` — run without
sudo). Use the wrapper:

```
./build_ut_docker.sh -s servergstplayer servermain   # native unit tests, ~2.5 min on 80 cores, host-owned
```

`build_ut_docker.sh` runs `build_ut.py` via **`sc docker run -l -t local rialto-build -- env
PROFILER_ENABLED=true python3 build_ut.py "$@"`**. `sc` launches the container **as the host user**
(gosu + `LOCAL_USER_ID`) and **binds the home dir transparently**, so artifacts are host-owned and paths
match the host — no manual `--user`/`-v`. Image **`rialto-build:local`** = `ubuntu:24.04` + native deps
(gstreamer1.0-dev/-plugins-base/-bad, protobuf, jsoncpp, yaml-cpp, libunwind — gst 1.24, matches hpz4)
**plus the sc entrypoint** (entrypoint.sh + bashext.sh + gosu + `/etc/entrypoint.d/*.env`, copied from
`ghcr.io/comcast-sky/core-ubuntu20.04`) so it is sc-launchable. Source + Dockerfile:
`~/rialto-build-docker/` (rebuild: `docker build -t rialto-build:local ~/rialto-build-docker/`). Validated
green via sc: servergstplayer 690/690, servermain 471/471. This is the **native mocked-unit-test inner
loop ONLY — NOT the production build** (production = official rdk-kirkstone/Yocto cross-build, which
builds its own gstreamer; that is why the apt gstreamer version here is immaterial — the tests mock
gstreamer). Caveat: `sc` space-joins the post-`--` command, so avoid shell-glob chars in `-gf` filters
(`FooTest.*` is fine; `*Foo*` may glob-expand). See [[ceres-sc-docker-build]].

## Tooling & environment (ceres)

- **gh CLI — two `github.com` accounts.** `Ulrond` = the **open** login (use for `rdkcentral` + the
  `Ulrond/rialto` fork); `gerald-weatherup_comcast` = the **ghec** (Enterprise Managed User) login (for
  `rdke` / `comcast-sky`). Both on the same host, only one active at a time. **Before any `gh` op on the
  fork/rdkcentral, run `gh auth switch --hostname github.com --user Ulrond`** — the active account
  silently defaults to whichever was set last. Re-create the Ulrond token from hpz4's keyring:
  `ssh hpz4 'gh auth token' | gh auth login --hostname github.com --git-protocol https --with-token`.
- **OpenSpec trial (spec-driven-dev tooling, fission-ai/openspec).** Installed globally (`openspec`,
  `npm i -g @fission-ai/openspec`) and `init`-ed for Claude Code **in `pipewire_example`** — adds
  `/opsx:explore|propose|apply|archive|sync` commands + skills under `.claude/`, and an `openspec/` dir.
  **Parked behind a local-only ignore (`.git/info/exclude`)** so it is invisible to git / never committed;
  Claude Code still loads it. It is **additive and optional** — our plan-docs/staged-commits/handoff
  method stays primary and the Confluence LLDs stay canonical. Slash commands need a **session reload** to
  appear. **First trial DONE:** ran `/opsx:propose` on the RESIDUAL CLEANUP →
  `playbin-removal-residual-cleanup` change (4/4 artifacts, validates). Early verdict: cleaner
  why/what/how/steps separation + the spec-as-contract (`explicit-pipeline-sink-access`) is a genuinely
  useful artifact our plan-docs lacked, but heavier ceremony for a dead-code deletion; the real value
  (code inventory) was added by an Explore agent, not driven by OpenSpec. **Full judgment pending
  `/opsx:apply`** (does the task list survive contact with the code). Remove the whole trial with:
  `rm -rf openspec .claude/commands/opsx .claude/skills/openspec-* && npm rm -g @fission-ai/openspec`
  then delete the OpenSpec block in `.git/info/exclude`. (Topology also in memory: [[gh-accounts-open-vs-ghec]].)
