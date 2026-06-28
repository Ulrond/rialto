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

#include "LinuxPlatformBackend.h"
#include "IRdkGstreamerUtilsWrapper.h"
#include "RialtoServerLogging.h"
#include <cstring>
#include <ctime>
#include <new>

namespace
{
const char kCustomInstantRateChangeEventName[] = "custom-instant-rate-change";
} // namespace

namespace firebolt::rialto::server
{
LinuxPlatformBackend::LinuxPlatformBackend(const PlatformHostContext &host)
    : m_gstWrapper{host.gstWrapper}, m_glibWrapper{host.glibWrapper},
      m_rdkGstreamerUtilsWrapper{host.rdkGstreamerUtilsWrapper}
{
}

const char *LinuxPlatformBackend::platformName() const
{
    return "linux";
}

GstElement *LinuxPlatformBackend::createAudioSink(const std::string &name)
{
    if (!m_gstWrapper)
        return nullptr;
    // The reference backend names no SoC: it returns only the generic autoaudiosink.
    // Vendor sink selection (amlhalasink / rtkaudiosink) lives in a per-SoC .so loaded
    // by PlatformBackendLoader over the versioned IPlatformBackend ABI.
    return m_gstWrapper->gstElementFactoryMake("autoaudiosink", name.c_str());
}

GstElement *LinuxPlatformBackend::createVideoSink(const std::string &name, uint32_t videoId)
{
    if (!m_gstWrapper)
        return nullptr;
    // The reference backend is plane-agnostic and names no SoC: it returns only the
    // generic autovideosink. The plane-bound vendor sink (e.g. westerossink via
    // setWesterosSinkVideoID) lives in a per-SoC .so.
    (void)videoId;
    return m_gstWrapper->gstElementFactoryMake("autovideosink", name.c_str());
}

PlatformMediaPath LinuxPlatformBackend::buildAudioPath(GstElement *pipeline, GstElement *source)
{
    if (!m_gstWrapper || !pipeline || !source)
        return {};

    // Reference (x86) audio topology, owned here as a peer to any per-SoC backend. decodebin autoplugs
    // only the decoder, keeping a ~4-element graph without a hand-maintained codec->factory map; the
    // static tail and the autoaudiosink are built explicitly. A device backend with a fused HW audio
    // path would instead build source -> vendor-sink and return {sink, nullptr, nullptr}.
    GstElement *decodebin = m_gstWrapper->gstElementFactoryMake("decodebin", "auddecodebin");
    GstElement *audioConvert = m_gstWrapper->gstElementFactoryMake("audioconvert", "audconvert");
    GstElement *audioResample = m_gstWrapper->gstElementFactoryMake("audioresample", "audresample");
    GstElement *audioSink = createAudioSink("audiosink");

    if (!decodebin || !audioConvert || !audioResample || !audioSink)
    {
        RIALTO_SERVER_LOG_ERROR("Failed to create the reference audio path elements");
        return {};
    }

    GstBin *pipelineBin = GST_BIN(pipeline);
    m_gstWrapper->gstBinAdd(pipelineBin, decodebin);
    m_gstWrapper->gstBinAdd(pipelineBin, audioConvert);
    m_gstWrapper->gstBinAdd(pipelineBin, audioResample);
    m_gstWrapper->gstBinAdd(pipelineBin, audioSink);

    // Static links: appsrc -> decodebin, and the static tail audioconvert -> audioresample -> sink.
    // The decoder's src pad is created dynamically by decodebin, so the engine links it to the returned
    // decoderLinkTarget (audioconvert) in its pad-added handler.
    m_gstWrapper->gstElementLink(source, decodebin);
    m_gstWrapper->gstElementLink(audioConvert, audioResample);
    m_gstWrapper->gstElementLink(audioResample, audioSink);

    return {audioSink, decodebin, audioConvert};
}

PlatformMediaPath LinuxPlatformBackend::buildVideoPath(GstElement *pipeline, GstElement *source, uint32_t videoId)
{
    if (!m_gstWrapper || !pipeline || !source)
        return {};

    // Reference (x86) video topology, mirroring the audio one: decodebin autoplugs only the decoder and
    // the plane-agnostic autovideosink is the output. The decoder's src pad links directly to the sink
    // (no convert tail), so the returned decoderLinkTarget is the sink itself.
    GstElement *decodebin = m_gstWrapper->gstElementFactoryMake("decodebin", "viddecodebin");
    GstElement *videoSink = createVideoSink("videosink", videoId);

    if (!decodebin || !videoSink)
    {
        RIALTO_SERVER_LOG_ERROR("Failed to create the reference video path elements");
        return {};
    }

    GstBin *pipelineBin = GST_BIN(pipeline);
    m_gstWrapper->gstBinAdd(pipelineBin, decodebin);
    m_gstWrapper->gstBinAdd(pipelineBin, videoSink);

    // Static link appsrc -> decodebin; the decoder's dynamic src pad is linked to the video sink by the
    // engine's pad-added handler (decoderLinkTarget == the sink).
    m_gstWrapper->gstElementLink(source, decodebin);

    return {videoSink, decodebin, videoSink};
}

bool LinuxPlatformBackend::isVideoMaster() const
{
    // The reference backend has no amlhalasink-style audio-master sink, so the Linux
    // platform is video-master. The audio-master vendor cases live in their per-SoC .so.
    return true;
}

bool LinuxPlatformBackend::applyPlaybackRate(GstElement *pipeline, double rate)
{
    if (!m_gstWrapper || !pipeline)
        return false;
    // The reference path signals the rate as a custom-instant-rate-change event sent
    // downstream on the pipeline. The sink-pad new-segment variant lives in a per-SoC .so.
    GstStructure *structure{
        m_gstWrapper->gstStructureNew(kCustomInstantRateChangeEventName, "rate", G_TYPE_DOUBLE, rate, NULL)};
    return m_gstWrapper->gstElementSendEvent(pipeline,
                                             m_gstWrapper->gstEventNewCustom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB, structure));
}

bool LinuxPlatformBackend::isAudioFadeSupported() const
{
    // The reference backend has no SoC audio path that eases volume, so the engine uses the
    // generic sink "audio-fade" property. SoC audio fade lives in a per-SoC .so.
    return false;
}

void LinuxPlatformBackend::audioFade(double target, uint32_t duration, firebolt::rialto::EaseType easeType)
{
    // No-op: the reference backend performs no SoC audio fade. SoC fade lives in a per-SoC .so.
    (void)target;
    (void)duration;
    (void)easeType;
}

bool LinuxPlatformBackend::processAudioGap(GstElement *pipeline, int64_t position, uint32_t duration,
                                           int64_t discontinuityGap, bool audioAac)
{
    // No-op: the reference backend handles no SoC audio gap. SoC audio-gap handling lives in a per-SoC .so.
    (void)pipeline;
    (void)position;
    (void)duration;
    (void)discontinuityGap;
    (void)audioAac;
    return false;
}

// ---------------------------------------------------------------------------
// Transitional amlhalasink audio codec switch.
//
// Moved verbatim out of GstGenericPlayer: the in-Rialto fork of
// rdk_gstreamer_utils::performAudioTrackCodecChannelSwitch plus its helpers. It
// operates on a local PlaybackGroupPrivate built from the neutral
// AudioCodecSwitchContext, not on any engine state. transitional -> per-SoC .so
// ---------------------------------------------------------------------------

void LinuxPlatformBackend::configAudioCap(firebolt::rialto::wrappers::AudioAttributesPrivate *pAttrib, bool *audioaac,
                                          bool svpenabled, GstCaps **appsrcCaps)
{
    // this function comes from rdk_gstreamer_utils
    if (!pAttrib || !audioaac || !appsrcCaps)
    {
        RIALTO_SERVER_LOG_ERROR("configAudioCap: invalid null parameter");
        return;
    }
    gchar *capsString;
    RIALTO_SERVER_LOG_DEBUG("Config audio codec %s sampling rate %d channel %d alignment %d",
                            pAttrib->m_codecParam.c_str(), pAttrib->m_samplesPerSecond, pAttrib->m_numberOfChannels,
                            pAttrib->m_blockAlignment);
    if (pAttrib->m_codecParam.compare(0, 4, std::string("mp4a")) == 0)
    {
        RIALTO_SERVER_LOG_DEBUG("Using AAC");
        capsString = m_glibWrapper->gStrdupPrintf("audio/mpeg, mpegversion=4, enable-svp=(string)%s",
                                                  svpenabled ? "true" : "false");
        *audioaac = true;
    }
    else
    {
        RIALTO_SERVER_LOG_DEBUG("Using EAC3");
        capsString = m_glibWrapper->gStrdupPrintf("audio/x-eac3, framed=(boolean)true, rate=(int)%u, channels=(int)%u, "
                                                  "alignment=(string)frame, enable-svp=(string)%s",
                                                  pAttrib->m_samplesPerSecond, pAttrib->m_numberOfChannels,
                                                  svpenabled ? "true" : "false");
        *audioaac = false;
    }
    *appsrcCaps = m_gstWrapper->gstCapsFromString(capsString);
    m_glibWrapper->gFree(capsString);
}

void LinuxPlatformBackend::haltAudioPlayback(firebolt::rialto::wrappers::PlaybackGroupPrivate &group)
{
    // this function comes from rdk_gstreamer_utils
    if (!group.m_curAudioPlaysinkBin || !group.m_curAudioDecodeBin)
    {
        RIALTO_SERVER_LOG_ERROR("haltAudioPlayback: audio playsink bin or decode bin is null");
        return;
    }
    GstState currentState{GST_STATE_VOID_PENDING}, pending{GST_STATE_VOID_PENDING};

    // Transition Playsink to Ready
    if (GST_STATE_CHANGE_FAILURE == m_gstWrapper->gstElementSetState(group.m_curAudioPlaysinkBin, GST_STATE_READY))
    {
        RIALTO_SERVER_LOG_WARN("Failed to set AudioPlaysinkBin to READY");
        return;
    }
    m_gstWrapper->gstElementGetState(group.m_curAudioPlaysinkBin, &currentState, &pending, GST_CLOCK_TIME_NONE);
    if (currentState == GST_STATE_PAUSED)
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioPlaySinkBin State = %d", currentState);
    // Transition Decodebin to Paused
    if (GST_STATE_CHANGE_FAILURE == m_gstWrapper->gstElementSetState(group.m_curAudioDecodeBin, GST_STATE_PAUSED))
    {
        RIALTO_SERVER_LOG_WARN("Failed to set AudioDecodeBin to PAUSED");
        return;
    }
    m_gstWrapper->gstElementGetState(group.m_curAudioDecodeBin, &currentState, &pending, GST_CLOCK_TIME_NONE);
    if (currentState == GST_STATE_PAUSED)
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current DecodeBin State = %d", currentState);
}

void LinuxPlatformBackend::resumeAudioPlayback(firebolt::rialto::wrappers::PlaybackGroupPrivate &group)
{
    // this function comes from rdk_gstreamer_utils
    if (!group.m_curAudioPlaysinkBin || !group.m_curAudioDecodeBin)
    {
        RIALTO_SERVER_LOG_ERROR("resumeAudioPlayback: audio playsink bin or decode bin is null");
        return;
    }
    GstState currentState{GST_STATE_VOID_PENDING}, pending{GST_STATE_VOID_PENDING};
    m_gstWrapper->gstElementSyncStateWithParent(group.m_curAudioPlaysinkBin);
    m_gstWrapper->gstElementGetState(group.m_curAudioPlaysinkBin, &currentState, &pending, GST_CLOCK_TIME_NONE);
    RIALTO_SERVER_LOG_DEBUG("OTF -> AudioPlaysinkbin State = %d Pending = %d", currentState, pending);
    m_gstWrapper->gstElementSyncStateWithParent(group.m_curAudioDecodeBin);
    m_gstWrapper->gstElementGetState(group.m_curAudioDecodeBin, &currentState, &pending, GST_CLOCK_TIME_NONE);
    RIALTO_SERVER_LOG_DEBUG("OTF -> Decodebin State = %d Pending = %d", currentState, pending);
}

void LinuxPlatformBackend::firstTimeSwitchFromAC3toAAC(firebolt::rialto::wrappers::PlaybackGroupPrivate &group,
                                                       GstCaps *newAudioCaps)
{
    // this function comes from rdk_gstreamer_utils
    if (!group.m_curAudioTypefind || !group.m_curAudioDecodeBin)
    {
        RIALTO_SERVER_LOG_ERROR("firstTimeSwitchFromAC3toAAC: audio typefind or decode bin is null");
        return;
    }
    GstState currentState{GST_STATE_VOID_PENDING}, pending{GST_STATE_VOID_PENDING};
    GstPad *pTypfdSrcPad = NULL;
    GstPad *pTypfdSrcPeerPad = NULL;
    GstPad *pNewAudioDecoderSrcPad = NULL;
    GstElement *newAudioParse = NULL;
    GstElement *newAudioDecoder = NULL;
    GstElement *newQueue = NULL;
    gboolean linkRet = false;

    /* Get the SinkPad of ASink - pTypfdSrcPeerPad */
    if ((pTypfdSrcPad = m_gstWrapper->gstElementGetStaticPad(group.m_curAudioTypefind, "src")) != NULL) // Unref the Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current Typefind SrcPad = %p", pTypfdSrcPad);
    if ((pTypfdSrcPeerPad = m_gstWrapper->gstPadGetPeer(pTypfdSrcPad)) != NULL) // Unref the Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current Typefind Src Downstream Element Pad = %p", pTypfdSrcPeerPad);
    // AudioDecoder Downstream Unlink
    if (m_gstWrapper->gstPadUnlink(pTypfdSrcPad, pTypfdSrcPeerPad) == FALSE)
        RIALTO_SERVER_LOG_DEBUG("OTF -> Typefind Downstream Unlink Failed");
    newAudioParse = m_gstWrapper->gstElementFactoryMake("aacparse", "aacparse");
    newAudioDecoder = m_gstWrapper->gstElementFactoryMake("avdec_aac", "avdec_aac");
    newQueue = m_gstWrapper->gstElementFactoryMake("queue", "aqueue");
    // Add new Decoder to Decodebin
    if (m_gstWrapper->gstBinAdd(GST_BIN(group.m_curAudioDecodeBin.load()), newAudioDecoder) == TRUE)
    {
        RIALTO_SERVER_LOG_DEBUG("OTF -> Added New AudioDecoder = %p", newAudioDecoder);
    }
    // Add new Parser to Decodebin
    if (m_gstWrapper->gstBinAdd(GST_BIN(group.m_curAudioDecodeBin.load()), newAudioParse) == TRUE)
    {
        RIALTO_SERVER_LOG_DEBUG("OTF -> Added New AudioParser = %p", newAudioParse);
    }
    // Add new Queue to Decodebin
    if (m_gstWrapper->gstBinAdd(GST_BIN(group.m_curAudioDecodeBin.load()), newQueue) == TRUE)
    {
        RIALTO_SERVER_LOG_DEBUG("OTF -> Added New queue = %p", newQueue);
    }
    if ((pNewAudioDecoderSrcPad = m_gstWrapper->gstElementGetStaticPad(newAudioDecoder, "src")) != NULL) // Unref the Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioDecoder Src Pad = %p", pNewAudioDecoderSrcPad);
    // Connect decoder to ASINK
    if (m_gstWrapper->gstPadLink(pNewAudioDecoderSrcPad, pTypfdSrcPeerPad) != GST_PAD_LINK_OK)
        RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioDecoder Downstream Link Failed");
    linkRet = m_gstWrapper->gstElementLink(newAudioParse, newQueue) &&
              m_gstWrapper->gstElementLink(newQueue, newAudioDecoder);
    if (!linkRet)
        RIALTO_SERVER_LOG_DEBUG("OTF -> Downstream Link Failed for typefind, parser, decoder");
    /* Force Caps */
    RIALTO_SERVER_LOG_DEBUG("OTF -> Typefind Setting to READY");
    if (GST_STATE_CHANGE_FAILURE == m_gstWrapper->gstElementSetState(group.m_curAudioTypefind, GST_STATE_READY))
    {
        RIALTO_SERVER_LOG_WARN("Failed to set Typefind to READY");
        m_gstWrapper->gstObjectUnref(pTypfdSrcPad);
        m_gstWrapper->gstObjectUnref(pTypfdSrcPeerPad);
        m_gstWrapper->gstObjectUnref(pNewAudioDecoderSrcPad);
        return;
    }
    m_glibWrapper->gObjectSet(G_OBJECT(group.m_curAudioTypefind), "force-caps", newAudioCaps, NULL);
    m_gstWrapper->gstElementSyncStateWithParent(group.m_curAudioTypefind);
    m_gstWrapper->gstElementGetState(group.m_curAudioTypefind, &currentState, &pending, GST_CLOCK_TIME_NONE);
    RIALTO_SERVER_LOG_DEBUG("OTF -> New Typefind State = %d Pending = %d", currentState, pending);
    RIALTO_SERVER_LOG_DEBUG("OTF -> Typefind Syncing with Parent");
    group.m_linkTypefindParser = true;
    /* Update the state */
    m_gstWrapper->gstElementSyncStateWithParent(newAudioDecoder);
    m_gstWrapper->gstElementGetState(newAudioDecoder, &currentState, &pending, GST_CLOCK_TIME_NONE);
    RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioDecoder State = %d Pending = %d", currentState, pending);
    m_gstWrapper->gstElementSyncStateWithParent(newQueue);
    m_gstWrapper->gstElementGetState(newQueue, &currentState, &pending, GST_CLOCK_TIME_NONE);
    RIALTO_SERVER_LOG_DEBUG("OTF -> New queue State = %d Pending = %d", currentState, pending);
    m_gstWrapper->gstElementSyncStateWithParent(newAudioParse);
    m_gstWrapper->gstElementGetState(newAudioParse, &currentState, &pending, GST_CLOCK_TIME_NONE);
    RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioParser State = %d Pending = %d", currentState, pending);
    m_gstWrapper->gstObjectUnref(pTypfdSrcPad);
    m_gstWrapper->gstObjectUnref(pTypfdSrcPeerPad);
    m_gstWrapper->gstObjectUnref(pNewAudioDecoderSrcPad);
    return;
}

bool LinuxPlatformBackend::applyAudioCodecSwitch(firebolt::rialto::wrappers::PlaybackGroupPrivate &group,
                                                 bool isAudioAAC, GstCaps *newAudioCaps)
{ // this function comes from rdk_gstreamer_utils
    bool ret = false;
    RIALTO_SERVER_LOG_DEBUG("Current Audio Codec AAC = %d Same as Incoming audio Codec AAC = %d", group.m_isAudioAAC,
                            isAudioAAC);
    if (group.m_isAudioAAC == isAudioAAC)
    {
        return ret;
    }
    if ((group.m_curAudioDecoder == NULL) && (!(group.m_isAudioAAC)) && (isAudioAAC))
    {
        firstTimeSwitchFromAC3toAAC(group, newAudioCaps);
        group.m_isAudioAAC = isAudioAAC;
        return true;
    }
    if (!group.m_curAudioDecoder || !group.m_curAudioParse || !group.m_curAudioDecodeBin)
    {
        RIALTO_SERVER_LOG_ERROR("switchAudioCodec: audio decoder, parser or decode bin is null");
        return false;
    }
    GstElement *newAudioParse = NULL;
    GstElement *newAudioDecoder = NULL;
    GstPad *newAudioParseSrcPad = NULL;
    GstPad *newAudioParseSinkPad = NULL;
    GstPad *newAudioDecoderSrcPad = NULL;
    GstPad *newAudioDecoderSinkPad = NULL;
    GstPad *audioDecSrcPad = NULL;
    GstPad *audioDecSinkPad = NULL;
    GstPad *audioDecSrcPeerPad = NULL;
    GstPad *audioDecSinkPeerPad = NULL;
    GstPad *audioParseSrcPad = NULL;
    GstPad *audioParseSinkPad = NULL;
    GstPad *audioParseSrcPeerPad = NULL;
    GstPad *audioParseSinkPeerPad = NULL;
    GstState currentState{GST_STATE_VOID_PENDING}, pending{GST_STATE_VOID_PENDING};

    // Get AudioDecoder Src Pads
    if ((audioDecSrcPad = m_gstWrapper->gstElementGetStaticPad(group.m_curAudioDecoder, "src")) != NULL) // Unref the Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioDecoder Src Pad = %p", audioDecSrcPad);
    // Get AudioDecoder Sink Pads
    if ((audioDecSinkPad = m_gstWrapper->gstElementGetStaticPad(group.m_curAudioDecoder, "sink")) != NULL) // Unref the Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioDecoder Sink Pad = %p", audioDecSinkPad);
    // Get AudioDecoder Src Peer i.e. Downstream Element Pad
    if ((audioDecSrcPeerPad = m_gstWrapper->gstPadGetPeer(audioDecSrcPad)) != NULL) // Unref the Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioDecoder Src Downstream Element Pad = %p", audioDecSrcPeerPad);
    // Get AudioDecoder Sink Peer i.e. Upstream Element Pad
    if ((audioDecSinkPeerPad = m_gstWrapper->gstPadGetPeer(audioDecSinkPad)) != NULL) // Unref the Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioDecoder Sink Upstream Element Pad = %p", audioDecSinkPeerPad);
    // Get AudioParser Src Pads
    if ((audioParseSrcPad = m_gstWrapper->gstElementGetStaticPad(group.m_curAudioParse, "src")) != NULL) // Unref the Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioParser Src Pad = %p", audioParseSrcPad);
    // Get AudioParser Sink Pads
    if ((audioParseSinkPad = m_gstWrapper->gstElementGetStaticPad(group.m_curAudioParse, "sink")) != NULL) // Unref the Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioParser Sink Pad = %p", audioParseSinkPad);
    // Get AudioParser Src Peer i.e. Downstream Element Pad
    if ((audioParseSrcPeerPad = m_gstWrapper->gstPadGetPeer(audioParseSrcPad)) != NULL) // Unref the Peer Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioParser Src Downstream Element Pad = %p", audioParseSrcPeerPad);
    // Get AudioParser Sink Peer i.e. Upstream Element Pad
    if ((audioParseSinkPeerPad = m_gstWrapper->gstPadGetPeer(audioParseSinkPad)) != NULL) // Unref the Peer Pad
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioParser Sink Upstream Element Pad = %p", audioParseSinkPeerPad);
    // AudioDecoder Downstream Unlink
    if (m_gstWrapper->gstPadUnlink(audioDecSrcPad, audioDecSrcPeerPad) == FALSE)
        RIALTO_SERVER_LOG_DEBUG("OTF -> AudioDecoder Downstream Unlink Failed");
    // AudioDecoder Upstream Unlink
    if (m_gstWrapper->gstPadUnlink(audioDecSinkPeerPad, audioDecSinkPad) == FALSE)
        RIALTO_SERVER_LOG_DEBUG("OTF -> AudioDecoder Upstream Unlink Failed");
    // AudioParser Downstream Unlink
    if (m_gstWrapper->gstPadUnlink(audioParseSrcPad, audioParseSrcPeerPad) == FALSE)
        RIALTO_SERVER_LOG_DEBUG("OTF -> AudioParser Downstream Unlink Failed");
    // AudioParser Upstream Unlink
    if (m_gstWrapper->gstPadUnlink(audioParseSinkPeerPad, audioParseSinkPad) == FALSE)
        RIALTO_SERVER_LOG_DEBUG("OTF -> AudioParser Upstream Unlink Failed");
    // Current Audio Decoder NULL
    if (GST_STATE_CHANGE_FAILURE == m_gstWrapper->gstElementSetState(group.m_curAudioDecoder, GST_STATE_NULL))
    {
        RIALTO_SERVER_LOG_WARN("Failed to set AudioDecoder to NULL");
    }
    m_gstWrapper->gstElementGetState(group.m_curAudioDecoder, &currentState, &pending, GST_CLOCK_TIME_NONE);
    if (currentState == GST_STATE_NULL)
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioDecoder State = %d", currentState);
    // Current Audio Parser NULL
    if (GST_STATE_CHANGE_FAILURE == m_gstWrapper->gstElementSetState(group.m_curAudioParse, GST_STATE_NULL))
    {
        RIALTO_SERVER_LOG_WARN("Failed to set AudioParser to NULL");
    }
    m_gstWrapper->gstElementGetState(group.m_curAudioParse, &currentState, &pending, GST_CLOCK_TIME_NONE);
    if (currentState == GST_STATE_NULL)
        RIALTO_SERVER_LOG_DEBUG("OTF -> Current AudioParser State = %d", currentState);
    // Remove Audio Decoder From Decodebin
    if (m_gstWrapper->gstBinRemove(GST_BIN(group.m_curAudioDecodeBin.load()), group.m_curAudioDecoder) == TRUE)
    {
        RIALTO_SERVER_LOG_DEBUG("OTF -> Removed AudioDecoder = %p", group.m_curAudioDecoder);
        group.m_curAudioDecoder = NULL;
    }
    // Remove Audio Parser From Decodebin
    if (m_gstWrapper->gstBinRemove(GST_BIN(group.m_curAudioDecodeBin.load()), group.m_curAudioParse) == TRUE)
    {
        RIALTO_SERVER_LOG_DEBUG("OTF -> Removed AudioParser = %p", group.m_curAudioParse);
        group.m_curAudioParse = NULL;
    }
    // Create new Audio Decoder and Parser. The inverse of the current
    if (group.m_isAudioAAC)
    {
        newAudioParse = m_gstWrapper->gstElementFactoryMake("ac3parse", "ac3parse");
        newAudioDecoder = m_gstWrapper->gstElementFactoryMake("identity", "fake_aud_ac3dec");
    }
    else
    {
        newAudioParse = m_gstWrapper->gstElementFactoryMake("aacparse", "aacparse");
        newAudioDecoder = m_gstWrapper->gstElementFactoryMake("avdec_aac", "avdec_aac");
    }
    {
        GstPadLinkReturn gstPadLinkRet = GST_PAD_LINK_OK;
        GstElement *audioParseUpstreamEl = NULL;
        // Add new Decoder to Decodebin
        if (m_gstWrapper->gstBinAdd(GST_BIN(group.m_curAudioDecodeBin.load()), newAudioDecoder) == TRUE)
        {
            RIALTO_SERVER_LOG_DEBUG("OTF -> Added New AudioDecoder = %p", newAudioDecoder);
        }
        // Add new Parser to Decodebin
        if (m_gstWrapper->gstBinAdd(GST_BIN(group.m_curAudioDecodeBin.load()), newAudioParse) == TRUE)
        {
            RIALTO_SERVER_LOG_DEBUG("OTF -> Added New AudioParser = %p", newAudioParse);
        }
        if ((newAudioDecoderSrcPad = m_gstWrapper->gstElementGetStaticPad(newAudioDecoder, "src")) !=
            NULL) // Unref the Pad
            RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioDecoder Src Pad = %p", newAudioDecoderSrcPad);
        if ((newAudioDecoderSinkPad = m_gstWrapper->gstElementGetStaticPad(newAudioDecoder, "sink")) !=
            NULL) // Unref the Pad
            RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioDecoder Sink Pad = %p", newAudioDecoderSinkPad);
        // Link New Decoder to Downstream followed by UpStream
        if ((gstPadLinkRet = m_gstWrapper->gstPadLink(newAudioDecoderSrcPad, audioDecSrcPeerPad)) != GST_PAD_LINK_OK)
            RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioDecoder Downstream Link Failed");
        if ((gstPadLinkRet = m_gstWrapper->gstPadLink(audioDecSinkPeerPad, newAudioDecoderSinkPad)) != GST_PAD_LINK_OK)
            RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioDecoder Upstream Link Failed");
        if ((newAudioParseSrcPad = m_gstWrapper->gstElementGetStaticPad(newAudioParse, "src")) != NULL) // Unref the Pad
            RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioParser Src Pad = %p", newAudioParseSrcPad);
        if ((newAudioParseSinkPad = m_gstWrapper->gstElementGetStaticPad(newAudioParse, "sink")) != NULL) // Unref the Pad
            RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioParser Sink Pad = %p", newAudioParseSinkPad);
        // Link New Parser to Downstream followed by UpStream
        if ((gstPadLinkRet = m_gstWrapper->gstPadLink(newAudioParseSrcPad, audioParseSrcPeerPad)) != GST_PAD_LINK_OK)
            RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioParser Downstream Link Failed %d", gstPadLinkRet);
        if ((audioParseUpstreamEl = GST_ELEMENT_CAST(m_gstWrapper->gstPadGetParent(audioParseSinkPeerPad))) ==
            group.m_curAudioTypefind)
        {
            RIALTO_SERVER_LOG_DEBUG("OTF -> Typefind Setting to READY");
            if (GST_STATE_CHANGE_FAILURE == m_gstWrapper->gstElementSetState(audioParseUpstreamEl, GST_STATE_READY))
            {
                RIALTO_SERVER_LOG_WARN("Failed to set Typefind to READY in switchAudioCodec");
            }
            m_glibWrapper->gObjectSet(G_OBJECT(audioParseUpstreamEl), "force-caps", newAudioCaps, NULL);
            m_gstWrapper->gstElementSyncStateWithParent(audioParseUpstreamEl);
            m_gstWrapper->gstElementGetState(audioParseUpstreamEl, &currentState, &pending, GST_CLOCK_TIME_NONE);
            RIALTO_SERVER_LOG_DEBUG("OTF -> New Typefind State = %d Pending = %d", currentState, pending);
            RIALTO_SERVER_LOG_DEBUG("OTF -> Typefind Syncing with Parent");
            group.m_linkTypefindParser = true;
            m_gstWrapper->gstObjectUnref(audioParseUpstreamEl);
        }
        m_gstWrapper->gstObjectUnref(newAudioDecoderSrcPad);
        m_gstWrapper->gstObjectUnref(newAudioDecoderSinkPad);
        m_gstWrapper->gstObjectUnref(newAudioParseSrcPad);
        m_gstWrapper->gstObjectUnref(newAudioParseSinkPad);
    }
    m_gstWrapper->gstObjectUnref(audioParseSinkPeerPad);
    m_gstWrapper->gstObjectUnref(audioParseSrcPeerPad);
    m_gstWrapper->gstObjectUnref(audioParseSinkPad);
    m_gstWrapper->gstObjectUnref(audioParseSrcPad);
    m_gstWrapper->gstObjectUnref(audioDecSinkPeerPad);
    m_gstWrapper->gstObjectUnref(audioDecSrcPeerPad);
    m_gstWrapper->gstObjectUnref(audioDecSinkPad);
    m_gstWrapper->gstObjectUnref(audioDecSrcPad);
    m_gstWrapper->gstElementSyncStateWithParent(newAudioDecoder);
    m_gstWrapper->gstElementGetState(newAudioDecoder, &currentState, &pending, GST_CLOCK_TIME_NONE);
    RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioDecoder State = %d Pending = %d", currentState, pending);
    m_gstWrapper->gstElementSyncStateWithParent(newAudioParse);
    m_gstWrapper->gstElementGetState(newAudioParse, &currentState, &pending, GST_CLOCK_TIME_NONE);
    RIALTO_SERVER_LOG_DEBUG("OTF -> New AudioParser State = %d Pending = %d", currentState, pending);
    group.m_isAudioAAC = isAudioAAC;
    return true;
}

bool LinuxPlatformBackend::performAudioTrackCodecChannelSwitch(
    firebolt::rialto::wrappers::PlaybackGroupPrivate &group, const void *pSampleAttr,
    firebolt::rialto::wrappers::AudioAttributesPrivate *pAudioAttr, uint32_t *pStatus, unsigned int *pui32Delay,
    long long *pAudioChangeTargetPts,                                  // NOLINT(runtime/int)
    const long long *pcurrentDispPts,                                  // NOLINT(runtime/int)
    unsigned int *audioChangeStage, GstCaps **appsrcCaps, bool *audioaac, bool svpenabled, GstElement *aSrc, bool *ret)
{
    // this function comes from rdk_gstreamer_utils
    if (!pStatus || !pui32Delay || !pAudioChangeTargetPts || !pcurrentDispPts || !audioChangeStage || !appsrcCaps ||
        !audioaac || !aSrc || !ret)
    {
        RIALTO_SERVER_LOG_ERROR("performAudioTrackCodecChannelSwitch: invalid null parameter");
        return false;
    }

    constexpr uint32_t kOk = 0;
    constexpr uint32_t kWaitWhileIdling = 100;
    constexpr int kAudioChangeGapThresholdMS = 40;
    constexpr unsigned int kAudchgAlign = 3;

    struct timespec ts, now;
    unsigned int reconfigDelayMs;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (*pStatus != kOk || pSampleAttr == nullptr)
    {
        RIALTO_SERVER_LOG_DEBUG("No audio data ready yet");
        *pui32Delay = kWaitWhileIdling;
        *ret = false;
        return true;
    }
    RIALTO_SERVER_LOG_DEBUG("Received first audio packet after a flush, PTS");
    if (pAudioAttr)
    {
        const char *pCodecStr = pAudioAttr->m_codecParam.c_str();
        const char *pCodecAcc = strstr(pCodecStr, "mp4a");
        bool isAudioAAC = (pCodecAcc) ? true : false;
        bool isCodecSwitch = false;
        RIALTO_SERVER_LOG_DEBUG("Audio Attribute format %s channel %d samp %d, bitrate %d blockAlignment %d", pCodecStr,
                                pAudioAttr->m_numberOfChannels, pAudioAttr->m_samplesPerSecond, pAudioAttr->m_bitrate,
                                pAudioAttr->m_blockAlignment);
        *pAudioChangeTargetPts = *pcurrentDispPts;
        *audioChangeStage = kAudchgAlign;
        if (*appsrcCaps)
        {
            m_gstWrapper->gstCapsUnref(*appsrcCaps);
            *appsrcCaps = NULL;
        }
        if (isAudioAAC != *audioaac)
            isCodecSwitch = true;
        configAudioCap(pAudioAttr, audioaac, svpenabled, appsrcCaps);
        {
            gboolean sendRet = FALSE;
            GstEvent *flushStart = NULL;
            GstEvent *flushStop = NULL;
            flushStart = m_gstWrapper->gstEventNewFlushStart();
            sendRet = m_gstWrapper->gstElementSendEvent(aSrc, flushStart);
            if (!sendRet)
                RIALTO_SERVER_LOG_DEBUG("failed to send flush-start event");
            flushStop = m_gstWrapper->gstEventNewFlushStop(TRUE);
            sendRet = m_gstWrapper->gstElementSendEvent(aSrc, flushStop);
            if (!sendRet)
                RIALTO_SERVER_LOG_DEBUG("failed to send flush-stop event");
        }
        if (!isCodecSwitch)
        {
            m_gstWrapper->gstAppSrcSetCaps(GST_APP_SRC(aSrc), *appsrcCaps);
        }
        else
        {
            RIALTO_SERVER_LOG_DEBUG("CODEC SWITCH mAudioAAC = %d", *audioaac);
            haltAudioPlayback(group);
            if (applyAudioCodecSwitch(group, *audioaac, *appsrcCaps) == false)
            {
                RIALTO_SERVER_LOG_DEBUG("CODEC SWITCH FAILED switchAudioCodec mAudioAAC = %d", *audioaac);
            }
            m_gstWrapper->gstAppSrcSetCaps(GST_APP_SRC(aSrc), *appsrcCaps);
            resumeAudioPlayback(group);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        reconfigDelayMs = now.tv_nsec > ts.tv_nsec ? (now.tv_nsec - ts.tv_nsec) / 1000000
                                                   : (1000 - (ts.tv_nsec - now.tv_nsec) / 1000000);
        (*pAudioChangeTargetPts) += (reconfigDelayMs + kAudioChangeGapThresholdMS);
    }
    else
    {
        RIALTO_SERVER_LOG_DEBUG("first audio after change no attribute drop!");
        *pui32Delay = 0;
        *ret = false;
        return true;
    }
    *ret = true;
    return true;
}

bool LinuxPlatformBackend::switchAudioCodec(const AudioCodecSwitchContext &ctx)
{
    if (!m_gstWrapper || !m_glibWrapper)
        return false;

    // Build a local playback group + audio attributes from the neutral context: no engine state
    // is touched. m_isAudioAAC propagates back out via ctx.isAudioAacState below; m_linkTypefindParser
    // is write-only inside the fork and need not propagate.
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

    long long currentDispPts{0}; // NOLINT(runtime/int)
    int sampleAttributes{0};     // performAudioTrackCodecChannelSwitch only checks this param != NULL.
    std::uint32_t status{0};     // must be 0 to make performAudioTrackCodecChannelSwitch work
    unsigned int ui32Delay{0};   // output param
    long long audioChangeTargetPts{-1}; // NOLINT(runtime/int) output param
    unsigned int audioChangeStage{0};   // output param
    bool audioAac{group.m_isAudioAAC};
    bool retVal{false};                 // output param
    GstCaps *appsrcCaps{nullptr};       // the backend builds its own switch caps internally

    bool result = false;
    // transitional -> per-SoC .so : the amlhalasink name-check folds into the backend. A per-SoC .so
    // implementing this same ABI replaces this whole branch; the engine core names no SoC.
    const char *playsinkBinName = ctx.audioPlaysinkBin ? GST_ELEMENT_NAME(ctx.audioPlaysinkBin) : "";
    if (m_glibWrapper->gStrHasPrefix(playsinkBinName, "amlhalasink"))
    {
        // due to problems audio codec change in prerolling, temporarily moved the code from rdk gstreamer utils to
        // Rialto and applied fixes
        result = performAudioTrackCodecChannelSwitch(group, &sampleAttributes, &attr, &status, &ui32Delay,
                                                     &audioChangeTargetPts, &currentDispPts, &audioChangeStage,
                                                     &appsrcCaps, &audioAac, ctx.svpEnabled, ctx.audioAppSrc, &retVal);
    }
    else
    {
        if (!m_rdkGstreamerUtilsWrapper)
        {
            RIALTO_SERVER_LOG_ERROR("switchAudioCodec: no rdk-gstreamer-utils wrapper available");
            return false;
        }
        appsrcCaps = (ctx.audioAppSrc ? m_gstWrapper->gstAppSrcGetCaps(GST_APP_SRC(ctx.audioAppSrc)) : nullptr);
        result = m_rdkGstreamerUtilsWrapper->performAudioTrackCodecChannelSwitch(
            &group, &sampleAttributes, &attr, &status, &ui32Delay, &audioChangeTargetPts, &currentDispPts,
            &audioChangeStage, &appsrcCaps, &audioAac, ctx.svpEnabled, ctx.audioAppSrc, &retVal);
    }

    // The fork manages its own appsrcCaps lifecycle (configAudioCap allocates, gstAppSrcSetCaps consumes
    // for use); the rdk-gstreamer-utils path leaves appsrcCaps owned here, so release it.
    if (appsrcCaps)
        m_gstWrapper->gstCapsUnref(appsrcCaps);

    // Write the resulting current-codec state back to the engine's playback group.
    if (ctx.isAudioAacState)
        *ctx.isAudioAacState = group.m_isAudioAAC;

    if (!result || !retVal)
    {
        RIALTO_SERVER_LOG_WARN("performAudioTrackCodecChannelSwitch failed! Result: %d, retval %d", result, retVal);
        return false;
    }
    return true;
}

bool LinuxPlatformBackend::shouldSkipCapabilityProbe(const std::string &elementName) const
{
    // transitional -> per-SoC .so : the rtkv1sink element-name check folds into the backend. A
    // per-SoC (realtek) .so implementing this same ABI answers true for its own problematic element;
    // the engine core names no SoC. The reference Linux backend has no such element, so the only name
    // it knows is the transitional realtek one carried here until a per-SoC .so is authored.
    //
    // WORKAROUND: instantiating "rtkv1sink" during capability probing turns another playback's video
    // black, so it must never be created just to read its properties.
    return elementName == "rtkv1sink";
}

} // namespace firebolt::rialto::server

/* Loader ABI — resolved by the core's dlopen of this backend's .so. */
extern "C" uint32_t rialtoPlatformBackendAbiVersion(void)
{
    return firebolt::rialto::server::kPlatformBackendAbiVersion;
}

extern "C" firebolt::rialto::server::IPlatformBackend *
rialtoCreatePlatformBackend(const firebolt::rialto::server::PlatformHostContext *host)
{
    if (!host)
        return nullptr;
    return new (std::nothrow) firebolt::rialto::server::LinuxPlatformBackend(*host);
}

extern "C" void rialtoDestroyPlatformBackend(firebolt::rialto::server::IPlatformBackend *backend)
{
    delete backend;
}
