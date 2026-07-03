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

#include "GenericGstBackend.h"
#include "BackendLog.h"
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
const char kCustomInstantRateChangeEventName[] = "custom-instant-rate-change";

firebolt::rialto::wrappers::rgu_Ease toRguEase(firebolt::rialto::EaseType easeType)
{
    switch (easeType)
    {
    case firebolt::rialto::EaseType::EASE_IN_CUBIC:
        return firebolt::rialto::wrappers::rgu_Ease::EaseInCubic;
    case firebolt::rialto::EaseType::EASE_OUT_CUBIC:
        return firebolt::rialto::wrappers::rgu_Ease::EaseOutCubic;
    case firebolt::rialto::EaseType::EASE_LINEAR:
    default:
        return firebolt::rialto::wrappers::rgu_Ease::EaseLinear;
    }
}
} // namespace

namespace firebolt::rialto::server::backends
{
GenericGstBackend::GenericGstBackend(const SocProfile &profile,
                                     const firebolt::rialto::server::PlatformHostContext &host)
    : m_profile{profile}, m_gstWrapper{host.gstWrapper}, m_glibWrapper{host.glibWrapper},
      m_rdkGstreamerUtilsWrapper{host.rdkGstreamerUtilsWrapper}
{
}

const char *GenericGstBackend::platformName() const
{
    return m_profile.name;
}

GstElement *GenericGstBackend::createAudioSink(const std::string &name)
{
    if (!m_gstWrapper)
        return nullptr;
    return m_gstWrapper->gstElementFactoryMake(m_profile.audioSinkFactory, name.c_str());
}

GstElement *GenericGstBackend::createVideoSink(const std::string &name, uint32_t videoId)
{
    if (!m_gstWrapper)
        return nullptr;
    GstElement *videoSink = m_gstWrapper->gstElementFactoryMake(m_profile.videoSinkFactory, name.c_str());
    if (videoSink && m_profile.bindsVideoPlane)
    {
        // A plane-bound vendor sink (e.g. westerossink) is pinned to the output plane here via the
        // SoC's setWesterosSinkVideoID(videoId). The concrete vendor call is a per-SoC extension point
        // (see README); the prototype records the intent so the wiring is observable end-to-end.
        BE_LOG_DEBUG("%s: bind video sink '%s' to plane videoId=%u (per-SoC hook)", m_profile.name,
                     m_profile.videoSinkFactory, videoId);
    }
    else
    {
        (void)videoId;
    }
    return videoSink;
}

firebolt::rialto::server::PlatformMediaPath GenericGstBackend::buildAudioPath(GstElement *pipeline, GstElement *source)
{
    if (!m_gstWrapper || !pipeline || !source)
        return {};

    GstBin *pipelineBin = GST_BIN(pipeline);

    if (m_profile.audioTopology == AudioTopology::FusedSink)
    {
        // Fused HW audio path (compressed passthrough): source -> vendor-sink. The engine wires no
        // decoder, so decodebin/decoderLinkTarget are null.
        GstElement *audioSink = createAudioSink("audiosink");
        if (!audioSink)
        {
            BE_LOG_ERROR("%s: failed to create fused audio sink '%s'", m_profile.name, m_profile.audioSinkFactory);
            return {};
        }
        m_gstWrapper->gstBinAdd(pipelineBin, audioSink);
        m_gstWrapper->gstElementLink(source, audioSink);
        return {audioSink, nullptr, nullptr};
    }

    // SplitDecode: decodebin autoplugs only the decoder; the static tail and the sink are explicit.
    GstElement *decodebin = m_gstWrapper->gstElementFactoryMake("decodebin", "auddecodebin");
    GstElement *audioConvert = m_gstWrapper->gstElementFactoryMake("audioconvert", "audconvert");
    GstElement *audioResample = m_gstWrapper->gstElementFactoryMake("audioresample", "audresample");
    GstElement *audioSink = createAudioSink("audiosink");

    if (!decodebin || !audioConvert || !audioResample || !audioSink)
    {
        BE_LOG_ERROR("%s: failed to create the split audio path elements", m_profile.name);
        return {};
    }

    m_gstWrapper->gstBinAdd(pipelineBin, decodebin);
    m_gstWrapper->gstBinAdd(pipelineBin, audioConvert);
    m_gstWrapper->gstBinAdd(pipelineBin, audioResample);
    m_gstWrapper->gstBinAdd(pipelineBin, audioSink);

    // Static links: appsrc -> decodebin, and the static tail audioconvert -> audioresample -> sink. The
    // decoder's src pad is created dynamically by decodebin, so the engine links it to decoderLinkTarget.
    m_gstWrapper->gstElementLink(source, decodebin);
    m_gstWrapper->gstElementLink(audioConvert, audioResample);
    m_gstWrapper->gstElementLink(audioResample, audioSink);

    return {audioSink, decodebin, audioConvert};
}

firebolt::rialto::server::PlatformMediaPath GenericGstBackend::buildVideoPath(GstElement *pipeline, GstElement *source,
                                                                              uint32_t videoId)
{
    if (!m_gstWrapper || !pipeline || !source)
        return {};

    // decodebin autoplugs only the decoder; the decoder's dynamic src pad links directly to the sink
    // (decoderLinkTarget == the sink), mirroring the reference video topology.
    GstElement *decodebin = m_gstWrapper->gstElementFactoryMake("decodebin", "viddecodebin");
    GstElement *videoSink = createVideoSink("videosink", videoId);

    if (!decodebin || !videoSink)
    {
        BE_LOG_ERROR("%s: failed to create the video path elements", m_profile.name);
        return {};
    }

    GstBin *pipelineBin = GST_BIN(pipeline);
    m_gstWrapper->gstBinAdd(pipelineBin, decodebin);
    m_gstWrapper->gstBinAdd(pipelineBin, videoSink);

    m_gstWrapper->gstElementLink(source, decodebin);

    return {videoSink, decodebin, videoSink};
}

bool GenericGstBackend::isVideoMaster() const
{
    return m_profile.videoMaster;
}

bool GenericGstBackend::applyPlaybackRate(GstElement *pipeline, double rate)
{
    if (!m_gstWrapper || !pipeline)
        return false;

    // SegmentToSinkPad is a per-SoC override (a new-segment event to the audio sink pad); the concrete
    // vendor form is an extension point. The prototype uses the instant-rate-change event for both so
    // the path is exercised end-to-end.
    if (m_profile.rateStrategy == RateStrategy::SegmentToSinkPad)
        BE_LOG_DEBUG("%s: SegmentToSinkPad rate is a per-SoC hook; using instant-rate event", m_profile.name);

    GstStructure *structure{
        m_gstWrapper->gstStructureNew(kCustomInstantRateChangeEventName, "rate", G_TYPE_DOUBLE, rate, NULL)};
    return m_gstWrapper->gstElementSendEvent(pipeline,
                                             m_gstWrapper->gstEventNewCustom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB, structure));
}

bool GenericGstBackend::isAudioFadeSupported() const
{
    // Delegated: the device's SoC lib answers. A SoC with no audio path (the reference) reports false and
    // the engine uses the generic sink "audio-fade" property.
    if (!m_profile.socAudioPath || !m_rdkGstreamerUtilsWrapper)
        return false;
    return m_rdkGstreamerUtilsWrapper->isSocAudioFadeSupported();
}

void GenericGstBackend::audioFade(double target, uint32_t duration, firebolt::rialto::EaseType easeType)
{
    // Delegated to the SoC audio path; a no-op where the platform has none.
    if (!m_profile.socAudioPath || !m_rdkGstreamerUtilsWrapper)
        return;
    m_rdkGstreamerUtilsWrapper->doAudioEasingonSoc(target, duration, toRguEase(easeType));
}

bool GenericGstBackend::processAudioGap(GstElement *pipeline, int64_t position, uint32_t duration,
                                        int64_t discontinuityGap, bool audioAac)
{
    // Delegated to the SoC audio path; a no-op returning false where the platform has none.
    if (!m_profile.socAudioPath || !m_rdkGstreamerUtilsWrapper)
        return false;
    m_rdkGstreamerUtilsWrapper->processAudioGap(pipeline, static_cast<gint64>(position),
                                                static_cast<gint32>(duration), static_cast<gint64>(discontinuityGap),
                                                audioAac);
    return true;
}

bool GenericGstBackend::switchAudioCodec(const firebolt::rialto::server::AudioCodecSwitchContext &ctx)
{
    if (!m_gstWrapper || !m_glibWrapper)
        return false;
    if (!m_rdkGstreamerUtilsWrapper)
    {
        BE_LOG_ERROR("%s: switchAudioCodec: no rdk-gstreamer-utils wrapper available", m_profile.name);
        return false;
    }

    // Job (1): marshal the neutral AudioCodecSwitchContext into the vendor-facing types. No engine state
    // is touched; the current-codec bit propagates back via ctx.isAudioAacState below.
    firebolt::rialto::wrappers::PlaybackGroupPrivate group;
    group.m_gstPipeline = ctx.pipeline;
    group.m_curAudioPlaysinkBin = ctx.audioPlaysinkBin;
    group.m_curAudioDecodeBin = ctx.audioDecodeBin;
    group.m_curAudioDecoder = ctx.audioDecoder;
    group.m_curAudioParse = ctx.audioParse;
    group.m_curAudioTypefind = ctx.audioTypefind;
    group.m_isAudioAAC = (ctx.isAudioAacState ? *ctx.isAudioAacState : false);

    firebolt::rialto::wrappers::AudioAttributesPrivate attr;
    attr.m_codecParam = (ctx.codecParam ? ctx.codecParam : "");
    attr.m_numberOfChannels = ctx.numberOfChannels;
    attr.m_samplesPerSecond = ctx.samplesPerSecond;
    attr.m_bitrate = ctx.bitrate;
    attr.m_blockAlignment = ctx.blockAlignment;
    attr.m_codecSpecificData = ctx.codecSpecificData;
    attr.m_codecSpecificDataLen = ctx.codecSpecificDataLen;

    long long currentDispPts{0};        // NOLINT(runtime/int)
    int sampleAttributes{0};            // the wrapper only checks this param != NULL
    std::uint32_t status{0};            // must be 0 for the switch to run
    unsigned int ui32Delay{0};          // output
    long long audioChangeTargetPts{-1}; // NOLINT(runtime/int) output
    unsigned int audioChangeStage{0};   // output
    bool audioAac{group.m_isAudioAAC};
    bool retVal{false}; // output

    // Job (2): delegate to the device's rdk_gstreamer_utils_<soc>.so. The vendor lib does the
    // SoC-specific work (including any amlhalasink handling) — the shim names no SoC.
    GstCaps *appsrcCaps = (ctx.audioAppSrc ? m_gstWrapper->gstAppSrcGetCaps(GST_APP_SRC(ctx.audioAppSrc)) : nullptr);
    bool result = m_rdkGstreamerUtilsWrapper->performAudioTrackCodecChannelSwitch(
        &group, &sampleAttributes, &attr, &status, &ui32Delay, &audioChangeTargetPts, &currentDispPts,
        &audioChangeStage, &appsrcCaps, &audioAac, ctx.svpEnabled, ctx.audioAppSrc, &retVal);

    // The rdk-gstreamer-utils path leaves appsrcCaps owned here; release it.
    if (appsrcCaps)
        m_gstWrapper->gstCapsUnref(appsrcCaps);

    // Write the resulting current-codec state back to the engine's playback group.
    if (ctx.isAudioAacState)
        *ctx.isAudioAacState = group.m_isAudioAAC;

    if (!result || !retVal)
    {
        BE_LOG_WARN("%s: performAudioTrackCodecChannelSwitch failed (result=%d retVal=%d)", m_profile.name, result,
                    retVal);
        return false;
    }
    return true;
}

std::vector<std::string>
GenericGstBackend::getSupportedProperties(firebolt::rialto::MediaSourceType mediaType,
                                          const std::vector<std::string> &propertyNames) const
{
    std::vector<std::string> propertiesFound;
    if (!m_gstWrapper || !m_glibWrapper)
        return propertiesFound;

    // The backend is the capability authority (ABI v8): it introspects the installed sink/decoder/parser
    // elements it can use rather than the engine scanning the registry. SoC-agnostic — identical across
    // every profile.
    GstElementFactoryListType factoryListType{GST_ELEMENT_FACTORY_TYPE_SINK | GST_ELEMENT_FACTORY_TYPE_DECODER |
                                              GST_ELEMENT_FACTORY_TYPE_PARSER};
    {
        static const std::unordered_map<firebolt::rialto::MediaSourceType, GstElementFactoryListType>
            kLookupExtraConditions{{firebolt::rialto::MediaSourceType::AUDIO, GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO},
                                   {firebolt::rialto::MediaSourceType::VIDEO, GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO},
                                   {firebolt::rialto::MediaSourceType::SUBTITLE,
                                    GST_ELEMENT_FACTORY_TYPE_MEDIA_SUBTITLE}};
        auto i = kLookupExtraConditions.find(mediaType);
        if (i != kLookupExtraConditions.end())
            factoryListType |= i->second;
    }

    GList *factories{m_gstWrapper->gstElementFactoryListGetElements(factoryListType, GST_RANK_NONE)};

    std::unordered_set<std::string> propertiesToLookFor{propertyNames.begin(), propertyNames.end()};
    for (GList *iter = factories; iter != nullptr && !propertiesToLookFor.empty(); iter = iter->next)
    {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(iter->data);

        // Instantiate the object: fetching the class directly was found to sometimes return no properties.
        GstElement *elementObj{m_gstWrapper->gstElementFactoryCreate(factory, nullptr)};
        if (elementObj)
        {
            GParamSpec **props;
            guint nProps;
            props = m_glibWrapper->gObjectClassListProperties(G_OBJECT_GET_CLASS(elementObj), &nProps);
            if (props)
            {
                for (guint j = 0; j < nProps && !propertiesToLookFor.empty(); ++j)
                {
                    std::string propName{props[j]->name};
                    auto it = propertiesToLookFor.find(propName);
                    if (it != propertiesToLookFor.end())
                    {
                        propertiesFound.push_back(std::move(propName));
                        propertiesToLookFor.erase(it);
                    }
                }
                m_glibWrapper->gFree(props);
            }
            m_gstWrapper->gstObjectUnref(elementObj);
        }
    }

    // A SoC audio path may perform audio fade even when no element advertises the "audio-fade" property.
    if (propertiesToLookFor.find("audio-fade") != propertiesToLookFor.end() && isAudioFadeSupported())
        propertiesFound.push_back("audio-fade");

    m_gstWrapper->gstPluginFeatureListFree(factories);
    return propertiesFound;
}
} // namespace firebolt::rialto::server::backends
