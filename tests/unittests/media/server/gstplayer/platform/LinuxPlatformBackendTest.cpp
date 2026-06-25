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

#include "GlibWrapperMock.h"
#include "GstWrapperMock.h"
#include "LinuxPlatformBackend.h"
#include <gtest/gtest.h>
#include <memory>

using namespace firebolt::rialto::server;
using namespace firebolt::rialto::wrappers;

using ::testing::Return;
using ::testing::StrEq;
using ::testing::StrictMock;

class LinuxPlatformBackendTest : public ::testing::Test
{
protected:
    std::shared_ptr<StrictMock<GstWrapperMock>> m_gstWrapperMock{std::make_shared<StrictMock<GstWrapperMock>>()};
    std::shared_ptr<StrictMock<GlibWrapperMock>> m_glibWrapperMock{std::make_shared<StrictMock<GlibWrapperMock>>()};
    LinuxPlatformBackend m_sut{PlatformHostContext{m_gstWrapperMock, m_glibWrapperMock}};

    GstElement m_sink{};
};

TEST_F(LinuxPlatformBackendTest, PlatformNameIsLinux)
{
    EXPECT_STREQ(m_sut.platformName(), "linux");
}

/**
 * The reference backend names no SoC: it returns autoaudiosink and performs no registry
 * probing for vendor names. The StrictMock guarantees no gstRegistryGet/lookup occurs.
 */
TEST_F(LinuxPlatformBackendTest, CreateAudioSinkReturnsAutoaudiosinkWithNoSocProbing)
{
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("autoaudiosink"), StrEq("webaudiosink")))
        .WillOnce(Return(&m_sink));

    EXPECT_EQ(m_sut.createAudioSink("webaudiosink"), &m_sink);
}

/**
 * The reference backend is plane-agnostic: it returns autovideosink and accepts (but
 * ignores) the videoId, performing no SoC-specific plane binding.
 */
TEST_F(LinuxPlatformBackendTest, CreateVideoSinkReturnsAutovideosink)
{
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("autovideosink"), StrEq("videosink")))
        .WillOnce(Return(&m_sink));

    EXPECT_EQ(m_sut.createVideoSink("videosink", 1), &m_sink);
}
