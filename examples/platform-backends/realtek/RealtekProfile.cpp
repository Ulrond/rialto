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

// Realtek. Deltas sourced from the dual-decode TRD-per-soc-pipeline-analysis and rdk-e/rdk-gstreamer-utils-realtek.
// Audio: rtkaudiosink (audio-master — `avsync-audio-skip` / `setAudioMasterSkip`), fed via audioconvert (split
// decode). NB: RTK additionally needs `audioresample` + `volume` in the audio tail (per TRD) — not yet modelled by
// SocProfile; a topology-hook extension for #7. Video: westerossink (embeds the decoder; plane-bound). SoC audio ops
// delegate to rdk_gstreamer_utils_realtek.so. Real-HW certification against #7 confirms the final values.
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
    p.name = "realtek";
    p.audioSinkFactory = "rtkaudiosink";
    p.videoSinkFactory = "westerossink";
    p.audioTopology = AudioTopology::SplitDecode;
    p.rateStrategy = RateStrategy::InstantRateEvent;
    p.videoMaster = false; // audio-master
    p.socAudioPath = true;
    p.bindsVideoPlane = true;
    return p;
}();
} // namespace

RIALTO_DEFINE_PLATFORM_BACKEND(kProfile)
