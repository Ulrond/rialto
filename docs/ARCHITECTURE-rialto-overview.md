# Rialto Architecture — Overview & the dlopen Platform Backend

**TL;DR** — Five pictures. (1) **High-level Rialto**: a client/server media stack split over
protobuf IPC, with two coexisting northbound interfaces converging on one `IMediaPipeline` contract,
one GStreamer engine, and the `IPlatformBackend` SoC seam. (2) **dlopen layering**: what stays in the
statically-linked core (the factory, `PlatformBackendLoader`, the reference backend, the wrappers, the
versioned ABI header) versus what is loaded at runtime (the per-SoC `.so`), and exactly what crosses
the `extern "C"` boundary. (3) **Where requirements live**: playbin used to define the pipeline
*implicitly* (flags + autoplug + signals + reactive clawback); the new model defines it *explicitly*
in Rialto's chain builders plus the backend. (4) **How a platform plugs in**: the SoC lower layer's *two
internal plug points* — the `IPlatformBackend` `.so` (dlopen, versioned ABI) and the
`rdk-gstreamer-utils` library (linked, via `RdkGstreamerUtilsWrapper`). (Not to be confused with the
two *external* surfaces — the MSE sinks vs the native API — in §1.) (5) **SoC migration map**: which inline engine concern moves to which
seam home (the `complete-soc-platform-isolation` plan). The diagrams render on GitHub and in VS Code
(Markdown Preview Mermaid Support); export with
`npx -y @mermaid-js/mermaid-cli -i ARCHITECTURE-rialto-overview.md -o out.svg`.

Companion: `ARCHITECTURE-playbin-vs-explicit.md` (the detailed southbound construction diagrams).

---

## 1. High-level architecture

Rialto is a **client/server** system: each app links a thin Rialto client that proxies the media API
to a central Rialto **server** over a per-app protobuf/unix-domain-socket. The server owns the single
GStreamer engine and selects SoC sinks through the `IPlatformBackend` seam.

```mermaid
flowchart TB
    subgraph APPS["Applications"]
        MSEAPP["MSE web apps<br/>(100s, browser EME/MSE)"]
        NATIVE["Native apps<br/>Netflix · YouTube · Cobalt"]
    end

    subgraph CLIENT["Rialto CLIENT (per app)"]
        RGST["rialto-gstreamer<br/>rialtomse*sink — NORTHBOUND A"]
        ROCDM["rialto-ocdm<br/>EME / CDM"]
        CLIB["Rialto client lib<br/>IMediaPipeline / IMediaKeys proxies<br/>canCreateSession / IMediaSession — NORTHBOUND B"]
    end

    IPC{{"IPC — protobuf over unix-domain socket<br/>one socket per app (Server Manager)"}}

    subgraph SERVER["Rialto SERVER"]
        SMGR["Server Manager<br/>session + socket lifecycle"]
        MP["IMediaPipeline — server public API<br/>THE CONTRACT (interface-version'd)"]
        GGP["GstGenericPlayer<br/>explicit construction (default)"]
        LOADER["PlatformBackendLoader"]
        BESEAM["IPlatformBackend — versioned ABI v2<br/>THE SoC SEAM"]
    end

    GST["GStreamer engine (single)"]
    REF["reference sinks<br/>autoaudiosink / autovideosink"]
    VEND["vendor sinks (per-SoC .so)<br/>amlhalasink · westerossink · ..."]
    HW["SoC hardware / RDK HAL"]

    MSEAPP --> RGST
    MSEAPP -. DRM .-> ROCDM
    NATIVE --> CLIB
    RGST --> CLIB
    ROCDM --> CLIB
    CLIB --> IPC --> SMGR --> MP --> GGP
    GGP --> GST
    GGP --> LOADER --> BESEAM
    BESEAM --> REF
    BESEAM --> VEND
    REF --> GST
    VEND --> GST
    GST --> HW

    classDef if fill:#e8eefc,stroke:#3559a8,color:#13265e
    classDef seam fill:#e3f2e3,stroke:#2e7d32,color:#1b5e20
    class RGST,CLIB,MP if
    class BESEAM,LOADER seam
```

- **Two external (northbound) surfaces coexist** (blue) — the app's choice, both supported:
  - **MSE GStreamer sinks** — old/current apps keep *their own* GStreamer pipeline and slot Rialto's
    `rialtomse*sink` elements in as the sink stage. No app rewrite.
  - **Native interface** — apps upgraded to *pass data through and make calls on* Rialto's native API
    (`canCreateSession`/`IMediaSession`), no app-side GStreamer (Netflix/YouTube/Cobalt).
  Both resolve to the same `IMediaPipeline` server contract. This coexistence is the backward-compatible
  spine of the transformation — these are the only "surfaces" external users see.
- **One engine, one seam** (green): the server runs a single GStreamer engine; all SoC specifics sit
  behind `IPlatformBackend`, acquired by `PlatformBackendLoader`.

---

## 2. The dlopen platform backend

### 2a. Layering — what stays in the core vs what is loaded

```mermaid
flowchart TB
    subgraph CORE["Rialto server core — statically linked, SoC-agnostic"]
        FACT["GstGenericPlayerFactory / GstWebAudioPlayerFactory<br/>::create()"]
        LOADER["PlatformBackendLoader<br/>discovery · dlopen · version-check · ownership"]
        REFB["LinuxPlatformBackend (reference)<br/>autoaudiosink / autovideosink — compiled in"]
        WRAP["IGstWrapper / IGlibWrapper<br/>(the DI / test seam)"]
        HDR["IPlatformBackend.h — versioned ABI<br/>kPlatformBackendAbiVersion = 2"]
    end

    BND{{"extern C ABI boundary<br/>IN: PlatformHostContext{ gstWrapper, glibWrapper }<br/>OUT: IPlatformBackend*<br/>SYMS: rialtoPlatformBackendAbiVersion · rialtoCreatePlatformBackend · rialtoDestroyPlatformBackend"}}

    subgraph SO["Loaded at runtime — per-SoC .so (NOT in the core)"]
        VEND["librialtoplatform-&lt;soc&gt;.so<br/>implements IPlatformBackend<br/>vendor sinks: amlhalasink · westerossink(plane) · ..."]
    end

    FACT --> LOADER
    LOADER -->|"no vendor .so → fallback (no dlopen)"| REFB
    LOADER -->|"vendor .so → dlopen"| BND
    BND --> VEND
    WRAP -. "injected via PlatformHostContext" .-> LOADER
    HDR -. "shared header — both sides compile against it" .- BND

    classDef seam fill:#e3f2e3,stroke:#2e7d32,color:#1b5e20
    classDef boundary fill:#fdeede,stroke:#c8761a,color:#6e3d05
    class LOADER,REFB seam
    class BND boundary
```

The core names **no SoC**. The reference backend is compiled in and is the **can't-fail fallback** (no
`dlopen`). A vendor layer ships exactly one `librialtoplatform-*.so`; only three C symbols, a
host-context in, and an `IPlatformBackend*` out ever cross the boundary — so the SoC layer is upgraded
without rebuilding or re-certifying the core.

### 2b. Load decision — discovery, version-check, fallback

```mermaid
flowchart TD
    A["GstGenericPlayer::create()"] --> B["PlatformBackendLoader::load(host)"]
    B --> C{"RIALTO_PLATFORM_BACKEND set?"}
    C -->|yes| P["resolve that .so path"]
    C -->|no| D{"single librialtoplatform-*.so in<br/>RIALTO_PLATFORM_DIR / compile-time default?"}
    D -->|yes| P
    D -->|"none / ambiguous"| REF["reference backend (in-core)<br/>plain deleter — no dlclose"]
    P --> E["dlopen RTLD_NOW | RTLD_LOCAL"]
    E -->|fail| REF
    E -->|ok| F["dlsym the 3 entrypoints"]
    F -->|"any missing"| G["log + dlclose"] --> REF
    F -->|ok| H{"abiVersion == kPlatformBackendAbiVersion?"}
    H -->|"no"| I["log file + both versions<br/>dlclose (no create)"] --> REF
    H -->|"yes"| J["rialtoCreatePlatformBackend(&host)"]
    J --> K["shared_ptr&lt;IPlatformBackend&gt;<br/>deleter: rialtoDestroyPlatformBackend → dlclose<br/>(.so stays mapped for the backend's life)"]

    classDef ok fill:#e3f2e3,stroke:#2e7d32,color:#1b5e20
    classDef fb fill:#eef1f5,stroke:#7a8aa0,color:#33414f
    class J,K ok
    class REF fb
```

---

## 3. Where the pipeline requirements live — playbin vs explicit

playbin used to *define the requirements implicitly*: its flags, autoplug rules, and signals decided
which elements and properties the graph had, and Rialto reached back in reactively to recover handles.
The new model makes every requirement *explicit* — Rialto's chain builders name and link each element,
and the backend owns SoC sink selection.

```mermaid
flowchart LR
    subgraph OLD["OLD — requirements IMPLICIT in playbin"]
        direction TB
        o1["playbin flags<br/>setPlaybinFlags · getGstPlayFlag · shouldEnableNativeAudio"]
        o2["autoplug picks elements<br/>decoder · parser · sinks · playsink"]
        o3["signals discover the graph<br/>source-setup · element-setup · deep-element-added"]
        o4["reactive clawback<br/>SetupElement · DeepElementAdded · playbackGroup · getSink-via-prop"]
        o1 --> o2 --> o3 --> o4
    end

    subgraph NEW["NEW — requirements EXPLICIT in Rialto + backend"]
        direction TB
        n1["buildAudioChain / buildVideoChain / buildSubtitleChain<br/>Rialto names + links every element"]
        n2["IPlatformBackend.createAudioSink / createVideoSink(id)<br/>SoC sink selection — the seam"]
        n3["SetupAudioDecoder / SetupVideoParser<br/>configure the one autoplugged decoder/parser"]
        n1 --> n2 --> n3
    end

    OLD == "playbin removed (#5c) · seam now load-bearing (#6)" ==> NEW
```

Net effect: the graph drops from ≈36 elements to ≈4 per stream, construction is deterministic (no
lazy-autoplug deadlock), and "what the pipeline requires" is now readable in the source rather than
inferred from playbin's behaviour. The northbound `IMediaPipeline` contract is unchanged throughout.

---

## 4. How a platform plugs in — the SoC lower layer (internal plug points)

These are **internal, southbound** plug points — the SoC lower layer — distinct from the two *external*
surfaces of §1 (which are the app's choice). A platform plugs into Rialto through **two** internal plug
points today. Both are SoC-specific, both ship per-platform, both have a Linux default in-core:

```mermaid
flowchart TB
    subgraph CORE["Rialto server core — SoC-agnostic upper package"]
        GGP["GstGenericPlayer / GstWebAudioPlayer"]
        LOADER["PlatformBackendLoader"]
        RGUW["RdkGstreamerUtilsWrapper<br/>(in-core wrapper / DI seam)"]
        REFB["LinuxPlatformBackend (reference)<br/>auto-sinks · no quirks"]
    end

    subgraph S1["SoC PLUG POINT 1 — IPlatformBackend .so"]
        SO["librialtoplatform-&lt;soc&gt;.so<br/>create sinks · caps flags · playback-rate · decoder cfg · quirks<br/>(element create / config / quirks)"]
    end

    subgraph S2["SoC PLUG POINT 2 — rdk-gstreamer-utils"]
        RGU["librdkgstreamerutils (vendor)<br/>performAudioTrackCodecChannelSwitch · processAudioGap<br/>doAudioEasingonSoc · isSocAudioFadeSupported<br/>(live-graph operations)"]
        STUB["Linux: stub (no-op / true)"]
    end

    GGP --> LOADER
    GGP --> RGUW
    LOADER -->|"dlopen + ABI version-check"| SO
    LOADER -->|"no .so → fallback (no dlopen)"| REFB
    RGUW -->|"links + calls vendor lib"| RGU
    RGUW -. "on Linux" .-> STUB

    classDef seam fill:#e3f2e3,stroke:#2e7d32,color:#1b5e20
    classDef s2 fill:#fdeede,stroke:#c8761a,color:#6e3d05
    class LOADER,SO,REFB seam
    class RGUW,RGU,STUB s2
```

- **Plug point 1 — `IPlatformBackend` `.so`** (green): *element-local* concerns — sink creation, element
  configuration, capability flags, platform quirks. Loaded by `dlopen` with a version-checked ABI; the
  reference backend is the in-core Linux default.
- **Plug point 2 — `rdk-gstreamer-utils`** (orange): *cross-cutting live-graph operations* the vendor
  already implements — seamless audio codec-channel switch, audio-gap processing, audio fade/easing.
  Linked as a library and reached through `RdkGstreamerUtilsWrapper` (the in-core DI seam); Linux uses
  the no-op stub. `performAudioTrackCodecChannelSwitch` takes the whole playback group and cannot be
  reimplemented per-element, which is why it stays distinct from the `.so` today.

Home rule: **element create / config / caps / quirks → plug point 1; live-graph ops already owned by the
vendor lib → plug point 2.** Both are versioned and shipped per-SoC; the goal is to collapse them to a
single seam, and the eventual AIDL-HAL lower layer subsumes both.

---

## 5. SoC migration map — what moves where

The `complete-soc-platform-isolation` plan finishes the isolation: every SoC concern still inline in
the engine moves to a seam home (additive `IPlatformBackend` v2 → v3), and per-SoC `.so`s carry it so
current platforms stay stable.

```mermaid
flowchart LR
    subgraph INLINE["TODAY — inline in the engine (core still names SoC)"]
        direction TB
        c1["GstCapabilities<br/>amlhalasink probe → isVideoMaster=false"]
        c2["SetPlaybackRate<br/>amlhalasink → new-segment vs custom instant-rate"]
        c3["pushSampleIfRequired<br/>amlogic double-segment position quirk"]
        c4["GstGenericPlayer<br/>enable-rate-correction (commented broadcom)"]
    end

    subgraph V3["→ IPlatformBackend v3 (.so) — additive"]
        direction TB
        m1["isVideoMaster()"]
        m2["applyPlaybackRate(sink, rate)"]
        m3["position-quirk hook"]
        m4["configureAudioDecoder()<br/>(or stays in core if truly generic probe)"]
    end

    subgraph DONE["already SoC-isolated"]
        direction TB
        d0["sink creation — IPlatformBackend v2 (#6, done)"]
        d1["codec-switch · audio-gap · fade — rdk-gstreamer-utils"]
    end

    c1 --> m1
    c2 --> m2
    c3 --> m3
    c4 --> m4

    classDef done fill:#e3f2e3,stroke:#2e7d32,color:#1b5e20
    class DONE,d0,d1 done
```

Per-concern detail:

| # | Concern (today) | Site | Moves to | Reference (Linux) default | Vendor `.so` does |
| --- | --- | --- | --- | --- | --- |
| 1 | video-master capability | `GstCapabilities` | `isVideoMaster()` (v3) | `true` | amlogic returns `false` |
| 2 | playback-rate application | `SetPlaybackRate` | `applyPlaybackRate(sink, rate)` (v3) | custom instant-rate event on pipeline | amlhalasink sends a new-segment event on the sink pad |
| 3 | position quirk | `pushSampleIfRequired` | position-quirk hook (v3) | no extra push | amlogic pushes the double segment |
| 4 | decoder rate-correction | `GstGenericPlayer` | `configureAudioDecoder()` (v3) — or **stays in core** if it is a pure property-probe with no SoC name | property-probe sets `enable-rate-correction` if present | same, owned by the backend |
| — | sink creation | `LinuxPlatformBackend` | `IPlatformBackend` v2 | auto-sinks | vendor sinks (**done, #6**) |
| — | codec-switch · audio-gap · fade | `RdkGstreamerUtilsWrapper` | `rdk-gstreamer-utils` lib | no-op stub | vendor lib implementation |

Each row is migrated **behaviour-preserving and test-first**, and a platform's inline branch is retired
only once its `.so` passes the conformance suite — so "complete the isolation" never means "regress a
current platform."
