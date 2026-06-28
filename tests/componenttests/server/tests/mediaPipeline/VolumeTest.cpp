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
#include "MediaPipelineTest.h"
#include "MessageBuilders.h"

using testing::_;
using testing::Invoke;
using testing::Return;
using testing::StrEq;

namespace firebolt::rialto::server::ct
{
class VolumeTest : public MediaPipelineTest
{
public:
    VolumeTest() = default;
    ~VolumeTest() override = default;

    // getSink now returns the backend-stored audio sink (the base m_audioSink, created by the explicit
    // audio chain on attach); there is no playbin "audio-sink" property to read.

    void willSetVolumeWhenVolumeDurationIsZero()
    {
        // duration == 0: the immediate-volume-change branch sets the volume directly on the pipeline; the
        // audio-fade property is never looked up. getSink refs/unrefs the audio sink. Each per-step sink
        // unref retires on saturation so it resolves independently of the harness AnyNumber unref and the
        // later per-step unrefs.
        EXPECT_CALL(*m_gstWrapperMock, gstStreamVolumeSetVolume(_, GST_STREAM_VOLUME_FORMAT_LINEAR, kVolume));
        EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_audioSink))
            .WillOnce(Invoke(this, &MediaPipelineTest::workerFinished))
            .RetiresOnSaturation();
    }

    void willSetVolumeWhenVolumeDurationMoreThanZero()
    {
        // duration > 0: the reference backend reports isAudioFadeSupported() == false, so the engine drives
        // the generic sink "audio-fade" property. The audio sink (base m_audioSink, returned by getSink)
        // exposes the property, so gObjectSet writes "<scaledTarget>,<duration>,<ease>" and audioFadeEnabled
        // becomes true. There is no SoC isSocAudioFadeSupported/doAudioEasingonSoc path anymore.
        EXPECT_CALL(*m_glibWrapperMock, gObjectClassFindProperty(_, StrEq("audio-fade")))
            .WillOnce(Return(&m_paramSpec))
            .RetiresOnSaturation();
        // audio-fade carries a string value ("<scaledTarget>,<duration>,<ease>"), so the glib wrapper
        // forwards to the string setter variant.
        EXPECT_CALL(*m_glibWrapperMock, gObjectSetStrStub(m_audioSink, StrEq("audio-fade"), _));
        EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_audioSink))
            .WillOnce(Invoke(this, &MediaPipelineTest::workerFinished))
            .RetiresOnSaturation();
    }

    void willGetFadeVolume()
    {
        // GetVolume after a fade: audioFadeEnabled is true and the sink exposes "fade-volume", so the
        // current volume is read from the sink (positive => fade volume returned directly).
        EXPECT_CALL(*m_glibWrapperMock, gObjectClassFindProperty(_, StrEq("fade-volume")))
            .WillOnce(Return(&m_paramSpec))
            .RetiresOnSaturation();

        EXPECT_CALL(*m_glibWrapperMock, gObjectGetStub(_, StrEq("fade-volume"), _))
            .WillOnce(Invoke(
                [](gpointer object, const gchar *first_property_name, void *val)
                {
                    gint *returnVal = reinterpret_cast<gint *>(val);
                    *returnVal = 50;
                }));

        EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(m_audioSink)).RetiresOnSaturation();
    }

    void willGetVolumeFromPipeline()
    {
        EXPECT_CALL(*m_gstWrapperMock, gstStreamVolumeGetVolume(_, GST_STREAM_VOLUME_FORMAT_LINEAR))
            .WillOnce(Return(kVolume));
    }

    void setVolumeNormal()
    {
        ConfigureAction<SetVolume>(m_clientStub).send(createSetVolumeNormalRequest(m_sessionId)).expectSuccess();
        waitWorker();
    }

    void setVolumeWithFade()
    {
        ConfigureAction<SetVolume>(m_clientStub).send(createSetVolumeWithFadeRequest(m_sessionId)).expectSuccess();
        waitWorker();
    }

    void getVolume()
    {
        ConfigureAction<GetVolume>(m_clientStub)
            .send(createGetVolumeRequest(m_sessionId))
            .expectSuccess()
            .matchResponse([&](const auto &resp) { EXPECT_EQ(resp.volume(), kVolume); });
    }

private:
    GParamSpec m_paramSpec{};
};

/*
 * Component Test: Volume Test
 * Test Objective:
 *  Test the Volume API. Set/Get Volume API calls should be supported by Rialto Server.
 *
 * Sequence Diagrams:
 *  Volume
 *   - https://wiki.rdkcentral.com/display/ASP/Rialto+MSE+Misc+Sequence+Diagrams
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
 *  Step 4: Pause
 *   Pause the content.
 *   Expect that gstreamer pipeline is paused.
 *
 *  Step 5: Set Volume with no fade
 *   Set the volume when the volume duration is zero
 *   Send SetVolumeReq and expect successful response
 *
 *  Step 6: Get Volume from pipeline
 *   Send GetVolumeReq and expect successful response and volume level from pipeline
 *
 *  Step 7: Set volume with fade
 *   Set the volume when the volume duration is more than zero
 *   Send SetVolumeReq and expect successful response
 *
 *  Step 8: Get Fade Volume
 *   Send GetVolumeReq and expect successful response and fade volume level
 *
 *  Step 9: Remove sources
 *   Remove the audio source.
 *   Expect that audio source is removed.
 *   Remove the video source.
 *   Expect that video source is removed.
 *
 *  Step 10: Stop
 *   Stop the playback.
 *   Expect that stop propagated to the gstreamer pipeline.
 *   Expect that server notifies the client that the Playback state has changed to STOPPED.
 *
 *  Step 11: Destroy media session
 *   Send DestroySessionRequest.
 *   Expect that the session is destroyed on the server.
 *
 * Test Teardown:
 *  Memory region created for the shared buffer is unmapped.
 *  Server is terminated.
 *
 * Expected Results:
 *  RenderFrame API call is successful
 *
 * Code:
 */
TEST_F(VolumeTest, Volume)
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

    // Step 5: Set volume with no fade
    willSetVolumeWhenVolumeDurationIsZero();
    setVolumeNormal();

    // Step 6: Get volume from pipeline
    willGetVolumeFromPipeline();
    getVolume();

    // Step 7: Set volume with fade
    willSetVolumeWhenVolumeDurationMoreThanZero();
    setVolumeWithFade();

    // Step 8: Get fade volume
    willGetFadeVolume();
    getVolume();

    // Step 9: Remove sources
    removeSource(m_audioSourceId);
    removeSource(m_videoSourceId);

    // Step 10: Stop
    willStop();
    stop();

    // Step 11: Destroy media session
    gstPlayerWillBeDestructed();
    destroySession();
}
} // namespace firebolt::rialto::server::ct
