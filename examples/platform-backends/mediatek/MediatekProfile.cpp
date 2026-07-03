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

// MediaTek: ABI-conformant stub. Real elements exist in rdk-e/gst-plugins-soc-mediatek
// (gstmtkaudiosink, mtk video sink). SoC audio ops delegated to rdk_gstreamer_utils_mtk.so.
// Fields marked TBD are placeholders to be confirmed against the vendor lib (Slice E).
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
    p.videoSinkFactory = "mtkvideosink";
    p.audioTopology = AudioTopology::SplitDecode; // TBD: confirm against the vendor lib
    p.rateStrategy = RateStrategy::InstantRateEvent;
    p.videoMaster = true; // TBD: confirm master role
    p.socAudioPath = true;
    p.bindsVideoPlane = true; // TBD: confirm plane-binding mechanism
    return p;
}();
} // namespace

RIALTO_DEFINE_PLATFORM_BACKEND(kProfile)
