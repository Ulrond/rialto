/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2023 Sky UK
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

#include "ActionTraits.h"
#include "ConfigureAction.h"
#include "Constants.h"
#include "ExpectMessage.h"
#include "Matchers.h"
#include "MediaPipelineTest.h"
#include "MessageBuilders.h"

using testing::_;
using testing::Invoke;
using testing::Return;
using testing::StrEq;

namespace
{
constexpr bool kSvpEnabled{true};
} // namespace

namespace firebolt::rialto::server::ct
{
class AudioSourceSwitchTest : public MediaPipelineTest
{
public:
    AudioSourceSwitchTest() = default;
    ~AudioSourceSwitchTest() override = default;

    // updateAudioPlaybackGroupHandles refreshes the audio decoder/parser/typefind from the live graph
    // before the codec switch. getDecoder + getParser each iterate the pipeline; getAudioTypefind iterates
    // the audio decodebin (the explicit chain stored it as m_curAudioDecodeBin). Nothing matches, so each
    // iteration completes immediately and the handles stay null.
    void willRefreshAudioPlaybackGroupHandles()
    {
        EXPECT_CALL(*m_gstWrapperMock, gstBinIterateRecurse(GST_BIN(&m_pipeline))).Times(2).WillRepeatedly(Return(&m_it));
        EXPECT_CALL(*m_gstWrapperMock, gstBinIterateRecurse(GST_BIN(&m_audioDecodebin))).WillOnce(Return(&m_it));
        EXPECT_CALL(*m_gstWrapperMock, gstIteratorNext(&m_it, _)).Times(3).WillRepeatedly(Return(GST_ITERATOR_DONE));
        EXPECT_CALL(*m_glibWrapperMock, gValueUnset(_)).Times(3);
        EXPECT_CALL(*m_gstWrapperMock, gstIteratorFree(&m_it)).Times(3);
    }

    void willSwitchAudioSource()
    {
        willRefreshAudioPlaybackGroupHandles();

        EXPECT_CALL(*m_gstWrapperMock, gstCapsNewEmptySimple(StrEq("audio/mpeg"))).WillOnce(Return(&m_audioCaps));
        EXPECT_CALL(*m_gstWrapperMock,
                    gstCapsSetSimpleStringStub(&m_audioCaps, StrEq("alignment"), G_TYPE_STRING, StrEq("nal")));
        EXPECT_CALL(*m_gstWrapperMock,
                    gstCapsSetSimpleStringStub(&m_audioCaps, StrEq("stream-format"), G_TYPE_STRING, StrEq("raw")));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&m_audioCaps, StrEq("mpegversion"), G_TYPE_INT, 4));
        EXPECT_CALL(*m_gstWrapperMock,
                    gstCapsSetSimpleIntStub(&m_audioCaps, StrEq("channels"), G_TYPE_INT, kNumOfChannels));
        EXPECT_CALL(*m_gstWrapperMock, gstStateLock(_)).Times(1);
        EXPECT_CALL(*m_gstWrapperMock, gstStateUnlock(_)).Times(1);
        EXPECT_CALL(*m_gstWrapperMock, gstElementGetState(_)).WillOnce(Return(GST_STATE_PAUSED));
        EXPECT_CALL(*m_gstWrapperMock, gstElementGetStateReturn(_)).WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsSetSimpleIntStub(&m_audioCaps, StrEq("rate"), G_TYPE_INT, kSampleRate));
        // reattachSource reads the appsrc caps first (for the equality decision), then switchAudioCodec on
        // the backend reads them again. Both target the same appsrc, so a sequence keeps them in order.
        testing::Sequence appsrcCapsSeq;
        EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(&m_audioAppSrc)).InSequence(appsrcCapsSeq).WillOnce(Return(&m_oldCaps));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsIsEqual(&m_audioCaps, &m_oldCaps)).WillOnce(Return(FALSE));
        // switchAudioCodec on the reference backend: the playsink-bin (base m_audioSink, the autoaudiosink)
        // is not an amlhalasink, so it takes the rdk-gstreamer-utils path: get the appsrc caps, delegate to
        // performAudioTrackCodecChannelSwitch, then unref the caps it owns.
        EXPECT_CALL(*m_glibWrapperMock, gStrHasPrefix(_, StrEq("amlhalasink"))).WillOnce(Return(FALSE));
        EXPECT_CALL(*m_gstWrapperMock, gstAppSrcGetCaps(GST_APP_SRC(&m_audioAppSrc)))
            .InSequence(appsrcCapsSeq)
            .WillOnce(Return(&m_switchCaps));
        EXPECT_CALL(*m_rdkGstreamerUtilsWrapperMock,
                    performAudioTrackCodecChannelSwitch(_, _, _, _, _, _, _, _, _, _, kSvpEnabled,
                                                        GST_ELEMENT(&m_audioAppSrc), _))
            .WillOnce(Return(true));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&m_switchCaps));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&m_oldCaps));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&m_audioCaps)).WillOnce(Invoke(this, &MediaPipelineTest::workerFinished));
    }

    void switchAudioSource()
    {
        auto attachAudioSourceReq{createAttachAudioSourceRequest(m_sessionId)};
        attachAudioSourceReq.set_switch_source(true);
        ConfigureAction<AttachSource>(m_clientStub).send(attachAudioSourceReq).expectSuccess();
        waitWorker();
    }

private:
    GstCaps m_oldCaps{};
    GstCaps m_switchCaps{};
    GstIterator m_it{};
};
/*
 * Component Test: Switch audio source procedure test
 * Test Objective:
 *  Test the audio switch procedure
 *
 * Sequence Diagrams:
 *  Rialto Dynamic Audio Stream Switching
 *   - https://wiki.rdkcentral.com/pages/viewpage.action?spaceKey=ASP&title=Rialto+Dynamic+Audio+Stream+Switching
 *
 * Test Setup:
 *  Language: C++
 *  Testing Framework: Google Test
 *  Components: MediaPipeline
 *
 * Test Initialize:
 *  Set Rialto Server to Active
 *  Connect Rialto Client Stub
 *  Map Shared Memory
 *
 * Test Steps:
 *  Step 1: Create a new media session
 *   Send CreateSessionRequest to Rialto Server
 *   Expect that successful CreateSessionResponse is received
 *   Save returned session id
 *
 *  Step 2: Load content
 *   Send LoadRequest to Rialto Server
 *   Expect that successful LoadResponse is received
 *   Expect that GstPlayer instance is created.
 *   Expect that client is notified that the NetworkState has changed to BUFFERING.
 *
 *  Step 3: Attach all sources
 *   Attach the audio source.
 *   Expect that audio source is attached.
 *   Attach the video source.
 *   Expect that video source is attached.
 *   Expect that rialto source is setup
 *   Expect that all sources are attached.
 *   Expect that the Playback state has changed to IDLE.
 *
 *  Step 4: Switch Audio Source
 *   Switch the audio source.
 *   Expect that audio source is switched.
 *
 *  Step 5: Remove sources
 *   Remove the audio source.
 *   Expect that audio source is removed.
 *   Remove the video source.
 *   Expect that video source is removed.
 *
 *  Step 6: Stop
 *   Stop the playback.
 *   Expect that stop propagated to the gstreamer pipeline.
 *   Expect that server notifies the client that the Playback state has changed to STOPPED.
 *
 *  Step 7: Destroy media session
 *   Send DestroySessionRequest.
 *   Expect that the session is destroyed on the server.
 *
 * Test Teardown:
 *  Memory region created for the shared buffer is unmapped.
 *  Server is terminated.
 *
 * Expected Results:
 *  Audio source switch procedure is executed when new audio source is attached.
 *
 * Code:
 */
TEST_F(AudioSourceSwitchTest, SwitchAudioSource)
{
    // Step 1: Create a new media session
    createSession();

    // Step 2: Load content
    gstPlayerWillBeCreated();
    load();

    // Step 3: Attach all sources
    audioSourceWillBeAttached();
    attachAudioSource();
    videoSourceWillBeAttached();
    attachVideoSource();
    sourceWillBeSetup();
    setupSource();
    willSetupAndAddSource(&m_audioAppSrc);
    willSetupAndAddSource(&m_videoAppSrc);
    willFinishSetupAndAddSource();
    indicateAllSourcesAttached({&m_audioAppSrc, &m_videoAppSrc});

    // Step 4: Pause
    willPause();
    pause();

    // Step 5: Switch Audio Source
    willSwitchAudioSource();
    switchAudioSource();

    // Step 6: Remove sources
    removeSource(m_audioSourceId);
    removeSource(m_videoSourceId);

    // Step 7: Stop
    willStop();
    stop();

    // Step 8: Destroy media session
    gstPlayerWillBeDestructed();
    destroySession();
}
} // namespace firebolt::rialto::server::ct
