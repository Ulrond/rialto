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
#include "RdkGstreamerUtilsWrapperMock.h"
#include <gst/gst.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace firebolt::rialto::server;
using namespace firebolt::rialto::wrappers;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrEq;
using ::testing::StrictMock;

class LinuxPlatformBackendTest : public ::testing::Test
{
protected:
    std::shared_ptr<StrictMock<GstWrapperMock>> m_gstWrapperMock{std::make_shared<StrictMock<GstWrapperMock>>()};
    std::shared_ptr<StrictMock<GlibWrapperMock>> m_glibWrapperMock{std::make_shared<StrictMock<GlibWrapperMock>>()};
    std::shared_ptr<StrictMock<RdkGstreamerUtilsWrapperMock>> m_rdkGstreamerUtilsWrapperMock{
        std::make_shared<StrictMock<RdkGstreamerUtilsWrapperMock>>()};
    LinuxPlatformBackend m_sut{PlatformHostContext{m_gstWrapperMock, m_glibWrapperMock, m_rdkGstreamerUtilsWrapperMock}};

    GstElement m_sink{};
    GstElement m_pipeline{};
    GstStructure m_structure{};
    GstEvent m_event{};
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
 * The reference backend owns the x86 audio topology (it is "just another SoC"): buildAudioPath
 * constructs decodebin -> audioconvert -> audioresample -> autoaudiosink, adds them to the pipeline,
 * statically links the appsrc head and the convert->resample->sink tail, and returns the handles the
 * engine needs — sink (autoaudiosink), decodebin, and decoderLinkTarget (audioconvert, the element the
 * dynamic decoder src pad links into). The engine creates and links no media element of its own.
 */
TEST_F(LinuxPlatformBackendTest, BuildAudioPathConstructsAndLinksReferenceTopology)
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

    const PlatformMediaPath path{m_sut.buildAudioPath(&m_pipeline, &source)};
    EXPECT_EQ(path.sink, &audioSink);
    EXPECT_EQ(path.decodebin, &decodebin);
    EXPECT_EQ(path.decoderLinkTarget, &audioConvert);
}

/**
 * The reference backend owns the x86 video topology: buildVideoPath constructs decodebin ->
 * autovideosink (plane-agnostic, videoId accepted but ignored), adds them, statically links the appsrc
 * head, and returns sink == decoderLinkTarget == autovideosink (the decoder src pad links straight to
 * the sink — no convert tail) plus the decodebin.
 */
TEST_F(LinuxPlatformBackendTest, BuildVideoPathConstructsAndLinksReferenceTopology)
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

    const PlatformMediaPath path{m_sut.buildVideoPath(&m_pipeline, &source, 1)};
    EXPECT_EQ(path.sink, &videoSink);
    EXPECT_EQ(path.decodebin, &decodebin);
    EXPECT_EQ(path.decoderLinkTarget, &videoSink);
}

/**
 * The reference backend has no amlhalasink-style audio-master sink, so the Linux platform is
 * video-master. The audio-master vendor cases live in their per-SoC .so.
 */
TEST_F(LinuxPlatformBackendTest, IsVideoMasterReturnsTrue)
{
    EXPECT_TRUE(m_sut.isVideoMaster());
}

/**
 * The reference backend applies a rate change as a custom-instant-rate-change event sent
 * downstream on the pipeline; the sink-pad new-segment variant lives in a per-SoC .so.
 */
TEST_F(LinuxPlatformBackendTest, ApplyPlaybackRateSendsInstantRateEventOnPipeline)
{
    EXPECT_CALL(*m_gstWrapperMock,
                gstStructureNewDoubleStub(StrEq("custom-instant-rate-change"), StrEq("rate"), G_TYPE_DOUBLE, 1.25))
        .WillOnce(Return(&m_structure));
    EXPECT_CALL(*m_gstWrapperMock, gstEventNewCustom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB, &m_structure))
        .WillOnce(Return(&m_event));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSendEvent(&m_pipeline, &m_event)).WillOnce(Return(TRUE));

    EXPECT_TRUE(m_sut.applyPlaybackRate(&m_pipeline, 1.25));
}

/**
 * The reference backend has no SoC audio path that eases volume, so audio fade is not supported
 * here (the engine uses the generic sink "audio-fade" property instead). SoC fade lives in a per-SoC .so.
 */
TEST_F(LinuxPlatformBackendTest, IsAudioFadeSupportedReturnsFalse)
{
    EXPECT_FALSE(m_sut.isAudioFadeSupported());
}

/**
 * The reference backend performs no SoC audio fade: audioFade is a callable no-op. SoC fade lives in
 * a per-SoC .so. The StrictMock guarantees no wrapper calls occur.
 */
TEST_F(LinuxPlatformBackendTest, AudioFadeIsNoOp)
{
    m_sut.audioFade(0.5, 1000, firebolt::rialto::EaseType::EASE_LINEAR);
}

/**
 * The reference backend handles no SoC audio gap: processAudioGap returns false and performs no
 * wrapper calls. SoC audio-gap handling lives in a per-SoC .so.
 */
TEST_F(LinuxPlatformBackendTest, ProcessAudioGapReturnsFalse)
{
    EXPECT_FALSE(m_sut.processAudioGap(&m_pipeline, 123, 456, 789, true));
}

/**
 * The reference backend is the capability authority (ABI v8): it discovers its supported properties by
 * introspecting the installed GStreamer elements it can use. It names no SoC and never asks the core to
 * instantiate a vendor sink, so there is no capability-probe skip hook. Here every requested property is
 * exposed by the (single) element, so all are reported.
 */
TEST_F(LinuxPlatformBackendTest, GetSupportedPropertiesReportsPropertiesFoundByIntrospection)
{
    // A real factory is needed because GST_ELEMENT_FACTORY casts the list data with a runtime type check.
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
    EXPECT_EQ(m_sut.getSupportedProperties(firebolt::rialto::MediaSourceType::VIDEO, kParamNames), kParamNames);

    gst_plugin_feature_list_free(factories);
}

/**
 * With no installed element exposing the requested properties (empty factory list) the reference reports
 * nothing. audio-fade in particular is not reported: the reference has no platform audio path that eases
 * volume (isAudioFadeSupported() is false), so the StrictMock proves no further wrapper calls occur.
 */
TEST_F(LinuxPlatformBackendTest, GetSupportedPropertiesReturnsEmptyWhenNoElementsAndNoPlatformFade)
{
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(_, GST_RANK_NONE)).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstPluginFeatureListFree(nullptr)).Times(1);

    const std::vector<std::string> kParamNames{"test-name-123", "audio-fade"};
    EXPECT_TRUE(m_sut.getSupportedProperties(firebolt::rialto::MediaSourceType::AUDIO, kParamNames).empty());
}

namespace
{
firebolt::rialto::server::AudioCodecSwitchContext makeAudioCodecSwitchContext(GstElement *appSrc, GstElement *playsinkBin,
                                                                             bool *isAudioAacState, const char *codecParam)
{
    firebolt::rialto::server::AudioCodecSwitchContext ctx;
    ctx.audioAppSrc = appSrc;
    ctx.audioPlaysinkBin = playsinkBin;
    ctx.isAudioAacState = isAudioAacState;
    ctx.svpEnabled = true;
    ctx.codecParam = codecParam;
    ctx.numberOfChannels = 2;
    ctx.samplesPerSecond = 48000;
    return ctx;
}
} // namespace

/**
 * Transitional amlhalasink fork (moved verbatim out of the engine core, ABI v5): when the playsink-bin
 * name begins with "amlhalasink" and the codec does not change, the backend runs the in-Rialto fork —
 * configAudioCap + flush events + gstAppSrcSetCaps — and names the SoC only here, behind the seam.
 */
TEST_F(LinuxPlatformBackendTest, SwitchAudioCodecAmlhalasinkNoCodecSwitch)
{
    GstAppSrc audioSrc{};
    GstCaps configCaps{};
    GstEvent flushStartEvent{};
    GstEvent flushStopEvent{};
    gchar configCapsStr[] = "audio/mpeg, mpegversion=4, enable-svp=(string)true";
    GstElement *playsinkBin = gst_element_factory_make("fakesink", "amlhalasink0");
    bool isAudioAac{true}; // current codec is AAC, incoming is AAC -> no codec switch

    EXPECT_CALL(*m_glibWrapperMock, gStrHasPrefix(StrEq("amlhalasink0"), StrEq("amlhalasink"))).WillOnce(Return(TRUE));
    // configAudioCap: creates AAC caps
    EXPECT_CALL(*m_glibWrapperMock, gStrdupPrintfStub(_)).WillOnce(Return(configCapsStr));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsFromString(configCapsStr)).WillOnce(Return(&configCaps));
    EXPECT_CALL(*m_glibWrapperMock, gFree(configCapsStr));
    // flush events
    EXPECT_CALL(*m_gstWrapperMock, gstEventNewFlushStart()).WillOnce(Return(&flushStartEvent));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSendEvent(GST_ELEMENT(&audioSrc), &flushStartEvent)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstEventNewFlushStop(TRUE)).WillOnce(Return(&flushStopEvent));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSendEvent(GST_ELEMENT(&audioSrc), &flushStopEvent)).WillOnce(Return(TRUE));
    // no codec switch - just set new caps
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&audioSrc), &configCaps));
    // the backend owns the fork's appsrc caps and releases them
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&configCaps));

    auto ctx = makeAudioCodecSwitchContext(GST_ELEMENT(&audioSrc), playsinkBin, &isAudioAac, "mp4a.40.2");
    EXPECT_TRUE(m_sut.switchAudioCodec(ctx));
    EXPECT_TRUE(isAudioAac);
    gst_object_unref(playsinkBin);
}

/**
 * Transitional amlhalasink fork (ABI v5): an EAC3->AAC switch with no existing decoder runs the
 * firstTimeSwitchFromAC3toAAC path (halt / build new aacparse+avdec_aac+queue / resume). This is the
 * same gst sequence the engine core used to assert, now exercised through the backend.
 */
TEST_F(LinuxPlatformBackendTest, SwitchAudioCodecAmlhalasinkFirstTimeCodecSwitch)
{
    GstAppSrc audioSrc{};
    GstCaps configCaps{};
    GstEvent flushStartEvent{};
    GstEvent flushStopEvent{};
    GstPad typefindSrcPad{};
    GstPad typefindSrcPeerPad{};
    GstElement newAudioDecoder{};
    GstElement newAudioParse{};
    GstElement newQueue{};
    GstPad newAudioDecoderSrcPad{};
    GstElement typefind{};
    GstElement decodeBin{};
    gchar configCapsStr[] = "audio/mpeg, mpegversion=4, enable-svp=(string)true";
    GstElement *playsinkBin = gst_element_factory_make("fakesink", "amlhalasink1");
    bool isAudioAac{false}; // current codec is EAC3, incoming AAC -> codec switch

    EXPECT_CALL(*m_glibWrapperMock, gStrHasPrefix(StrEq("amlhalasink1"), StrEq("amlhalasink"))).WillOnce(Return(TRUE));
    // configAudioCap: creates AAC caps
    EXPECT_CALL(*m_glibWrapperMock, gStrdupPrintfStub(_)).WillOnce(Return(configCapsStr));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsFromString(configCapsStr)).WillOnce(Return(&configCaps));
    EXPECT_CALL(*m_glibWrapperMock, gFree(configCapsStr));
    // flush events
    EXPECT_CALL(*m_gstWrapperMock, gstEventNewFlushStart()).WillOnce(Return(&flushStartEvent));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSendEvent(GST_ELEMENT(&audioSrc), &flushStartEvent)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstEventNewFlushStop(TRUE)).WillOnce(Return(&flushStopEvent));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSendEvent(GST_ELEMENT(&audioSrc), &flushStopEvent)).WillOnce(Return(TRUE));
    // haltAudioPlayback + resumeAudioPlayback: playsinkBin and decodeBin each touched. The name-checked
    // playsink-bin handle is the same one the halt/resume operate on.
    EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(playsinkBin, GST_STATE_READY))
        .WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(playsinkBin, _, _, GST_CLOCK_TIME_NONE)).Times(2);
    EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(&decodeBin, GST_STATE_PAUSED))
        .WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(&decodeBin, _, _, GST_CLOCK_TIME_NONE)).Times(2);
    // applyAudioCodecSwitch -> firstTimeSwitchFromAC3toAAC
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetStaticPad(&typefind, StrEq("src"))).WillOnce(Return(&typefindSrcPad));
    EXPECT_CALL(*m_gstWrapperMock, gstPadGetPeer(&typefindSrcPad)).WillOnce(Return(&typefindSrcPeerPad));
    EXPECT_CALL(*m_gstWrapperMock, gstPadUnlink(&typefindSrcPad, &typefindSrcPeerPad)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("aacparse"), StrEq("aacparse")))
        .WillOnce(Return(&newAudioParse));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("avdec_aac"), StrEq("avdec_aac")))
        .WillOnce(Return(&newAudioDecoder));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("queue"), StrEq("aqueue"))).WillOnce(Return(&newQueue));
    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(_, &newAudioDecoder)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(_, &newAudioParse)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(_, &newQueue)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetStaticPad(&newAudioDecoder, StrEq("src")))
        .WillOnce(Return(&newAudioDecoderSrcPad));
    EXPECT_CALL(*m_gstWrapperMock, gstPadLink(&newAudioDecoderSrcPad, &typefindSrcPeerPad))
        .WillOnce(Return(GST_PAD_LINK_OK));
    EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&newAudioParse, &newQueue)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&newQueue, &newAudioDecoder)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(&typefind, GST_STATE_READY))
        .WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(&typefind, StrEq("force-caps")));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSyncStateWithParent(&typefind)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(&typefind, _, _, GST_CLOCK_TIME_NONE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSyncStateWithParent(&newAudioDecoder)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(&newAudioDecoder, _, _, GST_CLOCK_TIME_NONE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSyncStateWithParent(&newQueue)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(&newQueue, _, _, GST_CLOCK_TIME_NONE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSyncStateWithParent(&newAudioParse)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(&newAudioParse, _, _, GST_CLOCK_TIME_NONE));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&typefindSrcPad));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&typefindSrcPeerPad));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&newAudioDecoderSrcPad));
    // gstAppSrcSetCaps after codec switch
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&audioSrc), &configCaps));
    // resumeAudioPlayback
    EXPECT_CALL(*m_gstWrapperMock, gstElementSyncStateWithParent(playsinkBin)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementSyncStateWithParent(&decodeBin)).WillOnce(Return(TRUE));
    // the backend owns the fork's appsrc caps and releases them
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&configCaps));

    auto ctx = makeAudioCodecSwitchContext(GST_ELEMENT(&audioSrc), playsinkBin, &isAudioAac, "mp4a.40.2");
    ctx.audioDecodeBin = &decodeBin;
    ctx.audioTypefind = &typefind;
    ctx.audioDecoder = nullptr; // first-time switch: no existing decoder

    EXPECT_TRUE(m_sut.switchAudioCodec(ctx));
    EXPECT_TRUE(isAudioAac);
    gst_object_unref(playsinkBin);
}

/**
 * Generic rdk-gstreamer-utils path (ABI v5): when the playsink-bin name is not amlhalasink, the backend
 * delegates to rdk_gstreamer_utils::performAudioTrackCodecChannelSwitch through the injected wrapper.
 */
TEST_F(LinuxPlatformBackendTest, SwitchAudioCodecNonAmlhalasinkUsesRdkGstreamerUtils)
{
    GstAppSrc audioSrc{};
    GstCaps appsrcCaps{};
    GstElement *playsinkBin = gst_element_factory_make("fakesink", "autoaudiosink0");
    bool isAudioAac{true};

    EXPECT_CALL(*m_glibWrapperMock, gStrHasPrefix(StrEq("autoaudiosink0"), StrEq("amlhalasink"))).WillOnce(Return(FALSE));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&appsrcCaps));
    EXPECT_CALL(*m_rdkGstreamerUtilsWrapperMock,
                performAudioTrackCodecChannelSwitch(_, _, _, _, _, _, _, _, _, _, true, GST_ELEMENT(&audioSrc), _))
        .WillOnce(DoAll(SetArgPointee<12>(true), Return(true)));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&appsrcCaps));

    auto ctx = makeAudioCodecSwitchContext(GST_ELEMENT(&audioSrc), playsinkBin, &isAudioAac, "mp4a.40.2");
    EXPECT_TRUE(m_sut.switchAudioCodec(ctx));
    gst_object_unref(playsinkBin);
}

/**
 * Generic path with no rdk-gstreamer-utils wrapper available: the backend logs and returns false rather
 * than dereferencing a null wrapper.
 */
TEST_F(LinuxPlatformBackendTest, SwitchAudioCodecNonAmlhalasinkWithoutRdkWrapperReturnsFalse)
{
    LinuxPlatformBackend sutNoRdk{PlatformHostContext{m_gstWrapperMock, m_glibWrapperMock}};
    GstAppSrc audioSrc{};
    GstElement *playsinkBin = gst_element_factory_make("fakesink", "autoaudiosink1");
    bool isAudioAac{true};

    EXPECT_CALL(*m_glibWrapperMock, gStrHasPrefix(StrEq("autoaudiosink1"), StrEq("amlhalasink"))).WillOnce(Return(FALSE));

    auto ctx = makeAudioCodecSwitchContext(GST_ELEMENT(&audioSrc), playsinkBin, &isAudioAac, "mp4a.40.2");
    EXPECT_FALSE(sutNoRdk.switchAudioCodec(ctx));
    gst_object_unref(playsinkBin);
}
