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
 * Both parameters are live: Playbin builds via GStreamer playbin, Explicit drives the
 * explicit-construction path (RIALTO_EXPLICIT_PIPELINE opt-in). The same assertions hold for both.
 */

#include "GstGenericPlayerTestCommon.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <utility>

using ::testing::ByMove;
using ::testing::Return;

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

    void TearDown() override { unsetenv("RIALTO_EXPLICIT_PIPELINE"); }

    // Builds the player through the construction path under test.
    void arrangeAndConstruct()
    {
        if (GetParam() == ConstructionMode::Explicit)
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
                                                   m_gstProtectionMetadataFactoryMock);
    }

    void destroy()
    {
        gstPlayerWillBeDestroyed();
        m_sut.reset();
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

INSTANTIATE_TEST_SUITE_P(PlaybinAndExplicit, GstGenericPlayerParityTest,
                         ::testing::Values(ConstructionMode::Playbin, ConstructionMode::Explicit),
                         [](const ::testing::TestParamInfo<ConstructionMode> &info)
                         { return info.param == ConstructionMode::Playbin ? "Playbin" : "Explicit"; });
