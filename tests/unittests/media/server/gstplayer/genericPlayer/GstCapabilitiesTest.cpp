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

#include "GstCapabilities.h"
#include "GlibWrapperFactoryMock.h"
#include "GlibWrapperMock.h"
#include "GstInitialiserMock.h"
#include "GstWrapperFactoryMock.h"
#include "GstWrapperMock.h"
#include "IFactoryAccessor.h"
#include "PlatformBackendMock.h"

#include <condition_variable>
#include <gtest/gtest.h>
#include <mutex>
#include <unordered_map>

using namespace firebolt::rialto;
using namespace firebolt::rialto::server;
using namespace firebolt::rialto::wrappers;

using ::testing::_;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::StrEq;
using ::testing::StrictMock;
using ::testing::UnorderedElementsAre;

template <typename T> class GListWrapper
{
public:
    explicit GListWrapper(std::initializer_list<T> elements)
    {
        for (T element : elements)
        {
            m_list = g_list_append(m_list, element);
        }
    }
    ~GListWrapper() { g_list_free(m_list); }
    GList *get() { return m_list; }

private:
    GList *m_list = nullptr;
};

class GstCapabilitiesTest : public testing::Test
{
public:
    GstCapabilitiesTest()
        // valid values of GstCaps are not needed, only addresses will be used
        : m_capsMap{{"audio/mpeg, mpegversion=(int)4", {}},
                    {"audio/mpeg, mpegversion=(int)1, layer=(int)3", {}},
                    {"audio/x-eac3", {}},
                    {"audio/x-opus", {}},
                    {"audio/x-opus, channel-mapping-family=(int)0", {}},
                    {"audio/b-wav", {}},
                    {"audio/x-flac", {}},
                    {"audio/x-raw", {}},
                    {"video/x-av1", {}},
                    {"video/x-h264", {}},
                    {"video/x-h265", {}},
                    {"video/x-vp9", {}},
                    {"video/mpeg, mpegversion=(int)4", {}},
                    {"video/x-h264(memory:DMABuf)", {}},
                    {"video/x-h265(memory:DMABuf)", {}},
                    {"video/x-av1(memory:DMABuf)", {}},
                    {"video/x-vp9(memory:DMABuf)", {}}}
    {
        IFactoryAccessor::instance().gstWrapperFactory() = m_gstWrapperFactoryMock;
        IFactoryAccessor::instance().glibWrapperFactory() = m_glibWrapperFactoryMock;
    }

    ~GstCapabilitiesTest() override
    {
        IFactoryAccessor::instance().gstWrapperFactory() = nullptr;
        IFactoryAccessor::instance().glibWrapperFactory() = nullptr;
    }

    void expectCapsToMimeMapping()
    {
        for (auto &capsMap : m_capsMap)
        {
            EXPECT_CALL(*m_gstWrapperMock, gstCapsFromString(StrEq(capsMap.first.c_str()))).WillOnce(Return(&capsMap.second));
            EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(&capsMap.second)).Times(1);
        }
    }

    void expectGetFactoryListAndFreeList(GList *list, GstElementFactoryListType type, GstRank minrank)
    {
        EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(type, minrank)).WillOnce(Return(list));
        EXPECT_CALL(*m_gstWrapperMock, gstPluginFeatureListFree(list)).Times(1);
    }

    void expectGetStaticPadTemplates(GstElementFactory *factory, GList *list, int count = 1)
    {
        EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryGetStaticPadTemplates(factory))
            .Times(count)
            .WillRepeatedly(Return(list));
    }

    void expectGetStaticCapsAndCapsUnref(GstStaticPadTemplate &padTemplate, GstCaps *caps, int count = 1)
    {
        EXPECT_CALL(*m_gstWrapperMock, gstStaticCapsGet(&padTemplate.static_caps)).Times(count).WillRepeatedly(Return(caps));
        EXPECT_CALL(*m_gstWrapperMock, gstCapsUnref(caps)).Times(count);
    }

    GstStaticPadTemplate createSinkPadTemplate()
    {
        GstStaticPadTemplate decoderPadTemplate;
        decoderPadTemplate.direction = GST_PAD_SINK;
        return decoderPadTemplate;
    }

    GstStaticPadTemplate createSrcPadTemplate()
    {
        GstStaticPadTemplate decoderPadTemplate;
        decoderPadTemplate.direction = GST_PAD_SRC;
        return decoderPadTemplate;
    }

    void createSutWithNoDecoderAndNoSink() { createSutWithNoDecoderAndNoSink(m_platformBackendMock); }

    void createSutWithNoDecoderAndNoSink(const std::shared_ptr<IPlatformBackend> &platformBackend)
    {
        // Some expectations are met in a thread in GstCapabilities constructor
        // We have to wait for full object construction before adding new expectations
        // Without that gtest may crash
        std::mutex mutex;
        std::condition_variable cv;
        bool initialised{false};
        EXPECT_CALL(*m_gstWrapperMock,
                    gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL))
            .WillOnce(Return(nullptr));
        EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
            .WillOnce(Invoke(
                [&](auto type, auto rank)
                {
                    std::unique_lock lock{mutex};
                    initialised = true;
                    cv.notify_one();
                    return nullptr;
                }));
        EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

        m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                                   platformBackend);
        std::unique_lock lock{mutex};
        cv.wait_for(lock, std::chrono::milliseconds{200}, [&]() { return initialised; });
    }

    std::shared_ptr<StrictMock<GstWrapperMock>> m_gstWrapperMock{std::make_shared<StrictMock<GstWrapperMock>>()};
    std::shared_ptr<StrictMock<GstWrapperFactoryMock>> m_gstWrapperFactoryMock{
        std::make_shared<StrictMock<GstWrapperFactoryMock>>()};
    std::shared_ptr<StrictMock<GlibWrapperFactoryMock>> m_glibWrapperFactoryMock{
        std::make_shared<StrictMock<GlibWrapperFactoryMock>>()};
    StrictMock<GstInitialiserMock> m_gstInitialiserMock;
    std::unordered_map<std::string, GstCaps> m_capsMap;
    std::unique_ptr<GstCapabilities> m_sut;
    std::shared_ptr<StrictMock<GlibWrapperMock>> m_glibWrapperMock{std::make_shared<StrictMock<GlibWrapperMock>>()};
    std::shared_ptr<StrictMock<PlatformBackendMock>> m_platformBackendMock{
        std::make_shared<StrictMock<PlatformBackendMock>>()};

    // Common sink factory type variables to be used in tests
    char m_dummySink = 0;
    GstElementFactory *m_sinkFactory{reinterpret_cast<GstElementFactory *>(&m_dummySink)};
    GstStaticPadTemplate m_sinkPadTemplateSink;
    GstStaticPadTemplate m_sinkPadTemplateSrc;
    GstCaps m_sinkTemplateCaps;
    GListWrapper<GstElementFactory *> m_sinkFactoryList{m_sinkFactory};
    GListWrapper<GstStaticPadTemplate *> m_sinkPadTemplatesList{&m_sinkPadTemplateSink, &m_sinkPadTemplateSrc};

    // Common decoder factory type variables to be used in tests
    char m_dummyDecoder = 0;
    GstElementFactory *m_decoderFactory{reinterpret_cast<GstElementFactory *>(&m_dummyDecoder)};
    GstStaticPadTemplate m_decoderPadTemplateSink;
    GstStaticPadTemplate m_decoderPadTemplateSink2;
    GstStaticPadTemplate m_decoderPadTemplateSrc;
    GstCaps m_decoderTemplateCapsSink;
    GstCaps m_decoderTemplateCapsSink2;
    GListWrapper<GstElementFactory *> m_decoderFactoryList{m_decoderFactory};
    GListWrapper<GstStaticPadTemplate *> m_decoderPadTemplateListWithSingleSink{&m_decoderPadTemplateSink};
    GListWrapper<GstStaticPadTemplate *> m_decoderPadTemplateListWithTwoSinks{&m_decoderPadTemplateSink,
                                                                              &m_decoderPadTemplateSink2};
    GListWrapper<GstStaticPadTemplate *> m_decoderPadTemplateListWithTwoSinksOneSrc{&m_decoderPadTemplateSink,
                                                                                    &m_decoderPadTemplateSink2,
                                                                                    &m_decoderPadTemplateSrc};

    // Common decoder parser factory type variables to be used in tests
    char m_dummyParser = 0;
    GstElementFactory *m_parserFactory{reinterpret_cast<GstElementFactory *>(&m_dummyParser)};
    GstStaticPadTemplate m_parserPadTemplateSink;
    GstStaticPadTemplate m_parserPadTemplateSrc;
    GstCaps m_parserTemplateCapsSink;
    GstCaps m_parserTemplateCapsSrc;
    GListWrapper<GstElementFactory *> m_parserFactoryList{m_parserFactory};
    GListWrapper<GstStaticPadTemplate *> m_parserPadTemplatesList{&m_parserPadTemplateSink, &m_parserPadTemplateSrc};
};

/**
 * Test the factory
 */
TEST_F(GstCapabilitiesTest, FactoryCreatesObject)
{
    EXPECT_CALL(*m_gstWrapperFactoryMock, getGstWrapper()).WillOnce(Return(m_gstWrapperMock));
    EXPECT_CALL(*m_glibWrapperFactoryMock, getGlibWrapper()).WillOnce(Return(m_glibWrapperMock));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    std::shared_ptr<firebolt::rialto::server::IGstCapabilitiesFactory> factory =
        firebolt::rialto::server::IGstCapabilitiesFactory::getFactory();
    EXPECT_NE(factory, nullptr);
    EXPECT_NE(factory->createGstCapabilities(), nullptr);
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_NoDecoderAndNoSink)
{
    createSutWithNoDecoderAndNoSink();
    EXPECT_TRUE(m_sut->getSupportedMimeTypes(MediaSourceType::AUDIO).empty());

    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/x-raw"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/mp4"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/x-eac3"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/aac"));
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_OnlyOneSinkElement)
{
    m_sinkPadTemplateSink = createSinkPadTemplate();
    m_sinkPadTemplateSrc = createSrcPadTemplate();

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    expectGetFactoryListAndFreeList(m_sinkFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_sinkFactory, m_sinkPadTemplatesList.get());

    expectGetStaticCapsAndCapsUnref(m_sinkPadTemplateSink, &m_sinkTemplateCaps);

    expectCapsToMimeMapping();
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["audio/x-raw"], &m_sinkTemplateCaps))
        .WillOnce(Return(true));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_THAT(m_sut->getSupportedMimeTypes(MediaSourceType::AUDIO), UnorderedElementsAre("audio/x-raw"));

    EXPECT_TRUE(m_sut->isMimeTypeSupported("audio/x-raw"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/mp4"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/x-eac3"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/aac"));
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_OnlyOneDecoderWithNoPads)
{
    expectGetFactoryListAndFreeList(m_decoderFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryGetStaticPadTemplates(m_decoderFactory)).WillOnce(Return(nullptr));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_TRUE(m_sut->getSupportedMimeTypes(MediaSourceType::AUDIO).empty());

    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/mp4"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/h264"));
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_OnlyOneDecoderWithTwoSinkPadsAndOneSrcPad)
{
    m_decoderPadTemplateSink = createSinkPadTemplate();
    m_decoderPadTemplateSink2 = createSinkPadTemplate();
    m_decoderPadTemplateSrc = createSrcPadTemplate();

    expectGetFactoryListAndFreeList(m_decoderFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_decoderFactory, m_decoderPadTemplateListWithTwoSinksOneSrc.get());

    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink, &m_decoderTemplateCapsSink);
    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink2, &m_decoderTemplateCapsSink2);

    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_decoderTemplateCapsSink2, &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_PARSER, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    expectCapsToMimeMapping();
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*m_gstWrapperMock,
                gstCapsCanIntersect(&m_capsMap["audio/mpeg, mpegversion=(int)4"], &m_decoderTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["audio/x-opus"], &m_decoderTemplateCapsSink2))
        .WillOnce(Return(true));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_THAT(m_sut->getSupportedMimeTypes(MediaSourceType::AUDIO),
                UnorderedElementsAre("audio/mp4", "audio/aac", "audio/x-eac3", "audio/x-opus"));

    EXPECT_TRUE(m_sut->isMimeTypeSupported("audio/mp4"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/beep-boop3"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/h264"));
}

// The platform backend is the capability authority (ABI v8): GstCapabilities delegates the query
// wholesale and returns the backend's answer. The introspection mechanism is exercised by the
// reference backend's own tests (LinuxPlatformBackendTest).
TEST_F(GstCapabilitiesTest, getSupportedPropertiesDelegatesToBackend)
{
    createSutWithNoDecoderAndNoSink();

    const std::vector<std::string> kParamNames{"test-name-123", "test2", "audio-fade"};
    const std::vector<std::string> kSupported{"test-name-123", "audio-fade"};
    EXPECT_CALL(*m_platformBackendMock, getSupportedProperties(MediaSourceType::VIDEO, kParamNames))
        .WillOnce(Return(kSupported));

    EXPECT_EQ(m_sut->getSupportedProperties(MediaSourceType::VIDEO, kParamNames), kSupported);
}

TEST_F(GstCapabilitiesTest, getSupportedPropertiesReturnsBackendEmptyResult)
{
    createSutWithNoDecoderAndNoSink();

    const std::vector<std::string> kParamNames{"test-name-123", "test2"};
    EXPECT_CALL(*m_platformBackendMock, getSupportedProperties(MediaSourceType::AUDIO, kParamNames))
        .WillOnce(Return(std::vector<std::string>{}));

    EXPECT_TRUE(m_sut->getSupportedProperties(MediaSourceType::AUDIO, kParamNames).empty());
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_OnlyOneDecoderWithTwoPadsWithTheSameCaps)
{
    m_decoderPadTemplateSink = createSinkPadTemplate();
    m_decoderPadTemplateSink2 = createSinkPadTemplate();

    expectGetFactoryListAndFreeList(m_decoderFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_decoderFactory, m_decoderPadTemplateListWithTwoSinks.get());

    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink, &m_decoderTemplateCapsSink);
    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink2, &m_decoderTemplateCapsSink2);
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_decoderTemplateCapsSink2, &m_decoderTemplateCapsSink))
        .WillOnce(Return(true));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_PARSER, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    expectCapsToMimeMapping();
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*m_gstWrapperMock,
                gstCapsCanIntersect(&m_capsMap["audio/mpeg, mpegversion=(int)4"], &m_decoderTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_THAT(m_sut->getSupportedMimeTypes(MediaSourceType::AUDIO),
                UnorderedElementsAre("audio/mp4", "audio/aac", "audio/x-eac3"));

    EXPECT_TRUE(m_sut->isMimeTypeSupported("audio/mp4"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/beep-boop3"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/h264"));
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_OneDecoderWithOneSinkPad_ParserWithConnectableSrcPad)
{
    m_decoderPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSrc = createSrcPadTemplate();

    expectGetFactoryListAndFreeList(m_decoderFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_decoderFactory, m_decoderPadTemplateListWithSingleSink.get());

    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink, &m_decoderTemplateCapsSink);

    expectGetFactoryListAndFreeList(m_parserFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_PARSER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_parserFactory, m_parserPadTemplatesList.get());
    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSrc, &m_parserTemplateCapsSrc);

    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSink, &m_parserTemplateCapsSink);
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_parserTemplateCapsSink, &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    expectCapsToMimeMapping();
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_decoderTemplateCapsSink, &m_parserTemplateCapsSrc))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h264"], &m_decoderTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h265"], &m_parserTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_THAT(m_sut->getSupportedMimeTypes(MediaSourceType::VIDEO), UnorderedElementsAre("video/h264", "video/h265"));

    EXPECT_TRUE(m_sut->isMimeTypeSupported("video/h264"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/x-av1"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/x-opus"));
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_OneDecoderWithOneSinkPad_ParserWithConnectableSrcPad_OneSinkElement)
{
    m_decoderPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSrc = createSrcPadTemplate();
    m_sinkPadTemplateSink = createSinkPadTemplate();
    m_sinkPadTemplateSrc = createSrcPadTemplate();

    expectGetFactoryListAndFreeList(m_decoderFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_decoderFactory, m_decoderPadTemplateListWithSingleSink.get());

    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink, &m_decoderTemplateCapsSink);

    expectGetFactoryListAndFreeList(m_parserFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_PARSER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_parserFactory, m_parserPadTemplatesList.get());
    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSrc, &m_parserTemplateCapsSrc);

    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSink, &m_parserTemplateCapsSink);
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_parserTemplateCapsSink, &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));

    expectGetFactoryListAndFreeList(m_sinkFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_sinkFactory, m_sinkPadTemplatesList.get());
    expectGetStaticCapsAndCapsUnref(m_sinkPadTemplateSink, &m_sinkTemplateCaps);

    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_sinkTemplateCaps, &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_sinkTemplateCaps, &m_parserTemplateCapsSink))
        .WillOnce(Return(false));

    expectCapsToMimeMapping();
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_decoderTemplateCapsSink, &m_parserTemplateCapsSrc))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h264"], &m_decoderTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h265"], &m_parserTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["audio/x-raw"], &m_sinkTemplateCaps))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock,
                gstCapsCanIntersect(&m_capsMap["audio/mpeg, mpegversion=(int)1, layer=(int)3"], &m_sinkTemplateCaps))
        .WillOnce(Return(true));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_THAT(m_sut->getSupportedMimeTypes(MediaSourceType::VIDEO), UnorderedElementsAre("video/h264", "video/h265"));

    EXPECT_TRUE(m_sut->isMimeTypeSupported("video/h264"));
    EXPECT_TRUE(m_sut->isMimeTypeSupported("audio/x-raw"));
    EXPECT_TRUE(m_sut->isMimeTypeSupported("audio/mp3"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/x-av1"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/x-opus"));
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_OneDecoderWithOneSinkPad_ParserWithNoConnectableSrcPad)
{
    m_decoderPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSrc = createSrcPadTemplate();

    expectGetFactoryListAndFreeList(m_decoderFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_decoderFactory, m_decoderPadTemplateListWithSingleSink.get());
    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink, &m_decoderTemplateCapsSink);

    expectGetFactoryListAndFreeList(m_parserFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_PARSER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_parserFactory, m_parserPadTemplatesList.get());
    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSrc, &m_parserTemplateCapsSrc);

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    expectCapsToMimeMapping();
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_decoderTemplateCapsSink, &m_parserTemplateCapsSrc))
        .WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock,
                gstCapsCanIntersect(&m_capsMap["video/mpeg, mpegversion=(int)4"], &m_decoderTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_THAT(m_sut->getSupportedMimeTypes(MediaSourceType::VIDEO), UnorderedElementsAre("video/mp4"));

    EXPECT_TRUE(m_sut->isMimeTypeSupported("video/mp4"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/h265"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/x-opus"));
}

TEST_F(GstCapabilitiesTest,
       CreateGstCapabilities_OneDecoderWithOneSinkPad_ParserWithConnectableSrcPadButNotRialtoMimeTypes)
{
    m_decoderPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSrc = createSrcPadTemplate();

    expectGetFactoryListAndFreeList(m_decoderFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_decoderFactory, m_decoderPadTemplateListWithSingleSink.get());
    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink, &m_decoderTemplateCapsSink);

    expectGetFactoryListAndFreeList(m_parserFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_PARSER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_parserFactory, m_parserPadTemplatesList.get());
    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSrc, &m_parserTemplateCapsSrc);

    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSink, &m_parserTemplateCapsSink);
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_parserTemplateCapsSink, &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    expectCapsToMimeMapping();
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_decoderTemplateCapsSink, &m_parserTemplateCapsSrc))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h264"], &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h265"], &m_parserTemplateCapsSink))
        .WillOnce(Return(false));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_TRUE(m_sut->getSupportedMimeTypes(MediaSourceType::AUDIO).empty());

    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/mp4"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/h264"));
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_TwoDecodersWithOneSinkPad_ParserWithMatchingSrcPad)
{
    char dummyDecoder2 = {};
    GstElementFactory *decoderFactory2 = reinterpret_cast<GstElementFactory *>(&dummyDecoder2);

    m_decoderPadTemplateSink = createSinkPadTemplate();
    m_decoderPadTemplateSink2 = createSinkPadTemplate();

    m_parserPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSrc = createSrcPadTemplate();

    // Create decoder list with with multiple decoders
    GListWrapper<GstElementFactory *> listDecoders{m_decoderFactory, decoderFactory2};

    // Create another decoder list with with one pad
    GListWrapper<GstStaticPadTemplate *> decoderPadTemplates2{&m_decoderPadTemplateSink2};

    expectGetFactoryListAndFreeList(listDecoders.get(), GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_decoderFactory, m_decoderPadTemplateListWithSingleSink.get());
    expectGetStaticPadTemplates(decoderFactory2, decoderPadTemplates2.get());

    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink, &m_decoderTemplateCapsSink);
    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink2, &m_decoderTemplateCapsSink2);

    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_decoderTemplateCapsSink2, &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));

    expectGetFactoryListAndFreeList(m_parserFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_PARSER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_parserFactory, m_parserPadTemplatesList.get(), 2);
    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSrc, &m_parserTemplateCapsSrc, 2);

    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSink, &m_parserTemplateCapsSink);
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_parserTemplateCapsSink, &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_parserTemplateCapsSink, &m_decoderTemplateCapsSink2))
        .WillOnce(Return(true));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    expectCapsToMimeMapping();
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_decoderTemplateCapsSink, &m_parserTemplateCapsSrc))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_decoderTemplateCapsSink2, &m_parserTemplateCapsSrc))
        .WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h264"], &m_decoderTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h265"], &m_decoderTemplateCapsSink2))
        .WillOnce(Return(true));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_THAT(m_sut->getSupportedMimeTypes(MediaSourceType::VIDEO), UnorderedElementsAre("video/h264", "video/h265"));

    EXPECT_TRUE(m_sut->isMimeTypeSupported("video/h264"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/x-av1"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/x-opus"));
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_OneDecodersWithOneSinkPad_ParserWithTwoSrcPadsAndSecondConnectable)
{
    m_decoderPadTemplateSink = createSinkPadTemplate();

    m_parserPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSrc = createSrcPadTemplate();
    GstStaticPadTemplate parserPadTemplateSink2 = createSrcPadTemplate();

    GstCaps parserTemplateCaps3;

    // Create decoder parser list with with three pads
    GListWrapper<GstStaticPadTemplate *> parserPadTemplates{&m_parserPadTemplateSink, &m_parserPadTemplateSrc,
                                                            &parserPadTemplateSink2};

    expectGetFactoryListAndFreeList(m_decoderFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_decoderFactory, m_decoderPadTemplateListWithSingleSink.get());

    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink, &m_decoderTemplateCapsSink);

    expectGetFactoryListAndFreeList(m_parserFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_PARSER, GST_RANK_MARGINAL);

    expectGetStaticPadTemplates(m_parserFactory, parserPadTemplates.get());
    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSrc, &m_parserTemplateCapsSrc);
    expectGetStaticCapsAndCapsUnref(parserPadTemplateSink2, &parserTemplateCaps3);

    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSink, &m_parserTemplateCapsSink);
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_parserTemplateCapsSink, &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    expectCapsToMimeMapping();
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_decoderTemplateCapsSink, &m_parserTemplateCapsSrc))
        .WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_decoderTemplateCapsSink, &parserTemplateCaps3))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h264"], &m_decoderTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h265"], &m_parserTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_THAT(m_sut->getSupportedMimeTypes(MediaSourceType::VIDEO), UnorderedElementsAre("video/h264", "video/h265"));

    EXPECT_TRUE(m_sut->isMimeTypeSupported("video/h264"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/x-av1"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/x-opus"));
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_OneDecodersWithOneSinkPads_TwoParsersWithConnectableSrcPads)
{
    char dummyParser2 = {};
    GstElementFactory *parserFactory2 = reinterpret_cast<GstElementFactory *>(&dummyParser2);

    m_decoderPadTemplateSink = createSinkPadTemplate();

    m_parserPadTemplateSink = createSinkPadTemplate();
    m_parserPadTemplateSrc = createSrcPadTemplate();

    GstStaticPadTemplate parserPadTemplateSink2 = createSinkPadTemplate();
    GstStaticPadTemplate parserPadTemplateSrc2 = createSrcPadTemplate();

    GstCaps parserPadTemplateCapsSink2;
    GstCaps parserPadTemplateCapsSrc2;

    // Create decoder list with with multiple parsers
    GListWrapper<GstElementFactory *> listParsers{m_parserFactory, parserFactory2};

    // Create another decoder parser list with with two pads
    GListWrapper<GstStaticPadTemplate *> parserPadTemplates2{&parserPadTemplateSink2, &parserPadTemplateSrc2};

    expectGetFactoryListAndFreeList(m_decoderFactoryList.get(), GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_decoderFactory, m_decoderPadTemplateListWithSingleSink.get());

    expectGetStaticCapsAndCapsUnref(m_decoderPadTemplateSink, &m_decoderTemplateCapsSink);

    expectGetFactoryListAndFreeList(listParsers.get(), GST_ELEMENT_FACTORY_TYPE_PARSER, GST_RANK_MARGINAL);
    expectGetStaticPadTemplates(m_parserFactory, m_parserPadTemplatesList.get());
    expectGetStaticPadTemplates(parserFactory2, parserPadTemplates2.get());

    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSrc, &m_parserTemplateCapsSrc);
    expectGetStaticCapsAndCapsUnref(parserPadTemplateSrc2, &parserPadTemplateCapsSrc2);

    expectGetStaticCapsAndCapsUnref(m_parserPadTemplateSink, &m_parserTemplateCapsSink);
    expectGetStaticCapsAndCapsUnref(parserPadTemplateSink2, &parserPadTemplateCapsSink2);

    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&m_parserTemplateCapsSink, &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&parserPadTemplateCapsSink2, &m_decoderTemplateCapsSink))
        .WillOnce(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsIsStrictlyEqual(&parserPadTemplateCapsSink2, &m_parserTemplateCapsSink))
        .WillOnce(Return(false));

    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));

    expectCapsToMimeMapping();
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(_, _)).WillRepeatedly(Return(false));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_decoderTemplateCapsSink, &m_parserTemplateCapsSrc))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_decoderTemplateCapsSink, &parserPadTemplateCapsSrc2))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h264"], &m_decoderTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-h265"], &m_parserTemplateCapsSink))
        .WillOnce(Return(true));
    EXPECT_CALL(*m_gstWrapperMock, gstCapsCanIntersect(&m_capsMap["video/x-av1"], &parserPadTemplateCapsSink2))
        .WillOnce(Return(true));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_THAT(m_sut->getSupportedMimeTypes(MediaSourceType::VIDEO),
                UnorderedElementsAre("video/h264", "video/h265", "video/x-av1"));

    EXPECT_TRUE(m_sut->isMimeTypeSupported("video/h264"));
    EXPECT_TRUE(m_sut->isMimeTypeSupported("video/x-av1"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/mp4"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("video/x-vp9"));
    EXPECT_FALSE(m_sut->isMimeTypeSupported("audio/x-opus"));
}

TEST_F(GstCapabilitiesTest, CreateGstCapabilities_GetSubtitlesMimeTypes)
{
    const std::vector<std::string> kSubtitleMimeTypes{"text/vtt", "text/ttml", "text/cc"};
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));
    EXPECT_CALL(*m_gstWrapperMock, gstElementFactoryListGetElements(GST_ELEMENT_FACTORY_TYPE_SINK, GST_RANK_MARGINAL))
        .WillOnce(Return(nullptr));
    EXPECT_CALL(m_gstInitialiserMock, waitForInitialisation());

    m_sut = std::make_unique<GstCapabilities>(m_gstWrapperMock, m_glibWrapperMock, m_gstInitialiserMock,
                                               m_platformBackendMock);

    EXPECT_EQ(m_sut->getSupportedMimeTypes(MediaSourceType::SUBTITLE), kSubtitleMimeTypes);
}

TEST_F(GstCapabilitiesTest, shouldFailToCheckIfVideoIsMasterWhenNoPlatformBackend)
{
    createSutWithNoDecoderAndNoSink(nullptr);

    bool isVideoMaster{false};
    EXPECT_FALSE(m_sut->isVideoMaster(isVideoMaster));
}

TEST_F(GstCapabilitiesTest, shouldCheckIfVideoIsMasterAndReturnTrue)
{
    createSutWithNoDecoderAndNoSink();

    EXPECT_CALL(*m_platformBackendMock, isVideoMaster()).WillOnce(Return(true));
    bool isVideoMaster{false};
    EXPECT_TRUE(m_sut->isVideoMaster(isVideoMaster));
    EXPECT_TRUE(isVideoMaster);
}

TEST_F(GstCapabilitiesTest, shouldCheckIfVideoIsMasterAndReturnFalse)
{
    createSutWithNoDecoderAndNoSink();

    EXPECT_CALL(*m_platformBackendMock, isVideoMaster()).WillOnce(Return(false));
    bool isVideoMaster{false};
    EXPECT_TRUE(m_sut->isVideoMaster(isVideoMaster));
    EXPECT_FALSE(isVideoMaster);
}
