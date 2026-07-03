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

#ifndef RIALTO_PLATFORM_BACKENDS_COMMON_GENERIC_GST_BACKEND_H_
#define RIALTO_PLATFORM_BACKENDS_COMMON_GENERIC_GST_BACKEND_H_

#include "IGlibWrapper.h"
#include "IGstWrapper.h"
#include "IPlatformBackend.h"
#include "IRdkGstreamerUtilsWrapper.h"
#include "SocProfile.h"
#include <memory>
#include <string>
#include <vector>

namespace firebolt::rialto::server::backends
{
/**
 * @brief The SoC-free implementation of IPlatformBackend, parameterised by a SocProfile.
 *
 * One class implements all of the versioned ABI (v8). The three jobs of the shim live here:
 *  (1) marshal neutral ABI types <-> vendor types (switchAudioCodec);
 *  (2) delegate the live-graph audio ops through IRdkGstreamerUtilsWrapper to the device's
 *      rdk_gstreamer_utils_<soc>.so (fade / gap / codec-switch / fade-supported);
 *  (3) implement itself the methods rdk-gstreamer-utils has no equivalent for
 *      (buildAudioPath / buildVideoPath / getSupportedProperties / isVideoMaster / createAudioSink /
 *      applyPlaybackRate).
 * The only SoC-specific input is the SocProfile; each per-SoC .so supplies one and the extern "C"
 * loader entrypoints (see PlatformBackendEntry.h).
 */
class GenericGstBackend : public firebolt::rialto::server::IPlatformBackend
{
public:
    GenericGstBackend(const SocProfile &profile, const firebolt::rialto::server::PlatformHostContext &host);
    ~GenericGstBackend() override = default;

    const char *platformName() const override;
    GstElement *createAudioSink(const std::string &name) override;
    firebolt::rialto::server::PlatformMediaPath buildAudioPath(GstElement *pipeline, GstElement *source) override;
    firebolt::rialto::server::PlatformMediaPath buildVideoPath(GstElement *pipeline, GstElement *source,
                                                               uint32_t videoId) override;
    bool isVideoMaster() const override;
    bool applyPlaybackRate(GstElement *pipeline, double rate) override;
    bool isAudioFadeSupported() const override;
    void audioFade(double target, uint32_t duration, firebolt::rialto::EaseType easeType) override;
    bool processAudioGap(GstElement *pipeline, int64_t position, uint32_t duration, int64_t discontinuityGap,
                         bool audioAac) override;
    bool switchAudioCodec(const firebolt::rialto::server::AudioCodecSwitchContext &ctx) override;
    std::vector<std::string> getSupportedProperties(firebolt::rialto::MediaSourceType mediaType,
                                                    const std::vector<std::string> &propertyNames) const override;

private:
    GstElement *createVideoSink(const std::string &name, uint32_t videoId);

    const SocProfile m_profile;
    std::shared_ptr<firebolt::rialto::wrappers::IGstWrapper> m_gstWrapper;
    std::shared_ptr<firebolt::rialto::wrappers::IGlibWrapper> m_glibWrapper;
    std::shared_ptr<firebolt::rialto::wrappers::IRdkGstreamerUtilsWrapper> m_rdkGstreamerUtilsWrapper;
};
} // namespace firebolt::rialto::server::backends

#endif // RIALTO_PLATFORM_BACKENDS_COMMON_GENERIC_GST_BACKEND_H_
