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

add_library(rialto-platform-linux SHARED
    source/LinuxPlatformBackend.cpp)

target_include_directories(rialto-platform-linux PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/interface
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../wrappers/interface
    ${GST_INCLUDE_DIRS})

target_link_libraries(rialto-platform-linux PRIVATE ${GST_LIBRARIES})

set_target_properties(rialto-platform-linux PROPERTIES OUTPUT_NAME rialto-platform-linux)
