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

// MediaTek. Deltas sourced from the dual-decode TRD-per-soc-pipeline-analysis, rdk-e/gst-plugins-soc-mediatek, and
// rdk-e/rdk-gstreamer-utils-mtk. Audio: mtkaudiosink (audio-master — `switchToAudioMasterMode_soc`), fed via
// audioconvert (split decode). Video: westerossink (embeds the decoder; plane-bound). SoC audio ops delegate to
// rdk_gstreamer_utils_mtk.so. Real-HW certification against #7 confirms the final values.
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
    p.name = "mediatek";
    p.audioSinkFactory = "mtkaudiosink";
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
