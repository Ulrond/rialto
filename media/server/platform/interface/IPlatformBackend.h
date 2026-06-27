/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2026 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FIREBOLT_RIALTO_SERVER_I_PLATFORM_BACKEND_H_
#define FIREBOLT_RIALTO_SERVER_I_PLATFORM_BACKEND_H_

#include "MediaCommon.h"

#include <cstdint>
#include <memory>
#include <string>

typedef struct _GstElement GstElement;

namespace firebolt::rialto::wrappers
{
class IGstWrapper;
class IGlibWrapper;
} // namespace firebolt::rialto::wrappers

namespace firebolt::rialto::server
{
/**
 * @brief Versioned platform-backend ABI.
 *
 * The platform backend isolates the SoC-specific media concerns behind a frozen,
 * versioned contract: element selection (today a registry-probe ladder inlined in
 * the engine: amlhalasink / rtkaudiosink / westerossink, with autoaudiosink /
 * autovideosink as the Linux fallback) and SoC capability flags (e.g. whether the
 * platform is video-master). The engine ("core heart") owns no SoC names; it asks
 * the backend to make the platform's sinks and to report its capability flags.
 *
 * Versioning is additive: v3 grows the seam with capability flags (isVideoMaster)
 * on top of v2's sink creation; v4 grows it further with the live-graph audio ops
 * (isAudioFadeSupported / audioFade / processAudioGap), moving the last SoC audio
 * knowledge out of the engine core. New methods are appended; existing ones are
 * frozen, so a v2 backend stays valid against v2 cases.
 *
 * The backend is loaded as a separate `.so` via the extern "C" entrypoints below
 * and version-checked, so a vendor layer can be upgraded without rebuilding or
 * re-certifying the core. The reference Linux backend (autoaudiosink /
 * autovideosink) is the first implementation and the playback proof on the
 * Rialto-for-Linux (NATIVE_BUILD) interface.
 *
 * Phase 1 keeps GStreamer as the engine, so sinks are `GstElement*`. The
 * engine-neutral generalisation is Phase 2 (see the Graphics Player / PipeWire
 * core work).
 */
constexpr uint32_t kPlatformBackendAbiVersion = 4;

/**
 * @brief Services the core hands the backend at creation, so it can build
 *        GStreamer elements through the same wrappers the engine uses (keeping
 *        the existing dependency-injection / test-seam discipline).
 */
struct PlatformHostContext
{
    std::shared_ptr<firebolt::rialto::wrappers::IGstWrapper> gstWrapper;
    std::shared_ptr<firebolt::rialto::wrappers::IGlibWrapper> glibWrapper;
};

class IPlatformBackend
{
public:
    virtual ~IPlatformBackend() = default;

    IPlatformBackend(const IPlatformBackend &) = delete;
    IPlatformBackend &operator=(const IPlatformBackend &) = delete;
    IPlatformBackend(IPlatformBackend &&) = delete;
    IPlatformBackend &operator=(IPlatformBackend &&) = delete;

    /**
     * @brief A short identifier for logging/introspection (e.g. "linux", "amlogic").
     */
    virtual const char *platformName() const = 0;

    /**
     * @brief Creates the platform's audio sink as a GStreamer element.
     *
     * Device backends return a vendor sink (amlhalasink / rtkaudiosink); the Linux
     * backend returns autoaudiosink. Returned element carries a floating ref for
     * the caller to add to the pipeline.
     *
     * @param[in] name : Element instance name.
     * @retval the new sink element, or nullptr on failure.
     */
    virtual GstElement *createAudioSink(const std::string &name) = 0;

    /**
     * @brief Creates the platform's video sink as a GStreamer element, bound to a video plane.
     *
     * Device backends return the vendor sink (e.g. westerossink) bound to the plane via
     * setWesterosSinkVideoID(videoId); the Linux backend returns autovideosink.
     *
     * @param[in] name    : Element instance name.
     * @param[in] videoId : Video/plane resource ID — a static binding to the output plane
     *                      (0 = Main, 1 = PiP), aligning with MediaSessionConfig.output. This
     *                      supersedes the primary/secondary boolean: setWesterossinkSecondaryVideo
     *                      is a capability query, not a sink-creation, so it never fit here.
     * @retval the new sink element, or nullptr on failure.
     */
    virtual GstElement *createVideoSink(const std::string &name, uint32_t videoId) = 0;

    /**
     * @brief Whether the platform drives playback as video-master (ABI v3).
     *
     * A SoC capability flag the engine reports through GstCapabilities. Device backends
     * answer from their fixed platform knowledge (e.g. an amlhalasink-based audio path is
     * audio-master, so returns false); the Linux reference backend has no such sink and is
     * video-master, so returns true. The core names no SoC.
     *
     * @retval true if the platform is video-master, false otherwise.
     */
    virtual bool isVideoMaster() const = 0;

    /**
     * @brief Applies a playback rate change the platform's way (ABI v3).
     *
     * Platforms differ in how a rate change is signalled: the Linux reference backend sends a
     * custom-instant-rate-change event downstream on the pipeline; a vendor backend may instead
     * send a new-segment event to its audio sink pad. The engine asks the backend to apply the
     * rate so it names no SoC; the backend uses the host GStreamer wrappers it was given.
     *
     * @param[in] pipeline : The live pipeline the rate applies to.
     * @param[in] rate     : The playback rate to apply.
     * @retval true if the rate-change event was sent successfully, false otherwise.
     */
    virtual bool applyPlaybackRate(GstElement *pipeline, double rate) = 0;

    /**
     * @brief Whether the platform's SoC audio path performs audio fade/easing (ABI v4).
     *
     * A SoC capability flag: device backends whose audio path eases volume in hardware/firmware
     * answer true; the Linux reference backend has no such path and returns false, so the engine
     * uses the generic sink "audio-fade" property instead. The core names no SoC.
     *
     * @retval true if the platform performs SoC audio fade, false otherwise.
     */
    virtual bool isAudioFadeSupported() const = 0;

    /**
     * @brief Applies a SoC audio fade/easing the platform's way (ABI v4).
     *
     * Only called when isAudioFadeSupported() is true. Device backends ease the volume through
     * their SoC audio path; the Linux reference backend has no such path and is a no-op. The
     * engine asks the backend to fade so it names no SoC.
     *
     * @param[in] target   : The target volume to fade to.
     * @param[in] duration : The fade duration.
     * @param[in] easeType : The easing curve to apply.
     */
    virtual void audioFade(double target, uint32_t duration, firebolt::rialto::EaseType easeType) = 0;

    /**
     * @brief Handles an audio gap/discontinuity the platform's way (ABI v4).
     *
     * Device backends bridge the gap through their SoC audio path; the Linux reference backend has
     * no such path and is a no-op returning false. The engine asks the backend to handle the gap so
     * it names no SoC; the backend uses the host GStreamer wrappers it was given.
     *
     * @param[in] pipeline        : The live pipeline the gap applies to.
     * @param[in] position        : Audio pts gap position.
     * @param[in] duration        : Audio pts gap duration.
     * @param[in] discontinuityGap : Audio discontinuity gap.
     * @param[in] audioAac        : True if the audio codec is AAC.
     *
     * @retval true if the platform handled the audio gap, false otherwise.
     */
    virtual bool processAudioGap(GstElement *pipeline, int64_t position, uint32_t duration,
                                 int64_t discontinuityGap, bool audioAac) = 0;

protected:
    IPlatformBackend() = default;
};

} // namespace firebolt::rialto::server

/*
 * Loader ABI — the core dlopen()s librialtoplatform-<soc>.so and resolves these.
 * extern "C" keeps the entrypoints ABI-stable across compilers/SDK versions; the
 * core rejects a backend whose rialtoPlatformBackendAbiVersion() differs from
 * kPlatformBackendAbiVersion. This is the seam that lets the vendor layer upgrade
 * without re-testing the core.
 */
extern "C" {

uint32_t rialtoPlatformBackendAbiVersion(void);

firebolt::rialto::server::IPlatformBackend *
rialtoCreatePlatformBackend(const firebolt::rialto::server::PlatformHostContext *host);

void rialtoDestroyPlatformBackend(firebolt::rialto::server::IPlatformBackend *backend);

} // extern "C"

#endif // FIREBOLT_RIALTO_SERVER_I_PLATFORM_BACKEND_H_
