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

// Broadcom: ABI-conformant stub. Audio via brcmaudiosink (MS12), plane-bound westerossink for video,
// SoC audio ops delegated to rdk_gstreamer_utils_brcm.so. Fields marked TBD are placeholders to be
// confirmed against the vendor lib before this stub is certified (Slice E).
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
    p.name = "broadcom";
    p.audioSinkFactory = "brcmaudiosink";
    p.videoSinkFactory = "westerossink";
    p.audioTopology = AudioTopology::SplitDecode; // TBD: confirm fused vs split against the vendor lib
    p.rateStrategy = RateStrategy::InstantRateEvent;
    p.videoMaster = true; // TBD: confirm master role
    p.socAudioPath = true;
    p.bindsVideoPlane = true;
    return p;
}();
} // namespace

RIALTO_DEFINE_PLATFORM_BACKEND(kProfile)
