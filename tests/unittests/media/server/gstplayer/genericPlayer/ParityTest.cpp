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
 * Playbin / explicit-construction PARITY suite.
 *
 * These cases assert the observable behaviour the MSE generic player must exhibit regardless of how
 * its pipeline is built. They are parameterised over the construction mode so that the same
 * behavioural spec runs against both the legacy `playbin` path and the explicit-construction path
 * during the playbin-removal migration (see PLAYBIN-REMOVAL-PLAN.md).
 *
 * Both parameters are live and EXECUTION-level: the player is built with the real
 * GenericPlayerTaskFactory and a synchronous worker (tasks run inline on enqueue), so the public
 * API drives real tasks through the real player against the mocked GStreamer wrappers. Playbin
 * builds via GStreamer playbin; Explicit drives the explicit-construction path
 * (RIALTO_EXPLICIT_PIPELINE opt-in) and the SoC sinks come from the PlatformBackend. The same
 * spec runs against both so the eventual default-flip is low-risk; where the two paths legitimately
 * differ (playbin autoplugs/detects later vs explicit builds the chain now) the per-mode arrange
 * encodes that, while the observable end-state asserted is the same.
 */

#include "GstGenericPlayerTestCommon.h"
#include "GstTextTrackSinkFactoryMock.h"
#include "tasks/generic/AttachSource.h"
#include "tasks/generic/Eos.h"
#include "tasks/generic/Pause.h"
#include "tasks/generic/Play.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <utility>

using ::testing::_;
using ::testing::ByMove;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::StrEq;

enum class ConstructionMode
{
    Playbin,
    Explicit
};

class GstGenericPlayerParityTest : public GstGenericPlayerTestCommon,
                                   public ::testing::WithParamInterface<ConstructionMode>
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

    bool isExplicit() const { return GetParam() == ConstructionMode::Explicit; }

    void TearDown() override { unsetenv("RIALTO_EXPLICIT_PIPELINE"); }

    // Builds the player through the construction path under test. Construction and teardown keep the
    // mocked task factory (so the shared helpers apply unchanged); the behavioural cases below make the
    // task factory return a REAL task for the one call under test, which the worker mock then executes
    // synchronously on enqueue. PlatformBackendMock is injected so the explicit chain can source its
    // sink.
    void arrangeAndConstruct()
    {
        if (isExplicit())
        {
            setenv("RIALTO_EXPLICIT_PIPELINE", "1", 1);
            gstPlayerWillBeCreatedExplicit();
        }
        else
        {
            gstPlayerWillBeCreated();
        }
        m_sut = std::make_unique<GstGenericPlayer>(&m_gstPlayerClient, m_decryptionServiceMock, MediaType::MSE,
                                                   m_videoReq, m_isLive, m_gstWrapperMock, m_glibWrapperMock,
                                                   m_rdkGstreamerUtilsWrapperMock, m_gstInitialiserMock,
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

    // Expectations for attaching an AAC audio source. Both paths build the audsrc appsrc and set its
    // caps; the explicit path additionally builds the deterministic decodebin chain and takes the SoC
    // sink from the PlatformBackend (playbin autoplugs the decoder/sink lazily, so neither happens at
    // attach time on that path).
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

        if (isExplicit())
        {
            EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("decodebin"), StrEq("auddecodebin")))
                .WillOnce(Return(&m_decodebin));
            EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("audioconvert"), StrEq("audconvert")))
                .WillOnce(Return(&m_audioConvert));
            EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("audioresample"), StrEq("audresample")))
                .WillOnce(Return(&m_audioResample));
            EXPECT_CALL(*m_platformBackendMock, createAudioSink(StrEq("audiosink"))).WillOnce(Return(&m_audioSink));
            EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &m_appSrc)).WillOnce(Return(TRUE));
            EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &m_decodebin)).WillOnce(Return(TRUE));
            EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &m_audioConvert)).WillOnce(Return(TRUE));
            EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &m_audioResample)).WillOnce(Return(TRUE));
            EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &m_audioSink)).WillOnce(Return(TRUE));
            EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&m_appSrc, &m_decodebin)).WillOnce(Return(TRUE));
            EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&m_audioConvert, &m_audioResample)).WillOnce(Return(TRUE));
            EXPECT_CALL(*m_gstWrapperMock, gstElementLink(&m_audioResample, &m_audioSink)).WillOnce(Return(TRUE));
            EXPECT_CALL(*m_glibWrapperMock, gSignalConnect(&m_decodebin, StrEq("pad-added"), _, _)).WillOnce(Return(1));
            EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(&m_audioSink)).WillOnce(Return(&m_audioSink));
            EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_audioSink)); // released by termPipeline at destroy
        }
    }

    std::unique_ptr<IMediaPipeline::MediaSource> makeAudioSource()
    {
        return std::make_unique<IMediaPipeline::MediaSourceAudio>("audio/aac", false);
    }

    // Run the real Play task; the pipeline state change is identical on both paths.
    void expectPlay()
    {
        EXPECT_CALL(m_taskFactoryMock, createPlay(_))
            .WillOnce(Invoke([](IGstGenericPlayerPrivate &player) -> std::unique_ptr<IPlayerTask>
                             { return std::make_unique<firebolt::rialto::server::tasks::generic::Play>(player); }));
        EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(&m_pipeline, GST_STATE_PLAYING))
            .WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    }

    // Run the real Pause task; the pipeline state change is identical on both paths.
    void expectPause()
    {
        EXPECT_CALL(m_taskFactoryMock, createPause(_, _))
            .WillOnce(Invoke(
                [](GenericPlayerContext &context, IGstGenericPlayerPrivate &player) -> std::unique_ptr<IPlayerTask>
                { return std::make_unique<firebolt::rialto::server::tasks::generic::Pause>(context, player); }));
        EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(&m_pipeline, GST_STATE_PAUSED))
            .WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    }

    // Run the real Eos task for the audio source; EOS is signalled on the audsrc appsrc on both paths.
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
};

/**
 * Parity: the player constructs a usable pipeline and tears it down cleanly, whichever construction
 * path is used.
 */
TEST_P(GstGenericPlayerParityTest, ConstructsAndDestroysSuccessfully)
{
    arrangeAndConstruct();

    ASSERT_NE(m_sut, nullptr);

    destroy();
}

/**
 * Parity: attaching an audio source builds the audsrc appsrc on both paths; the explicit path also
 * builds the decodebin chain and sources the sink from the PlatformBackend (the playbin path defers
 * decoder/sink creation to autoplug). Exercises attach -> real buildAudioChain end-to-end.
 */
TEST_P(GstGenericPlayerParityTest, AttachAudioSourceBuildsExpectedGraph)
{
    arrangeAndConstruct();

    expectAttachAudio();
    auto source{makeAudioSource()};
    m_sut->attachSource(source);

    destroy();
}

/**
 * Parity: play drives the pipeline to PLAYING regardless of construction path.
 */
TEST_P(GstGenericPlayerParityTest, PlayDrivesPipelineToPlaying)
{
    arrangeAndConstruct();

    expectPlay();
    bool async{false};
    m_sut->play(async);

    destroy();
}

/**
 * Parity: pause drives the pipeline to PAUSED regardless of construction path.
 */
TEST_P(GstGenericPlayerParityTest, PauseDrivesPipelineToPaused)
{
    arrangeAndConstruct();

    expectPause();
    m_sut->pause();

    destroy();
}

/**
 * Parity: after an audio source is attached, EOS is signalled on its appsrc regardless of
 * construction path.
 */
TEST_P(GstGenericPlayerParityTest, SetEosSignalsAudioAppSrc)
{
    arrangeAndConstruct();

    expectAttachAudio();
    auto source{makeAudioSource()};
    m_sut->attachSource(source);

    expectEosAudio();
    m_sut->setEos(MediaSourceType::AUDIO);

    destroy();
}

INSTANTIATE_TEST_SUITE_P(PlaybinAndExplicit, GstGenericPlayerParityTest,
                         ::testing::Values(ConstructionMode::Playbin, ConstructionMode::Explicit),
                         [](const ::testing::TestParamInfo<ConstructionMode> &info)
                         { return info.param == ConstructionMode::Playbin ? "Playbin" : "Explicit"; });
