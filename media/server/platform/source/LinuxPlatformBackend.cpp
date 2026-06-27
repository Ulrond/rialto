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

#include "LinuxPlatformBackend.h"
#include <new>

namespace
{
const char kCustomInstantRateChangeEventName[] = "custom-instant-rate-change";
} // namespace

namespace firebolt::rialto::server
{
LinuxPlatformBackend::LinuxPlatformBackend(const PlatformHostContext &host)
    : m_gstWrapper{host.gstWrapper}, m_glibWrapper{host.glibWrapper}
{
}

const char *LinuxPlatformBackend::platformName() const
{
    return "linux";
}

GstElement *LinuxPlatformBackend::createAudioSink(const std::string &name)
{
    if (!m_gstWrapper)
        return nullptr;
    // The reference backend names no SoC: it returns only the generic autoaudiosink.
    // Vendor sink selection (amlhalasink / rtkaudiosink) lives in a per-SoC .so loaded
    // by PlatformBackendLoader over the versioned IPlatformBackend ABI.
    return m_gstWrapper->gstElementFactoryMake("autoaudiosink", name.c_str());
}

GstElement *LinuxPlatformBackend::createVideoSink(const std::string &name, uint32_t videoId)
{
    if (!m_gstWrapper)
        return nullptr;
    // The reference backend is plane-agnostic and names no SoC: it returns only the
    // generic autovideosink. The plane-bound vendor sink (e.g. westerossink via
    // setWesterosSinkVideoID) lives in a per-SoC .so.
    (void)videoId;
    return m_gstWrapper->gstElementFactoryMake("autovideosink", name.c_str());
}

bool LinuxPlatformBackend::isVideoMaster() const
{
    // The reference backend has no amlhalasink-style audio-master sink, so the Linux
    // platform is video-master. The audio-master vendor cases live in their per-SoC .so.
    return true;
}

bool LinuxPlatformBackend::applyPlaybackRate(GstElement *pipeline, double rate)
{
    if (!m_gstWrapper || !pipeline)
        return false;
    // The reference path signals the rate as a custom-instant-rate-change event sent
    // downstream on the pipeline. The sink-pad new-segment variant lives in a per-SoC .so.
    GstStructure *structure{
        m_gstWrapper->gstStructureNew(kCustomInstantRateChangeEventName, "rate", G_TYPE_DOUBLE, rate, NULL)};
    return m_gstWrapper->gstElementSendEvent(pipeline,
                                             m_gstWrapper->gstEventNewCustom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB, structure));
}

bool LinuxPlatformBackend::isAudioFadeSupported() const
{
    // The reference backend has no SoC audio path that eases volume, so the engine uses the
    // generic sink "audio-fade" property. SoC audio fade lives in a per-SoC .so.
    return false;
}

void LinuxPlatformBackend::audioFade(double target, uint32_t duration, firebolt::rialto::EaseType easeType)
{
    // No-op: the reference backend performs no SoC audio fade. SoC fade lives in a per-SoC .so.
    (void)target;
    (void)duration;
    (void)easeType;
}

bool LinuxPlatformBackend::processAudioGap(GstElement *pipeline, int64_t position, uint32_t duration,
                                           int64_t discontinuityGap, bool audioAac)
{
    // No-op: the reference backend handles no SoC audio gap. SoC audio-gap handling lives in a per-SoC .so.
    (void)pipeline;
    (void)position;
    (void)duration;
    (void)discontinuityGap;
    (void)audioAac;
    return false;
}

} // namespace firebolt::rialto::server

/* Loader ABI — resolved by the core's dlopen of this backend's .so. */
extern "C" uint32_t rialtoPlatformBackendAbiVersion(void)
{
    return firebolt::rialto::server::kPlatformBackendAbiVersion;
}

extern "C" firebolt::rialto::server::IPlatformBackend *
rialtoCreatePlatformBackend(const firebolt::rialto::server::PlatformHostContext *host)
{
    if (!host)
        return nullptr;
    return new (std::nothrow) firebolt::rialto::server::LinuxPlatformBackend(*host);
}

extern "C" void rialtoDestroyPlatformBackend(firebolt::rialto::server::IPlatformBackend *backend)
{
    delete backend;
}
