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

    // Transitional SoC sink selection. The amlhalasink (Llama) and rtkaudiosink
    // (XiOne) branches belong in the per-SoC backend .so — the "SoC lower level" —
    // and will lift out of here when the dlopen loader lands, leaving the reference
    // Linux backend with only the autoaudiosink path. They live here for now so the
    // engine names no SoC while vendor hardware keeps its sink (no regression).
    GstRegistry *reg = m_gstWrapper->gstRegistryGet();
    if (!reg)
        return nullptr;

    GstPluginFeature *feature = nullptr;
    if (nullptr != (feature = m_gstWrapper->gstRegistryLookupFeature(reg, "amlhalasink")))
    {
        GstElement *sink = m_gstWrapper->gstElementFactoryMake("amlhalasink", name.c_str());
        if (sink && m_glibWrapper)
            m_glibWrapper->gObjectSet(G_OBJECT(sink), "direct-mode", FALSE, nullptr);
        m_gstWrapper->gstObjectUnref(feature);
        return sink;
    }
    else if (nullptr != (feature = m_gstWrapper->gstRegistryLookupFeature(reg, "rtkaudiosink")))
    {
        GstElement *sink = m_gstWrapper->gstElementFactoryMake("rtkaudiosink", name.c_str());
        if (sink && m_glibWrapper)
        {
            m_glibWrapper->gObjectSet(G_OBJECT(sink), "media-tunnel", FALSE, nullptr);
            m_glibWrapper->gObjectSet(G_OBJECT(sink), "audio-service", TRUE, nullptr);
        }
        m_gstWrapper->gstObjectUnref(feature);
        return sink;
    }

    return m_gstWrapper->gstElementFactoryMake("autoaudiosink", name.c_str());
}

GstElement *LinuxPlatformBackend::createVideoSink(const std::string &name)
{
    if (!m_gstWrapper)
        return nullptr;
    return m_gstWrapper->gstElementFactoryMake("autovideosink", name.c_str());
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
