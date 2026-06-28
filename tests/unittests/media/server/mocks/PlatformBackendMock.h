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

#ifndef FIREBOLT_RIALTO_SERVER_PLATFORM_BACKEND_MOCK_H_
#define FIREBOLT_RIALTO_SERVER_PLATFORM_BACKEND_MOCK_H_

#include "IPlatformBackend.h"
#include <gmock/gmock.h>
#include <string>

namespace firebolt::rialto::server
{
class PlatformBackendMock : public IPlatformBackend
{
public:
    MOCK_METHOD(const char *, platformName, (), (const, override));
    MOCK_METHOD(GstElement *, createAudioSink, (const std::string &name), (override));
    MOCK_METHOD(PlatformMediaPath, buildAudioPath, (GstElement * pipeline, GstElement *source), (override));
    MOCK_METHOD(PlatformMediaPath, buildVideoPath, (GstElement * pipeline, GstElement *source, uint32_t videoId),
                (override));
    MOCK_METHOD(bool, isVideoMaster, (), (const, override));
    MOCK_METHOD(bool, applyPlaybackRate, (GstElement * pipeline, double rate), (override));
    MOCK_METHOD(bool, isAudioFadeSupported, (), (const, override));
    MOCK_METHOD(void, audioFade, (double target, uint32_t duration, firebolt::rialto::EaseType easeType), (override));
    MOCK_METHOD(bool, processAudioGap,
                (GstElement * pipeline, int64_t position, uint32_t duration, int64_t discontinuityGap, bool audioAac),
                (override));
    MOCK_METHOD(bool, switchAudioCodec, (const AudioCodecSwitchContext &ctx), (override));
    MOCK_METHOD(bool, shouldSkipCapabilityProbe, (const std::string &elementName), (const, override));
};
} // namespace firebolt::rialto::server

#endif // FIREBOLT_RIALTO_SERVER_PLATFORM_BACKEND_MOCK_H_
