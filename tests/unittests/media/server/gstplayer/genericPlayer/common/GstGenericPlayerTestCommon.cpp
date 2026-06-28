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

#include "GstGenericPlayerTestCommon.h"
#include "Matchers.h"
#include "PlayerTaskMock.h"
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

using ::testing::_;
using ::testing::ByMove;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::StrEq;

void GstGenericPlayerTestCommon::gstPlayerWillBeCreated()
{
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());
    initFactories();
    expectMakePipeline();
    expectSetMessageCallback();

    EXPECT_CALL(*m_gstWrapperMock, gstElementSetState(&m_pipeline, GST_STATE_READY))
        .WillOnce(Return(GST_STATE_CHANGE_SUCCESS));
    EXPECT_CALL(*m_gstSrcMock, initSrc());
    EXPECT_CALL(*m_gstProfilerFactoryMock, createGstProfiler(&m_pipeline, _, _))
        .WillOnce(Return(ByMove(std::move(m_gstProfiler))));
    EXPECT_CALL(m_workerThreadFactoryMock, createWorkerThread()).WillOnce(Return(ByMove(std::move(workerThread))));
    EXPECT_CALL(*m_gstProtectionMetadataFactoryMock, createProtectionMetadataWrapper(_))
        .WillOnce(Return(ByMove(std::move(m_gstProtectionMetadataWrapper))));
    executeTaskWhenEnqueued();
}

void GstGenericPlayerTestCommon::gstPlayerWillBeDestroyed()
{
    expectShutdown();
    expectStop();
    EXPECT_CALL(*m_gstWrapperMock, gstPipelineGetBus(GST_PIPELINE(&m_pipeline))).WillOnce(Return(&m_bus));
    EXPECT_CALL(*m_gstWrapperMock, gstBusSetSyncHandler(&m_bus, nullptr, nullptr, nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_bus));
    // termPipeline releases each backend sink stored in the context (see expectGetAVSink). Declared here,
    // after the test body, so this absorbs the destroy-time unref while the getSink caller's own unref is
    // matched by the consumer's expectation.
    for (GstElement *sink : m_storedSinks)
    {
        EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(sink)).RetiresOnSaturation();
    }
    EXPECT_CALL(*m_gstWrapperMock, gstObjectUnref(&m_pipeline));
}

void GstGenericPlayerTestCommon::expectShutdown()
{
    std::unique_ptr<IPlayerTask> shutdownTask{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*shutdownTask), execute());
    EXPECT_CALL(m_taskFactoryMock, createShutdown(_)).WillOnce(Return(ByMove(std::move(shutdownTask))));
    EXPECT_CALL(m_workerThreadMock, join());
}

void GstGenericPlayerTestCommon::expectStop()
{
    std::unique_ptr<IPlayerTask> stopTask{std::make_unique<StrictMock<PlayerTaskMock>>()};
    EXPECT_CALL(dynamic_cast<StrictMock<PlayerTaskMock> &>(*stopTask), execute());
    EXPECT_CALL(m_taskFactoryMock, createStop(_, _)).WillOnce(Return(ByMove(std::move(stopTask))));
}

void GstGenericPlayerTestCommon::executeTaskWhenEnqueued()
{
    // It's hard to match std::unique_ptr<IPlayerTask> &&, so we will just execute task, when it's enqueued to check
    // if proper task was enqueued (EXPECT_CALL(task, execute())) has to be added for each task, which is expected to
    // be enqueued)
    EXPECT_CALL(m_workerThreadMock, enqueueTask(_))
        .WillRepeatedly(Invoke([](std::unique_ptr<IPlayerTask> &&task) { task->execute(); }));
}

void GstGenericPlayerTestCommon::setPipelineState(const GstState &state)
{
    GST_STATE(&m_pipeline) = state;
}

void GstGenericPlayerTestCommon::initFactories()
{
    EXPECT_CALL(*m_gstSrcFactoryMock, getGstSrc()).WillOnce(Return(m_gstSrcMock));
}

void GstGenericPlayerTestCommon::expectMakePipeline()
{
    EXPECT_CALL(*m_gstWrapperMock, gstPipelineNew(StrEq("media_pipeline"))).WillOnce(Return(&m_pipeline));
}

void GstGenericPlayerTestCommon::expectSetMessageCallback()
{
    EXPECT_CALL(m_gstDispatcherThreadFactoryMock, createGstDispatcherThread(_, _, _, _))
        .WillOnce(Return(ByMove(std::move(gstDispatcherThread))));
}

void GstGenericPlayerTestCommon::expectGetDecoder(GstElement *element)
{
    EXPECT_CALL(*m_gstWrapperMock, gstBinIterateRecurse(GST_BIN(&m_pipeline))).WillOnce(Return(&m_it));
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorNext(&m_it, _)).WillOnce(Return(GST_ITERATOR_OK));
    EXPECT_CALL(*m_glibWrapperMock, gValueGetObject(_)).WillOnce(Return(element));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetFactory(element)).WillOnce(Return(m_factory));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListIsType(m_factory, (GST_ELEMENT_FACTORY_TYPE_DECODER |
                                                                           GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO)))
        .WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(element)).WillOnce(Return(element));
    EXPECT_CALL(*m_glibWrapperMock, gValueUnset(_));
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorFree(&m_it));
}

void GstGenericPlayerTestCommon::expectGetVideoParser(GstElement *element)
{
    EXPECT_CALL(*m_gstWrapperMock, gstBinIterateRecurse(GST_BIN(&m_pipeline))).WillOnce(Return(&m_it));
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorNext(&m_it, _)).WillOnce(Return(GST_ITERATOR_OK));
    EXPECT_CALL(*m_glibWrapperMock, gValueGetObject(_)).WillOnce(Return(element));
    EXPECT_CALL(*m_gstWrapperMock, gstElementGetFactory(element)).WillOnce(Return(m_factory));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListIsType(m_factory, (GST_ELEMENT_FACTORY_TYPE_PARSER |
                                                                           GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO)))
        .WillOnce(Return(TRUE));
    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(element)).WillOnce(Return(element));
    EXPECT_CALL(*m_glibWrapperMock, gValueUnset(_));
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorFree(&m_it));
}

void GstGenericPlayerTestCommon::expectGetAVSink(const std::string &sinkName, GstElement *elementObj)
{
    // getSink returns the backend sink stored in the context (ref'd; the caller unrefs). Store it in the
    // matching context field so getSink finds it — there is no playbin property to read and no auto-sink
    // to unwrap.
    applyToContext(
        [&sinkName, elementObj](GenericPlayerContext &context)
        {
            if (sinkName == kAudioSinkStr)
                context.audioSink = elementObj;
            else if (sinkName == kVideoSinkStr)
                context.videoSink = elementObj;
            else
                context.subtitleSink = elementObj;
        });
    EXPECT_CALL(*m_gstWrapperMock, gstObjectRef(elementObj)).WillOnce(Return(elementObj));
    // The stored sink is released by termPipeline when the player is destroyed. That destroy-time unref is
    // expected in gstPlayerWillBeDestroyed (declared last, so it absorbs the destroy call rather than the
    // getSink caller's own unref, which the consumer expects during the test body).
    m_storedSinks.push_back(elementObj);
}

void GstGenericPlayerTestCommon::expectGetSink(const std::string &sinkName, GstElement *elementObj)
{
    expectGetAVSink(sinkName, elementObj);
}

void GstGenericPlayerTestCommon::expectNoDecoder()
{
    EXPECT_CALL(*m_gstWrapperMock, gstBinIterateRecurse(GST_BIN(&m_pipeline))).WillOnce(Return(&m_it));
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorNext(&m_it, _)).WillOnce(Return(GST_ITERATOR_DONE));
    EXPECT_CALL(*m_glibWrapperMock, gValueUnset(_));
    EXPECT_CALL(*m_gstWrapperMock, gstIteratorFree(&m_it));
}

void GstGenericPlayerTestCommon::expectNoParser()
{
    expectNoDecoder();
}
