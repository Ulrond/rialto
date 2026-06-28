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

#ifndef FIREBOLT_RIALTO_SERVER_LINUX_PLATFORM_BACKEND_H_
#define FIREBOLT_RIALTO_SERVER_LINUX_PLATFORM_BACKEND_H_

#include "IGlibWrapper.h"
#include "IGstWrapper.h"
#include "IPlatformBackend.h"
#include "IRdkGstreamerUtilsWrapper.h"
#include <memory>
#include <string>

typedef struct _GstCaps GstCaps;

namespace firebolt::rialto::server
{
/**
 * @brief Reference platform backend for Rialto-for-Linux (NATIVE_BUILD).
 *
 * Provides the generic GStreamer sinks — autoaudiosink / autovideosink — and names no
 * SoC. It is the guaranteed, zero-config fallback PlatformBackendLoader uses when no
 * vendor backend .so is present, and the playback proof for the SoC-isolation seam:
 * with no vendor sink, the core drives playback entirely through IPlatformBackend.
 *
 * Vendor sink selection (amlhalasink / rtkaudiosink / westerossink) lives in a per-SoC
 * .so that implements the same versioned ABI, not here.
 */
class LinuxPlatformBackend : public IPlatformBackend
{
public:
    explicit LinuxPlatformBackend(const PlatformHostContext &host);
    ~LinuxPlatformBackend() override = default;

    const char *platformName() const override;
    GstElement *createAudioSink(const std::string &name) override;
    GstElement *createVideoSink(const std::string &name, uint32_t videoId) override;
    bool isVideoMaster() const override;
    bool applyPlaybackRate(GstElement *pipeline, double rate) override;
    bool isAudioFadeSupported() const override;
    void audioFade(double target, uint32_t duration, firebolt::rialto::EaseType easeType) override;
    bool processAudioGap(GstElement *pipeline, int64_t position, uint32_t duration, int64_t discontinuityGap,
                         bool audioAac) override;
    bool switchAudioCodec(const AudioCodecSwitchContext &ctx) override;
    bool shouldSkipCapabilityProbe(const std::string &elementName) const override;

private:
    // Transitional amlhalasink fork helpers (moved verbatim out of the engine core; they will move
    // down into a per-SoC .so once one is authored). They operate on a local PlaybackGroupPrivate
    // built from the neutral AudioCodecSwitchContext, not on any engine state.
    void configAudioCap(firebolt::rialto::wrappers::AudioAttributesPrivate *pAttrib, bool *audioaac, bool svpenabled,
                        GstCaps **appsrcCaps);
    void haltAudioPlayback(firebolt::rialto::wrappers::PlaybackGroupPrivate &group);
    void resumeAudioPlayback(firebolt::rialto::wrappers::PlaybackGroupPrivate &group);
    void firstTimeSwitchFromAC3toAAC(firebolt::rialto::wrappers::PlaybackGroupPrivate &group, GstCaps *newAudioCaps);
    bool applyAudioCodecSwitch(firebolt::rialto::wrappers::PlaybackGroupPrivate &group, bool isAudioAAC,
                               GstCaps *newAudioCaps);
    bool performAudioTrackCodecChannelSwitch(firebolt::rialto::wrappers::PlaybackGroupPrivate &group,
                                             const void *pSampleAttr,
                                             firebolt::rialto::wrappers::AudioAttributesPrivate *pAudioAttr,
                                             uint32_t *pStatus, unsigned int *pui32Delay,
                                             long long *pAudioChangeTargetPts, // NOLINT(runtime/int)
                                             const long long *pcurrentDispPts, // NOLINT(runtime/int)
                                             unsigned int *audioChangeStage, GstCaps **appsrcCaps, bool *audioaac,
                                             bool svpenabled, GstElement *aSrc, bool *ret);

    std::shared_ptr<firebolt::rialto::wrappers::IGstWrapper> m_gstWrapper;
    std::shared_ptr<firebolt::rialto::wrappers::IGlibWrapper> m_glibWrapper;
    std::shared_ptr<firebolt::rialto::wrappers::IRdkGstreamerUtilsWrapper> m_rdkGstreamerUtilsWrapper;
};

} // namespace firebolt::rialto::server

#endif // FIREBOLT_RIALTO_SERVER_LINUX_PLATFORM_BACKEND_H_
