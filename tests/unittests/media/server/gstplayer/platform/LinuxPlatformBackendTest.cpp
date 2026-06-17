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

    GstRegistry m_reg{};
    GstObject m_feature{};
    GstElement m_sink{};
};

TEST_F(LinuxPlatformBackendTest, PlatformNameIsLinux)
{
    EXPECT_STREQ(m_sut.platformName(), "linux");
}

/**
 * Transitional SoC ladder coverage (relocated from the engine): amlhalasink wins the probe and is
 * configured for the Llama platform.
 */
TEST_F(LinuxPlatformBackendTest, CreateAudioSinkUsesAmlhalasinkWhenPresent)
{
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryGet()).WillOnce(Return(&m_reg));
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryLookupFeature(&m_reg, StrEq("amlhalasink")))
        .WillOnce(Return(GST_PLUGIN_FEATURE(&m_feature)));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("amlhalasink"), StrEq("webaudiosink")))
        .WillOnce(Return(&m_sink));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(G_OBJECT(&m_sink), StrEq("direct-mode")));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(GST_PLUGIN_FEATURE(&m_feature)));

    EXPECT_EQ(m_sut.createAudioSink("webaudiosink"), &m_sink);
}

/**
 * rtkaudiosink wins the probe when amlhalasink is absent and is configured for the XiOne platform.
 */
TEST_F(LinuxPlatformBackendTest, CreateAudioSinkUsesRtkaudiosinkWhenPresent)
{
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryGet()).WillOnce(Return(&m_reg));
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryLookupFeature(&m_reg, StrEq("amlhalasink"))).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryLookupFeature(&m_reg, StrEq("rtkaudiosink")))
        .WillOnce(Return(GST_PLUGIN_FEATURE(&m_feature)));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("rtkaudiosink"), StrEq("webaudiosink")))
        .WillOnce(Return(&m_sink));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(G_OBJECT(&m_sink), StrEq("media-tunnel")));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(G_OBJECT(&m_sink), StrEq("audio-service")));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(GST_PLUGIN_FEATURE(&m_feature)));

    EXPECT_EQ(m_sut.createAudioSink("webaudiosink"), &m_sink);
}

/**
 * With no vendor sink registered, the reference backend falls back to autoaudiosink.
 */
TEST_F(LinuxPlatformBackendTest, CreateAudioSinkFallsBackToAutoaudiosink)
{
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryGet()).WillOnce(Return(&m_reg));
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryLookupFeature(&m_reg, StrEq("amlhalasink"))).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryLookupFeature(&m_reg, StrEq("rtkaudiosink"))).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("autoaudiosink"), StrEq("webaudiosink")))
        .WillOnce(Return(&m_sink));

    EXPECT_EQ(m_sut.createAudioSink("webaudiosink"), &m_sink);
}

/**
 * If the registry is unavailable the backend cannot select a sink and returns nullptr.
 */
TEST_F(LinuxPlatformBackendTest, CreateAudioSinkReturnsNullWhenRegistryUnavailable)
{
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryGet()).WillOnce(Return(nullptr));

    EXPECT_EQ(m_sut.createAudioSink("webaudiosink"), nullptr);
}

/**
 * amlhalasink is selected but its element creation fails; the feature ref is still released and the
 * backend returns nullptr.
 */
TEST_F(LinuxPlatformBackendTest, CreateAudioSinkReturnsNullWhenAmlhalasinkMakeFails)
{
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryGet()).WillOnce(Return(&m_reg));
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryLookupFeature(&m_reg, StrEq("amlhalasink")))
        .WillOnce(Return(GST_PLUGIN_FEATURE(&m_feature)));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("amlhalasink"), StrEq("webaudiosink")))
        .WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(GST_PLUGIN_FEATURE(&m_feature)));

    EXPECT_EQ(m_sut.createAudioSink("webaudiosink"), nullptr);
}

TEST_F(LinuxPlatformBackendTest, CreateVideoSinkUsesAutovideosink)
{
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("autovideosink"), StrEq("videosink")))
        .WillOnce(Return(&m_sink));

    EXPECT_EQ(m_sut.createVideoSink("videosink"), &m_sink);
}
