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

# Reference Linux platform backend, built as a loadable .so the core dlopen()s.
# Buildable standalone for a quick compile check:
#   cmake -S media/server/platform -B media/server/platform/build && \
#   cmake --build media/server/platform/build
# In-tree wiring (add_subdirectory(platform) from media/server) lands with the
# factory loader conversion.

cmake_minimum_required(VERSION 3.16)
project(rialto-platform-linux CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED gstreamer-1.0 gstreamer-app-1.0 gstreamer-audio-1.0)

# The stable, versioned platform ABI, published as a standalone consumable so a per-platform
# backend team can build against the contract (IPlatformBackend.h, the extern "C" loader
# entrypoints, and kPlatformBackendAbiVersion) WITHOUT vendoring the core. It is an INTERFACE
# target: no compiled objects, just the header surface. IPlatformBackend.h includes the
# MediaCommon.h public header, so that include dir is part of the published surface too.
add_library(rialto-platform-abi INTERFACE)
target_include_directories(rialto-platform-abi INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/interface
    ${CMAKE_CURRENT_SOURCE_DIR}/../../public/include)

add_library(rialto-platform-linux SHARED
    source/LinuxPlatformBackend.cpp)

# The reference backend consumes the published ABI target (dogfooding the contract) and adds only
# the include dirs private to a backend implementation (its own headers + the GStreamer wrappers).
target_link_libraries(rialto-platform-linux PRIVATE rialto-platform-abi)
target_include_directories(rialto-platform-linux PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../wrappers/interface
    ${GST_INCLUDE_DIRS})

target_link_libraries(rialto-platform-linux PRIVATE ${GST_LIBRARIES})

# Publish the ABI header so a per-platform repo can install + build against it.
install(
        FILES ${CMAKE_CURRENT_SOURCE_DIR}/interface/IPlatformBackend.h
        DESTINATION include/rialto-platform-abi
        )

# Emitted as librialtoplatform-linux.so so the loader's librialtoplatform-*.so discovery
# finds it when this reference backend is shipped as a vendor object (override path).
set_target_properties(rialto-platform-linux PROPERTIES OUTPUT_NAME rialtoplatform-linux)
