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

// Test-only profile (NOT a shipped SoC): a fakesink-leaf backend so the end-to-end playback proof runs in
// the mocked-unit build image, whose GStreamer has core+base plugins but no vendor/autodetect sinks. It
// exercises the exact same GenericGstBackend + loader path as a real SoC — only the leaf element differs.
#include "PlatformBackendEntry.h"
#include "SocProfile.h"

namespace
{
using firebolt::rialto::server::backends::AudioTopology;
using firebolt::rialto::server::backends::RateStrategy;
using firebolt::rialto::server::backends::SocProfile;

const SocProfile kProfile = []
{
    SocProfile p;
    p.name = "fakesink-test";
    p.audioSinkFactory = "fakesink";
    p.videoSinkFactory = "fakesink";
    p.audioTopology = AudioTopology::SplitDecode;
    p.rateStrategy = RateStrategy::InstantRateEvent;
    p.videoMaster = true;
    p.socAudioPath = false;
    p.bindsVideoPlane = false;
    return p;
}();
} // namespace

RIALTO_DEFINE_PLATFORM_BACKEND(kProfile)
