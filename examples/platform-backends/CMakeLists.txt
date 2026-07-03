#
# If not stated otherwise in this file or this component's LICENSE file the
# following copyright and licenses apply:
#
# Copyright 2026 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# In-tree prototype of the per-SoC ABI-conversion layer (the shims). One SoC-free core
# (rialto-backend-common) plus one thin SocProfile per platform, each emitted as a loadable
# librialtoplatform-<soc>.so the core dlopen()s over the versioned IPlatformBackend ABI.
#
# Standalone compile-check:
#   cmake -S examples/platform-backends -B examples/platform-backends/build && \
#   cmake --build examples/platform-backends/build
#
# Relocation target: comcast-sky/rialto-platform-backends (built against the published
# rialto-platform-abi artifact instead of the in-tree header path).

cmake_minimum_required(VERSION 3.16)
project(rialto-platform-backends CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
# pbutils is needed by the real GstWrapper (gst_codec_utils_*) that the playback proof links.
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-audio-1.0 gstreamer-pbutils-1.0)

set(RIALTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# The stable, versioned platform ABI (header surface only). In the relocated repo this is the
# published rialto-platform-abi package; here it points at the in-tree header.
add_library(rialto-platform-abi INTERFACE)
target_include_directories(rialto-platform-abi INTERFACE
    ${RIALTO_ROOT}/media/server/platform/interface
    ${RIALTO_ROOT}/media/public/include)

# The SoC-free conversion core: marshalling, topology construction, capability introspection, and the
# wrapper-delegation of the live-graph audio ops. Shared by every per-SoC .so.
add_library(rialto-backend-common STATIC
    common/GenericGstBackend.cpp)
target_link_libraries(rialto-backend-common PUBLIC rialto-platform-abi ${GST_LIBRARIES})
target_include_directories(rialto-backend-common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${RIALTO_ROOT}/wrappers/interface
    ${GST_INCLUDE_DIRS})

# One loadable .so per SoC = GenericGstBackend + that SoC's SocProfile + the extern "C" loader ABI.
set(linux_SRC    linux/LinuxProfile.cpp)
set(amlogic_SRC  amlogic/AmlogicProfile.cpp)
set(broadcom_SRC broadcom/BroadcomProfile.cpp)
set(mediatek_SRC mediatek/MediatekProfile.cpp)
set(realtek_SRC  realtek/RealtekProfile.cpp)

foreach(soc linux amlogic broadcom mediatek realtek)
    add_library(rialto-platform-${soc} SHARED ${${soc}_SRC})
    target_link_libraries(rialto-platform-${soc} PRIVATE rialto-backend-common)
    set_target_properties(rialto-platform-${soc} PROPERTIES OUTPUT_NAME rialtoplatform-${soc})
endforeach()

# Test-only fixture backend (fakesink leaf) so the playback proof runs in the mocked-unit image, whose
# GStreamer lacks vendor/autodetect sinks. Same GenericGstBackend + loader path as a shipped SoC.
add_library(rialto-platform-fakesink SHARED test/TestProfileFakesink.cpp)
target_link_libraries(rialto-platform-fakesink PRIVATE rialto-backend-common)
set_target_properties(rialto-platform-fakesink PROPERTIES OUTPUT_NAME rialtoplatform-fakesink)

# Loader smoke test: dlopen each .so as the core does and check the reported ABI version.
add_executable(abi-smoke test/abi_smoke.cpp)
target_link_libraries(abi-smoke PRIVATE ${CMAKE_DL_LIBS})

# End-to-end playback proof: the real PlatformBackendLoader dlopens a shim and drives a real pipeline.
# Links the production loader + reference (its fallback) + real gst/glib wrappers + rialto logging.
add_executable(playback-proof
    test/playback_proof.cpp
    ${RIALTO_ROOT}/media/server/platform/source/PlatformBackendLoader.cpp
    ${RIALTO_ROOT}/media/server/platform/source/LinuxPlatformBackend.cpp
    ${RIALTO_ROOT}/wrappers/source/GstWrapper.cpp
    ${RIALTO_ROOT}/wrappers/source/GlibWrapper.cpp
    ${RIALTO_ROOT}/logging/source/RialtoLogging.cpp
    ${RIALTO_ROOT}/logging/source/LogFileHandle.cpp
    ${RIALTO_ROOT}/logging/source/EnvVariableParser.cpp)
target_include_directories(playback-proof PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${RIALTO_ROOT}/media/server/platform/interface
    ${RIALTO_ROOT}/media/server/platform/include
    ${RIALTO_ROOT}/media/public/include
    ${RIALTO_ROOT}/media/server/common/include
    ${RIALTO_ROOT}/wrappers/interface
    ${RIALTO_ROOT}/wrappers/include
    ${RIALTO_ROOT}/logging/include
    ${GST_INCLUDE_DIRS})
target_link_libraries(playback-proof PRIVATE ${GST_LIBRARIES} ${CMAKE_DL_LIBS})
