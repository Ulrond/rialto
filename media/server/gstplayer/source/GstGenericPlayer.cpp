/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2022 Sky UK
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

#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "FlushWatcher.h"
#include "GstDispatcherThread.h"
#include "GstGenericPlayer.h"
#include "GstProfiler.h"
#include "GstProtectionMetadata.h"
#include "IGstTextTrackSinkFactory.h"
#include "IMediaPipeline.h"
#include "ITimer.h"
#include "PlatformBackendLoader.h"
#include "RialtoServerLogging.h"
#include "TypeConverters.h"
#include "Utils.h"
#include "WorkerThread.h"
#include "tasks/generic/GenericPlayerTaskFactory.h"

namespace
{
/**
 * @brief Report position interval in ms.
 *        The position reporting timer should be started whenever the PLAYING state is entered and stopped
 *        whenever the session moves to another playback state.
 */
constexpr std::chrono::milliseconds kPositionReportTimerMs{250};
constexpr std::chrono::seconds kSubtitleClockResyncInterval{10};

bool operator==(const firebolt::rialto::server::SegmentData &lhs, const firebolt::rialto::server::SegmentData &rhs)
{
    return (lhs.position == rhs.position) && (lhs.resetTime == rhs.resetTime) && (lhs.appliedRate == rhs.appliedRate) &&
           (lhs.stopPosition == rhs.stopPosition);
}

// Underflow / first-video-frame signal callbacks for the explicit-construction path. These mirror the
// playbin path's SetupElement callbacks: the GStreamer thread emits the signal, and we schedule the
// corresponding task on the worker thread. The user data is the IGstGenericPlayerPrivate.
void explicitAudioUnderflowCallback(GstElement *object, guint fifoDepth, gpointer queueDepth, gpointer self)
{
    static_cast<firebolt::rialto::server::IGstGenericPlayerPrivate *>(self)->scheduleAudioUnderflow();
}

void explicitVideoUnderflowCallback(GstElement *object, guint fifoDepth, gpointer queueDepth, gpointer self)
{
    static_cast<firebolt::rialto::server::IGstGenericPlayerPrivate *>(self)->scheduleVideoUnderflow();
}

void explicitFirstVideoFrameCallback(GstElement *object, guint fifoDepth, gpointer queueDepth, gpointer self)
{
    static_cast<firebolt::rialto::server::IGstGenericPlayerPrivate *>(self)->scheduleFirstVideoFrameReceived();
}
} // namespace

namespace firebolt::rialto::server
{
std::weak_ptr<IGstGenericPlayerFactory> GstGenericPlayerFactory::m_factory;

std::shared_ptr<IGstGenericPlayerFactory> IGstGenericPlayerFactory::getFactory()
{
    std::shared_ptr<IGstGenericPlayerFactory> factory = GstGenericPlayerFactory::m_factory.lock();

    if (!factory)
    {
        try
        {
            factory = std::make_shared<GstGenericPlayerFactory>();
        }
        catch (const std::exception &e)
        {
            RIALTO_SERVER_LOG_ERROR("Failed to create the gstreamer player factory, reason: %s", e.what());
        }

        GstGenericPlayerFactory::m_factory = factory;
    }

    return factory;
}

std::unique_ptr<IGstGenericPlayer> GstGenericPlayerFactory::createGstGenericPlayer(
    IGstGenericPlayerClient *client, IDecryptionService &decryptionService, MediaType type,
    const VideoRequirements &videoRequirements, bool isLive,
    const std::shared_ptr<firebolt::rialto::wrappers::IRdkGstreamerUtilsWrapperFactory> &rdkGstreamerUtilsWrapperFactory)
{
    std::unique_ptr<IGstGenericPlayer> gstPlayer;

    try
    {
        auto gstWrapperFactory = firebolt::rialto::wrappers::IGstWrapperFactory::getFactory();
        auto glibWrapperFactory = firebolt::rialto::wrappers::IGlibWrapperFactory::getFactory();
        std::shared_ptr<firebolt::rialto::wrappers::IGstWrapper> gstWrapper;
        std::shared_ptr<firebolt::rialto::wrappers::IGlibWrapper> glibWrapper;
        std::shared_ptr<firebolt::rialto::wrappers::IRdkGstreamerUtilsWrapper> rdkGstreamerUtilsWrapper;
        if ((!gstWrapperFactory) || (!(gstWrapper = gstWrapperFactory->getGstWrapper())))
        {
            throw std::runtime_error("Cannot create GstWrapper");
        }
        if ((!glibWrapperFactory) || (!(glibWrapper = glibWrapperFactory->getGlibWrapper())))
        {
            throw std::runtime_error("Cannot create GlibWrapper");
        }
        if ((!rdkGstreamerUtilsWrapperFactory) ||
            (!(rdkGstreamerUtilsWrapper = rdkGstreamerUtilsWrapperFactory->createRdkGstreamerUtilsWrapper())))
        {
            throw std::runtime_error("Cannot create RdkGstreamerUtilsWrapper");
        }

        // Acquire the SoC platform backend through the loader: it dlopen()s a per-SoC
        // .so (version-checked against kPlatformBackendAbiVersion) and falls back to the
        // built-in reference backend when none is present. The core names no SoC; the
        // GstGenericPlayer constructor's platformBackend parameter stays the test seam.
        PlatformHostContext platformHostContext{gstWrapper, glibWrapper, rdkGstreamerUtilsWrapper};
        std::shared_ptr<IPlatformBackend> platformBackend{PlatformBackendLoader{}.load(platformHostContext)};

        gstPlayer = std::make_unique<
            GstGenericPlayer>(client, decryptionService, type, videoRequirements, isLive, gstWrapper, glibWrapper,
                              IGstInitialiser::instance(), std::make_unique<FlushWatcher>(),
                              IGstSrcFactory::getFactory(), IGstProfilerFactory::getFactory(),
                              common::ITimerFactory::getFactory(),
                              std::make_unique<GenericPlayerTaskFactory>(client, gstWrapper, glibWrapper,
                                                                         IGstTextTrackSinkFactory::createFactory()),
                              std::make_unique<WorkerThreadFactory>(), std::make_unique<GstDispatcherThreadFactory>(),
                              IGstProtectionMetadataHelperFactory::createFactory(), platformBackend);
    }
    catch (const std::exception &e)
    {
        RIALTO_SERVER_LOG_ERROR("Failed to create the gstreamer player, reason: %s", e.what());
    }

    return gstPlayer;
}

GstGenericPlayer::GstGenericPlayer(
    IGstGenericPlayerClient *client, IDecryptionService &decryptionService, MediaType type,
    const VideoRequirements &videoRequirements, bool isLive,
    const std::shared_ptr<firebolt::rialto::wrappers::IGstWrapper> &gstWrapper,
    const std::shared_ptr<firebolt::rialto::wrappers::IGlibWrapper> &glibWrapper,
    const IGstInitialiser &gstInitialiser, std::unique_ptr<IFlushWatcher> &&flushWatcher,
    const std::shared_ptr<IGstSrcFactory> &gstSrcFactory,
    const std::shared_ptr<IGstProfilerFactory> &gstProfilerFactory, std::shared_ptr<common::ITimerFactory> timerFactory,
    std::unique_ptr<IGenericPlayerTaskFactory> taskFactory, std::unique_ptr<IWorkerThreadFactory> workerThreadFactory,
    std::unique_ptr<IGstDispatcherThreadFactory> gstDispatcherThreadFactory,
    std::shared_ptr<IGstProtectionMetadataHelperFactory> gstProtectionMetadataFactory,
    const std::shared_ptr<IPlatformBackend> &platformBackend)
    : m_gstPlayerClient(client), m_gstWrapper{gstWrapper}, m_glibWrapper{glibWrapper},
      m_platformBackend{platformBackend}, m_gstProfilerFactory{gstProfilerFactory}, m_timerFactory{timerFactory},
      m_taskFactory{std::move(taskFactory)}, m_flushWatcher{std::move(flushWatcher)}
{
    RIALTO_SERVER_LOG_DEBUG("GstGenericPlayer is constructed.");

    gstInitialiser.waitForInitialisation();

    m_context.isLive = isLive;
    m_context.decryptionService = &decryptionService;

    if ((!gstSrcFactory) || (!(m_context.gstSrc = gstSrcFactory->getGstSrc())))
    {
        throw std::runtime_error("Cannot create GstSrc");
    }
    if (!m_gstProfilerFactory)
    {
        throw std::runtime_error("No gst profiler factory provided");
    }

    if (!timerFactory)
    {
        throw std::runtime_error("TimeFactory is invalid");
    }

    if ((!gstProtectionMetadataFactory) ||
        (!(m_protectionMetadataWrapper = gstProtectionMetadataFactory->createProtectionMetadataWrapper(m_gstWrapper))))
    {
        throw std::runtime_error("Cannot create protection metadata wrapper");
    }

    // Ensure that rialtosrc has been initalised
    m_context.gstSrc->initSrc();

    // Start task thread
    if ((!workerThreadFactory) || (!(m_workerThread = workerThreadFactory->createWorkerThread())))
    {
        throw std::runtime_error("Failed to create the worker thread");
    }

    // Initialise pipeline
    switch (type)
    {
    case MediaType::MSE:
    {
        initMsePipeline();
        break;
    }
    default:
    {
        resetWorkerThread();
        throw std::runtime_error("Media type not supported");
    }
    }

    // Check the video requirements for a limited video.
    // If the video requirements are set to anything lower than the minimum, this playback is assumed to be a secondary
    // video in a dual video scenario.
    if ((kMinPrimaryVideoWidth > videoRequirements.maxWidth) || (kMinPrimaryVideoHeight > videoRequirements.maxHeight))
    {
        RIALTO_SERVER_LOG_MIL("Secondary video playback selected");
        // Secondary/PiP plane: the resource id is passed to IPlatformBackend::createVideoSink in
        // buildVideoChain, which binds the vendor sink to the plane.
        m_context.videoId = 1;
    }
    else
    {
        RIALTO_SERVER_LOG_MIL("Primary video playback selected");
        m_context.videoId = 0; // primary/Main plane
    }

    m_gstDispatcherThread = gstDispatcherThreadFactory->createGstDispatcherThread(*this, m_context.pipeline,
                                                                                  m_context.flushOnPrerollController,
                                                                                  m_gstWrapper);
}

GstGenericPlayer::~GstGenericPlayer()
{
    RIALTO_SERVER_LOG_DEBUG("GstGenericPlayer is destructed.");
    m_gstDispatcherThread.reset();

    try
    {
        resetWorkerThread();
    }
    catch (const std::exception &e)
    {
        RIALTO_SERVER_LOG_ERROR("Exception during resetWorkerThread in destructor: %s", e.what());
    }
    catch (...)
    {
        RIALTO_SERVER_LOG_ERROR("Unknown exception during resetWorkerThread in destructor");
    }

    try
    {
        termPipeline();
    }
    catch (const std::exception &e)
    {
        RIALTO_SERVER_LOG_ERROR("Exception during termPipeline in destructor: %s", e.what());
    }
    catch (...)
    {
        RIALTO_SERVER_LOG_ERROR("Unknown exception during termPipeline in destructor");
    }
}

void GstGenericPlayer::initMsePipeline()
{
    // Explicit construction: a plain pipeline container. The per-stream chains
    // (appsrc -> parse -> decoder -> sink-from-backend) are built in AttachSource; nothing is
    // autoplugged, so playbin's signals / play-flags / playsink / uri are all absent.
    m_context.pipeline = m_gstWrapper->gstPipelineNew("media_pipeline");
    if (!m_context.pipeline)
    {
        throw std::runtime_error("Failed to create the pipeline");
    }

    m_context.gstProfiler = m_gstProfilerFactory->createGstProfiler(m_context.pipeline, m_gstWrapper, m_glibWrapper);
    if (!m_context.gstProfiler)
    {
        throw std::runtime_error("Cannot create GstProfiler");
    }

    if (GST_STATE_CHANGE_FAILURE == m_gstWrapper->gstElementSetState(m_context.pipeline, GST_STATE_READY))
    {
        GST_WARNING("Failed to set pipeline to READY state");
    }
    RIALTO_SERVER_LOG_MIL("New RialtoServer's explicit pipeline created");
    auto recordId = m_context.gstProfiler->createRecord("Pipeline Created");
    if (recordId)
        m_context.gstProfiler->logRecord(recordId.value());
}

void GstGenericPlayer::buildAudioChain(GstElement *source)
{
    if (!m_platformBackend)
    {
        RIALTO_SERVER_LOG_ERROR("No platform backend; cannot build the explicit audio chain");
        return;
    }

    // The engine builds no media topology: it owns the appsrc, adds it to the pipeline, and hands it to
    // the platform backend, which constructs and links its own audio subgraph (reference x86: decodebin
    // -> audioconvert -> audioresample -> autoaudiosink; a device backend may build a fused compressed-
    // passthrough path). The engine then does only the generic, non-SoC bookkeeping on the returned
    // handles. No playbin, no auto-sinks, no engine-owned topology.
    GstBin *pipelineBin = GST_BIN(m_context.pipeline);
    m_gstWrapper->gstBinAdd(pipelineBin, source);

    const PlatformMediaPath path{m_platformBackend->buildAudioPath(m_context.pipeline, source)};
    if (!path.sink)
    {
        RIALTO_SERVER_LOG_ERROR("Failed to build the explicit audio path");
        return;
    }
    GstElement *audioSink = path.sink;

    // The decoder's src pad is autoplugged dynamically inside decodebin, so it is linked to the backend's
    // decoderLinkTarget (audioconvert on the reference path) in the pad-added handler. A fused topology
    // (decodebin == nullptr) needs no such wiring — the backend owns decode inside its sink.
    m_explicitAudioConvert = path.decoderLinkTarget;
    if (path.decodebin)
    {
        m_glibWrapper->gSignalConnect(path.decodebin, "pad-added",
                                      G_CALLBACK(&GstGenericPlayer::audioDecodebinPadAdded), this);
    }

    // The audio sink exists now — unlike the playbin path, where it appeared later via element-setup
    // and SetupElement reacted to it. Store it so getSink and the audio-sink setters reach it, and
    // apply the pending audio-sink properties inline (relocating the isAudioSink branch of
    // SetupElement: low-latency and sync).
    m_gstWrapper->gstObjectRef(audioSink);
    m_context.audioSink = audioSink;
    if (m_context.pendingLowLatency.has_value())
    {
        setLowLatency();
    }
    if (m_context.pendingSync.has_value())
    {
        setSync();
    }

    // Wire underflow telemetry on the backend audio sink (the analogue of SetupElement's reactive
    // wiring on the playbin path). If the sink also carries an underflow signal, AAMP underflow
    // reporting is preserved; the autoplugged decoder is wired separately once it appears.
    connectStreamSignals(audioSink, MediaSourceType::AUDIO);

    // Populate the stable playback-group handles the audio-codec-switch machinery reads. On the
    // playbin path these are set reactively (DeepElementAdded/UpdatePlaybackGroup off playbin signals);
    // the explicit path has no such signals, so the handles we own at construction are stored here and
    // the per-switch ones (decoder/parser/typefind) are refreshed from the live graph in reattachSource.
    //   - m_gstPipeline / m_curAudioDecodeBin: owned directly here.
    //   - m_curAudioPlaysinkBin: the explicit topology has no playsink wrapper bin, so the backend audio
    //     sink stands in as the audio output branch that haltAudioPlayback/resumeAudioPlayback gate. An
    //     extra ref is taken to match the playbin path's ownership; termPipeline releases it.
    m_context.playbackGroup.m_gstPipeline = m_context.pipeline;
    m_context.playbackGroup.m_curAudioDecodeBin = path.decodebin;
    m_gstWrapper->gstObjectRef(audioSink);
    m_context.playbackGroup.m_curAudioPlaysinkBin = audioSink;

    RIALTO_SERVER_LOG_MIL("Explicit audio chain built via the platform backend audio path");
}

void GstGenericPlayer::audioDecodebinPadAdded(GstElement *decodebin, GstPad *pad, GstGenericPlayer *self)
{
    // decodebin has exposed the decoder's src pad; link it to the static audioconvert tail.
    if (!self || !self->m_explicitAudioConvert)
    {
        return;
    }
    GstPad *sinkPad = self->m_gstWrapper->gstElementGetStaticPad(self->m_explicitAudioConvert, "sink");
    if (sinkPad)
    {
        self->m_gstWrapper->gstPadLink(pad, sinkPad);
        self->m_gstWrapper->gstObjectUnref(sinkPad);
    }

    // The decoder has been autoplugged inside decodebin and is now reachable via getDecoder(AUDIO).
    // Apply the pending audio-decoder properties on the worker thread — the explicit-path analogue
    // of the playbin path's reactive SetupElement decoder branch.
    self->scheduleSetupAudioDecoder();
}

void GstGenericPlayer::buildVideoChain(GstElement *source)
{
    if (!m_platformBackend)
    {
        RIALTO_SERVER_LOG_ERROR("No platform backend; cannot build the explicit video chain");
        return;
    }

    // Mirroring the audio path: the engine owns the appsrc, adds it, and hands it to the backend, which
    // constructs and links its own video subgraph (reference x86: decodebin -> autovideosink; a device
    // backend builds its own topology and binds the vendor sink to the plane). No playbin, no auto-sinks,
    // no engine-owned topology.
    GstBin *pipelineBin = GST_BIN(m_context.pipeline);
    m_gstWrapper->gstBinAdd(pipelineBin, source);

    const PlatformMediaPath path{m_platformBackend->buildVideoPath(m_context.pipeline, source, m_context.videoId)};
    if (!path.sink)
    {
        RIALTO_SERVER_LOG_ERROR("Failed to build the explicit video path");
        return;
    }
    GstElement *videoSink = path.sink;

    // The decoder's src pad is autoplugged dynamically inside decodebin, so it is linked to the backend's
    // decoderLinkTarget (the video sink itself on the reference path) in the pad-added handler. A fused
    // topology (decodebin == nullptr) needs no such wiring.
    m_explicitVideoSink = path.decoderLinkTarget;
    if (path.decodebin)
    {
        m_glibWrapper->gSignalConnect(path.decodebin, "pad-added",
                                      G_CALLBACK(&GstGenericPlayer::videoDecodebinPadAdded), this);
    }

    // The video sink exists now — unlike the playbin path, where it appeared later via element-setup
    // and SetupElement reacted to it. Store it (an extra ref, released by termPipeline) so getSink and
    // the video-sink setters reach it, and apply the pending video-sink properties inline (relocating
    // the isVideoSink branch of SetupElement: geometry, immediate-output, render-frame, show-window).
    m_gstWrapper->gstObjectRef(videoSink);
    m_context.videoSink = videoSink;
    if (!m_context.pendingGeometry.empty())
    {
        setVideoSinkRectangle();
    }
    if (m_context.pendingImmediateOutputForVideo.has_value())
    {
        setImmediateOutput();
    }
    if (m_context.pendingRenderFrame)
    {
        setRenderFrame();
    }
    if (m_context.pendingShowVideoWindow.has_value())
    {
        setShowVideoWindow();
    }

    // Wire underflow + first-video-frame telemetry on the backend video sink (the analogue of
    // SetupElement's reactive wiring on the playbin path). The autoplugged decoder is wired separately
    // once it appears (via SetupVideoParser).
    connectStreamSignals(videoSink, MediaSourceType::VIDEO);

    RIALTO_SERVER_LOG_MIL("Explicit video chain built via the platform backend video path");
}

void GstGenericPlayer::videoDecodebinPadAdded(GstElement *decodebin, GstPad *pad, GstGenericPlayer *self)
{
    // decodebin has exposed the decoder's src pad; link it to the backend video sink.
    if (!self || !self->m_explicitVideoSink)
    {
        return;
    }
    GstPad *sinkPad = self->m_gstWrapper->gstElementGetStaticPad(self->m_explicitVideoSink, "sink");
    if (sinkPad)
    {
        self->m_gstWrapper->gstPadLink(pad, sinkPad);
        self->m_gstWrapper->gstObjectUnref(sinkPad);
    }

    // decodebin has autoplugged the video parser (reachable via getParser(VIDEO)), so the pending
    // video-parser property can now be applied on the worker thread — the explicit-path analogue of
    // the playbin path's reactive SetupElement parser branch.
    self->scheduleSetupVideoParser();
}

void GstGenericPlayer::connectStreamSignals(GstElement *element, const MediaSourceType &mediaSourceType)
{
    // Explicit-construction analogue of the SetupElement underflow/first-frame wiring. On the explicit
    // path the element's role (sink/decoder) and media type are already known, so the isDecoder/isSink/
    // isAudio/isVideo probing of the playbin path is unnecessary — we only scan for the signal name and
    // connect the matching callback. underflow applies to audio and video; first-video-frame to video.
    if (!element)
    {
        return;
    }

    std::optional<std::string> underflowSignalName = getUnderflowSignalName(*m_glibWrapper, element);
    if (underflowSignalName)
    {
        GCallback callback = mediaSourceType == MediaSourceType::AUDIO ? G_CALLBACK(explicitAudioUnderflowCallback)
                                                                       : G_CALLBACK(explicitVideoUnderflowCallback);
        m_glibWrapper->gSignalConnect(element, underflowSignalName.value().c_str(), callback,
                                      static_cast<IGstGenericPlayerPrivate *>(this));
    }

    if (mediaSourceType == MediaSourceType::VIDEO)
    {
        std::optional<std::string> firstFrameSignalName = getFirstFrameSignalName(*m_glibWrapper, element);
        if (firstFrameSignalName)
        {
            m_glibWrapper->gSignalConnect(element, firstFrameSignalName.value().c_str(),
                                          G_CALLBACK(explicitFirstVideoFrameCallback),
                                          static_cast<IGstGenericPlayerPrivate *>(this));
        }
    }
}

void GstGenericPlayer::connectDecoderSignals(const MediaSourceType &mediaSourceType)
{
    // The explicit chain's decodebin has autoplugged the decoder; wire its underflow/first-frame
    // telemetry now (the sink was wired earlier in buildAudioChain/buildVideoChain). Called from the
    // SetupAudioDecoder/SetupVideoParser tasks on the worker thread, once the decoder is reachable.
    GstElement *decoder = getDecoder(mediaSourceType);
    if (!decoder)
    {
        return;
    }
    connectStreamSignals(decoder, mediaSourceType);
    m_gstWrapper->gstObjectUnref(decoder);
}

void GstGenericPlayer::resetWorkerThread()
{
    // Shutdown task thread
    m_workerThread->enqueueTask(m_taskFactory->createShutdown(*this));
    m_workerThread->join();
    m_workerThread.reset();
}

void GstGenericPlayer::termPipeline()
{
    if (m_finishSourceSetupTimer && m_finishSourceSetupTimer->isActive())
    {
        m_finishSourceSetupTimer->cancel();
    }

    m_finishSourceSetupTimer.reset();

    for (auto &elem : m_context.streamInfo)
    {
        StreamInfo &streamInfo = elem.second;
        for (auto &buffer : streamInfo.buffers)
        {
            m_gstWrapper->gstBufferUnref(buffer);
        }

        streamInfo.buffers.clear();
    }

    m_taskFactory->createStop(m_context, *this)->execute();
    GstBus *bus = m_gstWrapper->gstPipelineGetBus(GST_PIPELINE(m_context.pipeline));
    m_gstWrapper->gstBusSetSyncHandler(bus, nullptr, nullptr, nullptr);
    m_gstWrapper->gstObjectUnref(bus);

    if (m_context.source)
    {
        m_gstWrapper->gstObjectUnref(m_context.source);
    }
    if (m_context.subtitleSink)
    {
        m_gstWrapper->gstObjectUnref(m_context.subtitleSink);
        m_context.subtitleSink = nullptr;
    }

    if (m_context.videoSink)
    {
        m_gstWrapper->gstObjectUnref(m_context.videoSink);
        m_context.videoSink = nullptr;
    }
    if (m_context.audioSink)
    {
        m_gstWrapper->gstObjectUnref(m_context.audioSink);
        m_context.audioSink = nullptr;
    }
    if (m_context.playbackGroup.m_curAudioPlaysinkBin)
    {
        m_gstWrapper->gstObjectUnref(m_context.playbackGroup.m_curAudioPlaysinkBin);
        m_context.playbackGroup.m_curAudioPlaysinkBin = nullptr;
    }

    auto recordId = m_context.gstProfiler->createRecord("Pipeline Terminated");
    if (recordId)
        m_context.gstProfiler->logRecord(recordId.value());
    m_context.gstProfiler->dumpToFile();

    // Delete the pipeline
    m_gstWrapper->gstObjectUnref(m_context.pipeline);

    RIALTO_SERVER_LOG_MIL("RialtoServer's pipeline terminated");
}

void GstGenericPlayer::attachSource(const std::unique_ptr<IMediaPipeline::MediaSource> &attachedSource)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createAttachSource(m_context, *this, attachedSource));
    }
}

void GstGenericPlayer::allSourcesAttached()
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createFinishSetupSource(m_context, *this));
    }
}

void GstGenericPlayer::attachSamples(const IMediaPipeline::MediaSegmentVector &mediaSegments)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createAttachSamples(m_context, *this, mediaSegments));
    }
}

void GstGenericPlayer::attachSamples(const std::shared_ptr<IDataReader> &dataReader)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createReadShmDataAndAttachSamples(m_context, *this, dataReader));
    }
}

void GstGenericPlayer::setPosition(std::int64_t position)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetPosition(m_context, *this, position));
    }
}

void GstGenericPlayer::setPlaybackRate(double rate)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetPlaybackRate(m_context, *this, rate));
    }
}

bool GstGenericPlayer::getPosition(std::int64_t &position)
{
    // We are on main thread here, but m_context.pipeline can be used, because it's modified only in GstGenericPlayer
    // constructor and destructor. GstGenericPlayer is created/destructed on main thread, so we won't have a crash here.
    position = getPosition(m_context.pipeline);
    if (position == -1)
    {
        return false;
    }

    return true;
}

bool GstGenericPlayer::getDuration(std::int64_t &duration)
{
    // We are on main thread here, but m_context.pipeline can be used, because it's modified only in GstGenericPlayer
    // constructor and destructor. GstGenericPlayer is created/destructed on main thread, so we won't have a crash here.
    if (!m_context.pipeline || !m_gstWrapper->gstElementQueryDuration(m_context.pipeline, GST_FORMAT_TIME, &duration))
    {
        RIALTO_SERVER_LOG_WARN("Failed to query duration");
        return false;
    }
    return true;
}

GstElement *GstGenericPlayer::getSink(const MediaSourceType &mediaSourceType) const
{
    // The backend-built chains store their sink in the context (audio/video in buildAudioChain /
    // buildVideoChain, subtitle in AttachSource's buildSubtitleChain). That stored sink is the exclusive
    // source: there is no playbin to read an "audio-sink"/"video-sink"/"text-sink" property off, and the
    // backend creates sinks explicitly (never via autovideosink/autoaudiosink wrappers, so there is no
    // auto-sink to unwrap either). The returned sink is ref'd; the caller unrefs it.
    GstElement *sink{nullptr};
    switch (mediaSourceType)
    {
    case MediaSourceType::AUDIO:
        sink = m_context.audioSink;
        break;
    case MediaSourceType::VIDEO:
        sink = m_context.videoSink;
        break;
    case MediaSourceType::SUBTITLE:
        sink = m_context.subtitleSink;
        break;
    default:
        RIALTO_SERVER_LOG_WARN("mediaSourceType not supported %d", static_cast<int>(mediaSourceType));
        break;
    }

    if (sink)
    {
        return GST_ELEMENT(m_gstWrapper->gstObjectRef(GST_OBJECT(sink)));
    }
    return nullptr;
}

void GstGenericPlayer::setSourceFlushed(const MediaSourceType &mediaSourceType)
{
    m_flushWatcher->setFlushed(mediaSourceType);
}

void GstGenericPlayer::notifyPlaybackInfo()
{
    PlaybackInfo info;
    getPosition(info.currentPosition);
    if (m_context.audioFadeEnabled)
    {
        info.volume = m_context.audioFadeVolume;
    }
    else
    {
        getVolume(info.volume);
    }
    m_gstPlayerClient->notifyPlaybackInfo(info);
}

GstElement *GstGenericPlayer::getDecoder(const MediaSourceType &mediaSourceType)
{
    GstIterator *it = m_gstWrapper->gstBinIterateRecurse(GST_BIN(m_context.pipeline));
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;

    while (!done)
    {
        switch (m_gstWrapper->gstIteratorNext(it, &item))
        {
        case GST_ITERATOR_OK:
        {
            GstElement *element = GST_ELEMENT(m_glibWrapper->gValueGetObject(&item));
            GstElementFactory *factory = m_gstWrapper->gstElementGetFactory(element);

            if (factory)
            {
                GstElementFactoryListType type = GST_ELEMENT_FACTORY_TYPE_DECODER;
                if (mediaSourceType == MediaSourceType::AUDIO)
                {
                    type |= GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO;
                }
                else if (mediaSourceType == MediaSourceType::VIDEO)
                {
                    type |= GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO;
                }

                if (m_gstWrapper->gstElementFactoryListIsType(factory, type))
                {
                    m_glibWrapper->gValueUnset(&item);
                    m_gstWrapper->gstIteratorFree(it);
                    return GST_ELEMENT(m_gstWrapper->gstObjectRef(element));
                }
            }

            m_glibWrapper->gValueUnset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            m_gstWrapper->gstIteratorResync(it);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }

    RIALTO_SERVER_LOG_WARN("Could not find decoder");

    m_glibWrapper->gValueUnset(&item);
    m_gstWrapper->gstIteratorFree(it);

    return nullptr;
}

GstElement *GstGenericPlayer::getParser(const MediaSourceType &mediaSourceType)
{
    GstIterator *it = m_gstWrapper->gstBinIterateRecurse(GST_BIN(m_context.pipeline));
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;

    while (!done)
    {
        switch (m_gstWrapper->gstIteratorNext(it, &item))
        {
        case GST_ITERATOR_OK:
        {
            GstElement *element = GST_ELEMENT(m_glibWrapper->gValueGetObject(&item));
            GstElementFactory *factory = m_gstWrapper->gstElementGetFactory(element);

            if (factory)
            {
                GstElementFactoryListType type = GST_ELEMENT_FACTORY_TYPE_PARSER;
                if (mediaSourceType == MediaSourceType::AUDIO)
                {
                    type |= GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO;
                }
                else if (mediaSourceType == MediaSourceType::VIDEO)
                {
                    type |= GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO;
                }

                if (m_gstWrapper->gstElementFactoryListIsType(factory, type))
                {
                    m_glibWrapper->gValueUnset(&item);
                    m_gstWrapper->gstIteratorFree(it);
                    return GST_ELEMENT(m_gstWrapper->gstObjectRef(element));
                }
            }

            m_glibWrapper->gValueUnset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            m_gstWrapper->gstIteratorResync(it);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }

    RIALTO_SERVER_LOG_WARN("Could not find parser");

    m_glibWrapper->gValueUnset(&item);
    m_gstWrapper->gstIteratorFree(it);

    return nullptr;
}

GstElement *GstGenericPlayer::getAudioTypefind()
{
    // The typefind has no decoder/parser factory type to match on, so it is located by name within the
    // audio decodebin (mirroring DeepElementAdded/UpdatePlaybackGroup's "typefind" name match on the
    // playbin path). Scoped to the audio decodebin so a per-stream video decodebin's typefind is not
    // picked up by mistake.
    if (!m_context.playbackGroup.m_curAudioDecodeBin)
    {
        return nullptr;
    }

    GstIterator *it = m_gstWrapper->gstBinIterateRecurse(GST_BIN(m_context.playbackGroup.m_curAudioDecodeBin.load()));
    GValue item = G_VALUE_INIT;
    gboolean done = FALSE;
    GstElement *typefind = nullptr;

    while (!done)
    {
        switch (m_gstWrapper->gstIteratorNext(it, &item))
        {
        case GST_ITERATOR_OK:
        {
            GstElement *element = GST_ELEMENT(m_glibWrapper->gValueGetObject(&item));
            gchar *name = m_gstWrapper->gstElementGetName(element);
            if (name && m_glibWrapper->gStrrstr(name, "typefind"))
            {
                typefind = GST_ELEMENT(m_gstWrapper->gstObjectRef(element));
            }
            m_glibWrapper->gFree(name);
            m_glibWrapper->gValueUnset(&item);
            if (typefind)
            {
                done = TRUE;
            }
            break;
        }
        case GST_ITERATOR_RESYNC:
            m_gstWrapper->gstIteratorResync(it);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = TRUE;
            break;
        }
    }

    m_glibWrapper->gValueUnset(&item);
    m_gstWrapper->gstIteratorFree(it);

    return typefind;
}

void GstGenericPlayer::updateAudioPlaybackGroupHandles()
{
    // Explicit-path analogue of DeepElementAdded: the codec-switch machinery (switchAudioCodec and the
    // external rdk-gstreamer-utils performAudioTrackCodecChannelSwitch) reads the current audio
    // decoder/parser/typefind off the playback group. There are no playbin signals to populate them, so
    // they are refreshed from the live decodebin just before the swap. Storing borrowed pointers matches
    // DeepElementAdded's non-owning contract (the decodebin owns the elements); refreshing per switch is
    // robust to switchAudioCodec nulling and recreating the decoder/parser across successive switches.
    GstElement *decoder = getDecoder(MediaSourceType::AUDIO);
    m_context.playbackGroup.m_curAudioDecoder = decoder;
    if (decoder)
    {
        m_gstWrapper->gstObjectUnref(decoder);
    }
    GstElement *parser = getParser(MediaSourceType::AUDIO);
    m_context.playbackGroup.m_curAudioParse = parser;
    if (parser)
    {
        m_gstWrapper->gstObjectUnref(parser);
    }
    GstElement *typefind = getAudioTypefind();
    m_context.playbackGroup.m_curAudioTypefind = typefind;
    if (typefind)
    {
        m_gstWrapper->gstObjectUnref(typefind);
    }
}

std::optional<firebolt::rialto::wrappers::AudioAttributesPrivate>
GstGenericPlayer::createAudioAttributes(const std::unique_ptr<IMediaPipeline::MediaSource> &source) const
{
    std::optional<firebolt::rialto::wrappers::AudioAttributesPrivate> audioAttributes;
    const IMediaPipeline::MediaSourceAudio *kSource = dynamic_cast<IMediaPipeline::MediaSourceAudio *>(source.get());
    if (kSource)
    {
        firebolt::rialto::AudioConfig audioConfig = kSource->getAudioConfig();
        audioAttributes =
            firebolt::rialto::wrappers::AudioAttributesPrivate{"", // param set below.
                                                               audioConfig.numberOfChannels, audioConfig.sampleRate,
                                                               0, // used only in one of logs in rdk_gstreamer_utils, no
                                                                  // need to set this param.
                                                               0, // used only in one of logs in rdk_gstreamer_utils, no
                                                                  // need to set this param.
                                                               audioConfig.codecSpecificConfig.data(),
                                                               static_cast<std::uint32_t>(
                                                                   audioConfig.codecSpecificConfig.size())};
        if (source->getMimeType() == "audio/mp4" || source->getMimeType() == "audio/aac")
        {
            audioAttributes->m_codecParam = "mp4a";
        }
        else if (source->getMimeType() == "audio/x-eac3")
        {
            audioAttributes->m_codecParam = "ec-3";
        }
        else if (source->getMimeType() == "audio/b-wav" || source->getMimeType() == "audio/x-raw")
        {
            audioAttributes->m_codecParam = "lpcm";
        }
    }
    else
    {
        RIALTO_SERVER_LOG_ERROR("Failed to cast source");
    }

    return audioAttributes;
}

bool GstGenericPlayer::setImmediateOutput(const MediaSourceType &mediaSourceType, bool immediateOutputParam)
{
    if (!m_workerThread)
        return false;

    m_workerThread->enqueueTask(
        m_taskFactory->createSetImmediateOutput(m_context, *this, mediaSourceType, immediateOutputParam));
    return true;
}

bool GstGenericPlayer::getImmediateOutput(const MediaSourceType &mediaSourceType, bool &immediateOutputRef)
{
    bool returnValue{false};
    GstElement *sink{getSink(mediaSourceType)};
    if (sink)
    {
        if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(sink), "immediate-output"))
        {
            m_glibWrapper->gObjectGet(sink, "immediate-output", &immediateOutputRef, nullptr);
            returnValue = true;
        }
        else
        {
            RIALTO_SERVER_LOG_ERROR("immediate-output not supported in element %s", GST_ELEMENT_NAME(sink));
        }
        m_gstWrapper->gstObjectUnref(sink);
    }
    else
    {
        RIALTO_SERVER_LOG_ERROR("Failed to set immediate-output property, sink is NULL");
    }

    return returnValue;
}

bool GstGenericPlayer::getStats(const MediaSourceType &mediaSourceType, uint64_t &renderedFrames, uint64_t &droppedFrames)
{
    bool returnValue{false};
    GstElement *sink{getSink(mediaSourceType)};
    if (sink)
    {
        GstStructure *stats{nullptr};
        m_glibWrapper->gObjectGet(sink, "stats", &stats, nullptr);
        if (!stats)
        {
            RIALTO_SERVER_LOG_ERROR("failed to get stats from '%s'", GST_ELEMENT_NAME(sink));
        }
        else
        {
            guint64 renderedFramesTmp;
            guint64 droppedFramesTmp;
            if (m_gstWrapper->gstStructureGetUint64(stats, "rendered", &renderedFramesTmp) &&
                m_gstWrapper->gstStructureGetUint64(stats, "dropped", &droppedFramesTmp))
            {
                renderedFrames = renderedFramesTmp;
                droppedFrames = droppedFramesTmp;
                returnValue = true;
            }
            else
            {
                RIALTO_SERVER_LOG_ERROR("failed to get 'rendered' or 'dropped' from structure (%s)",
                                        GST_ELEMENT_NAME(sink));
            }
            m_gstWrapper->gstStructureFree(stats);
        }
        m_gstWrapper->gstObjectUnref(sink);
    }
    else
    {
        RIALTO_SERVER_LOG_ERROR("Failed to get stats, sink is NULL");
    }

    return returnValue;
}

GstBuffer *GstGenericPlayer::createBuffer(const IMediaPipeline::MediaSegment &mediaSegment) const
{
    GstBuffer *gstBuffer = m_gstWrapper->gstBufferNewAllocate(nullptr, mediaSegment.getDataLength(), nullptr);
    m_gstWrapper->gstBufferFill(gstBuffer, 0, mediaSegment.getData(), mediaSegment.getDataLength());

    if (mediaSegment.isEncrypted())
    {
        GstBuffer *keyId = m_gstWrapper->gstBufferNewAllocate(nullptr, mediaSegment.getKeyId().size(), nullptr);
        m_gstWrapper->gstBufferFill(keyId, 0, mediaSegment.getKeyId().data(), mediaSegment.getKeyId().size());

        GstBuffer *initVector = m_gstWrapper->gstBufferNewAllocate(nullptr, mediaSegment.getInitVector().size(), nullptr);
        m_gstWrapper->gstBufferFill(initVector, 0, mediaSegment.getInitVector().data(),
                                    mediaSegment.getInitVector().size());
        GstBuffer *subsamples{nullptr};
        if (!mediaSegment.getSubSamples().empty())
        {
            auto subsamplesRawSize = mediaSegment.getSubSamples().size() * (sizeof(guint16) + sizeof(guint32));
            guint8 *subsamplesRaw = static_cast<guint8 *>(m_glibWrapper->gMalloc(subsamplesRawSize));
            GstByteWriter writer;
            m_gstWrapper->gstByteWriterInitWithData(&writer, subsamplesRaw, subsamplesRawSize, FALSE);

            for (const auto &subSample : mediaSegment.getSubSamples())
            {
                m_gstWrapper->gstByteWriterPutUint16Be(&writer, subSample.numClearBytes);
                m_gstWrapper->gstByteWriterPutUint32Be(&writer, subSample.numEncryptedBytes);
            }
            subsamples = m_gstWrapper->gstBufferNewWrapped(subsamplesRaw, subsamplesRawSize);
        }

        uint32_t crypt = 0;
        uint32_t skip = 0;
        bool encryptionPatternSet = mediaSegment.getEncryptionPattern(crypt, skip);

        GstRialtoProtectionData data = {mediaSegment.getMediaKeySessionId(),
                                        static_cast<uint32_t>(mediaSegment.getSubSamples().size()),
                                        mediaSegment.getInitWithLast15(),
                                        keyId,
                                        initVector,
                                        subsamples,
                                        mediaSegment.getCipherMode(),
                                        crypt,
                                        skip,
                                        encryptionPatternSet,
                                        m_context.decryptionService};

        if (!m_protectionMetadataWrapper->addProtectionMetadata(gstBuffer, data))
        {
            RIALTO_SERVER_LOG_ERROR("Failed to add protection metadata");
            if (keyId)
            {
                m_gstWrapper->gstBufferUnref(keyId);
            }
            if (initVector)
            {
                m_gstWrapper->gstBufferUnref(initVector);
            }
            if (subsamples)
            {
                m_gstWrapper->gstBufferUnref(subsamples);
            }
        }
    }

    GST_BUFFER_TIMESTAMP(gstBuffer) = mediaSegment.getTimeStamp();
    GST_BUFFER_DURATION(gstBuffer) = mediaSegment.getDuration();
    return gstBuffer;
}

void GstGenericPlayer::notifyNeedMediaData(const MediaSourceType mediaSource)
{
    auto elem = m_context.streamInfo.find(mediaSource);
    if (elem != m_context.streamInfo.end())
    {
        StreamInfo &streamInfo = elem->second;
        streamInfo.isNeedDataPending = false;

        // Send new NeedMediaData if we still need it
        if (m_gstPlayerClient && streamInfo.isDataNeeded)
        {
            streamInfo.isNeedDataPending = m_gstPlayerClient->notifyNeedMediaData(mediaSource);
        }
    }
    else
    {
        RIALTO_SERVER_LOG_WARN("Media type %s could not be found", common::convertMediaSourceType(mediaSource));
    }
}

void GstGenericPlayer::attachData(const firebolt::rialto::MediaSourceType mediaType)
{
    auto elem = m_context.streamInfo.find(mediaType);
    if (elem != m_context.streamInfo.end())
    {
        StreamInfo &streamInfo = elem->second;
        if (streamInfo.buffers.empty() || !streamInfo.isDataNeeded)
        {
            return;
        }

        if (firebolt::rialto::MediaSourceType::SUBTITLE == mediaType)
        {
            setTextTrackPositionIfRequired(streamInfo.appSrc);
        }
        else
        {
            pushSampleIfRequired(streamInfo.appSrc, common::convertMediaSourceType(mediaType));
        }
        if (mediaType == firebolt::rialto::MediaSourceType::AUDIO)
        {
            // This needs to be done before gstAppSrcPushBuffer() is
            // called because it can free the memory
            m_context.lastAudioSampleTimestamps = static_cast<int64_t>(GST_BUFFER_PTS(streamInfo.buffers.back()));
        }

        for (GstBuffer *buffer : streamInfo.buffers)
        {
            m_gstWrapper->gstAppSrcPushBuffer(GST_APP_SRC(streamInfo.appSrc), buffer);
        }
        streamInfo.buffers.clear();
        streamInfo.isDataPushed = true;

        const bool kIsSingle = m_context.streamInfo.size() == 1;
        bool allOtherStreamsPushed = std::all_of(m_context.streamInfo.begin(), m_context.streamInfo.end(),
                                                 [](const auto &entry) { return entry.second.isDataPushed; });

        if (!m_context.bufferedNotificationSent && (allOtherStreamsPushed || kIsSingle) && m_gstPlayerClient)
        {
            m_context.bufferedNotificationSent = true;
            m_gstPlayerClient->notifyNetworkState(NetworkState::BUFFERED);
            RIALTO_SERVER_LOG_MIL("Buffered NetworkState reached");
        }
        cancelUnderflow(mediaType);

        const auto eosInfoIt = m_context.endOfStreamInfo.find(mediaType);
        if (eosInfoIt != m_context.endOfStreamInfo.end() && eosInfoIt->second == EosState::PENDING)
        {
            setEos(mediaType);
        }
    }
}

void GstGenericPlayer::updateAudioCaps(int32_t rate, int32_t channels, const std::shared_ptr<CodecData> &codecData)
{
    auto elem = m_context.streamInfo.find(firebolt::rialto::MediaSourceType::AUDIO);
    if (elem != m_context.streamInfo.end())
    {
        StreamInfo &streamInfo = elem->second;

        constexpr int kInvalidRate{0}, kInvalidChannels{0};
        GstCaps *currentCaps = m_gstWrapper->gstAppSrcGetCaps(GST_APP_SRC(streamInfo.appSrc));
        GstCaps *newCaps = m_gstWrapper->gstCapsCopy(currentCaps);

        if (rate != kInvalidRate)
        {
            m_gstWrapper->gstCapsSetSimple(newCaps, "rate", G_TYPE_INT, rate, NULL);
        }

        if (channels != kInvalidChannels)
        {
            m_gstWrapper->gstCapsSetSimple(newCaps, "channels", G_TYPE_INT, channels, NULL);
        }

        setCodecData(newCaps, codecData);

        if (!m_gstWrapper->gstCapsIsEqual(currentCaps, newCaps))
        {
            m_gstWrapper->gstAppSrcSetCaps(GST_APP_SRC(streamInfo.appSrc), newCaps);
        }

        m_gstWrapper->gstCapsUnref(newCaps);
        m_gstWrapper->gstCapsUnref(currentCaps);
    }
}

void GstGenericPlayer::updateVideoCaps(int32_t width, int32_t height, Fraction frameRate,
                                       const std::shared_ptr<CodecData> &codecData)
{
    auto elem = m_context.streamInfo.find(firebolt::rialto::MediaSourceType::VIDEO);
    if (elem != m_context.streamInfo.end())
    {
        StreamInfo &streamInfo = elem->second;

        GstCaps *currentCaps = m_gstWrapper->gstAppSrcGetCaps(GST_APP_SRC(streamInfo.appSrc));
        GstCaps *newCaps = m_gstWrapper->gstCapsCopy(currentCaps);

        if (width > 0)
        {
            m_gstWrapper->gstCapsSetSimple(newCaps, "width", G_TYPE_INT, width, NULL);
        }

        if (height > 0)
        {
            m_gstWrapper->gstCapsSetSimple(newCaps, "height", G_TYPE_INT, height, NULL);
        }

        if ((kUndefinedSize != frameRate.numerator) && (kUndefinedSize != frameRate.denominator))
        {
            m_gstWrapper->gstCapsSetSimple(newCaps, "framerate", GST_TYPE_FRACTION, frameRate.numerator,
                                           frameRate.denominator, NULL);
        }

        setCodecData(newCaps, codecData);

        if (!m_gstWrapper->gstCapsIsEqual(currentCaps, newCaps))
        {
            m_gstWrapper->gstAppSrcSetCaps(GST_APP_SRC(streamInfo.appSrc), newCaps);
        }

        m_gstWrapper->gstCapsUnref(currentCaps);
        m_gstWrapper->gstCapsUnref(newCaps);
    }
}

void GstGenericPlayer::addAudioClippingToBuffer(GstBuffer *buffer, uint64_t clippingStart, uint64_t clippingEnd) const
{
    if (clippingStart || clippingEnd)
    {
        if (m_gstWrapper->gstBufferAddAudioClippingMeta(buffer, GST_FORMAT_TIME, clippingStart, clippingEnd))
        {
            RIALTO_SERVER_LOG_DEBUG("Added audio clipping to buffer %p, start: %" PRIu64 ", end %" PRIu64, buffer,
                                    clippingStart, clippingEnd);
        }
        else
        {
            RIALTO_SERVER_LOG_WARN("Failed to add audio clipping to buffer %p, start: %" PRIu64 ", end %" PRIu64,
                                   buffer, clippingStart, clippingEnd);
        }
    }
}

bool GstGenericPlayer::setCodecData(GstCaps *caps, const std::shared_ptr<CodecData> &codecData) const
{
    if (codecData && CodecDataType::BUFFER == codecData->type)
    {
        gpointer memory = m_glibWrapper->gMemdup(codecData->data.data(), codecData->data.size());
        GstBuffer *buf = m_gstWrapper->gstBufferNewWrapped(memory, codecData->data.size());
        m_gstWrapper->gstCapsSetSimple(caps, "codec_data", GST_TYPE_BUFFER, buf, nullptr);
        m_gstWrapper->gstBufferUnref(buf);
        return true;
    }
    if (codecData && CodecDataType::STRING == codecData->type)
    {
        std::string codecDataStr(codecData->data.begin(), codecData->data.end());
        m_gstWrapper->gstCapsSetSimple(caps, "codec_data", G_TYPE_STRING, codecDataStr.c_str(), nullptr);
        return true;
    }
    return false;
}

void GstGenericPlayer::pushSampleIfRequired(GstElement *source, const std::string &typeStr)
{
    auto initialPosition = m_context.initialPositions.find(source);
    if (m_context.initialPositions.end() == initialPosition)
    {
        // Sending initial sample not needed
        return;
    }
    // GstAppSrc does not replace segment, if it's the same as previous one.
    // It causes problems with position reporting on some platforms, so we need to push
    // two segments with different reset time value.
    pushAdditionalSegmentIfRequired(source);

    for (const auto &[position, resetTime, appliedRate, stopPosition] : initialPosition->second)
    {
        GstSeekFlags seekFlag = resetTime ? GST_SEEK_FLAG_FLUSH : GST_SEEK_FLAG_NONE;
        RIALTO_SERVER_LOG_DEBUG("Pushing new %s sample...", typeStr.c_str());
        GstSegment *segment{m_gstWrapper->gstSegmentNew()};
        m_gstWrapper->gstSegmentInit(segment, GST_FORMAT_TIME);
        if (!m_gstWrapper->gstSegmentDoSeek(segment, m_context.playbackRate, GST_FORMAT_TIME, seekFlag,
                                            GST_SEEK_TYPE_SET, position, GST_SEEK_TYPE_SET, stopPosition, nullptr))
        {
            RIALTO_SERVER_LOG_WARN("Segment seek failed.");
            m_gstWrapper->gstSegmentFree(segment);
            m_context.initialPositions.erase(initialPosition);
            return;
        }
        segment->applied_rate = appliedRate;
        RIALTO_SERVER_LOG_MIL("New %s segment: [%" GST_TIME_FORMAT ", %" GST_TIME_FORMAT
                              "], rate: %f, appliedRate %f, reset_time: %d\n",
                              typeStr.c_str(), GST_TIME_ARGS(segment->start), GST_TIME_ARGS(segment->stop),
                              segment->rate, segment->applied_rate, resetTime);
        auto recordId = m_context.gstProfiler->createRecord("First Segment Received", typeStr);
        if (recordId)
            m_context.gstProfiler->logRecord(recordId.value());

        GstCaps *currentCaps = m_gstWrapper->gstAppSrcGetCaps(GST_APP_SRC(source));
        // We can't pass buffer in GstSample, because implementation of gst_app_src_push_sample
        // uses gst_buffer_copy, which loses RialtoProtectionMeta (that causes problems with EME
        // for first frame).
        GstSample *sample = m_gstWrapper->gstSampleNew(nullptr, currentCaps, segment, nullptr);
        m_gstWrapper->gstAppSrcPushSample(GST_APP_SRC(source), sample);
        m_gstWrapper->gstSampleUnref(sample);
        m_gstWrapper->gstCapsUnref(currentCaps);

        m_gstWrapper->gstSegmentFree(segment);
    }
    m_context.currentPosition[source] = initialPosition->second.back();
    m_context.initialPositions.erase(initialPosition);
    return;
}

void GstGenericPlayer::pushAdditionalSegmentIfRequired(GstElement *source)
{
    auto currentPosition = m_context.currentPosition.find(source);
    if (m_context.currentPosition.end() == currentPosition)
    {
        return;
    }
    auto initialPosition = m_context.initialPositions.find(source);
    if (m_context.initialPositions.end() == initialPosition)
    {
        return;
    }
    if (initialPosition->second.size() == 1 && initialPosition->second.back().resetTime &&
        currentPosition->second == initialPosition->second.back())
    {
        RIALTO_SERVER_LOG_INFO("Adding additional segment with reset_time = false");
        SegmentData additionalSegment = initialPosition->second.back();
        additionalSegment.resetTime = false;
        initialPosition->second.push_back(additionalSegment);
    }
}

void GstGenericPlayer::setTextTrackPositionIfRequired(GstElement *source)
{
    auto initialPosition = m_context.initialPositions.find(source);
    if (m_context.initialPositions.end() == initialPosition)
    {
        // Sending initial sample not needed
        return;
    }

    RIALTO_SERVER_LOG_MIL("New subtitle position set %" GST_TIME_FORMAT,
                          GST_TIME_ARGS(initialPosition->second.back().position));
    m_glibWrapper->gObjectSet(m_context.subtitleSink, "position",
                              static_cast<guint64>(initialPosition->second.back().position), nullptr);

    m_context.initialPositions.erase(initialPosition);
}

bool GstGenericPlayer::reattachSource(const std::unique_ptr<IMediaPipeline::MediaSource> &source)
{
    if (m_context.streamInfo.find(source->getType()) == m_context.streamInfo.end())
    {
        RIALTO_SERVER_LOG_ERROR("Unable to switch source, type does not exist");
        return false;
    }
    if (source->getMimeType().empty())
    {
        RIALTO_SERVER_LOG_WARN("Skip switch audio source. Unknown mime type");
        return false;
    }
    std::optional<firebolt::rialto::wrappers::AudioAttributesPrivate> audioAttributes{createAudioAttributes(source)};
    if (!audioAttributes)
    {
        RIALTO_SERVER_LOG_ERROR("Failed to create audio attributes");
        return false;
    }

    // No playbin signals populate the playback group, so refresh the audio decoder/parser/typefind
    // from the live decodebin before the codec switch so the backend operates on current elements.
    // The stable handles (pipeline, decodebin, sink-as-playsink-bin) were stored at construction in
    // buildAudioChain.
    updateAudioPlaybackGroupHandles();

    long long currentDispPts = getPosition(m_context.pipeline); // NOLINT(runtime/int)
    GstCaps *caps{createCapsFromMediaSource(m_gstWrapper, m_glibWrapper, source)};
    GstAppSrc *appSrc{GST_APP_SRC(m_context.streamInfo[source->getType()].appSrc)};
    GstCaps *oldCaps = m_gstWrapper->gstAppSrcGetCaps(appSrc);

    if ((!oldCaps) || (!m_gstWrapper->gstCapsIsEqual(caps, oldCaps)))
    {
        RIALTO_SERVER_LOG_DEBUG("Caps not equal. Perform audio track codec channel switch.");

        // The mid-stream audio codec switch is folded behind the platform seam (ABI v5): the engine
        // hands the backend a neutral context (element handles + audio attributes) and names no SoC.
        // The backend reads the playsink-bin name itself and picks the platform-specific path. The
        // local `caps` above is only the equality decision; the backend builds its own switch caps
        // internally.
        if (m_platformBackend)
        {
            AudioCodecSwitchContext ctx;
            ctx.pipeline = m_context.pipeline;
            ctx.audioAppSrc = GST_ELEMENT(appSrc);
            ctx.audioDecodeBin = m_context.playbackGroup.m_curAudioDecodeBin.load();
            ctx.audioPlaysinkBin = m_context.playbackGroup.m_curAudioPlaysinkBin;
            ctx.audioDecoder = m_context.playbackGroup.m_curAudioDecoder;
            ctx.audioParse = m_context.playbackGroup.m_curAudioParse;
            ctx.audioTypefind = m_context.playbackGroup.m_curAudioTypefind;
            ctx.isAudioAacState = &m_context.playbackGroup.m_isAudioAAC;
            ctx.svpEnabled = true;
            ctx.codecParam = audioAttributes->m_codecParam.c_str();
            ctx.numberOfChannels = audioAttributes->m_numberOfChannels;
            ctx.samplesPerSecond = audioAttributes->m_samplesPerSecond;
            ctx.bitrate = audioAttributes->m_bitrate;
            ctx.blockAlignment = audioAttributes->m_blockAlignment;
            ctx.codecSpecificData = audioAttributes->m_codecSpecificData;
            ctx.codecSpecificDataLen = audioAttributes->m_codecSpecificDataLen;

            if (!m_platformBackend->switchAudioCodec(ctx))
            {
                RIALTO_SERVER_LOG_WARN("Audio track codec channel switch failed!");
            }
        }
    }
    else
    {
        RIALTO_SERVER_LOG_DEBUG("Skip switching audio source - caps are the same.");
    }

    m_context.lastAudioSampleTimestamps = currentDispPts;
    if (caps)
        m_gstWrapper->gstCapsUnref(caps);
    if (oldCaps)
        m_gstWrapper->gstCapsUnref(oldCaps);

    return true;
}

bool GstGenericPlayer::hasSourceType(const MediaSourceType &mediaSourceType) const
{
    return m_context.streamInfo.find(mediaSourceType) != m_context.streamInfo.end();
}

void GstGenericPlayer::scheduleNeedMediaData(GstAppSrc *src)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createNeedData(m_context, *this, src));
    }
}

void GstGenericPlayer::scheduleEnoughData(GstAppSrc *src)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createEnoughData(m_context, src));
    }
}

void GstGenericPlayer::scheduleAudioUnderflow()
{
    if (m_workerThread)
    {
        bool underflowEnabled = m_context.isPlaying;
        m_workerThread->enqueueTask(
            m_taskFactory->createUnderflow(m_context, *this, underflowEnabled, MediaSourceType::AUDIO));
    }
}

void GstGenericPlayer::scheduleVideoUnderflow()
{
    if (m_workerThread)
    {
        bool underflowEnabled = m_context.isPlaying;
        m_workerThread->enqueueTask(
            m_taskFactory->createUnderflow(m_context, *this, underflowEnabled, MediaSourceType::VIDEO));
    }
}

void GstGenericPlayer::scheduleFirstVideoFrameReceived()
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createFirstFrameReceived(m_context, *this, MediaSourceType::VIDEO));
    }
}

void GstGenericPlayer::scheduleAllSourcesAttached()
{
    allSourcesAttached();
}

void GstGenericPlayer::scheduleSetupAudioDecoder()
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetupAudioDecoder(m_context, *this));
    }
}

void GstGenericPlayer::scheduleSetupVideoParser()
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetupVideoParser(m_context, *this));
    }
}

void GstGenericPlayer::cancelUnderflow(firebolt::rialto::MediaSourceType mediaSource)
{
    auto elem = m_context.streamInfo.find(mediaSource);
    if (elem != m_context.streamInfo.end())
    {
        StreamInfo &streamInfo = elem->second;
        if (!streamInfo.underflowOccured)
        {
            return;
        }

        RIALTO_SERVER_LOG_DEBUG("Cancelling %s underflow", common::convertMediaSourceType(mediaSource));
        streamInfo.underflowOccured = false;
    }
}

void GstGenericPlayer::play(bool &async)
{
    async = true;
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createPlay(*this));
    }
}

void GstGenericPlayer::pause()
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createPause(m_context, *this));
    }
}

void GstGenericPlayer::stop()
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createStop(m_context, *this));
    }
}

GstStateChangeReturn GstGenericPlayer::changePipelineState(GstState newState)
{
    if (!m_context.pipeline)
    {
        RIALTO_SERVER_LOG_ERROR("Change state failed - pipeline is nullptr");
        if (m_gstPlayerClient)
            m_gstPlayerClient->notifyPlaybackState(PlaybackState::FAILURE);
        return GST_STATE_CHANGE_FAILURE;
    }
    m_context.flushOnPrerollController->setTargetState(newState);
    const GstStateChangeReturn result{m_gstWrapper->gstElementSetState(m_context.pipeline, newState)};
    if (result == GST_STATE_CHANGE_FAILURE)
    {
        RIALTO_SERVER_LOG_ERROR("Change state failed - Gstreamer returned an error");
        if (m_gstPlayerClient)
            m_gstPlayerClient->notifyPlaybackState(PlaybackState::FAILURE);
    }
    return result;
}

int64_t GstGenericPlayer::getPosition(GstElement *element)
{
    if (!element)
    {
        RIALTO_SERVER_LOG_WARN("Element is null");
        return -1;
    }

    m_gstWrapper->gstStateLock(element);

    if (m_gstWrapper->gstElementGetState(element) < GST_STATE_PAUSED ||
        (m_gstWrapper->gstElementGetStateReturn(element) == GST_STATE_CHANGE_ASYNC &&
         m_gstWrapper->gstElementGetStateNext(element) == GST_STATE_PAUSED))
    {
        RIALTO_SERVER_LOG_WARN("Element is prerolling or in invalid state - state: %s, return: %s, next: %s",
                               m_gstWrapper->gstElementStateGetName(m_gstWrapper->gstElementGetState(element)),
                               m_gstWrapper->gstElementStateChangeReturnGetName(
                                   m_gstWrapper->gstElementGetStateReturn(element)),
                               m_gstWrapper->gstElementStateGetName(m_gstWrapper->gstElementGetStateNext(element)));

        m_gstWrapper->gstStateUnlock(element);
        return -1;
    }
    m_gstWrapper->gstStateUnlock(element);

    gint64 position = -1;
    if (!m_gstWrapper->gstElementQueryPosition(m_context.pipeline, GST_FORMAT_TIME, &position))
    {
        RIALTO_SERVER_LOG_WARN("Failed to query position");
        return -1;
    }

    return position;
}

void GstGenericPlayer::setVideoGeometry(int x, int y, int width, int height)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(
            m_taskFactory->createSetVideoGeometry(m_context, *this, Rectangle{x, y, width, height}));
    }
}

void GstGenericPlayer::setEos(const firebolt::rialto::MediaSourceType &type)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createEos(m_context, *this, type));
    }
}

bool GstGenericPlayer::setVideoSinkRectangle()
{
    bool result = false;
    GstElement *videoSink{getSink(MediaSourceType::VIDEO)};
    if (videoSink)
    {
        if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(videoSink), "rectangle"))
        {
            std::string rect =
                std::to_string(m_context.pendingGeometry.x) + ',' + std::to_string(m_context.pendingGeometry.y) + ',' +
                std::to_string(m_context.pendingGeometry.width) + ',' + std::to_string(m_context.pendingGeometry.height);
            m_glibWrapper->gObjectSet(videoSink, "rectangle", rect.c_str(), nullptr);
            m_context.pendingGeometry.clear();
            result = true;
        }
        else
        {
            RIALTO_SERVER_LOG_ERROR("Failed to set the video rectangle");
        }
        m_gstWrapper->gstObjectUnref(videoSink);
    }
    else
    {
        RIALTO_SERVER_LOG_ERROR("Failed to set video rectangle, sink is NULL");
    }

    return result;
}

bool GstGenericPlayer::setImmediateOutput()
{
    bool result{false};
    if (m_context.pendingImmediateOutputForVideo.has_value())
    {
        GstElement *sink{getSink(MediaSourceType::VIDEO)};
        if (sink)
        {
            bool immediateOutput{m_context.pendingImmediateOutputForVideo.value()};
            RIALTO_SERVER_LOG_DEBUG("Set immediate-output to %s", immediateOutput ? "TRUE" : "FALSE");

            if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(sink), "immediate-output"))
            {
                gboolean immediateOutputGboolean{immediateOutput ? TRUE : FALSE};
                m_glibWrapper->gObjectSet(sink, "immediate-output", immediateOutputGboolean, nullptr);
                result = true;
            }
            else
            {
                RIALTO_SERVER_LOG_ERROR("Failed to set immediate-output property on sink '%s'", GST_ELEMENT_NAME(sink));
            }
            m_context.pendingImmediateOutputForVideo.reset();
            m_gstWrapper->gstObjectUnref(sink);
        }
        else
        {
            RIALTO_SERVER_LOG_DEBUG("Pending an immediate-output, sink is NULL");
        }
    }
    return result;
}

bool GstGenericPlayer::setShowVideoWindow()
{
    if (!m_context.pendingShowVideoWindow.has_value())
    {
        RIALTO_SERVER_LOG_WARN("No show video window value to be set. Aborting...");
        return false;
    }

    GstElement *videoSink{getSink(MediaSourceType::VIDEO)};
    if (!videoSink)
    {
        RIALTO_SERVER_LOG_DEBUG("Setting show video window queued. Video sink is NULL");
        return false;
    }
    bool result{false};
    if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(videoSink), "show-video-window"))
    {
        m_glibWrapper->gObjectSet(videoSink, "show-video-window", m_context.pendingShowVideoWindow.value(), nullptr);
        result = true;
    }
    else
    {
        RIALTO_SERVER_LOG_ERROR("Setting show video window failed. Property does not exist");
    }
    m_context.pendingShowVideoWindow.reset();
    m_gstWrapper->gstObjectUnref(GST_OBJECT(videoSink));
    return result;
}

bool GstGenericPlayer::setLowLatency()
{
    bool result{false};
    if (m_context.pendingLowLatency.has_value())
    {
        GstElement *sink{getSink(MediaSourceType::AUDIO)};
        if (sink)
        {
            bool lowLatency{m_context.pendingLowLatency.value()};
            RIALTO_SERVER_LOG_DEBUG("Set low-latency to %s", lowLatency ? "TRUE" : "FALSE");

            if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(sink), "low-latency"))
            {
                gboolean lowLatencyGboolean{lowLatency ? TRUE : FALSE};
                m_glibWrapper->gObjectSet(sink, "low-latency", lowLatencyGboolean, nullptr);
                result = true;
            }
            else
            {
                RIALTO_SERVER_LOG_ERROR("Failed to set low-latency property on sink '%s'", GST_ELEMENT_NAME(sink));
            }
            m_context.pendingLowLatency.reset();
            m_gstWrapper->gstObjectUnref(sink);
        }
        else
        {
            RIALTO_SERVER_LOG_DEBUG("Pending low-latency, sink is NULL");
        }
    }
    return result;
}

bool GstGenericPlayer::setSync()
{
    bool result{false};
    if (m_context.pendingSync.has_value())
    {
        GstElement *sink{getSink(MediaSourceType::AUDIO)};
        if (sink)
        {
            bool sync{m_context.pendingSync.value()};
            RIALTO_SERVER_LOG_DEBUG("Set sync to %s", sync ? "TRUE" : "FALSE");

            if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(sink), "sync"))
            {
                gboolean syncGboolean{sync ? TRUE : FALSE};
                m_glibWrapper->gObjectSet(sink, "sync", syncGboolean, nullptr);
                result = true;
            }
            else
            {
                RIALTO_SERVER_LOG_ERROR("Failed to set sync property on sink '%s'", GST_ELEMENT_NAME(sink));
            }
            m_context.pendingSync.reset();
            m_gstWrapper->gstObjectUnref(sink);
        }
        else
        {
            RIALTO_SERVER_LOG_DEBUG("Pending sync, sink is NULL");
        }
    }
    return result;
}

bool GstGenericPlayer::setSyncOff()
{
    bool result{false};
    if (m_context.pendingSyncOff.has_value())
    {
        GstElement *decoder = getDecoder(MediaSourceType::AUDIO);
        if (decoder)
        {
            bool syncOff{m_context.pendingSyncOff.value()};
            RIALTO_SERVER_LOG_DEBUG("Set sync-off to %s", syncOff ? "TRUE" : "FALSE");

            if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(decoder), "sync-off"))
            {
                gboolean syncOffGboolean{decoder ? TRUE : FALSE};
                m_glibWrapper->gObjectSet(decoder, "sync-off", syncOffGboolean, nullptr);
                result = true;
            }
            else
            {
                RIALTO_SERVER_LOG_ERROR("Failed to set sync-off property on decoder '%s'", GST_ELEMENT_NAME(decoder));
            }
            m_context.pendingSyncOff.reset();
            m_gstWrapper->gstObjectUnref(decoder);
        }
        else
        {
            RIALTO_SERVER_LOG_DEBUG("Pending sync-off, decoder is NULL");
        }
    }
    return result;
}

bool GstGenericPlayer::setStreamSyncMode(const MediaSourceType &type)
{
    bool result{false};
    int32_t streamSyncMode{0};
    {
        std::unique_lock lock{m_context.propertyMutex};
        if (m_context.pendingStreamSyncMode.find(type) == m_context.pendingStreamSyncMode.end())
        {
            return false;
        }
        streamSyncMode = m_context.pendingStreamSyncMode[type];
    }
    if (MediaSourceType::AUDIO == type)
    {
        GstElement *decoder = getDecoder(MediaSourceType::AUDIO);
        if (!decoder)
        {
            RIALTO_SERVER_LOG_DEBUG("Pending stream-sync-mode, decoder is NULL");
            return false;
        }

        RIALTO_SERVER_LOG_DEBUG("Set stream-sync-mode to %d", streamSyncMode);

        if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(decoder), "stream-sync-mode"))
        {
            gint streamSyncModeGint{static_cast<gint>(streamSyncMode)};
            m_glibWrapper->gObjectSet(decoder, "stream-sync-mode", streamSyncModeGint, nullptr);
            result = true;
        }
        else
        {
            RIALTO_SERVER_LOG_ERROR("Failed to set stream-sync-mode property on decoder '%s'", GST_ELEMENT_NAME(decoder));
        }
        m_gstWrapper->gstObjectUnref(decoder);
        std::unique_lock lock{m_context.propertyMutex};
        m_context.pendingStreamSyncMode.erase(type);
    }
    else if (MediaSourceType::VIDEO == type)
    {
        GstElement *parser = getParser(MediaSourceType::VIDEO);
        if (!parser)
        {
            RIALTO_SERVER_LOG_DEBUG("Pending syncmode-streaming, parser is NULL");
            return false;
        }

        gboolean streamSyncModeBoolean{static_cast<gboolean>(streamSyncMode)};
        RIALTO_SERVER_LOG_DEBUG("Set syncmode-streaming to %d", streamSyncMode);

        if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(parser), "syncmode-streaming"))
        {
            m_glibWrapper->gObjectSet(parser, "syncmode-streaming", streamSyncModeBoolean, nullptr);
            result = true;
        }
        else
        {
            RIALTO_SERVER_LOG_ERROR("Failed to set syncmode-streaming property on parser '%s'", GST_ELEMENT_NAME(parser));
        }
        m_gstWrapper->gstObjectUnref(parser);
        std::unique_lock lock{m_context.propertyMutex};
        m_context.pendingStreamSyncMode.erase(type);
    }
    return result;
}

bool GstGenericPlayer::setRenderFrame()
{
    bool result{false};
    if (m_context.pendingRenderFrame)
    {
        static const std::string kStepOnPrerollPropertyName = "frame-step-on-preroll";
        GstElement *sink{getSink(MediaSourceType::VIDEO)};
        if (sink)
        {
            if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(sink), kStepOnPrerollPropertyName.c_str()))
            {
                RIALTO_SERVER_LOG_INFO("Rendering preroll");

                m_glibWrapper->gObjectSet(sink, kStepOnPrerollPropertyName.c_str(), 1, nullptr);
                m_gstWrapper->gstElementSendEvent(sink, m_gstWrapper->gstEventNewStep(GST_FORMAT_BUFFERS, 1, 1.0, true,
                                                                                      false));
                m_glibWrapper->gObjectSet(sink, kStepOnPrerollPropertyName.c_str(), 0, nullptr);
                result = true;
            }
            else
            {
                RIALTO_SERVER_LOG_ERROR("Video sink doesn't have property `%s`", kStepOnPrerollPropertyName.c_str());
            }
            m_gstWrapper->gstObjectUnref(sink);
            m_context.pendingRenderFrame = false;
        }
        else
        {
            RIALTO_SERVER_LOG_DEBUG("Pending render frame, sink is NULL");
        }
    }
    return result;
}

bool GstGenericPlayer::setBufferingLimit()
{
    bool result{false};
    guint bufferingLimit{0};
    {
        std::unique_lock lock{m_context.propertyMutex};
        if (!m_context.pendingBufferingLimit.has_value())
        {
            return false;
        }
        bufferingLimit = static_cast<guint>(m_context.pendingBufferingLimit.value());
    }

    GstElement *decoder{getDecoder(MediaSourceType::AUDIO)};
    if (decoder)
    {
        RIALTO_SERVER_LOG_DEBUG("Set limit-buffering-ms to %u", bufferingLimit);

        if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(decoder), "limit-buffering-ms"))
        {
            m_glibWrapper->gObjectSet(decoder, "limit-buffering-ms", bufferingLimit, nullptr);
            result = true;
        }
        else
        {
            RIALTO_SERVER_LOG_ERROR("Failed to set limit-buffering-ms property on decoder '%s'",
                                    GST_ELEMENT_NAME(decoder));
        }
        m_gstWrapper->gstObjectUnref(decoder);
        std::unique_lock lock{m_context.propertyMutex};
        m_context.pendingBufferingLimit.reset();
    }
    else
    {
        RIALTO_SERVER_LOG_DEBUG("Pending limit-buffering-ms, decoder is NULL");
    }
    return result;
}

bool GstGenericPlayer::setEnableRateCorrection()
{
    // Live-only, like the playbin path's reactive SetupElement branch. There is no pending flag —
    // it is driven purely by isLive at the point the decoder appears.
    if (!m_context.isLive)
    {
        return false;
    }
    GstElement *decoder{getDecoder(MediaSourceType::AUDIO)};
    if (!decoder)
    {
        RIALTO_SERVER_LOG_DEBUG("Pending enable-rate-correction, decoder is NULL");
        return false;
    }
    bool result{false};
    if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(decoder), "enable-rate-correction"))
    {
        RIALTO_SERVER_LOG_INFO("Enabling rate correction on the decoder.");
        m_glibWrapper->gObjectSet(decoder, "enable-rate-correction", TRUE, nullptr);
        result = true;
    }
    m_gstWrapper->gstObjectUnref(decoder);
    return result;
}

bool GstGenericPlayer::setUseBuffering()
{
    std::unique_lock lock{m_context.propertyMutex};
    if (m_context.pendingUseBuffering.has_value())
    {
        if (m_context.playbackGroup.m_curAudioDecodeBin)
        {
            gboolean useBufferingGboolean{m_context.pendingUseBuffering.value() ? TRUE : FALSE};
            RIALTO_SERVER_LOG_DEBUG("Set use-buffering to %d", useBufferingGboolean);
            m_glibWrapper->gObjectSet(m_context.playbackGroup.m_curAudioDecodeBin, "use-buffering",
                                      useBufferingGboolean, nullptr);
            m_context.pendingUseBuffering.reset();
            return true;
        }
        else
        {
            RIALTO_SERVER_LOG_DEBUG("Pending use-buffering, decodebin is NULL");
        }
    }
    return false;
}

void GstGenericPlayer::startPositionReportingAndCheckAudioUnderflowTimer()
{
    if (m_positionReportingAndCheckAudioUnderflowTimer && m_positionReportingAndCheckAudioUnderflowTimer->isActive())
    {
        return;
    }

    m_positionReportingAndCheckAudioUnderflowTimer = m_timerFactory->createTimer(
        kPositionReportTimerMs,
        [this]()
        {
            if (m_workerThread)
            {
                m_workerThread->enqueueTask(m_taskFactory->createReportPosition(m_context, *this));
                m_workerThread->enqueueTask(m_taskFactory->createCheckAudioUnderflow(m_context, *this));
            }
        },
        firebolt::rialto::common::TimerType::PERIODIC);
}

void GstGenericPlayer::stopPositionReportingAndCheckAudioUnderflowTimer()
{
    if (m_positionReportingAndCheckAudioUnderflowTimer && m_positionReportingAndCheckAudioUnderflowTimer->isActive())
    {
        m_positionReportingAndCheckAudioUnderflowTimer->cancel();
        m_positionReportingAndCheckAudioUnderflowTimer.reset();
    }
}

void GstGenericPlayer::startNotifyPlaybackInfoTimer()
{
    static constexpr std::chrono::milliseconds kPlaybackInfoTimerMs{32};
    if (m_playbackInfoTimer && m_playbackInfoTimer->isActive())
    {
        return;
    }

    notifyPlaybackInfo();

    m_playbackInfoTimer =
        m_timerFactory
            ->createTimer(kPlaybackInfoTimerMs, [this]() { notifyPlaybackInfo(); }, firebolt::rialto::common::TimerType::PERIODIC);
}

void GstGenericPlayer::stopNotifyPlaybackInfoTimer()
{
    if (m_playbackInfoTimer && m_playbackInfoTimer->isActive())
    {
        m_playbackInfoTimer->cancel();
        m_playbackInfoTimer.reset();
    }
}

void GstGenericPlayer::startSubtitleClockResyncTimer()
{
    if (m_subtitleClockResyncTimer && m_subtitleClockResyncTimer->isActive())
    {
        return;
    }

    m_subtitleClockResyncTimer = m_timerFactory->createTimer(
        kSubtitleClockResyncInterval,
        [this]()
        {
            if (m_workerThread)
            {
                m_workerThread->enqueueTask(m_taskFactory->createSynchroniseSubtitleClock(m_context, *this));
            }
        },
        firebolt::rialto::common::TimerType::PERIODIC);
}

void GstGenericPlayer::stopSubtitleClockResyncTimer()
{
    if (m_subtitleClockResyncTimer && m_subtitleClockResyncTimer->isActive())
    {
        m_subtitleClockResyncTimer->cancel();
        m_subtitleClockResyncTimer.reset();
    }
}

void GstGenericPlayer::stopWorkerThread()
{
    if (m_workerThread)
    {
        m_workerThread->stop();
    }
}

void GstGenericPlayer::setPendingPlaybackRate()
{
    RIALTO_SERVER_LOG_INFO("Setting pending playback rate");
    setPlaybackRate(m_context.pendingPlaybackRate);
}

bool GstGenericPlayer::applyPlaybackRate(double rate)
{
    if (!m_platformBackend)
    {
        RIALTO_SERVER_LOG_ERROR("No platform backend; cannot apply playback rate");
        return false;
    }
    return m_platformBackend->applyPlaybackRate(m_context.pipeline, rate);
}

bool GstGenericPlayer::isAudioFadeSupported() const
{
    return m_platformBackend ? m_platformBackend->isAudioFadeSupported() : false;
}

void GstGenericPlayer::audioFade(double target, uint32_t duration, firebolt::rialto::EaseType easeType)
{
    if (!m_platformBackend)
    {
        RIALTO_SERVER_LOG_ERROR("No platform backend; cannot apply audio fade");
        return;
    }
    m_platformBackend->audioFade(target, duration, easeType);
}

bool GstGenericPlayer::processAudioGap(GstElement *pipeline, int64_t position, uint32_t duration,
                                       int64_t discontinuityGap, bool audioAac)
{
    if (!m_platformBackend)
    {
        RIALTO_SERVER_LOG_ERROR("No platform backend; cannot process audio gap");
        return false;
    }
    return m_platformBackend->processAudioGap(pipeline, position, duration, discontinuityGap, audioAac);
}

void GstGenericPlayer::renderFrame()
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createRenderFrame(m_context, *this));
    }
}

void GstGenericPlayer::setVolume(double targetVolume, uint32_t volumeDuration, firebolt::rialto::EaseType easeType)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(
            m_taskFactory->createSetVolume(m_context, *this, targetVolume, volumeDuration, easeType));
    }
}

bool GstGenericPlayer::getVolume(double &currentVolume)
{
    // We are on main thread here, but m_context.pipeline can be used, because it's modified only in GstGenericPlayer
    // constructor and destructor. GstGenericPlayer is created/destructed on main thread, so we won't have a crash here.
    if (!m_context.pipeline)
    {
        return false;
    }

    // NOTE: No gstreamer documentation for "fade-volume" could be found at the time this code was written.
    // Therefore the author performed several tests on a supported platform (Flex2) to determine the behaviour of this property.
    // The code has been written to be backwardly compatible on platforms that don't have this property.
    // The observed behaviour was:
    //    - if the returned fade volume is negative then audio-fade is not active. In this case the usual technique
    //      to find volume in the pipeline works and is used.
    //    - if the returned fade volume is positive then audio-fade is active. In this case the returned fade volume
    //      directly returns the current volume level 0=min to 100=max (and the pipeline's current volume level is
    //      meaningless and doesn't contribute in this case).
    GstElement *sink{getSink(MediaSourceType::AUDIO)};
    if (m_context.audioFadeEnabled && sink &&
        m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(sink), "fade-volume"))
    {
        gint fadeVolume{-100};
        m_glibWrapper->gObjectGet(sink, "fade-volume", &fadeVolume, NULL);
        if (fadeVolume < 0)
        {
            currentVolume = m_gstWrapper->gstStreamVolumeGetVolume(GST_STREAM_VOLUME(m_context.pipeline),
                                                                   GST_STREAM_VOLUME_FORMAT_LINEAR);
            RIALTO_SERVER_LOG_INFO("Fade volume is negative, using volume from pipeline: %f", currentVolume);
        }
        else
        {
            currentVolume = static_cast<double>(fadeVolume) / 100.0;
            RIALTO_SERVER_LOG_INFO("Fade volume is supported: %f", currentVolume);
        }
        m_context.audioFadeVolume = currentVolume;
    }
    else
    {
        currentVolume = m_gstWrapper->gstStreamVolumeGetVolume(GST_STREAM_VOLUME(m_context.pipeline),
                                                               GST_STREAM_VOLUME_FORMAT_LINEAR);
        RIALTO_SERVER_LOG_INFO("Fade volume is not supported, using volume from pipeline: %f", currentVolume);
    }

    if (sink)
        m_gstWrapper->gstObjectUnref(sink);

    return true;
}

void GstGenericPlayer::setMute(const MediaSourceType &mediaSourceType, bool mute)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetMute(m_context, *this, mediaSourceType, mute));
    }
}

bool GstGenericPlayer::getMute(const MediaSourceType &mediaSourceType, bool &mute)
{
    // We are on main thread here, but m_context.pipeline can be used, because it's modified only in GstGenericPlayer
    // constructor and destructor. GstGenericPlayer is created/destructed on main thread, so we won't have a crash here.
    if (mediaSourceType == MediaSourceType::SUBTITLE)
    {
        if (!m_context.subtitleSink)
        {
            RIALTO_SERVER_LOG_ERROR("There is no subtitle sink");
            return false;
        }
        gboolean muteValue{FALSE};
        m_glibWrapper->gObjectGet(m_context.subtitleSink, "mute", &muteValue, nullptr);
        mute = muteValue;
    }
    else if (mediaSourceType == MediaSourceType::AUDIO)
    {
        if (!m_context.pipeline)
        {
            return false;
        }
        mute = m_gstWrapper->gstStreamVolumeGetMute(GST_STREAM_VOLUME(m_context.pipeline));
    }
    else
    {
        RIALTO_SERVER_LOG_ERROR("Getting mute for type %s unsupported", common::convertMediaSourceType(mediaSourceType));
        return false;
    }

    return true;
}

bool GstGenericPlayer::isAsync(const MediaSourceType &mediaSourceType) const
{
    GstElement *sink = getSink(mediaSourceType);
    if (!sink)
    {
        RIALTO_SERVER_LOG_WARN("Sink not found for %s", common::convertMediaSourceType(mediaSourceType));
        return true; // Our sinks are async by default
    }
    gboolean returnValue{TRUE};
    m_glibWrapper->gObjectGet(sink, "async", &returnValue, nullptr);
    m_gstWrapper->gstObjectUnref(sink);
    return returnValue == TRUE;
}

void GstGenericPlayer::setTextTrackIdentifier(const std::string &textTrackIdentifier)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetTextTrackIdentifier(m_context, textTrackIdentifier));
    }
}

bool GstGenericPlayer::getTextTrackIdentifier(std::string &textTrackIdentifier)
{
    if (!m_context.subtitleSink)
    {
        RIALTO_SERVER_LOG_ERROR("There is no subtitle sink");
        return false;
    }

    gchar *identifier = nullptr;
    m_glibWrapper->gObjectGet(m_context.subtitleSink, "text-track-identifier", &identifier, nullptr);

    if (identifier)
    {
        textTrackIdentifier = identifier;
        m_glibWrapper->gFree(identifier);
        return true;
    }
    else
    {
        RIALTO_SERVER_LOG_ERROR("Failed to get text track identifier");
        return false;
    }
}

bool GstGenericPlayer::setLowLatency(bool lowLatency)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetLowLatency(m_context, *this, lowLatency));
    }
    return true;
}

bool GstGenericPlayer::setSync(bool sync)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetSync(m_context, *this, sync));
    }
    return true;
}

bool GstGenericPlayer::getSync(bool &sync)
{
    bool returnValue{false};
    GstElement *sink{getSink(MediaSourceType::AUDIO)};
    if (sink)
    {
        if (m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(sink), "sync"))
        {
            m_glibWrapper->gObjectGet(sink, "sync", &sync, nullptr);
            returnValue = true;
        }
        else
        {
            RIALTO_SERVER_LOG_ERROR("Sync not supported in sink '%s'", GST_ELEMENT_NAME(sink));
        }
        m_gstWrapper->gstObjectUnref(sink);
    }
    else if (m_context.pendingSync.has_value())
    {
        RIALTO_SERVER_LOG_DEBUG("Returning queued value");
        sync = m_context.pendingSync.value();
        returnValue = true;
    }
    else
    {
        // We dont know the default setting on the sync, so return failure here
        RIALTO_SERVER_LOG_WARN("No audio sink attached or queued value");
    }

    return returnValue;
}

bool GstGenericPlayer::setSyncOff(bool syncOff)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetSyncOff(m_context, *this, syncOff));
    }
    return true;
}

bool GstGenericPlayer::setStreamSyncMode(const MediaSourceType &mediaSourceType, int32_t streamSyncMode)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(
            m_taskFactory->createSetStreamSyncMode(m_context, *this, mediaSourceType, streamSyncMode));
    }
    return true;
}

bool GstGenericPlayer::getStreamSyncMode(int32_t &streamSyncMode)
{
    bool returnValue{false};
    GstElement *decoder = getDecoder(MediaSourceType::AUDIO);
    if (decoder && m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(decoder), "stream-sync-mode"))
    {
        m_glibWrapper->gObjectGet(decoder, "stream-sync-mode", &streamSyncMode, nullptr);
        returnValue = true;
    }
    else
    {
        std::unique_lock lock{m_context.propertyMutex};
        if (m_context.pendingStreamSyncMode.find(MediaSourceType::AUDIO) != m_context.pendingStreamSyncMode.end())
        {
            RIALTO_SERVER_LOG_DEBUG("Returning queued value");
            streamSyncMode = m_context.pendingStreamSyncMode[MediaSourceType::AUDIO];
            returnValue = true;
        }
        else
        {
            RIALTO_SERVER_LOG_ERROR("Stream sync mode not supported in decoder '%s'",
                                    (decoder ? GST_ELEMENT_NAME(decoder) : "null"));
        }
    }

    if (decoder)
        m_gstWrapper->gstObjectUnref(GST_OBJECT(decoder));

    return returnValue;
}

void GstGenericPlayer::ping(std::unique_ptr<IHeartbeatHandler> &&heartbeatHandler)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createPing(std::move(heartbeatHandler)));
    }
}

void GstGenericPlayer::flush(const MediaSourceType &mediaSourceType, bool resetTime, bool &async)
{
    if (m_workerThread)
    {
        async = isAsync(mediaSourceType);
        m_flushWatcher->setFlushing(mediaSourceType, async);
        m_workerThread->enqueueTask(m_taskFactory->createFlush(m_context, *this, mediaSourceType, resetTime, async));
    }
}

void GstGenericPlayer::setSourcePosition(const MediaSourceType &mediaSourceType, int64_t position, bool resetTime,
                                         double appliedRate, uint64_t stopPosition)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetSourcePosition(m_context, mediaSourceType, position,
                                                                           resetTime, appliedRate, stopPosition));
    }
}

void GstGenericPlayer::setSubtitleOffset(int64_t position)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetSubtitleOffset(m_context, position));
    }
}

void GstGenericPlayer::processAudioGap(int64_t position, uint32_t duration, int64_t discontinuityGap, bool audioAac)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(
            m_taskFactory->createProcessAudioGap(m_context, *this, position, duration, discontinuityGap, audioAac));
    }
}

void GstGenericPlayer::setBufferingLimit(uint32_t limitBufferingMs)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetBufferingLimit(m_context, *this, limitBufferingMs));
    }
}

bool GstGenericPlayer::getBufferingLimit(uint32_t &limitBufferingMs)
{
    bool returnValue{false};
    GstElement *decoder = getDecoder(MediaSourceType::AUDIO);
    if (decoder && m_glibWrapper->gObjectClassFindProperty(G_OBJECT_GET_CLASS(decoder), "limit-buffering-ms"))
    {
        m_glibWrapper->gObjectGet(decoder, "limit-buffering-ms", &limitBufferingMs, nullptr);
        returnValue = true;
    }
    else
    {
        std::unique_lock lock{m_context.propertyMutex};
        if (m_context.pendingBufferingLimit.has_value())
        {
            RIALTO_SERVER_LOG_DEBUG("Returning queued value");
            limitBufferingMs = m_context.pendingBufferingLimit.value();
            returnValue = true;
        }
        else
        {
            RIALTO_SERVER_LOG_ERROR("buffering limit not supported in decoder '%s'",
                                    (decoder ? GST_ELEMENT_NAME(decoder) : "null"));
        }
    }

    if (decoder)
        m_gstWrapper->gstObjectUnref(GST_OBJECT(decoder));

    return returnValue;
}

void GstGenericPlayer::setUseBuffering(bool useBuffering)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSetUseBuffering(m_context, *this, useBuffering));
    }
}

bool GstGenericPlayer::getUseBuffering(bool &useBuffering)
{
    if (m_context.playbackGroup.m_curAudioDecodeBin)
    {
        m_glibWrapper->gObjectGet(m_context.playbackGroup.m_curAudioDecodeBin, "use-buffering", &useBuffering, nullptr);
        return true;
    }
    else
    {
        std::unique_lock lock{m_context.propertyMutex};
        if (m_context.pendingUseBuffering.has_value())
        {
            RIALTO_SERVER_LOG_DEBUG("Returning queued value");
            useBuffering = m_context.pendingUseBuffering.value();
            return true;
        }
    }
    return false;
}

void GstGenericPlayer::switchSource(const std::unique_ptr<IMediaPipeline::MediaSource> &mediaSource)
{
    if (m_workerThread)
    {
        m_workerThread->enqueueTask(m_taskFactory->createSwitchSource(*this, mediaSource));
    }
}

void GstGenericPlayer::handleBusMessage(GstMessage *message)
{
    m_workerThread->enqueueTask(m_taskFactory->createHandleBusMessage(m_context, *this, message, *m_flushWatcher));
}

void GstGenericPlayer::updatePlaybackGroup(GstElement *typefind, const GstCaps *caps)
{
    m_workerThread->enqueueTask(m_taskFactory->createUpdatePlaybackGroup(m_context, *this, typefind, caps));
}

}; // namespace firebolt::rialto::server
