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

// Amlogic: amlhalasink fuses audio decode+render (compressed passthrough), so the audio path is
// source -> amlhalasink and the platform is audio-master. Video is a plane-bound westerossink. The
// SoC audio ops (fade / gap / codec-switch) delegate to the device's rdk_gstreamer_utils_amlogic.so.
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
    p.name = "amlogic";
    p.audioSinkFactory = "amlhalasink";
    p.videoSinkFactory = "westerossink";
    p.audioTopology = AudioTopology::FusedSink;
    p.rateStrategy = RateStrategy::InstantRateEvent;
    p.videoMaster = false; // amlhalasink-based audio path is audio-master
    p.socAudioPath = true;
    p.bindsVideoPlane = true;
    return p;
}();
} // namespace

RIALTO_DEFINE_PLATFORM_BACKEND(kProfile)
