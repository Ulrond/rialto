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

/*
 * Test fixture for PlatformBackendLoaderTest: a minimal vendor backend .so built in three
 * variants from this one source via compile defines, exercised by real dlopen.
 *   - default            : matching ABI version, all three entrypoints  -> loads ("fixture")
 *   - FIXTURE_MISMATCH    : ABI version != kPlatformBackendAbiVersion    -> refused, fallback
 *   - FIXTURE_OMIT_VERSION: rialtoPlatformBackendAbiVersion not exported -> missing symbol, fallback
 */

#include "IPlatformBackend.h"
#include <new>
#include <string>
#include <vector>

namespace
{
class FixtureBackend : public firebolt::rialto::server::IPlatformBackend
{
public:
    const char *platformName() const override { return "fixture"; }
    GstElement *createAudioSink(const std::string &) override { return nullptr; }
    firebolt::rialto::server::PlatformMediaPath buildAudioPath(GstElement *, GstElement *) override { return {}; }
    firebolt::rialto::server::PlatformMediaPath buildVideoPath(GstElement *, GstElement *, uint32_t) override
    {
        return {};
    }
    bool isVideoMaster() const override { return true; }
    bool applyPlaybackRate(GstElement *, double) override { return true; }
    bool isAudioFadeSupported() const override { return false; }
    void audioFade(double, uint32_t, firebolt::rialto::EaseType) override {}
    bool processAudioGap(GstElement *, int64_t, uint32_t, int64_t, bool) override { return false; }
    bool switchAudioCodec(const firebolt::rialto::server::AudioCodecSwitchContext &) override { return true; }
    std::vector<std::string> getSupportedProperties(firebolt::rialto::MediaSourceType,
                                                    const std::vector<std::string> &) const override
    {
        return {};
    }
};
} // namespace

#ifndef FIXTURE_OMIT_VERSION
extern "C" uint32_t rialtoPlatformBackendAbiVersion(void)
{
#ifdef FIXTURE_MISMATCH
    return firebolt::rialto::server::kPlatformBackendAbiVersion + 1000;
#else
    return firebolt::rialto::server::kPlatformBackendAbiVersion;
#endif
}
#endif

extern "C" firebolt::rialto::server::IPlatformBackend *
rialtoCreatePlatformBackend(const firebolt::rialto::server::PlatformHostContext *host)
{
    if (!host)
        return nullptr;
    return new (std::nothrow) FixtureBackend();
}

extern "C" void rialtoDestroyPlatformBackend(firebolt::rialto::server::IPlatformBackend *backend)
{
    delete backend;
}
