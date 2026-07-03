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

#ifndef RIALTO_PLATFORM_BACKENDS_COMMON_SOC_PROFILE_H_
#define RIALTO_PLATFORM_BACKENDS_COMMON_SOC_PROFILE_H_

namespace firebolt::rialto::server::backends
{
/**
 * @brief How the backend wires the audio decode path.
 *
 * SplitDecode: source -> decodebin -> audioconvert -> audioresample -> sink (the x86 reference
 * topology; decodebin autoplugs the stock decoder). FusedSink: source -> vendor-sink (the SoC sink
 * fuses decode+render / takes compressed passthrough), so the engine wires no decoder.
 */
enum class AudioTopology
{
    SplitDecode,
    FusedSink
};

/**
 * @brief How the backend signals a playback-rate change.
 *
 * InstantRateEvent: a custom-instant-rate-change event sent downstream on the pipeline (the reference
 * path). SegmentToSinkPad: a new-segment event to the audio sink pad (a per-SoC variant; the concrete
 * vendor implementation is a per-SoC override — see README "extension points").
 */
enum class RateStrategy
{
    InstantRateEvent,
    SegmentToSinkPad
};

/**
 * @brief The per-SoC delta that parameterises GenericGstBackend.
 *
 * Everything SoC-specific about a platform reduces to this small descriptor; the marshalling, topology
 * construction, and capability introspection are SoC-free and live in GenericGstBackend. "Linux is just
 * another SoC" — the reference is simply the profile with autosinks, video-master, and no SoC audio path.
 *
 * Pure data by design (see README). Genuine behavioural deltas that cannot be expressed as data — e.g.
 * a westeros plane binding or a SegmentToSinkPad rate — are the documented extension points: add a hook
 * to this struct, or subclass GenericGstBackend, only when a SoC actually needs it.
 */
struct SocProfile
{
    const char *name{"unknown"};              // platformName()
    const char *audioSinkFactory{"fakesink"}; // e.g. autoaudiosink / amlhalasink / brcmaudiosink
    const char *videoSinkFactory{"fakesink"}; // e.g. autovideosink / westerossink / brcmvideosink

    AudioTopology audioTopology{AudioTopology::SplitDecode};
    RateStrategy rateStrategy{RateStrategy::InstantRateEvent};

    bool videoMaster{true};    // isVideoMaster()
    bool socAudioPath{false};  // has a SoC audio path -> delegate fade/gap/fade-supported to the wrapper
    bool bindsVideoPlane{false}; // buildVideoPath binds the vendor sink to videoId (e.g. setWesterosSinkVideoID)
};
} // namespace firebolt::rialto::server::backends

#endif // RIALTO_PLATFORM_BACKENDS_COMMON_SOC_PROFILE_H_
