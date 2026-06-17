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

#include "GlibWrapperFactoryMock.h"
#include "GstWebAudioPlayerTestCommon.h"
#include "GstWrapperFactoryMock.h"
#include "IFactoryAccessor.h"
#include "Matchers.h"
#include <condition_variable>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>

class RialtoServerCreateGstWebAudioPlayerTest : public GstWebAudioPlayerTestCommon
{
protected:
    std::unique_ptr<IGstWebAudioPlayer> m_gstPlayer;
};

/**
 * Test that a GstWebAudioPlayer object can be created and destroyed successfully. The audio sink
 * comes from the platform backend; the engine no longer selects a SoC-specific sink.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, CreateDestroySuccess)
{
    gstPlayerWillBeCreated();

    EXPECT_NO_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                      m_glibWrapperMock, m_gstInitialiserMock,
                                                                      m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                      std::move(workerThreadFactory),
                                                                      std::move(gstDispatcherThreadFactory), m_platformBackendMock));
    EXPECT_NE(m_gstPlayer, nullptr);

    gstPlayerWillBeDestroyed();
    m_gstPlayer.reset();
}

/**
 * Test the factory
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, FactoryCreatesObject)
{
    bool dispatcherThreadCreated{false};
    std::mutex dispatcherThreadMutex{};
    std::condition_variable dispatcherThreadCv;
    std::shared_ptr<StrictMock<firebolt::rialto::wrappers::GlibWrapperFactoryMock>> glibWrapperFactoryMock{
        std::make_shared<StrictMock<firebolt::rialto::wrappers::GlibWrapperFactoryMock>>()};
    std::shared_ptr<StrictMock<firebolt::rialto::wrappers::GstWrapperFactoryMock>> gstWrapperFactoryMock{
        std::make_shared<StrictMock<firebolt::rialto::wrappers::GstWrapperFactoryMock>>()};
    firebolt::rialto::wrappers::IFactoryAccessor::instance().glibWrapperFactory() = glibWrapperFactoryMock;
    firebolt::rialto::wrappers::IFactoryAccessor::instance().gstWrapperFactory() = gstWrapperFactoryMock;
    EXPECT_CALL(*glibWrapperFactoryMock, getGlibWrapper()).WillRepeatedly(Return(m_glibWrapperMock));
    EXPECT_CALL(*gstWrapperFactoryMock, getGstWrapper()).WillRepeatedly(Return(m_gstWrapperMock));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryFind(StrEq("rialtosrc"))).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstElementRegister(0, StrEq("rialtosrc"), GST_RANK_PRIMARY + 100, _))
        .WillOnce(Return(true));
    expectCreatePipeline();
    expectInitAppSrc();
    // The real factory builds the real (compiled-in) LinuxPlatformBackend, which runs the
    // SoC probe ladder on the injected wrapper mocks. With amlhalasink present it makes that
    // sink and applies its config — the ladder now lives in the backend, not the engine.
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryGet()).WillOnce(Return(&m_reg));
    EXPECT_CALL(*m_gstWrapperMock, gstRegistryLookupFeature(&m_reg, StrEq("amlhalasink")))
        .WillOnce(Return(GST_PLUGIN_FEATURE(&m_feature)));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("amlhalasink"), StrEq("webaudiosink")))
        .WillOnce(Return(&m_sink));
    EXPECT_CALL(*m_glibWrapperMock, gObjectSetStub(G_OBJECT(&m_sink), StrEq("direct-mode")));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(GST_PLUGIN_FEATURE(&m_feature)));
    expectLinkElements();
    EXPECT_CALL(*m_gstWrapperMock, gstPipelineGetBus(GST_PIPELINE(&m_pipeline)))
        .WillOnce(Invoke(
            [&](GstPipeline *) // NOLINT(readability/casting)
            {
                std::unique_lock lock{dispatcherThreadMutex};
                dispatcherThreadCreated = true;
                dispatcherThreadCv.notify_one();
                return nullptr; // hack to avoid more expects ;)
            }));
    std::shared_ptr<firebolt::rialto::server::IGstWebAudioPlayerFactory> factory =
        firebolt::rialto::server::IGstWebAudioPlayerFactory::getFactory();
    EXPECT_NE(factory, nullptr);
    auto webAudioPlayer{factory->createGstWebAudioPlayer(&m_gstPlayerClient, m_priority)};
    EXPECT_NE(webAudioPlayer, nullptr);

    // Wait for dispatcher thread creation
    {
        std::unique_lock lock{dispatcherThreadMutex};
        dispatcherThreadCv.wait_for(lock, std::chrono::milliseconds(50), [&]() { return dispatcherThreadCreated; });
        EXPECT_TRUE(dispatcherThreadCreated);
    }

    EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(&m_pipeline, GST_STATE_NULL))
        .WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    expectTermPipeline();
    webAudioPlayer.reset();
    firebolt::rialto::wrappers::IFactoryAccessor::instance().glibWrapperFactory() = nullptr;
    firebolt::rialto::wrappers::IFactoryAccessor::instance().gstWrapperFactory() = nullptr;
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to get the gstreamer source.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, getGstSrcFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    EXPECT_CALL(*m_gstSrcFactoryMock, getGstSrc()).WillOnce(Return(nullptr));

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to create the worker thread.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, createWorkerThreadFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    EXPECT_CALL(m_workerThreadFactoryMock, createWorkerThread()).WillOnce(Return(nullptr));

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to create the pipeline.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, createPipelineFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    expectInitWorkerThread();
    EXPECT_CALL(*m_gstWrapperMock, gstPipelineNew(_)).WillOnce(Return(nullptr));

    // Reset worker thread on failure
    expectResetWorkerThread();

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to create the app src.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, createAppSrcFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    expectCreatePipeline();
    EXPECT_CALL(m_workerThreadFactoryMock, createWorkerThread()).WillOnce(Return(ByMove(std::move(workerThread))));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(_, _)).WillOnce(Return(nullptr));

    // Reset worker thread and pipeline on failure
    EXPECT_CALL(*m_gstWrapperMock, gstPipelineGetBus(GST_PIPELINE(&m_pipeline))).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_pipeline));
    expectTaskStop();
    expectResetWorkerThread();

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if the platform backend fails to create the
 * audio sink. The SoC sink-selection failure modes (registry / amlhalasink / rtkaudiosink) are
 * now exercised by the platform backend's own tests.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, createAudioSinkFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    expectCreatePipeline();
    expectInitAppSrc();
    expectInitWorkerThread();
    EXPECT_CALL(*m_platformBackendMock, createAudioSink(StrEq("webaudiosink"))).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_appSrc));

    // Reset worker thread and pipeline on failure
    gstPlayerWillBeDestroyed();

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to create audio convert.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, createAudioConvertFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    expectCreatePipeline();
    expectInitAppSrc();
    expectCreateAudioSink();
    expectInitWorkerThread();
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("audioconvert"), _)).WillOnce(Return(nullptr));

    // Reset worker thread and pipeline on failure
    gstPlayerWillBeDestroyed();
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_sink));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_appSrc));

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to create audio resample.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, createAudioResampleFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    expectCreateAudioSink();
    expectCreatePipeline();
    expectInitAppSrc();
    expectInitWorkerThread();

    GstElement convert{};
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("audioconvert"), _)).WillOnce(Return(&convert));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("audioresample"), _)).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&convert));

    // Reset worker thread and pipeline on failure
    gstPlayerWillBeDestroyed();
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_sink));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_appSrc));

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to create queue.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, createAudioQueueFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    expectCreateAudioSink();
    expectCreatePipeline();
    expectInitAppSrc();
    expectInitWorkerThread();

    GstElement convert{};
    GstElement resample{};
    GstElement volume{};

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("audioconvert"), _)).WillOnce(Return(&convert));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("audioresample"), _)).WillOnce(Return(&resample));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("volume"), _)).WillOnce(Return(&volume));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryMake(StrEq("queue"), _)).WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&convert));

    // Reset worker thread and pipeline on failure
    gstPlayerWillBeDestroyed();
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&resample));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&volume));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_sink));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_appSrc));

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to add the audio sink elements to the bin.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, linkAudioSinkBinFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    expectCreatePipeline();
    expectInitAppSrc();
    expectCreateAudioSink();
    expectAddBinFailure();
    expectInitWorkerThread();

    // Reset worker thread and pipeline on failure
    gstPlayerWillBeDestroyed();

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to link audio sink elements.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, linkElementFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    expectCreatePipeline();
    expectInitAppSrc();
    expectCreateAudioSink();
    expectLinkElementFailure();
    expectInitWorkerThread();

    // Reset worker thread and pipeline on failure
    gstPlayerWillBeDestroyed();

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to create the dispatcher thread.
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, createGstDispatcherThreadFailure)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    expectCreatePipeline();
    expectInitAppSrc();
    expectCreateAudioSink();
    expectLinkElements();
    expectInitWorkerThread();
    EXPECT_CALL(m_gstDispatcherThreadFactoryMock, createGstDispatcherThread(_, _, _, _)).WillOnce(Return(nullptr));

    // Reset worker thread and pipeline on failure
    gstPlayerWillBeDestroyed();

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}

/**
 * Test that a GstWebAudioPlayer throws an exception if there is a failure to create a volume element
 */
TEST_F(RialtoServerCreateGstWebAudioPlayerTest, createGstDispatcherAfterFailureToCreateVolume)
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    expectInitRialtoSrc();
    expectInitWorkerThread();
    expectCreatePipeline();
    expectInitAppSrc();
    expectCreateAudioSink();

    expectLinkElementsExceptVolume();

    expectTaskStop();
    expectTermPipeline();
    expectResetWorkerThread();

    EXPECT_THROW(m_gstPlayer = std::make_unique<GstWebAudioPlayer>(&m_gstPlayerClient, m_priority, m_gstWrapperMock,
                                                                   m_glibWrapperMock, m_gstInitialiserMock,
                                                                   m_gstSrcFactoryMock, std::move(m_taskFactory),
                                                                   std::move(workerThreadFactory),
                                                                   std::move(gstDispatcherThreadFactory), m_platformBackendMock),
                 std::runtime_error);
    EXPECT_EQ(m_gstPlayer, nullptr);
}
