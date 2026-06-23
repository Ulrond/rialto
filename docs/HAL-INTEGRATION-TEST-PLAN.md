# HAL Integration Test Plan — Versioned Conformance for the Rialto / SoC ABI

**TL;DR** — Stand up a two-layer test strategy whose organising principle is **versioned
conformance against the `IPlatformBackend` / AIDL ABI**. The fast inner loop stays the existing
mocked gtest suites (1100+ tests, exact wrapper-call assertions); the outer loop is **ut-core +
python raft** driving the real binder/AIDL HAL via its control plane, on vDevices and racks. Two
versioning axes compose: a **pinned ut-core** (consistent harness — runner, reporting, config) and
an **ABI-versioned conformance suite** (consistent contract — the test content tied to
`kPlatformBackendAbiVersion` / the AIDL interface version). The conformance suite is the
verification half of the versioned-ABI thesis: a SoC backend declaring ABI vN is *certified* by
passing the vN suite, bumps are additive, and old backends need no re-cert — which is what makes
"upgrade the SoC layer without re-testing all platforms" enforceable rather than asserted. The
existing gtest suite joins this world with **no conversion** — re-parent a handful of common
fixtures to a `UtCoreGTestBase` and add a gtest/gmock→ut-core event listener. The coverage matrix
is the versioned requirement record.

_Status: PROPOSAL. Companion to `PLAYBIN-REMOVAL-PLAN.md`; crosses into `pipewire_example/hal`._

---

## 1. Principle: versioned conformance, not exhaustive testing

The target is not "test every property × every caps" (combinatorial, low value, and the mocked
layer cannot validate real decoder behaviour anyway). It is **conformance to a versioned
contract**: the finite surface the system actually exercises — ~31 element properties, ~10
codecs/caps, ~39 `IMediaPipeline` operations, and the AIDL control-plane interfaces — verified
against the ABI version a backend declares.

This makes the central promise of the transformation **checkable**: the SoC layer sits behind a
versioned ABI (`kPlatformBackendAbiVersion`, today v2), and "upgrading it must not force re-test of
all platforms" is only enforceable if a backend can be *certified against its ABI version*.

## 2. Two layers

| Layer | Vehicle | Validates | Speed |
|---|---|---|---|
| **Inner — unit** | mocked gtest/gmock (existing 1100+) | the engine issues the *right* calls (property name, caps field, sequence) | seconds |
| **Outer — integration / conformance** | **ut-core + raft** over the real binder/AIDL HAL | the call *took effect* on real (or virtual) hardware — metrics, behaviour, timing | minutes, on device |

The inner layer cannot prove a decoder did the right thing; the outer layer can. The mocked suites
stay the fast inner loop. raft + ut-core are the outer loop and the execution/scaling substrate for
both.

## 3. Two versioning axes (they compose)

| Axis | What is versioned | Gives |
|---|---|---|
| **Pinned ut-core** | the **harness** — runner, reporting format, config schema, feature set | consistent, reproducible *execution* across components and over time; comparable results because the tool is version-locked. ut-core is **ingested at a fixed version** (submodule/package/manifest pin) and bumped deliberately, in lockstep — never drifting per component. |
| **ABI-versioned conformance suite** | the **contract** — the test *content*, tied to `kPlatformBackendAbiVersion` and the AIDL interface version | per-version *certifiability*: "this SoC backend conforms to ABI vN." |

Pinned tool **running** versioned contract. Neither alone suffices: a pinned harness without a
versioned contract cannot certify the ABI; a versioned contract without a pinned harness drifts in
how it is run and reported.

## 4. The control plane

The integration layer drives the HAL through the control plane **already defined in the AIDL** —
the `I<X>Controller` / `I<X>Manager` / `I<X>EventListener` / `I<X>ControllerListener` interfaces
(e.g. `IAudioSinkController`). raft uses these to apply deterministic **stimulus** and observe
**responses + metrics**, against the real services brought up by `hal/scripts/services_up.sh`. This
is integrated-HAL testing via the control plane — not merely wrapping unit tests.

## 5. Execution: ut-core + python raft

raft provides the execution and control substrate, applied to **both** layers:

- **Multi-run scaling** — fan the same suite across configs/devices; repeat-runs for flake
  detection.
- **HW + SW control** — power, serial, network, flashing.
- **vDevice** — virtual targets, so the conformance matrix runs without physical silicon.
- **Racks** — the same control plane whether a local bench or a lab rack.
- **Per-platform config** — YAML device profiles parameterise the *same* matrix across
  BCM / RTK / AML / MTK (the "inherit configuration" benefit).

ut-core is the on-target harness raft invokes; the per-platform profile selects the device and the
ABI version under test.

## 6. ut-core integration without test conversion

The existing 1100+ gtest/gmock tests join the pinned ut-core harness with **no per-test rewrite**:

- `TEST_F(Fixture, Name)` only requires `Fixture` to derive (transitively) from `::testing::Test`.
- Provide one **`UtCoreGTestBase`** (`: public ::testing::Test`, adding ut-core setup/teardown,
  config-profile access, RDK reporting hooks).
- **Re-parent the shared base fixtures** to it — `GstGenericPlayerTestCommon`,
  `GenericTasksTestsBase`, `MediaPipelineTestBase`, and the other common-fixture roots. Every
  derived test inherits ut-core integration.
- Register one **`::testing::TestEventListener`** in the runner that forwards gtest/gmock results
  (including `EXPECT_CALL` failures) into ut-core's reporting. This listener *is* the
  gmock-in-ut-core integration (see §10).
- `EXPECT_*` / `EXPECT_CALL` keep working through the listener; optional `UT_*` aliases let *new*
  tests be authored ut-core-native, but existing tests need nothing.

Change surface: one new base class + the listener + re-parenting ~4 fixtures. No flag-day.

## 7. Coverage matrix — the versioned requirement record

The matrix is the framework-agnostic requirement spec, versioned with the ABI (each version
adds/changes cells). Derived from code, not imagination:

- **Properties** (31) × {playbin, explicit} — set, and asserted, on each path.
- **Codecs / caps** (~10) × {playbin, explicit} — constructed, and asserted, on each path.
- **`IMediaPipeline` operations** (39) × path.
- **AIDL control-plane operations** × backend — the integration-layer contract surface.

For the playbin removal, the only interesting cells are **"playbin sets it, explicit does not yet"**
— the real risk of a property silently dropped in the rewrite. Current standing:

- audio-sink properties (`low-latency`, `sync`) — closed (stage 3c).
- audio-decoder properties (`sync-off`, `stream-sync-mode`, `limit-buffering-ms`,
  `enable-rate-correction`) — open: routed through `getDecoder(AUDIO)`, which must reach the decoder
  inside `decodebin` (the decoder analogue of the 3c sink work).
- vendor-sink properties (`wait-video`, `disable-xrun`, `async`) — owned by the per-SoC backend
  `.so`, tracked there, not a builder gap.
- video + subtitle properties — stage 4.

## 8. Certification model

1. A backend declares its ABI version via `rialtoPlatformBackendAbiVersion()`. The core already
   *rejects a mismatch*; conformance adds the *positive* check.
2. Run the **vN conformance suite** (ut-core, via raft, on the target/vDevice) against the backend.
   PASS ⇒ certified for ABI vN.
3. ABI bumps are **additive** (v1→v2 added `createVideoSink(id)`): the vN+1 suite = vN + new cases.
   A backend implementing vN certifies against the vN suite; it is not re-certified when the ABI
   advances — the "no re-test" guarantee, made enforceable.
4. Results are recorded per {platform, backend version, ABI version, ut-core version} — a versioned
   conformance record, comparable across the estate.

## 9. Phasing

1. **Pin + ingest ut-core**; bring up `UtCoreGTestBase` + the event listener; re-parent the shared
   fixtures. Outcome: the existing suites run under the pinned ut-core with RDK reporting/config,
   raft-orchestratable — no test conversion.
2. **Stand up the control-plane harness** — raft drives the real HAL (`hal/`) via the AIDL
   `I<X>Controller` interfaces on a **local rack / vDevice** first.
3. **Author the ABI-vN conformance suite** from the coverage matrix — ut-core-native, generated
   where possible from the AIDL interfaces, matrix-driven assertions.
4. **Per-platform rollout** — the same suite under each YAML profile (BCM/RTK/AML/MTK), recording
   versioned conformance.

## 10. Dependencies & open items

- **gmock-in-ut-core** (ticket) — scope: the `UtCoreGTestBase` + the gtest/gmock→ut-core
  `TestEventListener` adapter + CMake integration + one reference mocked test proving the pattern.
- **ut-core version pin** — the fixed version to ingest, and the bump policy (lockstep).
- **raft device profiles** — local rack + vDevice definitions; per-SoC profiles.
- **`getDecoder(AUDIO)` explicit reach** — prerequisite for closing the audio-decoder coverage
  cells (§7); the next explicit-builder sub-step after 3c.
- Suite home and ownership across `external/rialto` and `pipewire_example/hal`.
