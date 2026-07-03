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
 * Parity oracle for the extracted GenericGstBackend (the per-SoC shim's SoC-free core). With the linux
 * SocProfile it must behave identically to the reference LinuxPlatformBackend — the tests below mirror
 * LinuxPlatformBackendTest one-for-one on the shared behaviour, over the same mocked wrappers.
 *
 * Two intentional differences from LinuxPlatformBackendTest document the Slice B simplification: the
 * transitional in-Rialto amlhalasink fork is gone, so switchAudioCodec is now a single marshal-and-delegate
 * path with no "amlhalasink" name check (the device's vendor lib does any SoC-specific work). The amlogic
 * SocProfile tests then prove the per-SoC deltas (audio-master, fused topology, delegated audio ops).
 */

#include "GenericGstBackend.h"
#include "GlibWrapperMock.h"
#include "GstWrapperMock.h"
#include "RdkGstreamerUtilsWrapperMock.h"
#include "SocProfile.h"
#include <gst/gst.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace firebolt::rialto::server;
using namespace firebolt::rialto::server::backends;
using namespace firebolt::rialto::wrappers;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrEq;
using ::testing::StrictMock;

namespace
{
SocProfile makeLinuxProfile()
{
    SocProfile p;
    p.name = "linux";
    p.audioSinkFactory = "autoaudiosink";
    p.videoSinkFactory = "autovideosink";
    p.audioTopology = AudioTopology::SplitDecode;
    p.rateStrategy = RateStrategy::InstantRateEvent;
    p.videoMaster = true;
    p.socAudioPath = false;
    p.bindsVideoPlane = false;
    return p;
}

SocProfile makeAmlogicProfile()
{
    SocProfile p;
    p.name = "amlogic";
    p.audioSinkFactory = "amlhalasink";
    p.videoSinkFactory = "westerossink";
    p.audioTopology = AudioTopology::FusedSink;
    p.rateStrategy = RateStrategy::InstantRateEvent;
    p.videoMaster = false;
    p.socAudioPath = true;
    p.bindsVideoPlane = true;
    return p;
}

AudioCodecSwitchContext makeAudioCodecSwitchContext(GstElement *appSrc, bool *isAudioAacState, const char *codecParam)
{
    AudioCodecSwitchContext ctx;
    ctx.audioAppSrc = appSrc;
    ctx.isAudioAacState = isAudioAacState;
    ctx.svpEnabled = true;
    ctx.codecParam = codecParam;
    ctx.numberOfChannels = 2;
    ctx.samplesPerSecond = 48000;
    return ctx;
}
} // namespace

class GenericGstBackendTest : public ::testing::Test
{
protected:
    std::shared_ptr<StrictMock<GstWrapperMock>> m_gstWrapperMock{std::make_shared<StrictMock<GstWrapperMock>>()};
    std::shared_ptr<StrictMock<GlibWrapperMock>> m_glibWrapperMock{std::make_shared<StrictMock<GlibWrapperMock>>()};
    std::shared_ptr<StrictMock<RdkGstreamerUtilsWrapperMock>> m_rdkGstreamerUtilsWrapperMock{
        std::make_shared<StrictMock<RdkGstreamerUtilsWrapperMock>>()};

    PlatformHostContext host() const
    {
        return PlatformHostContext{m_gstWrapperMock, m_glibWrapperMock, m_rdkGstreamerUtilsWrapperMock};
    }
    GenericGstBackend linuxSut() { return GenericGstBackend{makeLinuxProfile(), host()}; }
    GenericGstBackend amlogicSut() { return GenericGstBackend{makeAmlogicProfile(), host()}; }

    GstElement m_pipeline{};
    GstStructure m_structure{};
    GstEvent m_event{};
};

// ---------------------------------------------------------------------------
// Parity with LinuxPlatformBackend: identical behaviour on the linux profile.
// ---------------------------------------------------------------------------

TEST_F(GenericGstBackendTest, PlatformNameComesFromProfile)
{
    EXPECT_STREQ(linuxSut().platformName(), "linux");
}

TEST_F(GenericGstBackendTest, CreateAudioSinkReturnsProfileSinkWithNoSocProbing)
{
    GstElement sink{};
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("autoaudiosink"), StrEq("webaudiosink")))
        .WillOnce(Return(&sink));

    EXPECT_EQ(linuxSut().createAudioSink("webaudiosink"), &sink);
}

TEST_F(GenericGstBackendTest, BuildAudioPathSplitDecodeConstructsAndLinksReferenceTopology)
{
    GstElement source{};
    GstElement decodebin{};
    GstElement audioConvert{};
    GstElement audioResample{};
    GstElement audioSink{};

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("decodebin"), StrEq("auddecodebin")))
        .WillOnce(Return(&decodebin));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("audioconvert"), StrEq("audconvert")))
        .WillOnce(Return(&audioConvert));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("audioresample"), StrEq("audresample")))
        .WillOnce(Return(&audioResample));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("autoaudiosink"), StrEq("audiosink")))
        .WillOnce(Return(&audioSink));

    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &decodebin)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &audioConvert)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &audioResample)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &audioSink)).WillOnce(Return(TRUE));

    EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&source, &decodebin)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&audioConvert, &audioResample)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&audioResample, &audioSink)).WillOnce(Return(TRUE));

    const PlatformMediaPath path{linuxSut().buildAudioPath(&m_pipeline, &source)};
    EXPECT_EQ(path.sink, &audioSink);
    EXPECT_EQ(path.decodebin, &decodebin);
    EXPECT_EQ(path.decoderLinkTarget, &audioConvert);
}

TEST_F(GenericGstBackendTest, BuildVideoPathConstructsAndLinksReferenceTopology)
{
    GstElement source{};
    GstElement decodebin{};
    GstElement videoSink{};

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("decodebin"), StrEq("viddecodebin")))
        .WillOnce(Return(&decodebin));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("autovideosink"), StrEq("videosink")))
        .WillOnce(Return(&videoSink));

    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &decodebin)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &videoSink)).WillOnce(Return(TRUE));

    EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&source, &decodebin)).WillOnce(Return(TRUE));

    const PlatformMediaPath path{linuxSut().buildVideoPath(&m_pipeline, &source, 1)};
    EXPECT_EQ(path.sink, &videoSink);
    EXPECT_EQ(path.decodebin, &decodebin);
    EXPECT_EQ(path.decoderLinkTarget, &videoSink);
}

TEST_F(GenericGstBackendTest, IsVideoMasterReturnsProfileValue)
{
    EXPECT_TRUE(linuxSut().isVideoMaster());
}

TEST_F(GenericGstBackendTest, ApplyPlaybackRateSendsInstantRateEventOnPipeline)
{
    EXPECT_CALL(*m_gstWrapperMock,
                gstStructureNewDoubleStub(StrEq("custom-instant-rate-change"), StrEq("rate"), G_TYPE_DOUBLE, 1.25))
        .WillOnce(Return(&m_structure));
    EXPECT_CALL(*m_gstWrapperMock, gstEventNewCustom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB, &m_structure))
        .WillOnce(Return(&m_event));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSendEvent(&m_pipeline, &m_event)).WillOnce(Return(TRUE));

    EXPECT_TRUE(linuxSut().applyPlaybackRate(&m_pipeline, 1.25));
}

TEST_F(GenericGstBackendTest, IsAudioFadeSupportedFalseWithNoSocAudioPath)
{
    // socAudioPath == false -> answered locally, StrictMock proves the wrapper is not consulted.
    EXPECT_FALSE(linuxSut().isAudioFadeSupported());
}

TEST_F(GenericGstBackendTest, AudioFadeIsNoOpWithNoSocAudioPath)
{
    linuxSut().audioFade(0.5, 1000, firebolt::rialto::EaseType::EASE_LINEAR);
}

TEST_F(GenericGstBackendTest, ProcessAudioGapReturnsFalseWithNoSocAudioPath)
{
    EXPECT_FALSE(linuxSut().processAudioGap(&m_pipeline, 123, 456, 789, true));
}

TEST_F(GenericGstBackendTest, GetSupportedPropertiesReportsPropertiesFoundByIntrospection)
{
    GstElementFactory *factory = gst_element_factory_find("fakesrc");
    ASSERT_TRUE(factory);
    GList *factories = g_list_append(nullptr, factory);

    GstElement element{};
    GParamSpec param0{};
    GParamSpec param1{};
    param0.name = "test-name-123";
    param1.name = "test2";
    GParamSpec *params[] = {&param0, &param1};
    const guint kNumParams{2};

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(_, GST_RANK_NONE)).WillOnce(Return(factories));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryCreate(factory, nullptr)).WillOnce(Return(&element));
    EXPECT_CALL(*m_glibWrapperMock, gObjectClassListProperties(_, _))
        .WillOnce(DoAll(SetArgPointee<1>(kNumParams), Return(params)));
    EXPECT_CALL(*m_glibWrapperMock, gFree(params)).Times(1);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&element)).Times(1);
    EXPECT_CALL(*m_gstWrapperMock, gstPluginFeatureListFree(factories)).Times(1);

    const std::vector<std::string> kParamNames{"test-name-123", "test2"};
    EXPECT_EQ(linuxSut().getSupportedProperties(firebolt::rialto::MediaSourceType::VIDEO, kParamNames), kParamNames);

    gst_plugin_feature_list_free(factories);
}

TEST_F(GenericGstBackendTest, GetSupportedPropertiesReturnsEmptyWhenNoElementsAndNoPlatformFade)
{
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(_, GST_RANK_NONE)).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstPluginFeatureListFree(nullptr)).Times(1);

    const std::vector<std::string> kParamNames{"test-name-123", "audio-fade"};
    EXPECT_TRUE(linuxSut().getSupportedProperties(firebolt::rialto::MediaSourceType::AUDIO, kParamNames).empty());
}

// ---------------------------------------------------------------------------
// Slice B simplification: switchAudioCodec is one marshal-and-delegate path, no
// "amlhalasink" name check. (Replaces the two amlhalasink-fork tests.)
// ---------------------------------------------------------------------------

TEST_F(GenericGstBackendTest, SwitchAudioCodecMarshalsAndDelegatesToRdkGstreamerUtils)
{
    GstAppSrc audioSrc{};
    GstCaps appsrcCaps{};
    bool isAudioAac{true};

    // No gStrHasPrefix name check: the shim marshals and delegates unconditionally.
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&appsrcCaps));
    EXPECT_CALL(*m_rdkGstreamerUtilsWrapperMock,
                performAudioTrackCodecChannelSwitch(_, _, _, _, _, _, _, _, _, _, true, GST_ELEMENT(&audioSrc), _))
        .WillOnce(DoAll(SetArgPointee<12>(true), Return(true)));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&appsrcCaps));

    auto ctx = makeAudioCodecSwitchContext(GST_ELEMENT(&audioSrc), &isAudioAac, "mp4a.40.2");
    EXPECT_TRUE(linuxSut().switchAudioCodec(ctx));
}

TEST_F(GenericGstBackendTest, SwitchAudioCodecWithoutRdkWrapperReturnsFalse)
{
    GenericGstBackend sutNoRdk{makeLinuxProfile(), PlatformHostContext{m_gstWrapperMock, m_glibWrapperMock}};
    GstAppSrc audioSrc{};
    bool isAudioAac{true};

    // Null wrapper: the shim returns false before any gst work (StrictMock proves no calls occur).
    auto ctx = makeAudioCodecSwitchContext(GST_ELEMENT(&audioSrc), &isAudioAac, "mp4a.40.2");
    EXPECT_FALSE(sutNoRdk.switchAudioCodec(ctx));
}

// ---------------------------------------------------------------------------
// Per-SoC deltas via the amlogic profile: audio-master, fused topology, delegated audio ops.
// ---------------------------------------------------------------------------

TEST_F(GenericGstBackendTest, AmlogicIsAudioMaster)
{
    EXPECT_FALSE(amlogicSut().isVideoMaster());
}

TEST_F(GenericGstBackendTest, AmlogicBuildAudioPathIsFusedSink)
{
    GstElement source{};
    GstElement audioSink{};

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("amlhalasink"), StrEq("audiosink")))
        .WillOnce(Return(&audioSink));
    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &audioSink)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&source, &audioSink)).WillOnce(Return(TRUE));

    const PlatformMediaPath path{amlogicSut().buildAudioPath(&m_pipeline, &source)};
    EXPECT_EQ(path.sink, &audioSink);
    EXPECT_EQ(path.decodebin, nullptr);        // fused: the engine wires no decoder
    EXPECT_EQ(path.decoderLinkTarget, nullptr);
}

TEST_F(GenericGstBackendTest, AmlogicIsAudioFadeSupportedDelegatesToWrapper)
{
    EXPECT_CALL(*m_rdkGstreamerUtilsWrapperMock, isSocAudioFadeSupported()).WillOnce(Return(true));
    EXPECT_TRUE(amlogicSut().isAudioFadeSupported());
}

TEST_F(GenericGstBackendTest, AmlogicAudioFadeDelegatesToWrapper)
{
    EXPECT_CALL(*m_rdkGstreamerUtilsWrapperMock, doAudioEasingonSoc(0.5, 1000, rgu_Ease::EaseLinear));
    amlogicSut().audioFade(0.5, 1000, firebolt::rialto::EaseType::EASE_LINEAR);
}

TEST_F(GenericGstBackendTest, AmlogicProcessAudioGapDelegatesToWrapper)
{
    EXPECT_CALL(*m_rdkGstreamerUtilsWrapperMock, processAudioGap(&m_pipeline, 123, 456, 789, true));
    EXPECT_TRUE(amlogicSut().processAudioGap(&m_pipeline, 123, 456, 789, true));
}
