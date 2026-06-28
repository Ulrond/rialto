/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2026 Sky UK
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

#include "ExpectMessage.h"
#include "Matchers.h"
#include "MediaPipelineTest.h"
#include <gst/gst.h>

using testing::_;
using testing::Invoke;
using testing::Return;
using testing::StrEq;

namespace
{
constexpr unsigned kFramesToPush{1};
constexpr gulong kSignalId{123};
} // namespace

namespace firebolt::rialto::server::ct
{
class FirstFrameNotificationTest : public MediaPipelineTest
{
public:
    FirstFrameNotificationTest()
    {
        m_elementFactory = gst_element_factory_find("fakesrc");
        m_videoDecoder = gst_element_factory_create(m_elementFactory, nullptr);
    }

    ~FirstFrameNotificationTest() override
    {
        gst_object_unref(m_videoDecoder);
        gst_object_unref(m_elementFactory);
    }

    // The explicit path autoplugs the video decoder via decodebin's pad-added; once it appears,
    // SetupVideoParser runs on the worker and connectDecoderSignals locates the decoder (getDecoder
    // iterates the pipeline) and scans it for the underflow (no match) and first-video-frame signals,
    // connecting the first-frame callback.
    void willConnectVideoDecoderFirstFrame()
    {
        // getDecoder(VIDEO): iterate the pipeline, the video decoder factory matches DECODER|MEDIA_VIDEO.
        EXPECT_CALL(*m_gstWrapperMock, gstBinIterateRecurse(GST_BIN(&m_pipeline))).WillOnce(Return(&m_it));
        EXPECT_CALL(*m_gstWrapperMock, gstIteratorNext(&m_it, _)).WillOnce(Return(GST_ITERATOR_OK));
        EXPECT_CALL(*m_glibWrapperMock, gValueGetObject(_)).WillOnce(Return(m_videoDecoder));
        EXPECT_CALL(*m_gstWrapperMock, gstElementGetFactory(m_videoDecoder)).WillOnce(Return(m_elementFactory));
        EXPECT_CALL(*m_gstWrapperMock,
                    gstElementFactoryListIsType(m_elementFactory, GST_ELEMENT_FACTORY_TYPE_DECODER |
                                                                      GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO))
            .WillOnce(Return(TRUE));
        EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(m_videoDecoder)).WillOnce(Return(m_videoDecoder));
        EXPECT_CALL(*m_glibWrapperMock, gValueUnset(_));
        EXPECT_CALL(*m_gstWrapperMock, gstIteratorFree(&m_it));

        // connectStreamSignals(VIDEO): the decoder exposes only the first-video-frame signal. Both the
        // underflow scan and the first-frame scan walk the same signal list, so the signal name is
        // queried twice; only the first-frame scan finds a match and connects.
        EXPECT_CALL(*m_glibWrapperMock, gObjectType(m_videoDecoder)).Times(2).WillRepeatedly(Return(G_TYPE_PARAM));
        EXPECT_CALL(*m_glibWrapperMock, gSignalListIds(_, _))
            .Times(2)
            .WillRepeatedly(Invoke(
                [&](GType, guint *nIds)
                {
                    *nIds = 1;
                    return m_signals;
                }));
        EXPECT_CALL(*m_glibWrapperMock, gSignalQuery(m_signals[0], _))
            .Times(2)
            .WillRepeatedly(Invoke([&](guint, GSignalQuery *query) { query->signal_name = "first-video-frame-callback"; }));
        EXPECT_CALL(*m_glibWrapperMock, gFree(m_signals)).Times(2);
        EXPECT_CALL(*m_glibWrapperMock, gSignalConnect(m_videoDecoder, StrEq("first-video-frame-callback"), _, _))
            .WillOnce(Invoke(
                [&](gpointer, const gchar *, GCallback c_handler, gpointer data)
                {
                    m_firstVideoFrameCallback = c_handler;
                    m_firstVideoFrameData = data;
                    return kSignalId;
                }));
        EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_videoDecoder))
            .WillOnce(Invoke(this, &MediaPipelineTest::workerFinished));
    }

    void setupVideoDecoder()
    {
        triggerVideoPadAdded();
        waitWorker();
    }

    void firstVideoFrameReceived()
    {
        ExpectMessage<FirstFrameReceivedEvent> expectedFirstFrameReceived{m_clientStub};
        expectedFirstFrameReceived.setFilter([&](const auto &msg) { return msg.source_id() == m_videoSourceId; });

        ASSERT_TRUE(m_firstVideoFrameCallback);
        ASSERT_TRUE(m_firstVideoFrameData);
        reinterpret_cast<void (*)(GstElement *, guint, gpointer, gpointer)>(
            m_firstVideoFrameCallback)(m_videoDecoder, 0, nullptr, m_firstVideoFrameData);

        auto receivedFirstFrameReceived{expectedFirstFrameReceived.getMessage()};
        ASSERT_TRUE(receivedFirstFrameReceived);
        EXPECT_EQ(receivedFirstFrameReceived->session_id(), m_sessionId);
        EXPECT_EQ(receivedFirstFrameReceived->source_id(), m_videoSourceId);
    }

private:
    GstElementFactory *m_elementFactory{nullptr};
    GstElement *m_videoDecoder{nullptr};
    GstIterator m_it{};
    guint m_signals[1]{123};
    GCallback m_firstVideoFrameCallback{};
    gpointer m_firstVideoFrameData{nullptr};
};

/*
 * Component Test: First frame notification test
 * Test Objective:
 *  Test if Rialto Server handles gstreamer first frame signals correctly. The notification should be forwarded to
 *  Rialto Client with FirstFrameReceivedEvent message.
 *
 * Sequence Diagrams:
 *  First frame notification
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
 *  Step 3: Attach video source
 *   Attach the video source.
 *   Expect that video source is attached.
 *   Expect that all sources are attached.
 *   Expect that the Playback state has changed to IDLE.
 *
 *  Step 4: Autoplug video decoder
 *   Fire the video decodebin pad-added callback.
 *   First frame callback should be registered on the autoplugged decoder.
 *
 *  Step 5: Pause
 *   Pause the content.
 *   Expect that gstreamer pipeline is paused.
 *
 *  Step 6: Write 1 video frame
 *   Gstreamer Stub notifies, that it needs video data.
 *   Expect that server notifies the client that it needs 3 frames of video data.
 *   Write 1 frame of video data to the shared buffer.
 *   Send HaveData message.
 *   Expect that server notifies the client that it needs 3 frames of video data.
 *
 *  Step 7: Notify buffered and Paused
 *   Expect that server notifies the client that the Network state has changed to BUFFERED.
 *   Gstreamer Stub notifies, that pipeline state is in PAUSED state.
 *   Expect that server notifies the client that the Network state has changed to PAUSED.
 *
 *  Step 8: First video frame received
 *   Rialto Server will receive first video frame signal.
 *   Rialto Server should send FirstFrameReceivedEvent with video source.
 *
 *  Step 9: End of video stream
 *   Send video haveData with one frame and EOS status.
 *   Expect that Gstreamer is notified about end of stream.
 *
 *  Step 10: Notify end of stream
 *   Simulate, that gst_message_eos is received by Rialto Server.
 *   Expect that server notifies the client that the Network state has changed to END_OF_STREAM.
 *
 *  Step 11: Remove source
 *   Remove the video source.
 *   Expect that video source is removed.
 *
 *  Step 12: Stop
 *   Stop the playback.
 *   Expect that stop propagated to the gstreamer pipeline.
 *   Expect that server notifies the client that the Playback state has changed to STOPPED.
 *
 *  Step 13: Destroy media session
 *   Send DestroySessionRequest.
 *   Expect that the session is destroyed on the server.
 *
 * Test Teardown:
 *  Memory region created for the shared buffer is unmapped.
 *  Server is terminated.
 *
 * Expected Results:
 *  First frame signal is handled by Rialto Server.
 *
 * Code:
 */
TEST_F(FirstFrameNotificationTest, firstFrameNotification)
{
    // Step 1: Create a new media session
    createSession();

    // Step 2: Load content
    gstPlayerWillBeCreated();
    load();

    // Step 3: Attach video source
    videoSourceWillBeAttached();
    attachVideoSource();
    sourceWillBeSetup();
    setupSource();
    willSetupAndAddSource(&m_videoAppSrc);
    willFinishSetupAndAddSource();
    indicateAllSourcesAttached({&m_videoAppSrc});

    // Step 4: Autoplug video decoder
    willConnectVideoDecoderFirstFrame();
    setupVideoDecoder();

    // Step 5: Pause
    willPause();
    pause();

    // Step 6: Write 1 video frame
    // Step 7: Notify buffered and Paused
    {
        ExpectMessage<firebolt::rialto::NetworkStateChangeEvent> expectedNetworkStateChange{m_clientStub};

        pushVideoData(kFramesToPush);

        auto receivedNetworkStateChange{expectedNetworkStateChange.getMessage()};
        ASSERT_TRUE(receivedNetworkStateChange);
        EXPECT_EQ(receivedNetworkStateChange->session_id(), m_sessionId);
        EXPECT_EQ(receivedNetworkStateChange->state(), ::firebolt::rialto::NetworkStateChangeEvent_NetworkState_BUFFERED);
    }
    willNotifyPaused();
    notifyPaused();

    // Step 8: First video frame received
    firstVideoFrameReceived();

    // Step 9: End of video stream
    willEos(&m_videoAppSrc);
    eosVideo(kFramesToPush);

    // Step 10: Notify end of stream
    gstNotifyEos();

    // Step 11: Remove source
    removeSource(m_videoSourceId);

    // Step 12: Stop
    willStop();
    stop();

    // Step 13: Destroy media session
    gstPlayerWillBeDestructed();
    destroySession();
}
} // namespace firebolt::rialto::server::ct
