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
    MOCK_METHOD(GstElement *, createVideoSink, (const std::string &name, uint32_t videoId), (override));
    MOCK_METHOD(bool, isVideoMaster, (), (const, override));
};
} // namespace firebolt::rialto::server

#endif // FIREBOLT_RIALTO_SERVER_PLATFORM_BACKEND_MOCK_H_
