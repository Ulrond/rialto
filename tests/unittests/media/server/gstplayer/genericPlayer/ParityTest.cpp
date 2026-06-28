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
 * Explicit-construction EXECUTION suite.
 *
 * These cases assert the observable behaviour the MSE generic player exhibits when it builds its
 * pipeline explicitly (appsrc -> decodebin -> ... -> backend sink). They are EXECUTION-level: the
 * player is built with the real GenericPlayerTaskFactory and a synchronous worker (tasks run inline
 * on enqueue), so the public API drives real tasks through the real player against the mocked
 * GStreamer wrappers, and the SoC sinks come from the PlatformBackend.
 */

#include "GstGenericPlayerTestCommon.h"
#include "GstTextTrackSinkFactoryMock.h"
#include "tasks/generic/AttachSource.h"
#include "tasks/generic/Eos.h"
#include "tasks/generic/Pause.h"
#include "tasks/generic/Play.h"
#include <gtest/gtest.h>
#include <memory>
#include <utility>

using ::testing::_;
using ::testing::ByMove;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::StrEq;

class GstGenericPlayerExplicitConstructionTest : public GstGenericPlayerTestCommon
{
protected:
    std::unique_ptr<IGstGenericPlayer> m_sut;
    VideoRequirements m_videoReq{kMinPrimaryVideoWidth, kMinPrimaryVideoHeight};
    bool m_isLive{false};

    std::shared_ptr<StrictMock<GstTextTrackSinkFactoryMock>> m_textTrackSinkFactoryMock{
        std::make_shared<StrictMock<GstTextTrackSinkFactoryMock>>()};

    // Dummy elements/caps the real tasks operate on through the mocked wrappers.
    GstElement m_appSrc{};
    GstElement m_decodebin{};
    GstElement m_audioConvert{};
    GstElement m_audioResample{};
    GstElement m_audioSink{};
    GstCaps m_audioCaps{};
    gchar m_capsStr{};
    GstElement m_videoAppSrc{};
    GstElement m_videoDecodebin{};
    GstElement m_videoSink{};
    GstCaps m_videoCaps{};
    GstElement m_subtitleAppSrc{};
    GstElement m_textTrackSink{};
    GstCaps m_subtitleCaps{};
    guint m_signalIds{};

    // The explicit chain builders scan the backend sink for underflow / first-frame telemetry signals;
    // the reference autoaudiosink/autovideosink expose none, so nothing is connected.
    void expectNoSinkSignals(GstElement *sink, bool isVideo)
    {
        const int kScans = isVideo ? 2 : 1; // underflow always; first-video-frame for video too
        EXPECT_CALL(*m_glibWrapperMock, gObjectType(sink)).Times(kScans).WillRepeatedly(Return(G_TYPE_PARAM));
        EXPECT_CALL(*m_glibWrapperMock, gSignalListIds(_, _))
            .Times(kScans)
            .WillRepeatedly(Invoke(
                [this](GType, guint *nIds)
                {
                    *nIds = 0;
                    return &m_signalIds;
                }));
        EXPECT_CALL(*m_glibWrapperMock, gFree(&m_signalIds)).Times(kScans);
    }

    // Builds the player. Construction and teardown keep the mocked task factory (so the shared helpers
    // apply unchanged); the behavioural cases below make the task factory return a REAL task for the one
    // call under test, which the worker mock then executes synchronously on enqueue. PlatformBackendMock
    // is injected so the explicit chain can source its sinks.
    void arrangeAndConstruct()
    {
        gstPlayerWillBeCreated();
        m_sut = std::make_unique<GstGenericPlayer>(&m_gstPlayerClient, m_decryptionServiceMock, MediaType::MSE,
                                                   m_videoReq, m_isLive, m_gstWrapperMock, m_glibWrapperMock,
                                                   m_gstInitialiserMock,
                                                   std::move(m_flushWatcher), m_gstSrcFactoryMock,
                                                   m_gstProfilerFactoryMock, m_timerFactoryMock,
                                                   std::move(m_taskFactory), std::move(workerThreadFactory),
                                                   std::move(gstDispatcherThreadFactory),
                                                   m_gstProtectionMetadataFactoryMock, m_platformBackendMock);
    }

    void destroy()
    {
        gstPlayerWillBeDestroyed();
        m_sut.reset();
    }

    // Attaching an AAC audio source builds the audsrc appsrc, sets its caps, builds the deterministic
    // decodebin chain and takes the SoC sink from the PlatformBackend.
    void expectAttachAudio()
    {
        // Run the real AttachSource task for this call (construction/teardown keep their mock tasks).
        EXPECT_CALL(m_taskFactoryMock, createAttachSource(_, _, _))
            .WillOnce(Invoke(
                [this](GenericPlayerContext &context, IGstGenericPlayerPrivate &player,
                       const std::unique_ptr<IMediaPipeline::MediaSource> &source) -> std::unique_ptr<IPlayerTask>
                {
                    return std::make_unique<firebolt::rialto::server::tasks::generic::AttachSource>(
                        context, m_gstWrapperMock, m_glibWrapperMock, m_textTrackSinkFactoryMock, player, source);
                }));

        EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmptySimple(StrEq("audio/mpeg"))).WillOnce(Return(&m_audioCaps));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&m_audioCaps, StrEq("mpegversion"), G_TYPE_INT, 4));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsToString(&m_audioCaps)).WillOnce(Return(&m_capsStr));
        EXPECT_CALL(*m_glibWrapperMock, gFree(&m_capsStr));
        EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(_, StrEq("audsrc"))).WillOnce(Return(&m_appSrc));
        EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&m_appSrc), &m_audioCaps));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&m_audioCaps));

        // The engine adds its appsrc, then the backend builds + links its own audio subgraph and returns
        // the handles; the engine creates and links no media element itself.
        EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &m_appSrc)).WillOnce(Return(TRUE));
        EXPECT_CALL(*m_platformBackendMock, buildAudioPath(&m_pipeline, &m_appSrc))
            .WillOnce(Return(PlatformMediaPath{&m_audioSink, &m_decodebin, &m_audioConvert}));
        EXPECT_CALL(*m_glibWrapperMock, gSignalConnect(&m_decodebin, StrEq("pad-added"), _, _)).WillOnce(Return(1));
        // The sink is stored twice (each with its own ref): as m_context.audioSink and as the
        // playback group's audio playsink-bin analogue; both released by termPipeline at destroy.
        EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(&m_audioSink)).Times(2).WillRepeatedly(Return(&m_audioSink));
        EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_audioSink)).Times(2);
        expectNoSinkSignals(&m_audioSink, false);
    }

    std::unique_ptr<IMediaPipeline::MediaSource> makeAudioSource()
    {
        return std::make_unique<IMediaPipeline::MediaSourceAudio>("audio/aac", false);
    }

    // Run the real Play task.
    void expectPlay()
    {
        EXPECT_CALL(m_taskFactoryMock, createPlay(_))
            .WillOnce(Invoke([](IGstGenericPlayerPrivate &player) -> std::unique_ptr<IPlayerTask>
                             { return std::make_unique<firebolt::rialto::server::tasks::generic::Play>(player); }));
        EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(&m_pipeline, GST_STATE_PLAYING))
            .WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    }

    // Run the real Pause task.
    void expectPause()
    {
        EXPECT_CALL(m_taskFactoryMock, createPause(_, _))
            .WillOnce(Invoke(
                [](GenericPlayerContext &context, IGstGenericPlayerPrivate &player) -> std::unique_ptr<IPlayerTask>
                { return std::make_unique<firebolt::rialto::server::tasks::generic::Pause>(context, player); }));
        EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(&m_pipeline, GST_STATE_PAUSED))
            .WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    }

    // Run the real Eos task for the audio source; EOS is signalled on the audsrc appsrc.
    void expectEosAudio()
    {
        EXPECT_CALL(m_taskFactoryMock, createEos(_, _, MediaSourceType::AUDIO))
            .WillOnce(Invoke(
                [this](GenericPlayerContext &context, IGstGenericPlayerPrivate &player,
                       const firebolt::rialto::MediaSourceType &type) -> std::unique_ptr<IPlayerTask> {
                    return std::make_unique<firebolt::rialto::server::tasks::generic::Eos>(context, player,
                                                                                          m_gstWrapperMock, type);
                }));
        EXPECT_CALL(*m_gstWrapperMock, gstAppSrcEndOfStream(GST_APP_SRC(&m_appSrc))).WillOnce(Return(GST_FLOW_OK));
    }

    // Attaching an H264 video source builds the vidsrc appsrc, sets its caps, builds the deterministic
    // decodebin chain and takes the SoC video sink from the PlatformBackend keyed by the video id derived
    // at construction (0 = primary for the default video requirements).
    void expectAttachVideo()
    {
        EXPECT_CALL(m_taskFactoryMock, createAttachSource(_, _, _))
            .WillOnce(Invoke(
                [this](GenericPlayerContext &context, IGstGenericPlayerPrivate &player,
                       const std::unique_ptr<IMediaPipeline::MediaSource> &source) -> std::unique_ptr<IPlayerTask>
                {
                    return std::make_unique<firebolt::rialto::server::tasks::generic::AttachSource>(
                        context, m_gstWrapperMock, m_glibWrapperMock, m_textTrackSinkFactoryMock, player, source);
                }));

        EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmptySimple(StrEq("video/x-h264"))).WillOnce(Return(&m_videoCaps));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsToString(&m_videoCaps)).WillOnce(Return(&m_capsStr));
        EXPECT_CALL(*m_glibWrapperMock, gFree(&m_capsStr));
        EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(_, StrEq("vidsrc"))).WillOnce(Return(&m_videoAppSrc));
        EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&m_videoAppSrc), &m_videoCaps));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&m_videoCaps));

        // The engine adds its appsrc, then the backend builds the video path (sink == decoderLinkTarget on
        // the reference path) keyed by the derived video id.
        EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &m_videoAppSrc)).WillOnce(Return(TRUE));
        EXPECT_CALL(*m_platformBackendMock, buildVideoPath(&m_pipeline, &m_videoAppSrc, 0u))
            .WillOnce(Return(PlatformMediaPath{&m_videoSink, &m_videoDecodebin, &m_videoSink}));
        EXPECT_CALL(*m_glibWrapperMock, gSignalConnect(&m_videoDecodebin, StrEq("pad-added"), _, _)).WillOnce(Return(1));
        EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(&m_videoSink)).WillOnce(Return(&m_videoSink));
        EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_videoSink)); // released by termPipeline at destroy
        expectNoSinkSignals(&m_videoSink, true);
    }

    std::unique_ptr<IMediaPipeline::MediaSource> makeVideoSource()
    {
        return std::make_unique<IMediaPipeline::MediaSourceVideo>("video/h264", false);
    }

    // Run the real Eos task for the video source; EOS is signalled on the vidsrc appsrc.
    void expectEosVideo()
    {
        EXPECT_CALL(m_taskFactoryMock, createEos(_, _, MediaSourceType::VIDEO))
            .WillOnce(Invoke(
                [this](GenericPlayerContext &context, IGstGenericPlayerPrivate &player,
                       const firebolt::rialto::MediaSourceType &type) -> std::unique_ptr<IPlayerTask> {
                    return std::make_unique<firebolt::rialto::server::tasks::generic::Eos>(context, player,
                                                                                          m_gstWrapperMock, type);
                }));
        EXPECT_CALL(*m_gstWrapperMock, gstAppSrcEndOfStream(GST_APP_SRC(&m_videoAppSrc))).WillOnce(Return(GST_FLOW_OK));
    }

    // Attaching a subtitle source builds the subsrc appsrc, sets its caps, creates the text-track sink
    // and builds appsrc -> RialtoTextTrackSink itself (an extra ref taken on the stored sink). The stored
    // subtitle sink is released by termPipeline at destroy.
    void expectAttachSubtitle()
    {
        EXPECT_CALL(m_taskFactoryMock, createAttachSource(_, _, _))
            .WillOnce(Invoke(
                [this](GenericPlayerContext &context, IGstGenericPlayerPrivate &player,
                       const std::unique_ptr<IMediaPipeline::MediaSource> &source) -> std::unique_ptr<IPlayerTask>
                {
                    return std::make_unique<firebolt::rialto::server::tasks::generic::AttachSource>(
                        context, m_gstWrapperMock, m_glibWrapperMock, m_textTrackSinkFactoryMock, player, source);
                }));

        EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmpty()).WillOnce(Return(&m_subtitleCaps));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsToString(&m_subtitleCaps)).WillOnce(Return(&m_capsStr));
        EXPECT_CALL(*m_glibWrapperMock, gFree(&m_capsStr));
        EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(_, StrEq("subsrc"))).WillOnce(Return(&m_subtitleAppSrc));
        EXPECT_CALL(*m_textTrackSinkFactoryMock, createGstTextTrackSink()).WillOnce(Return(&m_textTrackSink));
        EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&m_subtitleAppSrc), &m_subtitleCaps));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&m_subtitleCaps));
        EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_textTrackSink)); // released by termPipeline at destroy

        EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &m_subtitleAppSrc)).WillOnce(Return(TRUE));
        EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &m_textTrackSink)).WillOnce(Return(TRUE));
        EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&m_subtitleAppSrc, &m_textTrackSink)).WillOnce(Return(TRUE));
        EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(&m_textTrackSink)).WillOnce(Return(&m_textTrackSink));
    }

    std::unique_ptr<IMediaPipeline::MediaSource> makeSubtitleSource()
    {
        return std::make_unique<IMediaPipeline::MediaSourceSubtitle>("application/ttml+xml", "text-track-id");
    }
};

/**
 * The player constructs a usable pipeline and tears it down cleanly.
 */
TEST_F(GstGenericPlayerExplicitConstructionTest, ConstructsAndDestroysSuccessfully)
{
    arrangeAndConstruct();

    ASSERT_NE(m_sut, nullptr);

    destroy();
}

/**
 * Attaching an audio source builds the audsrc appsrc, the decodebin chain and sources the sink from
 * the PlatformBackend. Exercises attach -> real buildAudioChain end-to-end.
 */
TEST_F(GstGenericPlayerExplicitConstructionTest, AttachAudioSourceBuildsExpectedGraph)
{
    arrangeAndConstruct();

    expectAttachAudio();
    auto source{makeAudioSource()};
    m_sut->attachSource(source);

    destroy();
}

/**
 * Play drives the pipeline to PLAYING.
 */
TEST_F(GstGenericPlayerExplicitConstructionTest, PlayDrivesPipelineToPlaying)
{
    arrangeAndConstruct();

    expectPlay();
    bool async{false};
    m_sut->play(async);

    destroy();
}

/**
 * Pause drives the pipeline to PAUSED.
 */
TEST_F(GstGenericPlayerExplicitConstructionTest, PauseDrivesPipelineToPaused)
{
    arrangeAndConstruct();

    expectPause();
    m_sut->pause();

    destroy();
}

/**
 * After an audio source is attached, EOS is signalled on its appsrc.
 */
TEST_F(GstGenericPlayerExplicitConstructionTest, SetEosSignalsAudioAppSrc)
{
    arrangeAndConstruct();

    expectAttachAudio();
    auto source{makeAudioSource()};
    m_sut->attachSource(source);

    expectEosAudio();
    m_sut->setEos(MediaSourceType::AUDIO);

    destroy();
}

/**
 * Attaching a video source builds the vidsrc appsrc, the decodebin chain and sources the sink from
 * the PlatformBackend keyed by the video id. Exercises attach -> real buildVideoChain end-to-end.
 */
TEST_F(GstGenericPlayerExplicitConstructionTest, AttachVideoSourceBuildsExpectedGraph)
{
    arrangeAndConstruct();

    expectAttachVideo();
    auto source{makeVideoSource()};
    m_sut->attachSource(source);

    destroy();
}

/**
 * After a video source is attached, EOS is signalled on its appsrc.
 */
TEST_F(GstGenericPlayerExplicitConstructionTest, SetEosSignalsVideoAppSrc)
{
    arrangeAndConstruct();

    expectAttachVideo();
    auto source{makeVideoSource()};
    m_sut->attachSource(source);

    expectEosVideo();
    m_sut->setEos(MediaSourceType::VIDEO);

    destroy();
}

/**
 * Attaching a subtitle source builds the subsrc appsrc, the text-track sink and
 * appsrc -> RialtoTextTrackSink. Exercises attach -> real buildSubtitleChain end-to-end.
 */
TEST_F(GstGenericPlayerExplicitConstructionTest, AttachSubtitleSourceBuildsExpectedGraph)
{
    arrangeAndConstruct();

    expectAttachSubtitle();
    auto source{makeSubtitleSource()};
    m_sut->attachSource(source);

    destroy();
}
