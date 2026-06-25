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

#include "GlibWrapperMock.h"
#include "GstWrapperMock.h"
#include "PlatformBackendLoader.h"
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>

using namespace firebolt::rialto::server;
using namespace firebolt::rialto::wrappers;

using ::testing::StrictMock;

namespace
{
constexpr char kBackendEnvVar[]{"RIALTO_PLATFORM_BACKEND"};
constexpr char kBackendDirEnvVar[]{"RIALTO_PLATFORM_DIR"};
} // namespace

/*
 * Exercises the real dlopen / dlsym / version-check path against fixture .so files built
 * by CMake (FIXTURE_*_PATH / FIXTURE_*_DIR defines). The host wrappers are StrictMocks: the
 * fixture backend ignores them and the reference fallback only touches them on sink creation
 * (never called here), so no expectations are set.
 */
class PlatformBackendLoaderTest : public ::testing::Test
{
protected:
    std::shared_ptr<StrictMock<GstWrapperMock>> m_gstWrapperMock{std::make_shared<StrictMock<GstWrapperMock>>()};
    std::shared_ptr<StrictMock<GlibWrapperMock>> m_glibWrapperMock{std::make_shared<StrictMock<GlibWrapperMock>>()};
    PlatformHostContext m_host{m_gstWrapperMock, m_glibWrapperMock};
    PlatformBackendLoader m_sut;

    void SetUp() override
    {
        ::unsetenv(kBackendEnvVar);
        ::unsetenv(kBackendDirEnvVar);
    }

    void TearDown() override
    {
        ::unsetenv(kBackendEnvVar);
        ::unsetenv(kBackendDirEnvVar);
    }
};

TEST_F(PlatformBackendLoaderTest, ExplicitOverridePathLoadsMatchingBackend)
{
    ::setenv(kBackendEnvVar, FIXTURE_MATCH_PATH, 1);

    std::shared_ptr<IPlatformBackend> backend{m_sut.load(m_host)};

    ASSERT_TRUE(backend);
    EXPECT_STREQ(backend->platformName(), "fixture");
}

TEST_F(PlatformBackendLoaderTest, SingleObjectInConfiguredDirectoryIsLoaded)
{
    ::setenv(kBackendDirEnvVar, FIXTURE_MATCH_DIR, 1);

    std::shared_ptr<IPlatformBackend> backend{m_sut.load(m_host)};

    ASSERT_TRUE(backend);
    EXPECT_STREQ(backend->platformName(), "fixture");
}

TEST_F(PlatformBackendLoaderTest, MissingEntrypointFallsBackToReference)
{
    ::setenv(kBackendEnvVar, FIXTURE_NOSYM_PATH, 1);

    std::shared_ptr<IPlatformBackend> backend{m_sut.load(m_host)};

    ASSERT_TRUE(backend);
    EXPECT_STREQ(backend->platformName(), "linux");
}

TEST_F(PlatformBackendLoaderTest, AbiVersionMismatchIsRefusedAndFallsBackToReference)
{
    ::setenv(kBackendEnvVar, FIXTURE_MISMATCH_PATH, 1);

    std::shared_ptr<IPlatformBackend> backend{m_sut.load(m_host)};

    ASSERT_TRUE(backend);
    EXPECT_STREQ(backend->platformName(), "linux");
}

TEST_F(PlatformBackendLoaderTest, NonexistentExplicitPathFallsBackToReference)
{
    ::setenv(kBackendEnvVar, "/nonexistent/librialtoplatform-missing.so", 1);

    std::shared_ptr<IPlatformBackend> backend{m_sut.load(m_host)};

    ASSERT_TRUE(backend);
    EXPECT_STREQ(backend->platformName(), "linux");
}

TEST_F(PlatformBackendLoaderTest, NoVendorObjectUsesReferenceBackend)
{
    // No override, and a directory with no librialtoplatform-*.so present.
    ::setenv(kBackendDirEnvVar, FIXTURE_EMPTY_DIR, 1);

    std::shared_ptr<IPlatformBackend> backend{m_sut.load(m_host)};

    ASSERT_TRUE(backend);
    EXPECT_STREQ(backend->platformName(), "linux");
}

TEST_F(PlatformBackendLoaderTest, LoadedBackendIsReleasedWithoutCrashWhenLastOwnerDropped)
{
    ::setenv(kBackendEnvVar, FIXTURE_MATCH_PATH, 1);

    {
        std::shared_ptr<IPlatformBackend> backend{m_sut.load(m_host)};
        ASSERT_TRUE(backend);
        EXPECT_STREQ(backend->platformName(), "fixture");
    } // deleter runs here: rialtoDestroyPlatformBackend + dlclose, no crash
}
