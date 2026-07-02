/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2022 Sky UK
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

#include "GstExpect.h"
#include "GstGenericPlayerTestCommon.h"
#include "Matchers.h"
#include "MediaSourceUtil.h"
#include "PlayerTaskMock.h"
#include "TimerMock.h"

#include <gst/audio/audio.h>

using testing::_;
using testing::AllOf;
using testing::ByMove;
using testing::Field;
using testing::Invoke;
using testing::Return;
using testing::StrEq;

using ::firebolt::rialto::server::testcommon::expectPropertyDoesntExist;
using ::firebolt::rialto::server::testcommon::expectSetProperty;

namespace
{
constexpr std::chrono::milliseconds kPositionReportTimerMs{250};
constexpr std::chrono::milliseconds kPlaybackInfoTimerMs{32};
constexpr int32_t kSampleRate{13};
constexpr int32_t kNumberOfChannels{4};
constexpr int32_t kInvalidSampleRate{0};
constexpr int32_t kInvalidNumberOfChannels{0};
constexpr int32_t kWidth{1024};
constexpr int32_t kHeight{768};
constexpr Fraction kFrameRate{15, 1};
constexpr int32_t kSourceId{2};
constexpr int64_t kTimeStamp{123};
constexpr int64_t kDuration{432};
constexpr int32_t kMediaKeySessionId{4235};
const std::vector<uint8_t> kKeyId{1, 2, 3, 4};
const std::vector<uint8_t> kInitVector{5, 6, 7, 8};
constexpr uint32_t kInitWithLast15{1};
constexpr size_t kNumClearBytes{3};
constexpr size_t kNumEncryptedBytes{5};
constexpr uint32_t kCrypt{7};
constexpr uint32_t kSkip{7};
constexpr firebolt::rialto::CipherMode kCipherMode{firebolt::rialto::CipherMode::CENS};
constexpr VideoRequirements m_videoReq{kMinPrimaryVideoWidth, kMinPrimaryVideoHeight};
constexpr uint32_t kBufferingLimit{123};
constexpr bool kUseBuffering{true};
const std::shared_ptr<firebolt::rialto::CodecData> kEmptyCodecData{};
const std::shared_ptr<firebolt::rialto::CodecData> kCodecDataWithBuffer{std::make_shared<firebolt::rialto::CodecData>(
    firebolt::rialto::CodecData{std::vector<std::uint8_t>{1, 2, 3, 4}, firebolt::rialto::CodecDataType::BUFFER})};
const std::string kCodecDataStr{"test"};
const std::shared_ptr<firebolt::rialto::CodecData> kCodecDataWithString{std::make_shared<firebolt::rialto::CodecData>(
    firebolt::rialto::CodecData{std::vector<std::uint8_t>{kCodecDataStr.begin(), kCodecDataStr.end()},
                                firebolt::rialto::CodecDataType::STRING})};
constexpr bool kResetTime{true};
const std::string kImmediateOutputStr{"immediate-output"};
const std::string kLowLatencyStr{"low-latency"};
const std::string kSyncStr{"sync"};
const std::string kSyncOffStr{"sync-off"};
const std::string kStreamSyncModeStr{"stream-sync-mode"};
const std::string kSyncModeStreamingStr{"syncmode-streaming"};
const std::string kBufferingLimitStr{"limit-buffering-ms"};
const std::string kEnableRateCorrectionStr{"enable-rate-correction"};
const std::string kUseBufferingStr{"use-buffering"};
const std::string kFrameStepOnPrerollStr{"frame-step-on-preroll"};
constexpr bool kShowVideoWindow{true};
constexpr gint64 kPosition{123};
constexpr double kVolume{0.5};
constexpr firebolt::rialto::PlaybackInfo kPlaybackInfo{kPosition, kVolume};
constexpr bool kIsLive{false};
} // namespace

bool operator==(const GstRialtoProtectionData &lhs, const GstRialtoProtectionData &rhs)
{
    return lhs.keySessionId == rhs.keySessionId && lhs.subsampleCount == rhs.subsampleCount &&
           lhs.initWithLast15 == rhs.initWithLast15 && lhs.key == rhs.key && lhs.iv == rhs.iv &&
           lhs.subsamples == rhs.subsamples && lhs.cipherMode == rhs.cipherMode && lhs.crypt == rhs.crypt &&
           lhs.skip == rhs.skip;
}

namespace firebolt::rialto
{
bool operator==(const PlaybackInfo &lhs, const PlaybackInfo &rhs)
{
    return lhs.currentPosition == rhs.currentPosition && lhs.volume == rhs.volume;
}
} // namespace firebolt::rialto

class GstGenericPlayerPrivateTest : public GstGenericPlayerTestCommon
{
protected:
    std::unique_ptr<IGstGenericPlayerPrivate> m_sut;

    GstElement *m_realElement;
    GstElement m_element{};
    GParamSpec m_rectangleSpec{};
    GParamSpec m_showVideoWindowSpec{};
    GstEvent m_event{};
    guint m_signalIds{};

    // Expectations for the explicit-path telemetry scan on an element that exposes no underflow /
    // first-frame signal (the autoaudiosink/autovideosink reference case): getUnderflowSignalName (and,
    // for video, getFirstFrameSignalName) lists the element's signals, finds none, and connects nothing.
    void expectNoStreamSignals(GstElement *element, bool isVideo)
    {
        const int kScans = isVideo ? 2 : 1; // underflow always; first-video-frame for video too
        EXPECT_CALL(*m_glibWrapperMock, gObjectType(element)).Times(kScans).WillRepeatedly(Return(G_TYPE_PARAM));
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

    GstGenericPlayerPrivateTest()
    {
        gstPlayerWillBeCreated();
        m_sut = std::make_unique<GstGenericPlayer>(&m_gstPlayerClient, m_decryptionServiceMock, MediaType::MSE,
                                                   m_videoReq, kIsLive, m_gstWrapperMock, m_glibWrapperMock,
                                                   m_gstInitialiserMock, std::move(m_flushWatcher), m_gstSrcFactoryMock,
                                                   m_gstProfilerFactoryMock, m_timerFactoryMock, std::move(m_taskFactory),
                                                   std::move(workerThreadFactory), std::move(gstDispatcherThreadFactory),
                                                   m_gstProtectionMetadataFactoryMock, m_platformBackendMock);
        m_realElement = initRealElement();
    }

    ~GstGenericPlayerPrivateTest() override
    {
        gst_object_unref(m_realElement);
        gstPlayerWillBeDestroyed();
        m_sut.reset();
    }

    void applyToContext(const std::function<void(GenericPlayerContext &)> &fun) override { modifyContext(fun); }

    void modifyContext(const std::function<void(GenericPlayerContext &)> &fun)
    {
        std::mutex m_waitMutex;
        std::condition_variable m_waitCv;
        bool m_waitBool = false;

        // Call any method to modify GstGenericPlayer context
        GstAppSrc appSrc{};
        std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
        EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
        EXPECT_CALL(m_taskFactoryMock, createNeedData(_, _, &appSrc))
            .WillOnce(Invoke(
                [&](GenericPlayerContext &context, IGstGenericPlayerPrivate &player, GstAppSrc *src)
                {
                    fun(context);
                    std::unique_lock<std::mutex> lock{m_waitMutex};
                    m_waitBool = true;
                    m_waitCv.notify_one();
                    return std::move(task);
                }));

        m_sut->scheduleNeedMediaData(&appSrc);

        std::unique_lock<std::mutex> lock{m_waitMutex};
        m_waitCv.wait(lock, [&] { return m_waitBool; });
    }

    GstElement *initRealElement()
    {
        GstElementFactory *elementFactory = gst_element_factory_find("fakesrc");
        GstElement *element = gst_element_factory_create(elementFactory, nullptr);
        gst_object_unref(elementFactory);
        return element;
    }

    // reattachSource refreshes the audio playback group from the live graph before the codec switch
    // (updateAudioPlaybackGroupHandles). When no audio decoder/parser is plugged and no audio decodebin
    // is set, getDecoder/getParser iterate the pipeline and find nothing and getAudioTypefind returns
    // null immediately, so the handles are left null.
    void expectRefreshAudioPlaybackGroupHandlesEmpty()
    {
        EXPECT_CALL(*m_gstWrapperMock, gstBinIterateRecurse(GST_BIN(&m_pipeline))).Times(2).WillRepeatedly(Return(&m_it));
        EXPECT_CALL(*m_gstWrapperMock, gstIteratorNext(&m_it, _)).Times(2).WillRepeatedly(Return(GST_ITERATOR_DONE));
        EXPECT_CALL(*m_glibWrapperMock, gValueUnset(_)).Times(2);
        EXPECT_CALL(*m_gstWrapperMock, gstIteratorFree(&m_it)).Times(2);
    }

    GenericPlayerContext *getPlayerContext()
    {
        GenericPlayerContext *saveContext;
        modifyContext([&](GenericPlayerContext &context) { saveContext = &context; });
        return saveContext;
    }

    void willNotifyPlaybackInfo()
    {
        EXPECT_CALL(*m_gstWrapperMock, gstStateLock(_)).WillOnce(Return());
        EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(_)).WillOnce(Return(GST_STATE_PLAYING));
        EXPECT_CALL(*m_gstWrapperMock, gstElementGetStateReturn(_)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
        EXPECT_CALL(*m_gstWrapperMock, gstStateUnlock(_)).WillOnce(Return());
        EXPECT_CALL(*m_gstWrapperMock, gstElementQueryPosition(_, GST_FORMAT_TIME, _))
            .WillOnce(Invoke(
                [&](GstElement *element, GstFormat format, gint64 *cur)
                {
                    *cur = kPosition;
                    return TRUE;
                }));

        EXPECT_CALL(*m_gstWrapperMock, gstStreamVolumeGetVolume(_, GST_STREAM_VOLUME_FORMAT_LINEAR))
            .WillOnce(Return(kVolume));

        EXPECT_CALL(m_gstPlayerClient, notifyPlaybackInfo(kPlaybackInfo));
    }

    void willNotifyPlaybackInfoWithAudioFade()
    {
        modifyContext(
            [&](GenericPlayerContext &context)
            {
                context.audioFadeEnabled = true;
                context.audioFadeVolume = kVolume;
            });
        EXPECT_CALL(*m_gstWrapperMock, gstStateLock(_)).WillOnce(Return());
        EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(_)).WillOnce(Return(GST_STATE_PLAYING));
        EXPECT_CALL(*m_gstWrapperMock, gstElementGetStateReturn(_)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
        EXPECT_CALL(*m_gstWrapperMock, gstStateUnlock(_)).WillOnce(Return());
        EXPECT_CALL(*m_gstWrapperMock, gstElementQueryPosition(_, GST_FORMAT_TIME, _))
            .WillOnce(Invoke(
                [&](GstElement *element, GstFormat format, gint64 *cur)
                {
                    *cur = kPosition;
                    return TRUE;
                }));

        EXPECT_CALL(m_gstPlayerClient, notifyPlaybackInfo(kPlaybackInfo));
    }
};

TEST_F(GstGenericPlayerPrivateTest, shouldScheduleNeedData)
{
    GstAppSrc appSrc{};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(m_taskFactoryMock, createNeedData(_, _, &appSrc)).WillOnce(Return(ByMove(std::move(task))));

    m_sut->scheduleNeedMediaData(&appSrc);
}

TEST_F(GstGenericPlayerPrivateTest, shouldScheduleEnoughDataData)
{
    GstAppSrc appSrc{};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(m_taskFactoryMock, createEnoughData(_, &appSrc)).WillOnce(Return(ByMove(std::move(task))));

    m_sut->scheduleEnoughData(&appSrc);
}

TEST_F(GstGenericPlayerPrivateTest, shouldScheduleAudioUnderflowWithUnderflowEnabled)
{
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.isPlaying = true;
            context.audioSourceRemoved = false;
        });

    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(m_taskFactoryMock, createUnderflow(_, _, true, MediaSourceType::AUDIO))
        .WillOnce(Return(ByMove(std::move(task))));

    m_sut->scheduleAudioUnderflow();
}

TEST_F(GstGenericPlayerPrivateTest, shouldScheduleAudioUnderflowWithUnderflowDisabledNotPlaying)
{
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.isPlaying = false;
            context.audioSourceRemoved = false;
        });

    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(m_taskFactoryMock, createUnderflow(_, _, false, MediaSourceType::AUDIO))
        .WillOnce(Return(ByMove(std::move(task))));

    m_sut->scheduleAudioUnderflow();
}

TEST_F(GstGenericPlayerPrivateTest, shouldScheduleAudioUnderflowWithUnderflowDisabledRemoveSource)
{
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.isPlaying = true;
            context.audioSourceRemoved = true;
        });

    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(m_taskFactoryMock, createUnderflow(_, _, false, MediaSourceType::AUDIO))
        .WillOnce(Return(ByMove(std::move(task))));

    m_sut->scheduleAudioUnderflow();
}

TEST_F(GstGenericPlayerPrivateTest, shouldScheduleVideoUnderflowWithUnderflowEnabled)
{
    modifyContext([&](GenericPlayerContext &context) { context.isPlaying = true; });

    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(m_taskFactoryMock, createUnderflow(_, _, true, MediaSourceType::VIDEO))
        .WillOnce(Return(ByMove(std::move(task))));

    m_sut->scheduleVideoUnderflow();
}

TEST_F(GstGenericPlayerPrivateTest, shouldScheduleVideoUnderflowWithUnderflowDisabled)
{
    modifyContext([&](GenericPlayerContext &context) { context.isPlaying = false; });

    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(m_taskFactoryMock, createUnderflow(_, _, false, MediaSourceType::VIDEO))
        .WillOnce(Return(ByMove(std::move(task))));

    m_sut->scheduleVideoUnderflow();
}

TEST_F(GstGenericPlayerPrivateTest, shouldScheduleFirstVideoFrameReceived)
{
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(m_taskFactoryMock, createFirstFrameReceived(_, _, MediaSourceType::VIDEO))
        .WillOnce(Return(ByMove(std::move(task))));

    m_sut->scheduleFirstVideoFrameReceived();
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotSetVideoRectangleWhenVideoSinkIsNull)
{
    EXPECT_FALSE(m_sut->setVideoSinkRectangle());
}

TEST_F(GstGenericPlayerPrivateTest, shouldApplyPlaybackRateViaPlatformBackend)
{
    modifyContext([&](GenericPlayerContext &context) { context.pipeline = &m_pipeline; });
    EXPECT_CALL(*m_platformBackendMock, applyPlaybackRate(&m_pipeline, 1.25)).WillOnce(Return(true));
    EXPECT_TRUE(m_sut->applyPlaybackRate(1.25));
}

TEST_F(GstGenericPlayerPrivateTest, shouldReportAudioFadeSupportViaPlatformBackend)
{
    EXPECT_CALL(*m_platformBackendMock, isAudioFadeSupported()).WillOnce(Return(true));
    EXPECT_TRUE(m_sut->isAudioFadeSupported());
}

TEST_F(GstGenericPlayerPrivateTest, shouldApplyAudioFadeViaPlatformBackend)
{
    constexpr double kTarget{0.5};
    constexpr uint32_t kDuration{1000};
    constexpr firebolt::rialto::EaseType kEaseType{firebolt::rialto::EaseType::EASE_LINEAR};
    EXPECT_CALL(*m_platformBackendMock, audioFade(kTarget, kDuration, kEaseType));
    m_sut->audioFade(kTarget, kDuration, kEaseType);
}

TEST_F(GstGenericPlayerPrivateTest, shouldProcessAudioGapViaPlatformBackend)
{
    constexpr int64_t kPosition{123};
    constexpr uint32_t kDuration{456};
    constexpr int64_t kDiscontinuityGap{789};
    constexpr bool kAudioAac{true};
    EXPECT_CALL(*m_platformBackendMock,
                processAudioGap(&m_pipeline, kPosition, kDuration, kDiscontinuityGap, kAudioAac))
        .WillOnce(Return(true));
    EXPECT_TRUE(m_sut->processAudioGap(&m_pipeline, kPosition, kDuration, kDiscontinuityGap, kAudioAac));
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotSetVideoRectangleWhenVideoSinkDoesNotHaveRectangleProperty)
{
    expectGetAVSink(kVideoSinkStr, m_realElement);
    EXPECT_CALL(*m_glibWrapperMock, gObjectClassFindProperty(_, StrEq("rectangle"))).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement));
    EXPECT_FALSE(m_sut->setVideoSinkRectangle());
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetVideoRectangle)
{
    expectGetAVSink(kVideoSinkStr, m_realElement);
    EXPECT_CALL(*m_glibWrapperMock, gObjectClassFindProperty(_, StrEq("rectangle"))).WillOnce(Return(&m_rectangleSpec));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(m_realElement, StrEq("rectangle")));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement));
    EXPECT_TRUE(m_sut->setVideoSinkRectangle());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetImmediateOutputIfSinkIsNull)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingImmediateOutputForVideo = true; });
    EXPECT_FALSE(m_sut->setImmediateOutput());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetImmediateOutputIfPropertyDoesntExist)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingImmediateOutputForVideo = true; });

    expectGetAVSink(kVideoSinkStr, m_realElement);

    expectPropertyDoesntExist(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kImmediateOutputStr);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_FALSE(m_sut->setImmediateOutput());
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetImmediateOutput)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingImmediateOutputForVideo = true; });

    expectGetAVSink(kVideoSinkStr, m_realElement);

    expectSetProperty(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kImmediateOutputStr, true);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_TRUE(m_sut->setImmediateOutput());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetLowLatencyIfSinkIsNull)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingLowLatency = true; });
    EXPECT_FALSE(m_sut->setLowLatency());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetLowLatencyIfPropertyDoesntExist)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingLowLatency = true; });

    expectGetAVSink(kAudioSinkStr, m_realElement);

    expectPropertyDoesntExist(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kLowLatencyStr);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_FALSE(m_sut->setLowLatency());
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetLowLatency)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingLowLatency = true; });

    expectGetAVSink(kAudioSinkStr, m_realElement);

    expectSetProperty(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kLowLatencyStr, true);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_TRUE(m_sut->setLowLatency());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetSyncIfSinkIsNull)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingSync = true; });
    EXPECT_FALSE(m_sut->setSync());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetSyncIfPropertyDoesntExist)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingSync = true; });

    expectGetAVSink(kAudioSinkStr, m_realElement);

    expectPropertyDoesntExist(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kSyncStr);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_FALSE(m_sut->setSync());
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetSync)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingSync = true; });

    expectGetAVSink(kAudioSinkStr, m_realElement);

    expectSetProperty(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kSyncStr, true);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_TRUE(m_sut->setSync());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetSyncOffIfDecoderIsNull)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingSyncOff = true; });
    expectNoDecoder();
    EXPECT_FALSE(m_sut->setSyncOff());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetSyncOffIfPropertyDoesntExist)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingSyncOff = true; });

    expectGetDecoder(m_realElement);

    expectPropertyDoesntExist(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kSyncOffStr);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_FALSE(m_sut->setSyncOff());
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetSyncOff)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingSyncOff = true; });

    expectGetDecoder(m_realElement);

    expectSetProperty(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kSyncOffStr, true);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_TRUE(m_sut->setSyncOff());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetAudioStreamSyncModeIfDecoderIsNull)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingStreamSyncMode[MediaSourceType::AUDIO] = 1; });
    expectNoDecoder();
    EXPECT_FALSE(m_sut->setStreamSyncMode(MediaSourceType::AUDIO));
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetAudioStreamSyncModeIfPropertyDoesntExist)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingStreamSyncMode[MediaSourceType::AUDIO] = 1; });

    expectGetDecoder(m_realElement);

    expectPropertyDoesntExist(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kStreamSyncModeStr);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_FALSE(m_sut->setStreamSyncMode(MediaSourceType::AUDIO));
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetAudioStreamSyncMode)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingStreamSyncMode[MediaSourceType::AUDIO] = 1; });

    expectGetDecoder(m_realElement);

    expectSetProperty(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kStreamSyncModeStr, 1);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_TRUE(m_sut->setStreamSyncMode(MediaSourceType::AUDIO));
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetVideoStreamSyncModeIfParserIsNull)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingStreamSyncMode[MediaSourceType::VIDEO] = 1; });
    expectNoParser();
    EXPECT_FALSE(m_sut->setStreamSyncMode(MediaSourceType::VIDEO));
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetVideoStreamSyncModeIfPropertyDoesntExist)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingStreamSyncMode[MediaSourceType::VIDEO] = 1; });

    expectGetVideoParser(m_realElement);

    expectPropertyDoesntExist(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kSyncModeStreamingStr);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_FALSE(m_sut->setStreamSyncMode(MediaSourceType::VIDEO));
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetVideoStreamSyncMode)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingStreamSyncMode[MediaSourceType::VIDEO] = 1; });

    expectGetVideoParser(m_realElement);

    expectSetProperty(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kSyncModeStreamingStr, true);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_TRUE(m_sut->setStreamSyncMode(MediaSourceType::VIDEO));
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetBufferingLimitIfDecoderIsNull)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingBufferingLimit = kBufferingLimit; });
    expectNoDecoder();
    EXPECT_FALSE(m_sut->setBufferingLimit());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetBufferingLimitIfPropertyDoesntExist)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingBufferingLimit = kBufferingLimit; });

    expectGetDecoder(m_realElement);

    expectPropertyDoesntExist(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kBufferingLimitStr);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_FALSE(m_sut->setBufferingLimit());
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetBufferingLimit)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingBufferingLimit = kBufferingLimit; });

    expectGetDecoder(m_realElement);

    expectSetProperty(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kBufferingLimitStr,
                      static_cast<int>(kBufferingLimit));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_TRUE(m_sut->setBufferingLimit());
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotSetEnableRateCorrectionWhenNotLive)
{
    // Not a live stream: never touches the decoder.
    EXPECT_FALSE(m_sut->setEnableRateCorrection());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetEnableRateCorrectionIfDecoderIsNull)
{
    modifyContext([&](GenericPlayerContext &context) { context.isLive = true; });
    expectNoDecoder();
    EXPECT_FALSE(m_sut->setEnableRateCorrection());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetEnableRateCorrectionIfPropertyDoesntExist)
{
    modifyContext([&](GenericPlayerContext &context) { context.isLive = true; });

    expectGetDecoder(m_realElement);

    expectPropertyDoesntExist(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kEnableRateCorrectionStr);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_FALSE(m_sut->setEnableRateCorrection());
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetEnableRateCorrection)
{
    modifyContext([&](GenericPlayerContext &context) { context.isLive = true; });

    expectGetDecoder(m_realElement);

    // enable-rate-correction routes through the generic gObjectSetStub (value ignored), matching the
    // playbin path's reactive SetupElement behaviour.
    GParamSpec dummySpec{};
    EXPECT_CALL(*m_glibWrapperMock,
                gObjectClassFindProperty(G_OBJECT_GET_CLASS(m_realElement), StrEq(kEnableRateCorrectionStr.c_str())))
        .WillOnce(Return(&dummySpec));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(m_realElement, StrEq(kEnableRateCorrectionStr.c_str())));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_TRUE(m_sut->setEnableRateCorrection());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetUseBufferingIfDecodebinIsNull)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingUseBuffering = kUseBuffering; });
    EXPECT_FALSE(m_sut->setUseBuffering());
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetUseBuffering)
{
    GstElement decoder{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.pendingUseBuffering = kUseBuffering;
            context.playbackGroup.m_curAudioDecodeBin = &decoder;
        });

    EXPECT_CALL(*m_glibWrapperMock, gObjectSetBoolStub(_, StrEq(kUseBufferingStr.c_str()), kUseBuffering));
    EXPECT_TRUE(m_sut->setUseBuffering());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetRenderFrameIfSinkIsNull)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingRenderFrame = true; });
    EXPECT_FALSE(m_sut->setRenderFrame());
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToSetRenderFrameIfPropertyDoesntExist)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingRenderFrame = true; });

    expectGetAVSink(kVideoSinkStr, m_realElement);

    expectPropertyDoesntExist(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kFrameStepOnPrerollStr);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_FALSE(m_sut->setRenderFrame());
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetRenderFrame)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingRenderFrame = true; });

    expectGetAVSink(kVideoSinkStr, m_realElement);

    expectSetProperty(m_glibWrapperMock, m_gstWrapperMock, m_realElement, kFrameStepOnPrerollStr, 1);

    EXPECT_CALL(*m_gstWrapperMock, gstEventNewStep(GST_FORMAT_BUFFERS, 1, 1.0, true, false)).WillOnce(Return(&m_event));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetIntStub(_, StrEq(kFrameStepOnPrerollStr.c_str()), 0)).Times(1);
    EXPECT_CALL(*m_gstWrapperMock, gstElementSendEvent(m_realElement, &m_event));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)).Times(1);
    EXPECT_TRUE(m_sut->setRenderFrame());
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotifyNeedAudioData)
{
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].isDataNeeded = true; });

    EXPECT_CALL(m_gstPlayerClient, notifyNeedMediaData(MediaSourceType::AUDIO)).WillOnce(Return(true));
    m_sut->notifyNeedMediaData(MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotifyNeedVideoData)
{
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].isDataNeeded = true; });

    EXPECT_CALL(m_gstPlayerClient, notifyNeedMediaData(MediaSourceType::VIDEO)).WillOnce(Return(true));
    m_sut->notifyNeedMediaData(MediaSourceType::VIDEO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotifyNeedAudioDataWithDelay)
{
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].isDataNeeded = true; });

    EXPECT_CALL(m_gstPlayerClient, notifyNeedMediaDataWithDelay(MediaSourceType::AUDIO)).WillOnce(Return(true));
    m_sut->notifyNeedMediaDataWithDelay(MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotNotifyNeedAudioDataWhenNotNeeded)
{
    m_sut->notifyNeedMediaData(MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotNotifyNeedVideoDataWhenNotNeeded)
{
    m_sut->notifyNeedMediaData(MediaSourceType::VIDEO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotNotifyNeedAudioDataWithDelayWhenNotNeeded)
{
    m_sut->notifyNeedMediaDataWithDelay(MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldCreateClearGstBuffer)
{
    GstBuffer buffer{};
    IMediaPipeline::MediaSegmentVideo mediaSegment{kSourceId, kTimeStamp, kDuration, kWidth, kHeight, kFrameRate};
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewAllocate(nullptr, mediaSegment.getDataLength(), nullptr))
        .WillOnce(Return(&buffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferFill(&buffer, 0, mediaSegment.getData(), mediaSegment.getDataLength()));
    m_sut->createBuffer(mediaSegment);
    EXPECT_EQ(GST_BUFFER_TIMESTAMP(&buffer), kTimeStamp);
    EXPECT_EQ(GST_BUFFER_DURATION(&buffer), kDuration);
}

TEST_F(GstGenericPlayerPrivateTest, shouldCreateCENSEncryptedGstBuffer)
{
    GstBuffer buffer{}, initVectorBuffer{}, keyIdBuffer{}, subSamplesBuffer{};
    guint8 subSamplesData{0};
    auto subSamplesSize{sizeof(guint16) + sizeof(guint32)};
    IMediaPipeline::MediaSegmentVideo mediaSegment{kSourceId, kTimeStamp, kDuration, kWidth, kHeight, kFrameRate};
    GstMeta meta;
    mediaSegment.setEncrypted(true);
    mediaSegment.setMediaKeySessionId(kMediaKeySessionId);
    mediaSegment.setKeyId(kKeyId);
    mediaSegment.setInitVector(kInitVector);
    mediaSegment.addSubSample(kNumClearBytes, kNumEncryptedBytes);
    mediaSegment.setInitWithLast15(kInitWithLast15);
    mediaSegment.setCipherMode(kCipherMode);
    mediaSegment.setEncryptionPattern(kCrypt, kSkip);
    testing::InSequence s;
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewAllocate(nullptr, mediaSegment.getDataLength(), nullptr))
        .WillOnce(Return(&buffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferFill(&buffer, 0, mediaSegment.getData(), mediaSegment.getDataLength()));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewAllocate(nullptr, mediaSegment.getKeyId().size(), nullptr))
        .WillOnce(Return(&keyIdBuffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferFill(&keyIdBuffer, 0, _, mediaSegment.getKeyId().size()));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewAllocate(nullptr, mediaSegment.getInitVector().size(), nullptr))
        .WillOnce(Return(&initVectorBuffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferFill(&initVectorBuffer, 0, _, mediaSegment.getInitVector().size()));
    EXPECT_CALL(*m_glibWrapperMock, gMalloc(subSamplesSize)).WillOnce(Return(&subSamplesData));
    EXPECT_CALL(*m_gstWrapperMock, gstByteWriterInitWithData(_, &subSamplesData, subSamplesSize, FALSE));
    EXPECT_CALL(*m_gstWrapperMock, gstByteWriterPutUint16Be(_, kNumClearBytes));
    EXPECT_CALL(*m_gstWrapperMock, gstByteWriterPutUint32Be(_, kNumEncryptedBytes));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewWrapped(&subSamplesData, subSamplesSize)).WillOnce(Return(&subSamplesBuffer));
    GstRialtoProtectionData data = {mediaSegment.getMediaKeySessionId(),
                                    static_cast<uint32_t>(mediaSegment.getSubSamples().size()),
                                    mediaSegment.getInitWithLast15(),
                                    &keyIdBuffer,
                                    &initVectorBuffer,
                                    &subSamplesBuffer,
                                    kCipherMode,
                                    kCrypt,
                                    kSkip,
                                    true,
                                    &m_decryptionServiceMock};
    EXPECT_CALL(*m_gstProtectionMetadataWrapperMock, addProtectionMetadata(&buffer, data)).WillOnce(Return(&meta));

    m_sut->createBuffer(mediaSegment);
    EXPECT_EQ(GST_BUFFER_TIMESTAMP(&buffer), kTimeStamp);
    EXPECT_EQ(GST_BUFFER_DURATION(&buffer), kDuration);
}

TEST_F(GstGenericPlayerPrivateTest, shouldCreateCENCEncryptedGstBuffer)
{
    GstBuffer buffer{}, initVectorBuffer{}, keyIdBuffer{}, subSamplesBuffer{};
    guint8 subSamplesData{0};
    auto subSamplesSize{sizeof(guint16) + sizeof(guint32)};
    firebolt::rialto::CipherMode cipherMode{firebolt::rialto::CipherMode::CENC};
    IMediaPipeline::MediaSegmentVideo mediaSegment{kSourceId, kTimeStamp, kDuration, kWidth, kHeight, kFrameRate};
    GstMeta meta;
    mediaSegment.setEncrypted(true);
    mediaSegment.setMediaKeySessionId(kMediaKeySessionId);
    mediaSegment.setKeyId(kKeyId);
    mediaSegment.setInitVector(kInitVector);
    mediaSegment.addSubSample(kNumClearBytes, kNumEncryptedBytes);
    mediaSegment.setInitWithLast15(kInitWithLast15);
    mediaSegment.setCipherMode(cipherMode);
    testing::InSequence s;
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewAllocate(nullptr, mediaSegment.getDataLength(), nullptr))
        .WillOnce(Return(&buffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferFill(&buffer, 0, mediaSegment.getData(), mediaSegment.getDataLength()));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewAllocate(nullptr, mediaSegment.getKeyId().size(), nullptr))
        .WillOnce(Return(&keyIdBuffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferFill(&keyIdBuffer, 0, _, mediaSegment.getKeyId().size()));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewAllocate(nullptr, mediaSegment.getInitVector().size(), nullptr))
        .WillOnce(Return(&initVectorBuffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferFill(&initVectorBuffer, 0, _, mediaSegment.getInitVector().size()));
    EXPECT_CALL(*m_glibWrapperMock, gMalloc(subSamplesSize)).WillOnce(Return(&subSamplesData));
    EXPECT_CALL(*m_gstWrapperMock, gstByteWriterInitWithData(_, &subSamplesData, subSamplesSize, FALSE));
    EXPECT_CALL(*m_gstWrapperMock, gstByteWriterPutUint16Be(_, kNumClearBytes));
    EXPECT_CALL(*m_gstWrapperMock, gstByteWriterPutUint32Be(_, kNumEncryptedBytes));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewWrapped(&subSamplesData, subSamplesSize)).WillOnce(Return(&subSamplesBuffer));
    GstRialtoProtectionData data = {mediaSegment.getMediaKeySessionId(),
                                    static_cast<uint32_t>(mediaSegment.getSubSamples().size()),
                                    mediaSegment.getInitWithLast15(),
                                    &keyIdBuffer,
                                    &initVectorBuffer,
                                    &subSamplesBuffer,
                                    cipherMode,
                                    0,
                                    0,
                                    false,
                                    &m_decryptionServiceMock};
    EXPECT_CALL(*m_gstProtectionMetadataWrapperMock, addProtectionMetadata(&buffer, data)).WillOnce(Return(&meta));

    m_sut->createBuffer(mediaSegment);
    EXPECT_EQ(GST_BUFFER_TIMESTAMP(&buffer), kTimeStamp);
    EXPECT_EQ(GST_BUFFER_DURATION(&buffer), kDuration);
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToAddProtectionMetadata)
{
    GstBuffer buffer{}, initVectorBuffer{}, keyIdBuffer{}, subSamplesBuffer{};
    guint8 subSamplesData{0};
    auto subSamplesSize{sizeof(guint16) + sizeof(guint32)};
    IMediaPipeline::MediaSegmentVideo mediaSegment{kSourceId, kTimeStamp, kDuration, kWidth, kHeight, kFrameRate};
    mediaSegment.setEncrypted(true);
    mediaSegment.setMediaKeySessionId(kMediaKeySessionId);
    mediaSegment.setKeyId(kKeyId);
    mediaSegment.setInitVector(kInitVector);
    mediaSegment.addSubSample(kNumClearBytes, kNumEncryptedBytes);
    mediaSegment.setInitWithLast15(kInitWithLast15);
    mediaSegment.setCipherMode(kCipherMode);
    mediaSegment.setEncryptionPattern(kCrypt, kSkip);
    testing::InSequence s;
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewAllocate(nullptr, mediaSegment.getDataLength(), nullptr))
        .WillOnce(Return(&buffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferFill(&buffer, 0, mediaSegment.getData(), mediaSegment.getDataLength()));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewAllocate(nullptr, mediaSegment.getKeyId().size(), nullptr))
        .WillOnce(Return(&keyIdBuffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferFill(&keyIdBuffer, 0, _, mediaSegment.getKeyId().size()));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewAllocate(nullptr, mediaSegment.getInitVector().size(), nullptr))
        .WillOnce(Return(&initVectorBuffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferFill(&initVectorBuffer, 0, _, mediaSegment.getInitVector().size()));
    EXPECT_CALL(*m_glibWrapperMock, gMalloc(subSamplesSize)).WillOnce(Return(&subSamplesData));
    EXPECT_CALL(*m_gstWrapperMock, gstByteWriterInitWithData(_, &subSamplesData, subSamplesSize, FALSE));
    EXPECT_CALL(*m_gstWrapperMock, gstByteWriterPutUint16Be(_, kNumClearBytes));
    EXPECT_CALL(*m_gstWrapperMock, gstByteWriterPutUint32Be(_, kNumEncryptedBytes));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewWrapped(&subSamplesData, subSamplesSize)).WillOnce(Return(&subSamplesBuffer));
    GstRialtoProtectionData data = {mediaSegment.getMediaKeySessionId(),
                                    static_cast<uint32_t>(mediaSegment.getSubSamples().size()),
                                    mediaSegment.getInitWithLast15(),
                                    &keyIdBuffer,
                                    &initVectorBuffer,
                                    &subSamplesBuffer,
                                    kCipherMode,
                                    kCrypt,
                                    kSkip,
                                    true,
                                    &m_decryptionServiceMock};
    EXPECT_CALL(*m_gstProtectionMetadataWrapperMock, addProtectionMetadata(&buffer, data)).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&keyIdBuffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&initVectorBuffer));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&subSamplesBuffer));

    m_sut->createBuffer(mediaSegment);
    EXPECT_EQ(GST_BUFFER_TIMESTAMP(&buffer), kTimeStamp);
    EXPECT_EQ(GST_BUFFER_DURATION(&buffer), kDuration);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotAttachAudioDataWhenItIsNotNeeded)
{
    GstBuffer buffer{};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].buffers.emplace_back(&buffer); });
    m_sut->attachData(firebolt::rialto::MediaSourceType::AUDIO);
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&buffer)); // In destructor
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotAttachAudioWhenSourceIsNotPresent)
{
    m_sut->attachData(firebolt::rialto::MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAttachAudioData)
{
    GstBuffer buffer{};
    GstAppSrc audioSrc{};
    GstAppSrc videoSrc{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].isDataNeeded = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAttachAudioDataWhenAttachingSampleFails)
{
    constexpr std::int64_t kPosition{124};
    constexpr double kRate{1.0};
    constexpr double kAppliedRate{1.0};
    constexpr uint64_t kStopPosition{3453425};
    GstBuffer buffer{};
    GstAppSrc audioSrc{};
    GstAppSrc videoSrc{};
    GstSegment segment{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].isDataNeeded = true;
            context.playbackRate = kRate;
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
            context.initialPositions[GST_ELEMENT(&audioSrc)].emplace_back(
                SegmentData{kPosition, kResetTime, kAppliedRate, kStopPosition});
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
        });
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentNew()).WillOnce(Return(&segment));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentInit(&segment, GST_FORMAT_TIME));
    EXPECT_CALL(*m_gstWrapperMock,
                gstSegmentDoSeek(&segment, kRate, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, kPosition,
                                 GST_SEEK_TYPE_SET, kStopPosition, nullptr))
        .WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentFree(&segment));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAttachAudioSample)
{
    constexpr std::int64_t kPosition{124};
    constexpr double kRate{1.0};
    constexpr bool kDoNotResetTime{false};
    constexpr double kAppliedRate{1.0};
    constexpr uint64_t kStopPosition{3453425};
    GstBuffer buffer{};
    GstAppSrc audioSrc{};
    GstAppSrc videoSrc{};
    GstSegment segment{};
    GstSample *sample{nullptr};
    GstCaps caps{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].isDataNeeded = true;
            context.playbackRate = kRate;
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
            context.initialPositions[GST_ELEMENT(&audioSrc)].emplace_back(
                SegmentData{kPosition, kDoNotResetTime, kAppliedRate, kStopPosition});
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(&audioSrc)).WillOnce(Return(&caps));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentNew()).WillOnce(Return(&segment));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentInit(&segment, GST_FORMAT_TIME));
    EXPECT_CALL(*m_gstWrapperMock,
                gstSegmentDoSeek(&segment, kRate, GST_FORMAT_TIME, GST_SEEK_FLAG_NONE, GST_SEEK_TYPE_SET, kPosition,
                                 GST_SEEK_TYPE_SET, kStopPosition, nullptr))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstSampleNew(nullptr, &caps, &segment, nullptr)).WillOnce(Return(sample));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushSample(&audioSrc, sample));
    EXPECT_CALL(*m_gstWrapperMock, gstSampleUnref(sample));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentFree(&segment));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&caps));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::AUDIO);
    EXPECT_EQ(segment.applied_rate, kAppliedRate);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAttachAdditionalAudioSample)
{
    constexpr std::int64_t kPosition{124};
    constexpr double kRate{1.0};
    constexpr bool kResetTime{true};
    constexpr double kAppliedRate{1.0};
    constexpr uint64_t kStopPosition{3453425};
    GstBuffer buffer{};
    GstAppSrc audioSrc{};
    GstAppSrc videoSrc{};
    GstSegment segment{};
    GstSample *sample{nullptr};
    GstCaps caps{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].isDataNeeded = true;
            context.playbackRate = kRate;
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
            context.initialPositions[GST_ELEMENT(&audioSrc)].emplace_back(
                SegmentData{kPosition, kResetTime, kAppliedRate, kStopPosition});
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
            context.currentPosition[GST_ELEMENT(&audioSrc)] =
                SegmentData{kPosition, kResetTime, kAppliedRate, kStopPosition};
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(&audioSrc)).WillRepeatedly(Return(&caps));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentNew()).WillRepeatedly(Return(&segment));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentInit(&segment, GST_FORMAT_TIME)).Times(2);
    EXPECT_CALL(*m_gstWrapperMock,
                gstSegmentDoSeek(&segment, kRate, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, kPosition,
                                 GST_SEEK_TYPE_SET, kStopPosition, nullptr))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock,
                gstSegmentDoSeek(&segment, kRate, GST_FORMAT_TIME, GST_SEEK_FLAG_NONE, GST_SEEK_TYPE_SET, kPosition,
                                 GST_SEEK_TYPE_SET, kStopPosition, nullptr))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstSampleNew(nullptr, &caps, &segment, nullptr)).WillRepeatedly(Return(sample));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushSample(&audioSrc, sample)).Times(2);
    EXPECT_CALL(*m_gstWrapperMock, gstSampleUnref(sample)).Times(2);
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentFree(&segment)).Times(2);
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&caps)).Times(2);
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::AUDIO);
    EXPECT_EQ(segment.applied_rate, kAppliedRate);
}

TEST_F(GstGenericPlayerPrivateTest, undefinedStopPositionInSetSourcePosition)
{
    constexpr std::int64_t kPosition{124};
    constexpr double kRate{1.0};
    constexpr bool kDoNotResetTime{false};
    constexpr double kAppliedRate{1.0};
    GstBuffer buffer{};
    GstAppSrc audioSrc{};
    GstAppSrc videoSrc{};
    GstSegment segment{};
    GstSample *sample{nullptr};
    GstCaps caps{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].isDataNeeded = true;
            context.playbackRate = kRate;
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
            context.initialPositions[GST_ELEMENT(&audioSrc)].emplace_back(
                SegmentData{kPosition, kDoNotResetTime, kAppliedRate, kUndefinedPosition});
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(&audioSrc)).WillOnce(Return(&caps));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentNew()).WillOnce(Return(&segment));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentInit(&segment, GST_FORMAT_TIME));
    EXPECT_CALL(*m_gstWrapperMock,
                gstSegmentDoSeek(&segment, kRate, GST_FORMAT_TIME, GST_SEEK_FLAG_NONE, GST_SEEK_TYPE_SET, kPosition,
                                 GST_SEEK_TYPE_SET, GST_CLOCK_TIME_NONE, nullptr))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstSampleNew(nullptr, &caps, &segment, nullptr)).WillOnce(Return(sample));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushSample(&audioSrc, sample));
    EXPECT_CALL(*m_gstWrapperMock, gstSampleUnref(sample));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentFree(&segment));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&caps));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldPushSubtitleBuffer)
{
    GstBuffer buffer{};
    GstAppSrc subSrc{};
    GstAppSrc videoSrc{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::SUBTITLE].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::SUBTITLE].isDataNeeded = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::SUBTITLE].appSrc = GST_ELEMENT(&subSrc);
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::SUBTITLE);
}

TEST_F(GstGenericPlayerPrivateTest, shouldPushSubtitleBufferAndSetPosition)
{
    constexpr std::int64_t kPosition{124};
    constexpr bool kDoNotResetTime{false};
    constexpr double kAppliedRate{1.0};
    constexpr uint64_t kStopPosition{3453425};
    GstBuffer buffer{};
    GstAppSrc subSrc{};
    GstAppSrc videoSrc{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::SUBTITLE].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::SUBTITLE].isDataNeeded = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::SUBTITLE].appSrc = GST_ELEMENT(&subSrc);
            context.initialPositions[GST_ELEMENT(&subSrc)].emplace_back(
                SegmentData{kPosition, kDoNotResetTime, kAppliedRate, kStopPosition});
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
        });
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(_, StrEq("position")));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::SUBTITLE);
}

TEST_F(GstGenericPlayerPrivateTest, shouldCancelAudioUnderflowAndResume)
{
    GstBuffer buffer{};
    GstAppSrc audioSrc{};
    GstAppSrc videoSrc{};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    GenericPlayerContext *playerContext;
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            playerContext = &context;
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].isDataNeeded = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].underflowOccured = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));

    m_sut->attachData(firebolt::rialto::MediaSourceType::AUDIO);
    EXPECT_FALSE(playerContext->streamInfo[firebolt::rialto::MediaSourceType::AUDIO].underflowOccured);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotAttachVideoDataWhenBuffersAreEmpty)
{
    m_sut->attachData(firebolt::rialto::MediaSourceType::VIDEO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotAttachVideoDataWhenItIsNotNeeded)
{
    GstBuffer buffer{};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].buffers.emplace_back(&buffer); });
    m_sut->attachData(firebolt::rialto::MediaSourceType::VIDEO);
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&buffer)); // In destructor
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotAttachVideoWhenSourceIsNotPresent)
{
    m_sut->attachData(firebolt::rialto::MediaSourceType::VIDEO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAttachVideoData)
{
    GstBuffer buffer{};
    GstAppSrc audioSrc{};
    GstAppSrc videoSrc{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].isDataNeeded = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::VIDEO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAttachVideoSample)
{
    constexpr std::int64_t kPosition{124};
    constexpr double kRate{1.0};
    constexpr double kAppliedRate{2.0};
    constexpr uint64_t kStopPosition{3453425};
    GstBuffer buffer{};
    GstAppSrc audioSrc{};
    GstAppSrc videoSrc{};
    GstSegment segment{};
    GstSample *sample{nullptr};
    GstCaps caps{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].isDataNeeded = true;
            context.playbackRate = kRate;
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
            context.initialPositions[GST_ELEMENT(&videoSrc)].emplace_back(
                SegmentData{kPosition, kResetTime, kAppliedRate, kStopPosition});
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(&videoSrc)).WillOnce(Return(&caps));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentNew()).WillOnce(Return(&segment));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentInit(&segment, GST_FORMAT_TIME));
    EXPECT_CALL(*m_gstWrapperMock,
                gstSegmentDoSeek(&segment, kRate, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, kPosition,
                                 GST_SEEK_TYPE_SET, kStopPosition, nullptr))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstSampleNew(nullptr, &caps, &segment, nullptr)).WillOnce(Return(sample));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushSample(&videoSrc, sample));
    EXPECT_CALL(*m_gstWrapperMock, gstSampleUnref(sample));
    EXPECT_CALL(*m_gstWrapperMock, gstSegmentFree(&segment));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&caps));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::VIDEO);
    EXPECT_EQ(segment.applied_rate, kAppliedRate);
}

TEST_F(GstGenericPlayerPrivateTest, shouldCancelVideoUnderflowAndResume)
{
    GstBuffer buffer{};
    GstAppSrc audioSrc{};
    GstAppSrc videoSrc{};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    GenericPlayerContext *playerContext;
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            playerContext = &context;
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].isDataNeeded = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].underflowOccured = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));

    m_sut->attachData(firebolt::rialto::MediaSourceType::VIDEO);
    EXPECT_FALSE(playerContext->streamInfo[firebolt::rialto::MediaSourceType::VIDEO].underflowOccured);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotCancelVideoUnderflowWhenAudioUnderflowIsActive)
{
    GstBuffer buffer{};
    GstAppSrc videoSrc{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].buffers.emplace_back(&buffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].isDataNeeded = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].underflowOccured = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].underflowOccured = true;
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &buffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::VIDEO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAttachAudioAndVideoData)
{
    GstBuffer audioBuffer{};
    GstBuffer videoBuffer{};
    GstAppSrc audioSrc{};
    GstAppSrc videoSrc{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].buffers.emplace_back(&audioBuffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].buffers.emplace_back(&videoBuffer);
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].isDataNeeded = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].isDataNeeded = true;
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
            context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc);
        });
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &audioBuffer));
    m_sut->attachData(firebolt::rialto::MediaSourceType::AUDIO);
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcPushBuffer(_, &videoBuffer));
    EXPECT_CALL(m_gstPlayerClient, notifyNetworkState(NetworkState::BUFFERED));
    m_sut->attachData(firebolt::rialto::MediaSourceType::VIDEO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateAudioCaps)
{
    GstAppSrc audioSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    GstBuffer buf;
    gpointer memory{nullptr};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("rate"), G_TYPE_INT, kSampleRate));
    EXPECT_CALL(*m_gstWrapperMock,
                gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("channels"), G_TYPE_INT, kNumberOfChannels));
    EXPECT_CALL(*m_glibWrapperMock, gMemdup(arrayMatcher(kCodecDataWithBuffer->data), kCodecDataWithBuffer->data.size()))
        .WillOnce(Return(memory));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewWrapped(memory, kCodecDataWithBuffer->data.size())).WillOnce(Return(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleBufferStub(&dummyCaps2, StrEq("codec_data"), GST_TYPE_BUFFER, &buf));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&audioSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateAudioCaps(kSampleRate, kNumberOfChannels, kCodecDataWithBuffer);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateAudioCapsWithStringCodecData)
{
    GstAppSrc audioSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("rate"), G_TYPE_INT, kSampleRate));
    EXPECT_CALL(*m_gstWrapperMock,
                gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("channels"), G_TYPE_INT, kNumberOfChannels));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleStringStub(&dummyCaps2, StrEq("codec_data"), G_TYPE_STRING,
                                                              StrEq(kCodecDataStr.c_str())));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&audioSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateAudioCaps(kSampleRate, kNumberOfChannels, kCodecDataWithString);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateAudioCapsSampleRateOnly)
{
    GstAppSrc audioSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("rate"), G_TYPE_INT, kSampleRate));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&audioSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateAudioCaps(kSampleRate, kInvalidNumberOfChannels, kEmptyCodecData);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateAudioCapsNumOfChannelsOnly)
{
    GstAppSrc audioSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock,
                gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("channels"), G_TYPE_INT, kNumberOfChannels));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&audioSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateAudioCaps(kInvalidSampleRate, kNumberOfChannels, kEmptyCodecData);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateAudioCapsCodecDataOnly)
{
    GstAppSrc audioSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    GstBuffer buf;
    gpointer memory{nullptr};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_glibWrapperMock, gMemdup(arrayMatcher(kCodecDataWithBuffer->data), kCodecDataWithBuffer->data.size()))
        .WillOnce(Return(memory));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewWrapped(memory, kCodecDataWithBuffer->data.size())).WillOnce(Return(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleBufferStub(&dummyCaps2, StrEq("codec_data"), GST_TYPE_BUFFER, &buf));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&audioSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateAudioCaps(kInvalidSampleRate, kInvalidNumberOfChannels, kCodecDataWithBuffer);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotUpdateAudioCapsWhenValuesAreInvalid)
{
    GstAppSrc audioSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateAudioCaps(kInvalidSampleRate, kInvalidNumberOfChannels, kEmptyCodecData);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotUpdateAudioCapsWhenNoSrc)
{
    m_sut->updateAudioCaps(kSampleRate, kNumberOfChannels, kEmptyCodecData);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateVideoCaps)
{
    GstAppSrc videoSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    GstBuffer buf;
    gpointer memory{nullptr};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&videoSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("width"), G_TYPE_INT, kWidth));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("height"), G_TYPE_INT, kHeight));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleFractionStub(&dummyCaps2, StrEq("framerate"), GST_TYPE_FRACTION,
                                                                kFrameRate.numerator, kFrameRate.denominator));
    EXPECT_CALL(*m_glibWrapperMock, gMemdup(arrayMatcher(kCodecDataWithBuffer->data), kCodecDataWithBuffer->data.size()))
        .WillOnce(Return(memory));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewWrapped(memory, kCodecDataWithBuffer->data.size())).WillOnce(Return(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleBufferStub(&dummyCaps2, StrEq("codec_data"), GST_TYPE_BUFFER, &buf));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&videoSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateVideoCaps(kWidth, kHeight, kFrameRate, kCodecDataWithBuffer);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateVideoCapsWithStringCodecData)
{
    GstAppSrc videoSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&videoSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("width"), G_TYPE_INT, kWidth));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("height"), G_TYPE_INT, kHeight));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleFractionStub(&dummyCaps2, StrEq("framerate"), GST_TYPE_FRACTION,
                                                                kFrameRate.numerator, kFrameRate.denominator));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleStringStub(&dummyCaps2, StrEq("codec_data"), G_TYPE_STRING,
                                                              StrEq(kCodecDataStr.c_str())));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&videoSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateVideoCaps(kWidth, kHeight, kFrameRate, kCodecDataWithString);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateVideoCapsWithoutCodecData)
{
    GstAppSrc videoSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&videoSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("width"), G_TYPE_INT, kWidth));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("height"), G_TYPE_INT, kHeight));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleFractionStub(&dummyCaps2, StrEq("framerate"), GST_TYPE_FRACTION,
                                                                kFrameRate.numerator, kFrameRate.denominator));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&videoSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateVideoCaps(kWidth, kHeight, kFrameRate, kEmptyCodecData);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateVideoCapsWithoutFrameRate)
{
    GstAppSrc videoSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    GstBuffer buf;
    gpointer memory{nullptr};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&videoSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("width"), G_TYPE_INT, kWidth));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("height"), G_TYPE_INT, kHeight));
    EXPECT_CALL(*m_glibWrapperMock, gMemdup(arrayMatcher(kCodecDataWithBuffer->data), kCodecDataWithBuffer->data.size()))
        .WillOnce(Return(memory));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewWrapped(memory, kCodecDataWithBuffer->data.size())).WillOnce(Return(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleBufferStub(&dummyCaps2, StrEq("codec_data"), GST_TYPE_BUFFER, &buf));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&videoSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    Fraction frameRate{kUndefinedSize, kUndefinedSize};
    m_sut->updateVideoCaps(kWidth, kHeight, frameRate, kCodecDataWithBuffer);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateVideoCapsWithoutWidth)
{
    GstAppSrc videoSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    GstBuffer buf;
    gpointer memory{nullptr};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&videoSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("height"), G_TYPE_INT, kHeight));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleFractionStub(&dummyCaps2, StrEq("framerate"), GST_TYPE_FRACTION,
                                                                kFrameRate.numerator, kFrameRate.denominator));
    EXPECT_CALL(*m_glibWrapperMock, gMemdup(arrayMatcher(kCodecDataWithBuffer->data), kCodecDataWithBuffer->data.size()))
        .WillOnce(Return(memory));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewWrapped(memory, kCodecDataWithBuffer->data.size())).WillOnce(Return(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleBufferStub(&dummyCaps2, StrEq("codec_data"), GST_TYPE_BUFFER, &buf));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&videoSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateVideoCaps(0, kHeight, kFrameRate, kCodecDataWithBuffer);
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdateVideoCapsWithoutHeight)
{
    GstAppSrc videoSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    GstBuffer buf;
    gpointer memory{nullptr};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&videoSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("width"), G_TYPE_INT, kWidth));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleFractionStub(&dummyCaps2, StrEq("framerate"), GST_TYPE_FRACTION,
                                                                kFrameRate.numerator, kFrameRate.denominator));
    EXPECT_CALL(*m_glibWrapperMock, gMemdup(arrayMatcher(kCodecDataWithBuffer->data), kCodecDataWithBuffer->data.size()))
        .WillOnce(Return(memory));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewWrapped(memory, kCodecDataWithBuffer->data.size())).WillOnce(Return(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleBufferStub(&dummyCaps2, StrEq("codec_data"), GST_TYPE_BUFFER, &buf));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcSetCaps(GST_APP_SRC(&videoSrc), &dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateVideoCaps(kWidth, 0, kFrameRate, kCodecDataWithBuffer);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotUpdateVideoCapsNoChange)
{
    GstAppSrc videoSrc{};
    GstCaps dummyCaps1;
    GstCaps dummyCaps2;
    GstBuffer buf;
    gpointer memory{nullptr};
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc); });

    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&videoSrc))).WillOnce(Return(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCopy(&dummyCaps1)).WillOnce(Return(&dummyCaps2));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("width"), G_TYPE_INT, kWidth));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&dummyCaps2, StrEq("height"), G_TYPE_INT, kHeight));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleFractionStub(&dummyCaps2, StrEq("framerate"), GST_TYPE_FRACTION,
                                                                kFrameRate.numerator, kFrameRate.denominator));
    EXPECT_CALL(*m_glibWrapperMock, gMemdup(arrayMatcher(kCodecDataWithBuffer->data), kCodecDataWithBuffer->data.size()))
        .WillOnce(Return(memory));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferNewWrapped(memory, kCodecDataWithBuffer->data.size())).WillOnce(Return(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleBufferStub(&dummyCaps2, StrEq("codec_data"), GST_TYPE_BUFFER, &buf));
    EXPECT_CALL(*m_gstWrapperMock, gstBufferUnref(&buf));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&dummyCaps1, &dummyCaps2)).WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps1));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&dummyCaps2));

    m_sut->updateVideoCaps(kWidth, kHeight, kFrameRate, kCodecDataWithBuffer);
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotUpdateAudioVideoCapsWhenNoSrc)
{
    m_sut->updateVideoCaps(kWidth, kHeight, kFrameRate, kEmptyCodecData);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAddClippingMetaWhenStartAndEndNotZero)
{
    GstBuffer buf;
    uint64_t clippingStart{1024};
    uint64_t clippingEnd{2048};
    GstAudioClippingMeta clippingMeta{};

    EXPECT_CALL(*m_gstWrapperMock, gstBufferAddAudioClippingMeta(&buf, GST_FORMAT_TIME, clippingStart, clippingEnd))
        .WillOnce(Return(&clippingMeta));
    m_sut->addAudioClippingToBuffer(&buf, clippingStart, clippingEnd);
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToAddClipping)
{
    GstBuffer buf;
    uint64_t clippingStart{1024};
    uint64_t clippingEnd{2048};

    EXPECT_CALL(*m_gstWrapperMock, gstBufferAddAudioClippingMeta(&buf, GST_FORMAT_TIME, clippingStart, clippingEnd))
        .WillOnce(Return(nullptr));
    m_sut->addAudioClippingToBuffer(&buf, clippingStart, clippingEnd);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAddClippingMetaWhenStartNotZero)
{
    GstBuffer buf;
    uint64_t clippingStart{1024};
    GstAudioClippingMeta clippingMeta{};

    EXPECT_CALL(*m_gstWrapperMock, gstBufferAddAudioClippingMeta(&buf, GST_FORMAT_TIME, clippingStart, 0))
        .WillOnce(Return(&clippingMeta));
    m_sut->addAudioClippingToBuffer(&buf, clippingStart, 0);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAddClippingMetaWhenEndNotZero)
{
    GstBuffer buf;
    uint64_t clippingEnd{2048};
    GstAudioClippingMeta clippingMeta{};

    EXPECT_CALL(*m_gstWrapperMock, gstBufferAddAudioClippingMeta(&buf, GST_FORMAT_TIME, 0, clippingEnd))
        .WillOnce(Return(&clippingMeta));
    m_sut->addAudioClippingToBuffer(&buf, 0, clippingEnd);
}

TEST_F(GstGenericPlayerPrivateTest, shouldAddClippingMetaWhenStartAndEndZero)
{
    GstBuffer buf;
    m_sut->addAudioClippingToBuffer(&buf, 0, 0);
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToChangePlaybackStateWhenPipelineIsNull)
{
    GstElement *pipelineCopy; // to make generic test destructor happy :-)
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            pipelineCopy = context.pipeline;
            context.pipeline = nullptr;
        });
    EXPECT_CALL(m_gstPlayerClient, notifyPlaybackState(PlaybackState::FAILURE));
    EXPECT_FALSE(m_sut->changePipelineState(GST_STATE_PLAYING));
    modifyContext([&](GenericPlayerContext &context) { context.pipeline = pipelineCopy; });
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToChangePlaybackStateWhenSetStateFails)
{
    EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(_, GST_STATE_PLAYING)).WillOnce(Return(GST_STATE_CHANGE_FAILURE));
    EXPECT_CALL(m_gstPlayerClient, notifyPlaybackState(PlaybackState::FAILURE));
    EXPECT_FALSE(m_sut->changePipelineState(GST_STATE_PLAYING));
}

TEST_F(GstGenericPlayerPrivateTest, shouldChangePlaybackState)
{
    EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(_, GST_STATE_PLAYING)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_TRUE(m_sut->changePipelineState(GST_STATE_PLAYING));
}

TEST_F(GstGenericPlayerPrivateTest, shouldStartPositionReportingTimer)
{
    std::unique_ptr<common::ITimer> audioUnderflowTimerMock = std::make_unique<StrictMock<TimerMock>>();
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPositionReportTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Return(ByMove(std::move(audioUnderflowTimerMock))));

    m_sut->startPositionReportingAndCheckAudioUnderflowTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldStartPlaybackInfoTimer)
{
    willNotifyPlaybackInfo();
    std::unique_ptr<common::ITimer> playbackInfoTimerMock = std::make_unique<StrictMock<TimerMock>>();
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPlaybackInfoTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Return(ByMove(std::move(playbackInfoTimerMock))));

    m_sut->startNotifyPlaybackInfoTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotifyPlaybackInfoWithAudioFade)
{
    willNotifyPlaybackInfoWithAudioFade();
    std::unique_ptr<common::ITimer> playbackInfoTimerMock = std::make_unique<StrictMock<TimerMock>>();
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPlaybackInfoTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Return(ByMove(std::move(playbackInfoTimerMock))));

    m_sut->startNotifyPlaybackInfoTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotStartPositionReportingTimerWhenItIsActive)
{
    std::unique_ptr<common::ITimer> timerMock = std::make_unique<StrictMock<TimerMock>>();
    EXPECT_CALL(dynamic_cast<StrictMock<TimerMock> &>(*timerMock), isActive()).WillOnce(Return(true));
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPositionReportTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Return(ByMove(std::move(timerMock))));

    m_sut->startPositionReportingAndCheckAudioUnderflowTimer();
    m_sut->startPositionReportingAndCheckAudioUnderflowTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotStartPlaybackInfoTimerWhenItIsActive)
{
    std::unique_ptr<common::ITimer> timerMock = std::make_unique<StrictMock<TimerMock>>();
    EXPECT_CALL(dynamic_cast<StrictMock<TimerMock> &>(*timerMock), isActive()).WillOnce(Return(true));
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPlaybackInfoTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Return(ByMove(std::move(timerMock))));
    willNotifyPlaybackInfo();
    m_sut->startNotifyPlaybackInfoTimer();
    m_sut->startNotifyPlaybackInfoTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldScheduleReportPositionWhenPositionReportingTimerIsFired)
{
    std::unique_ptr<common::ITimer> timerMock = std::make_unique<StrictMock<TimerMock>>();
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    std::unique_ptr<IPlayerTask> task2{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task2), execute());
    EXPECT_CALL(m_taskFactoryMock, createReportPosition(_, _)).WillOnce(Return(ByMove(std::move(task))));
    EXPECT_CALL(m_taskFactoryMock, createCheckAudioUnderflow(_, _)).WillOnce(Return(ByMove(std::move(task2))));
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPositionReportTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Invoke(
            [&](const std::chrono::milliseconds &timeout, const std::function<void()> &callback, common::TimerType timerType)
            {
                callback();
                return std::move(timerMock);
            }));
    m_sut->startPositionReportingAndCheckAudioUnderflowTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldSchedulePlaybackInfoWhenPlaybackInfoTimerIsFired)
{
    std::unique_ptr<common::ITimer> timerMock = std::make_unique<StrictMock<TimerMock>>();
    willNotifyPlaybackInfo();
    std::unique_ptr<common::ITimer> playbackInfoTimerMock = std::make_unique<StrictMock<TimerMock>>();
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPlaybackInfoTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Invoke(
            [&](const std::chrono::milliseconds &timeout, const std::function<void()> &callback, common::TimerType timerType)
            {
                willNotifyPlaybackInfo();
                callback();
                return std::move(playbackInfoTimerMock);
            }));
    m_sut->startNotifyPlaybackInfoTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldStopActivePositionReportingTimer)
{
    std::unique_ptr<common::ITimer> timerMock = std::make_unique<StrictMock<TimerMock>>();
    EXPECT_CALL(dynamic_cast<StrictMock<TimerMock> &>(*timerMock), isActive()).WillOnce(Return(true));
    EXPECT_CALL(dynamic_cast<StrictMock<TimerMock> &>(*timerMock), cancel());
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPositionReportTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Return(ByMove(std::move(timerMock))));

    m_sut->startPositionReportingAndCheckAudioUnderflowTimer();
    m_sut->stopPositionReportingAndCheckAudioUnderflowTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldStopActivePlaybackInfoTimerTimer)
{
    willNotifyPlaybackInfo();
    std::unique_ptr<common::ITimer> playbackInfoTimerMock = std::make_unique<StrictMock<TimerMock>>();
    EXPECT_CALL(dynamic_cast<StrictMock<TimerMock> &>(*playbackInfoTimerMock), isActive()).WillOnce(Return(true));
    EXPECT_CALL(dynamic_cast<StrictMock<TimerMock> &>(*playbackInfoTimerMock), cancel());
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPlaybackInfoTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Return(ByMove(std::move(playbackInfoTimerMock))));

    m_sut->startNotifyPlaybackInfoTimer();
    m_sut->stopNotifyPlaybackInfoTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotStopInactivePositionReportingTimer)
{
    std::unique_ptr<common::ITimer> timerMock = std::make_unique<StrictMock<TimerMock>>();
    EXPECT_CALL(dynamic_cast<StrictMock<TimerMock> &>(*timerMock), isActive()).WillOnce(Return(false));
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPositionReportTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Return(ByMove(std::move(timerMock))));

    m_sut->startPositionReportingAndCheckAudioUnderflowTimer();
    m_sut->stopPositionReportingAndCheckAudioUnderflowTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotStopInactivePlaybackInfoTimer)
{
    willNotifyPlaybackInfo();
    std::unique_ptr<common::ITimer> playbackInfoTimerMock = std::make_unique<StrictMock<TimerMock>>();
    EXPECT_CALL(dynamic_cast<StrictMock<TimerMock> &>(*playbackInfoTimerMock), isActive()).WillOnce(Return(false));
    EXPECT_CALL(*m_timerFactoryMock, createTimer(kPlaybackInfoTimerMs, _, common::TimerType::PERIODIC))
        .WillOnce(Return(ByMove(std::move(playbackInfoTimerMock))));

    m_sut->startNotifyPlaybackInfoTimer();
    m_sut->stopNotifyPlaybackInfoTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldNotStopInactivePositionReportingTimerWhenThereIsNoTimer)
{
    m_sut->stopPositionReportingAndCheckAudioUnderflowTimer();
}

TEST_F(GstGenericPlayerPrivateTest, shouldStopWorkerThread)
{
    EXPECT_CALL(m_workerThreadMock, stop());
    m_sut->stopWorkerThread();
}

TEST_F(GstGenericPlayerPrivateTest, shouldUpdatePlaybackGroup)
{
    GstElement typefind;
    GstCaps caps;
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(m_taskFactoryMock, createUpdatePlaybackGroup(_, _, &typefind, &caps))
        .WillOnce(Return(ByMove(std::move(task))));

    m_sut->updatePlaybackGroup(&typefind, &caps);
}

TEST_F(GstGenericPlayerPrivateTest, shouldScheduleAllSourcesAttached)
{
    std::unique_ptr<IPlayerTask> task{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*task), execute());
    EXPECT_CALL(m_taskFactoryMock, createFinishSetupSource(_, _)).WillOnce(Return(ByMove(std::move(task))));

    m_sut->scheduleAllSourcesAttached();
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToReattachSourceWhenSourceIsNotPresent)
{
    std::unique_ptr<firebolt::rialto::IMediaPipeline::MediaSource> source =
        std::make_unique<firebolt::rialto::IMediaPipeline::MediaSourceAudio>("audio/aac", false);
    EXPECT_FALSE(m_sut->reattachSource(source));
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToReattachAudioSourceWithEmptyMimeType)
{
    GstAppSrc audioSrc{};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });
    std::unique_ptr<IMediaPipeline::MediaSource> source =
        std::make_unique<firebolt::rialto::IMediaPipeline::MediaSourceAudio>("", false);
    EXPECT_FALSE(m_sut->reattachSource(source));
}

TEST_F(GstGenericPlayerPrivateTest, shouldFailToReattachVideoSource)
{
    GstAppSrc videoSrc{};
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::VIDEO].appSrc = GST_ELEMENT(&videoSrc); });
    std::unique_ptr<IMediaPipeline::MediaSource> source =
        std::make_unique<IMediaPipeline::MediaSourceVideo>("video/h264");
    EXPECT_FALSE(m_sut->reattachSource(source));
}

TEST_F(GstGenericPlayerPrivateTest, shouldSkipReattachingAudioSource)
{
    GstAppSrc audioSrc{};
    GstCaps newGstCaps{};
    GstCaps oldGstCaps{};
    setPipelineState(GST_STATE_PAUSED);
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
        });
    expectRefreshAudioPlaybackGroupHandlesEmpty();

    EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmptySimple(StrEq("audio/mpeg"))).WillOnce(Return(&newGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&newGstCaps, StrEq("mpegversion"), G_TYPE_INT, 4));
    EXPECT_CALL(*m_gstWrapperMock, gstStateLock(_)).WillOnce(Return());
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(_)).WillOnce(Return(GST_STATE_PAUSED));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetStateReturn(_)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_gstWrapperMock, gstStateUnlock(_)).WillOnce(Return());
    EXPECT_CALL(*m_gstWrapperMock, gstElementQueryPosition(_, GST_FORMAT_TIME, _)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&newGstCaps, &oldGstCaps)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&newGstCaps));

    std::unique_ptr<firebolt::rialto::IMediaPipeline::MediaSource> source =
        std::make_unique<firebolt::rialto::IMediaPipeline::MediaSourceAudio>("audio/aac", false);
    EXPECT_TRUE(m_sut->reattachSource(source));
}

TEST_F(GstGenericPlayerPrivateTest, shouldReattachMpegAudioSource)
{
    // After the ABI v5 fold, reattachSource delegates the codec switch to the platform backend: it
    // builds a neutral AudioCodecSwitchContext (the backend reads the playsink-bin name and picks the
    // amlhalasink fork or the rdk-gstreamer-utils path itself). The engine names no SoC.
    GstAppSrc audioSrc{};
    GstCaps newGstCaps{};
    GstCaps oldGstCaps{};
    setPipelineState(GST_STATE_PAUSED);
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });
    expectRefreshAudioPlaybackGroupHandlesEmpty();

    EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmptySimple(StrEq("audio/mpeg"))).WillOnce(Return(&newGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&newGstCaps, StrEq("mpegversion"), G_TYPE_INT, 4));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&newGstCaps, &oldGstCaps)).WillOnce(Return(FALSE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(_)).WillOnce(Return(GST_STATE_PAUSED));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetStateReturn(_)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_gstWrapperMock, gstStateLock(_)).WillOnce(Return());
    EXPECT_CALL(*m_gstWrapperMock, gstElementQueryPosition(_, GST_FORMAT_TIME, _)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstStateUnlock(_)).WillOnce(Return());
    EXPECT_CALL(*m_platformBackendMock,
                switchAudioCodec(Field(&AudioCodecSwitchContext::audioAppSrc, GST_ELEMENT(&audioSrc))))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&newGstCaps));

    std::unique_ptr<firebolt::rialto::IMediaPipeline::MediaSource> source =
        std::make_unique<firebolt::rialto::IMediaPipeline::MediaSourceAudio>("audio/aac", false);
    EXPECT_TRUE(m_sut->reattachSource(source));
}

TEST_F(GstGenericPlayerPrivateTest, shouldRefreshPlaybackGroupHandlesOnExplicitReattach)
{
    // On the explicit-construction path there are no playbin signals (deep-element-added / typefind
    // have-type) to populate the playback group the codec-switch machinery reads. reattachSource must
    // refresh the audio decoder/parser/typefind from the live decodebin before the switch and hand them
    // to the backend through the neutral AudioCodecSwitchContext.
    GstAppSrc audioSrc{};
    GstCaps newGstCaps{};
    GstCaps oldGstCaps{};
    GstElement decodebin{};
    GstElement audioDecoder{};
    GstElement audioParse{};
    GstElement audioTypefind{};
    GstIterator typefindIt{};
    gchar typefindName[]{"audiotypefind0"};
    setPipelineState(GST_STATE_PAUSED);
    firebolt::rialto::wrappers::PlaybackGroupPrivate *playbackGroup{nullptr};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
            context.playbackGroup.m_curAudioDecodeBin = &decodebin;
            playbackGroup = &context.playbackGroup;
        });

    // updateAudioPlaybackGroupHandles: getDecoder + getParser iterate the pipeline; getAudioTypefind
    // iterates the audio decodebin (a distinct iterator) and matches by name.
    EXPECT_CALL(*m_gstWrapperMock, gstBinIterateRecurse(GST_BIN(&m_pipeline)))
        .WillOnce(Return(&m_it))
        .WillOnce(Return(&m_it));
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorNext(&m_it, _))
        .WillOnce(Return(GST_ITERATOR_OK))
        .WillOnce(Return(GST_ITERATOR_OK));
    EXPECT_CALL(*m_glibWrapperMock, gValueGetObject(_))
        .WillOnce(Return(&audioDecoder))
        .WillOnce(Return(&audioParse))
        .WillOnce(Return(&audioTypefind));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetFactory(&audioDecoder)).WillOnce(Return(m_factory));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetFactory(&audioParse)).WillOnce(Return(m_factory));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListIsType(m_factory, (GST_ELEMENT_FACTORY_TYPE_DECODER |
                                                                           GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO)))
        .WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListIsType(m_factory, (GST_ELEMENT_FACTORY_TYPE_PARSER |
                                                                           GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO)))
        .WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstBinIterateRecurse(GST_BIN(&decodebin))).WillOnce(Return(&typefindIt));
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorNext(&typefindIt, _)).WillOnce(Return(GST_ITERATOR_OK));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetName(&audioTypefind)).WillOnce(Return(typefindName));
    EXPECT_CALL(*m_glibWrapperMock, gStrrstr(typefindName, StrEq("typefind"))).WillOnce(Return(typefindName));
    EXPECT_CALL(*m_glibWrapperMock, gFree(typefindName));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(&audioDecoder)).WillOnce(Return(&audioDecoder));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(&audioParse)).WillOnce(Return(&audioParse));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(&audioTypefind)).WillOnce(Return(&audioTypefind));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&audioDecoder));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&audioParse));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&audioTypefind));
    EXPECT_CALL(*m_glibWrapperMock, gValueUnset(_)).Times(4);
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorFree(&m_it)).Times(2);
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorFree(&typefindIt));

    // getPosition + caps comparison drive the switch.
    EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmptySimple(StrEq("audio/mpeg"))).WillOnce(Return(&newGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&newGstCaps, StrEq("mpegversion"), G_TYPE_INT, 4));
    EXPECT_CALL(*m_gstWrapperMock, gstStateLock(_)).WillOnce(Return());
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(_)).WillOnce(Return(GST_STATE_PAUSED));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetStateReturn(_)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_gstWrapperMock, gstStateUnlock(_)).WillOnce(Return());
    EXPECT_CALL(*m_gstWrapperMock, gstElementQueryPosition(_, GST_FORMAT_TIME, _)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&newGstCaps, &oldGstCaps)).WillOnce(Return(FALSE));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&newGstCaps));

    // The backend receives the now-populated live decoder/parser/typefind through the neutral context.
    EXPECT_CALL(*m_platformBackendMock,
                switchAudioCodec(AllOf(Field(&AudioCodecSwitchContext::audioDecoder, &audioDecoder),
                                       Field(&AudioCodecSwitchContext::audioParse, &audioParse),
                                       Field(&AudioCodecSwitchContext::audioTypefind, &audioTypefind),
                                       Field(&AudioCodecSwitchContext::audioDecodeBin, &decodebin))))
        .WillOnce(Return(true));

    std::unique_ptr<firebolt::rialto::IMediaPipeline::MediaSource> source =
        std::make_unique<firebolt::rialto::IMediaPipeline::MediaSourceAudio>("audio/aac", false);
    EXPECT_TRUE(m_sut->reattachSource(source));

    // The live decoder/parser/typefind were refreshed onto the playback group before the switch.
    EXPECT_EQ(playbackGroup->m_curAudioDecoder, &audioDecoder);
    EXPECT_EQ(playbackGroup->m_curAudioParse, &audioParse);
    EXPECT_EQ(playbackGroup->m_curAudioTypefind, &audioTypefind);
}

TEST_F(GstGenericPlayerPrivateTest, shouldReattachEac3AudioSource)
{
    GstAppSrc audioSrc{};
    GstCaps newGstCaps{};
    GstCaps oldGstCaps{};
    setPipelineState(GST_STATE_PAUSED);
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });
    expectRefreshAudioPlaybackGroupHandlesEmpty();

    EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmptySimple(StrEq("audio/x-eac3"))).WillOnce(Return(&newGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&newGstCaps, &oldGstCaps)).WillOnce(Return(FALSE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(_)).WillOnce(Return(GST_STATE_PAUSED));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetStateReturn(_)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_gstWrapperMock, gstStateLock(_)).WillOnce(Return());
    EXPECT_CALL(*m_gstWrapperMock, gstElementQueryPosition(_, GST_FORMAT_TIME, _)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstStateUnlock(_)).WillOnce(Return());
    EXPECT_CALL(*m_platformBackendMock,
                switchAudioCodec(Field(&AudioCodecSwitchContext::codecParam, StrEq("ec-3"))))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&newGstCaps));

    std::unique_ptr<firebolt::rialto::IMediaPipeline::MediaSource> source =
        std::make_unique<firebolt::rialto::IMediaPipeline::MediaSourceAudio>("audio/x-eac3", false);
    EXPECT_TRUE(m_sut->reattachSource(source));
}

TEST_F(GstGenericPlayerPrivateTest, shouldReattachRawAudioSource)
{
    GstAppSrc audioSrc{};
    GstCaps newGstCaps{};
    GstCaps oldGstCaps{};
    setPipelineState(GST_STATE_PAUSED);
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });
    expectRefreshAudioPlaybackGroupHandlesEmpty();

    EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmptySimple(StrEq("audio/x-raw"))).WillOnce(Return(&newGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&newGstCaps, &oldGstCaps)).WillOnce(Return(FALSE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(_)).WillOnce(Return(GST_STATE_PAUSED));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetStateReturn(_)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_gstWrapperMock, gstStateLock(_)).WillOnce(Return());
    EXPECT_CALL(*m_gstWrapperMock, gstElementQueryPosition(_, GST_FORMAT_TIME, _)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstStateUnlock(_)).WillOnce(Return());
    EXPECT_CALL(*m_platformBackendMock,
                switchAudioCodec(Field(&AudioCodecSwitchContext::codecParam, StrEq("lpcm"))))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&newGstCaps));

    std::unique_ptr<firebolt::rialto::IMediaPipeline::MediaSource> source =
        std::make_unique<firebolt::rialto::IMediaPipeline::MediaSourceAudio>("audio/x-raw", false);
    EXPECT_TRUE(m_sut->reattachSource(source));
}

TEST_F(GstGenericPlayerPrivateTest, shouldReattachAudioSourceAndForwardPlaybackGroupHandlesToBackend)
{
    // The amlhalasink fork vs rdk-gstreamer-utils dispatch now lives inside the backend; the engine
    // forwards the live playsink-bin handle (and isAudioAacState bit) through the neutral context.
    GstAppSrc audioSrc{};
    GstCaps newGstCaps{};
    GstCaps oldGstCaps{};
    GstElement playsinkBin{};
    setPipelineState(GST_STATE_PAUSED);
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc);
            context.playbackGroup.m_isAudioAAC = true;
            context.playbackGroup.m_curAudioPlaysinkBin = &playsinkBin;
        });
    expectRefreshAudioPlaybackGroupHandlesEmpty();

    EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmptySimple(StrEq("audio/mpeg"))).WillOnce(Return(&newGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&newGstCaps, StrEq("mpegversion"), G_TYPE_INT, 4));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&newGstCaps, &oldGstCaps)).WillOnce(Return(FALSE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(_)).WillOnce(Return(GST_STATE_PAUSED));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetStateReturn(_)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_gstWrapperMock, gstStateLock(_)).WillOnce(Return());
    EXPECT_CALL(*m_gstWrapperMock, gstElementQueryPosition(_, GST_FORMAT_TIME, _)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstStateUnlock(_)).WillOnce(Return());
    EXPECT_CALL(*m_platformBackendMock,
                switchAudioCodec(Field(&AudioCodecSwitchContext::audioPlaysinkBin, &playsinkBin)))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&newGstCaps));
    // termPipeline unrefs the stored playsink bin at teardown.
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&playsinkBin));

    std::unique_ptr<firebolt::rialto::IMediaPipeline::MediaSource> source =
        std::make_unique<firebolt::rialto::IMediaPipeline::MediaSourceAudio>("audio/aac", false);
    EXPECT_TRUE(m_sut->reattachSource(source));
}

TEST_F(GstGenericPlayerPrivateTest, shouldWarnWhenBackendCodecSwitchFails)
{
    GstAppSrc audioSrc{};
    GstCaps newGstCaps{};
    GstCaps oldGstCaps{};
    setPipelineState(GST_STATE_PAUSED);
    modifyContext([&](GenericPlayerContext &context)
                  { context.streamInfo[firebolt::rialto::MediaSourceType::AUDIO].appSrc = GST_ELEMENT(&audioSrc); });
    expectRefreshAudioPlaybackGroupHandlesEmpty();

    EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmptySimple(StrEq("audio/mpeg"))).WillOnce(Return(&newGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&newGstCaps, StrEq("mpegversion"), G_TYPE_INT, 4));
    EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&audioSrc))).WillOnce(Return(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&newGstCaps, &oldGstCaps)).WillOnce(Return(FALSE));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(_)).WillOnce(Return(GST_STATE_PAUSED));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetStateReturn(_)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_gstWrapperMock, gstStateLock(_)).WillOnce(Return());
    EXPECT_CALL(*m_gstWrapperMock, gstElementQueryPosition(_, GST_FORMAT_TIME, _)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstStateUnlock(_)).WillOnce(Return());
    // Backend reports failure: reattachSource logs a warning but still completes the caps lifecycle.
    EXPECT_CALL(*m_platformBackendMock, switchAudioCodec(_)).WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&oldGstCaps));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&newGstCaps));

    std::unique_ptr<firebolt::rialto::IMediaPipeline::MediaSource> source =
        std::make_unique<firebolt::rialto::IMediaPipeline::MediaSourceAudio>("audio/aac", false);
    EXPECT_TRUE(m_sut->reattachSource(source));
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetSourceFlushed)
{
    EXPECT_CALL(m_flushWatcherMock, setFlushed(MediaSourceType::AUDIO));
    m_sut->setSourceFlushed(MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, failToSetShowVideoWindowNoValue)
{
    EXPECT_FALSE(m_sut->setShowVideoWindow());
}

TEST_F(GstGenericPlayerPrivateTest, failToSetShowVideoWindowNoSink)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingShowVideoWindow = true; });
    EXPECT_FALSE(m_sut->setShowVideoWindow());
}

TEST_F(GstGenericPlayerPrivateTest, failToSetShowVideoWindowNoProperty)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingShowVideoWindow = true; });
    expectGetAVSink(kVideoSinkStr, m_realElement);
    EXPECT_CALL(*m_glibWrapperMock, gObjectClassFindProperty(_, StrEq("show-video-window"))).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement));
    EXPECT_FALSE(m_sut->setShowVideoWindow());
}

TEST_F(GstGenericPlayerPrivateTest, shouldSetShowVideoWindow)
{
    modifyContext([&](GenericPlayerContext &context) { context.pendingShowVideoWindow = true; });

    expectGetAVSink(kVideoSinkStr, m_realElement);
    EXPECT_CALL(*m_glibWrapperMock, gObjectClassFindProperty(_, StrEq("show-video-window")))
        .WillOnce(Return(&m_showVideoWindowSpec));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(m_realElement, StrEq("show-video-window")));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement));
    EXPECT_TRUE(m_sut->setShowVideoWindow());
}

TEST_F(GstGenericPlayerPrivateTest, shouldBuildExplicitAudioChain)
{
    GstElement appSrc{};
    GstElement decodebin{};
    GstElement decoderLinkTarget{};
    GstElement audioSink{};

    // The engine builds no media topology: it adds its appsrc, then the backend constructs + links its
    // own audio subgraph and returns the handles (sink, decodebin, decoderLinkTarget). The engine creates
    // and links no media element of its own.
    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &appSrc)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_platformBackendMock, buildAudioPath(&m_pipeline, &appSrc))
        .WillOnce(Return(PlatformMediaPath{&audioSink, &decodebin, &decoderLinkTarget}));

    // decodebin is non-null, so the engine wires the generic decoder pad-added handler.
    EXPECT_CALL(*m_glibWrapperMock, gSignalConnect(&decodebin, StrEq("pad-added"), _, _)).WillOnce(Return(1));

    // The sink is stored twice (each with its own ref): once as m_context.audioSink so getSink / the
    // audio-sink setters reach it, and once as the playback group's audio playsink-bin analogue (the
    // explicit topology has no playsink wrapper). Both refs are released by termPipeline at teardown.
    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(&audioSink)).Times(2).WillRepeatedly(Return(&audioSink));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&audioSink)).Times(2);

    // Underflow telemetry is scanned on the backend sink (none on the reference autoaudiosink).
    expectNoStreamSignals(&audioSink, false);

    firebolt::rialto::wrappers::PlaybackGroupPrivate *playbackGroup{nullptr};
    modifyContext([&](GenericPlayerContext &context) { playbackGroup = &context.playbackGroup; });

    m_sut->buildAudioChain(&appSrc);

    // The stable codec-switch handles are populated at construction: the pipeline, the audio decodebin,
    // and the backend sink standing in for the audio playsink bin.
    EXPECT_EQ(playbackGroup->m_gstPipeline, &m_pipeline);
    EXPECT_EQ(playbackGroup->m_curAudioDecodeBin.load(), &decodebin);
    EXPECT_EQ(playbackGroup->m_curAudioPlaysinkBin, &audioSink);
}

TEST_F(GstGenericPlayerPrivateTest, shouldGetExplicitAudioSink)
{
    GstElement audioSink{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.audioSink = &audioSink;
        });

    // Explicit construction returns the stored sink directly (ref'd for the caller), not off playbin.
    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(GST_OBJECT(&audioSink))).WillOnce(Return(&audioSink));
    EXPECT_EQ(&audioSink, m_sut->getSink(firebolt::rialto::MediaSourceType::AUDIO));

    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&audioSink));   // released by termPipeline at teardown
}

TEST_F(GstGenericPlayerPrivateTest, shouldBuildExplicitVideoChain)
{
    GstElement appSrc{};
    GstElement decodebin{};
    GstElement videoSink{};

    // The engine adds its appsrc, then the backend builds the video path keyed by the video id derived at
    // construction (0 = primary for the default video requirements) and returns the handles. On the
    // reference path the decoder src pad links straight to the sink, so decoderLinkTarget == the sink.
    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &appSrc)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_platformBackendMock, buildVideoPath(&m_pipeline, &appSrc, 0u))
        .WillOnce(Return(PlatformMediaPath{&videoSink, &decodebin, &videoSink}));

    EXPECT_CALL(*m_glibWrapperMock, gSignalConnect(&decodebin, StrEq("pad-added"), _, _)).WillOnce(Return(1));

    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(&videoSink)).WillOnce(Return(&videoSink));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&videoSink));   // released by termPipeline at teardown

    // Underflow + first-video-frame telemetry is scanned on the backend sink (none on autovideosink).
    expectNoStreamSignals(&videoSink, true);

    m_sut->buildVideoChain(&appSrc);
}

// buildVideoChain relocates the reactive isVideoSink branch of SetupElement: any video-sink property
// pending at attach time is applied inline once the backend sink is built (4c). This asserts all four
// branches fire — geometry, immediate-output, render-frame, show-window — each reaching the stored sink
// through the explicit getSink(VIDEO) path.
TEST_F(GstGenericPlayerPrivateTest, shouldBuildExplicitVideoChainAppliesPendingVideoProps)
{
    GstElement appSrc{};
    GstElement decodebin{};
    GstElement videoSink{};

    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.pendingGeometry.width = 1920;
            context.pendingGeometry.height = 1080;
            context.pendingImmediateOutputForVideo = true;
            context.pendingRenderFrame = true;
            context.pendingShowVideoWindow = true;
        });

    EXPECT_CALL(*m_gstWrapperMock, gstBinAdd(GST_BIN(&m_pipeline), &appSrc)).WillOnce(Return(TRUE));
    EXPECT_CALL(*m_platformBackendMock, buildVideoPath(&m_pipeline, &appSrc, 0u))
        .WillOnce(Return(PlatformMediaPath{&videoSink, &decodebin, &videoSink}));
    EXPECT_CALL(*m_glibWrapperMock, gSignalConnect(&decodebin, StrEq("pad-added"), _, _)).WillOnce(Return(1));

    // The sink is ref'd once for the context store plus once per getSink call (one per applied prop);
    // each prop's setter and the teardown release a ref.
    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(&videoSink)).Times(5).WillRepeatedly(Return(&videoSink));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&videoSink)).Times(5);

    // geometry -> rectangle
    EXPECT_CALL(*m_glibWrapperMock, gObjectClassFindProperty(_, StrEq("rectangle"))).WillOnce(Return(&m_rectangleSpec));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(&videoSink, StrEq("rectangle")));
    // immediate-output
    expectSetProperty(m_glibWrapperMock, m_gstWrapperMock, &videoSink, kImmediateOutputStr, true);
    // render-frame -> frame-step-on-preroll (set to 1, step event, reset to 0)
    expectSetProperty(m_glibWrapperMock, m_gstWrapperMock, &videoSink, kFrameStepOnPrerollStr, 1);
    EXPECT_CALL(*m_gstWrapperMock, gstEventNewStep(GST_FORMAT_BUFFERS, 1, 1.0, true, false)).WillOnce(Return(&m_event));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetIntStub(_, StrEq(kFrameStepOnPrerollStr.c_str()), 0)).Times(1);
    EXPECT_CALL(*m_gstWrapperMock, gstElementSendEvent(&videoSink, &m_event));
    // show-video-window
    EXPECT_CALL(*m_glibWrapperMock, gObjectClassFindProperty(_, StrEq("show-video-window")))
        .WillOnce(Return(&m_showVideoWindowSpec));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(&videoSink, StrEq("show-video-window")));

    // Underflow + first-video-frame telemetry is scanned on the backend sink (none on autovideosink).
    expectNoStreamSignals(&videoSink, true);

    m_sut->buildVideoChain(&appSrc);
}

TEST_F(GstGenericPlayerPrivateTest, shouldGetExplicitVideoSink)
{
    GstElement videoSink{};
    modifyContext(
        [&](GenericPlayerContext &context)
        {
            context.videoSink = &videoSink;
        });

    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(GST_OBJECT(&videoSink))).WillOnce(Return(&videoSink));
    EXPECT_EQ(&videoSink, m_sut->getSink(firebolt::rialto::MediaSourceType::VIDEO));

    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&videoSink));   // released by termPipeline at teardown
}

// Stage 5a: connectDecoderSignals wires AAMP underflow / first-video-frame telemetry on the
// autoplugged decoder once decodebin exposes it (the sink is wired during chain construction). This is
// the explicit-path analogue of the playbin path's reactive SetupElement signal wiring.
TEST_F(GstGenericPlayerPrivateTest, shouldConnectAudioDecoderUnderflowSignal)
{
    expectGetDecoder(m_realElement); // audio decoder, ref'd by getDecoder

    EXPECT_CALL(*m_glibWrapperMock, gObjectType(m_realElement)).WillOnce(Return(G_TYPE_PARAM));
    EXPECT_CALL(*m_glibWrapperMock, gSignalListIds(_, _))
        .WillOnce(Invoke(
            [this](GType, guint *nIds)
            {
                *nIds = 1;
                return &m_signalIds;
            }));
    EXPECT_CALL(*m_glibWrapperMock, gSignalQuery(_, _))
        .WillOnce(Invoke([](guint, GSignalQuery *query) { query->signal_name = "buffer-underflow-callback"; }));
    EXPECT_CALL(*m_glibWrapperMock, gFree(&m_signalIds));
    EXPECT_CALL(*m_glibWrapperMock, gSignalConnect(m_realElement, StrEq("buffer-underflow-callback"), _, _))
        .WillOnce(Return(1));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)); // getDecoder ref released

    m_sut->connectDecoderSignals(firebolt::rialto::MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldConnectVideoDecoderUnderflowAndFirstFrameSignals)
{
    // getDecoder(VIDEO) — like expectGetDecoder but keyed on the video factory type.
    EXPECT_CALL(*m_gstWrapperMock, gstBinIterateRecurse(GST_BIN(&m_pipeline))).WillOnce(Return(&m_it));
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorNext(&m_it, _)).WillOnce(Return(GST_ITERATOR_OK));
    EXPECT_CALL(*m_glibWrapperMock, gValueGetObject(_)).WillOnce(Return(m_realElement));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetFactory(m_realElement)).WillOnce(Return(m_factory));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListIsType(m_factory, (GST_ELEMENT_FACTORY_TYPE_DECODER |
                                                                           GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO)))
        .WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(m_realElement)).WillOnce(Return(m_realElement));
    EXPECT_CALL(*m_glibWrapperMock, gValueUnset(_));
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorFree(&m_it));

    // Two scans on the decoder: underflow then first-video-frame, both present.
    EXPECT_CALL(*m_glibWrapperMock, gObjectType(m_realElement)).Times(2).WillRepeatedly(Return(G_TYPE_PARAM));
    EXPECT_CALL(*m_glibWrapperMock, gSignalListIds(_, _))
        .Times(2)
        .WillRepeatedly(Invoke(
            [this](GType, guint *nIds)
            {
                *nIds = 1;
                return &m_signalIds;
            }));
    EXPECT_CALL(*m_glibWrapperMock, gSignalQuery(_, _))
        .WillOnce(Invoke([](guint, GSignalQuery *query) { query->signal_name = "buffer-underflow-callback"; }))
        .WillOnce(Invoke([](guint, GSignalQuery *query) { query->signal_name = "first-video-frame-callback"; }));
    EXPECT_CALL(*m_glibWrapperMock, gFree(&m_signalIds)).Times(2);
    EXPECT_CALL(*m_glibWrapperMock, gSignalConnect(m_realElement, StrEq("buffer-underflow-callback"), _, _))
        .WillOnce(Return(1));
    EXPECT_CALL(*m_glibWrapperMock, gSignalConnect(m_realElement, StrEq("first-video-frame-callback"), _, _))
        .WillOnce(Return(1));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)); // getDecoder ref released

    m_sut->connectDecoderSignals(firebolt::rialto::MediaSourceType::VIDEO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldConnectNoDecoderSignalsWhenDecoderHasNoSignals)
{
    expectGetDecoder(m_realElement);
    // audio: underflow scan only, none found
    expectNoStreamSignals(m_realElement, false);
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_realElement)); // getDecoder ref released

    m_sut->connectDecoderSignals(firebolt::rialto::MediaSourceType::AUDIO);
}

TEST_F(GstGenericPlayerPrivateTest, shouldConnectNoDecoderSignalsWhenNoDecoder)
{
    expectNoDecoder(); // getDecoder returns null -> nothing to wire, no unref
    m_sut->connectDecoderSignals(firebolt::rialto::MediaSourceType::AUDIO);
}
