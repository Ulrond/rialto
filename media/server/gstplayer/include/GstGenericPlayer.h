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

#ifndef FIREBOLT_RIALTO_SERVER_GST_GENERIC_PLAYER_H_
#define FIREBOLT_RIALTO_SERVER_GST_GENERIC_PLAYER_H_

#include "GenericPlayerContext.h"
#include "IFlushWatcher.h"
#include "IGlibWrapper.h"
#include "IGstDispatcherThread.h"
#include "IGstDispatcherThreadClient.h"
#include "IGstGenericPlayer.h"
#include "IGstGenericPlayerPrivate.h"
#include "IGstInitialiser.h"
#include "IGstProfiler.h"
#include "IGstProtectionMetadataHelperFactory.h"
#include "IGstSrc.h"
#include "IPlatformBackend.h"
#include "IGstWrapper.h"
#include "ITimer.h"
#include "IWorkerThread.h"
#include "tasks/IGenericPlayerTaskFactory.h"
#include "tasks/IPlayerTask.h"
#include <IMediaPipeline.h>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace firebolt::rialto::server
{
constexpr uint32_t kMinPrimaryVideoWidth{1920};
constexpr uint32_t kMinPrimaryVideoHeight{1080};

/**
 * @brief IGstGenericPlayer factory class definition.
 */
class GstGenericPlayerFactory : public IGstGenericPlayerFactory
{
public:
    /**
     * @brief Weak pointer to the singleton factory object.
     */
    static std::weak_ptr<IGstGenericPlayerFactory> m_factory;

    std::unique_ptr<IGstGenericPlayer>
    createGstGenericPlayer(IGstGenericPlayerClient *client, IDecryptionService &decryptionService, MediaType type,
                           const VideoRequirements &videoRequirements, bool isLive,
                           const std::shared_ptr<firebolt::rialto::wrappers::IRdkGstreamerUtilsWrapperFactory>
                               &rdkGstreamerUtilsWrapperFactory) override;
};

/**
 * @brief The definition of the GstGenericPlayer.
 */
class GstGenericPlayer : public IGstGenericPlayer, public IGstGenericPlayerPrivate, public IGstDispatcherThreadClient
{
public:
    /**
     * @brief The constructor.
     *
     * @param[in] client                       : The gstreamer player client.
     * @param[in] decryptionService            : The decryption service
     * @param[in] type                         : The media type the gstreamer player shall support.
     * @param[in] videoRequirements            : The video requirements for the playback.
     * @param[in] gstWrapper                   : The gstreamer wrapper.
     * @param[in] glibWrapper                  : The glib wrapper.
     * @param[in] gstInitialiser               : The gst initialiser
     * @param[in] flushWatcher                 : The flush watcher
     * @param[in] gstSrcFactory                : The gstreamer rialto src factory.
     * @param[in] gstProfilerFactory           : The gstreamer rialto profiler factory.
     * @param[in] timerFactory                 : The Timer factory
     * @param[in] taskFactory                  : The task factory
     * @param[in] workerThreadFactory          : The worker thread factory
     * @param[in] gstDispatcherThreadFactory   : The gst dispatcher thread factory
     */
    GstGenericPlayer(IGstGenericPlayerClient *client, IDecryptionService &decryptionService, MediaType type,
                     const VideoRequirements &videoRequirements, bool isLive,
                     const std::shared_ptr<firebolt::rialto::wrappers::IGstWrapper> &gstWrapper,
                     const std::shared_ptr<firebolt::rialto::wrappers::IGlibWrapper> &glibWrapper,
                     const IGstInitialiser &gstInitialiser, std::unique_ptr<IFlushWatcher> &&flushWatcher,
                     const std::shared_ptr<IGstSrcFactory> &gstSrcFactory,
                     const std::shared_ptr<IGstProfilerFactory> &gstProfilerFactory,
                     std::shared_ptr<common::ITimerFactory> timerFactory,
                     std::unique_ptr<IGenericPlayerTaskFactory> taskFactory,
                     std::unique_ptr<IWorkerThreadFactory> workerThreadFactory,
                     std::unique_ptr<IGstDispatcherThreadFactory> gstDispatcherThreadFactory,
                     std::shared_ptr<IGstProtectionMetadataHelperFactory> gstProtectionMetadataFactory,
                     const std::shared_ptr<IPlatformBackend> &platformBackend = nullptr);

    /**
     * @brief Virtual destructor.
     */
    virtual ~GstGenericPlayer();

    void attachSource(const std::unique_ptr<IMediaPipeline::MediaSource> &mediaSource) override;
    void allSourcesAttached() override;
    void play(bool &async) override;
    void pause() override;
    void stop() override;
    void attachSamples(const IMediaPipeline::MediaSegmentVector &mediaSegments) override;
    void attachSamples(const std::shared_ptr<IDataReader> &dataReader) override;
    void setPosition(std::int64_t position) override;
    void setVideoGeometry(int x, int y, int width, int height) override;
    void setEos(const firebolt::rialto::MediaSourceType &type) override;
    void setPlaybackRate(double rate) override;
    bool getPosition(std::int64_t &position) override;
    bool getDuration(std::int64_t &duration) override;
    bool setImmediateOutput(const MediaSourceType &mediaSourceType, bool immediateOutput) override;
    bool getImmediateOutput(const MediaSourceType &mediaSourceType, bool &immediateOutput) override;
    bool getStats(const MediaSourceType &mediaSourceType, uint64_t &renderedFrames, uint64_t &droppedFrames) override;
    void setVolume(double targetVolume, uint32_t volumeDuration, firebolt::rialto::EaseType easeType) override;
    bool getVolume(double &volume) override;
    void setMute(const MediaSourceType &mediaSourceType, bool mute) override;
    bool getMute(const MediaSourceType &mediaSourceType, bool &mute) override;
    void setTextTrackIdentifier(const std::string &textTrackIdentifier) override;
    bool getTextTrackIdentifier(std::string &textTrackIdentifier) override;
    bool setLowLatency(bool lowLatency) override;
    bool setSync(bool sync) override;
    bool getSync(bool &sync) override;
    bool setSyncOff(bool syncOff) override;
    bool setStreamSyncMode(const MediaSourceType &mediaSourceType, int32_t streamSyncMode) override;
    bool getStreamSyncMode(int32_t &streamSyncMode) override;
    void ping(std::unique_ptr<IHeartbeatHandler> &&heartbeatHandler) override;
    void flush(const MediaSourceType &mediaSourceType, bool resetTime, bool &async) override;
    void setSourcePosition(const MediaSourceType &mediaSourceType, int64_t position, bool resetTime, double appliedRate,
                           uint64_t stopPosition) override;
    void setSubtitleOffset(int64_t position) override;
    void processAudioGap(int64_t position, uint32_t duration, int64_t discontinuityGap, bool audioAac) override;
    void setBufferingLimit(uint32_t limitBufferingMs) override;
    bool getBufferingLimit(uint32_t &limitBufferingMs) override;
    void setUseBuffering(bool useBuffering) override;
    bool getUseBuffering(bool &useBuffering) override;
    void switchSource(const std::unique_ptr<IMediaPipeline::MediaSource> &mediaSource) override;

private:
    void scheduleNeedMediaData(GstAppSrc *src) override;
    void scheduleEnoughData(GstAppSrc *src) override;
    void scheduleAudioUnderflow() override;
    void scheduleVideoUnderflow() override;
    void scheduleFirstVideoFrameReceived() override;
    void scheduleAllSourcesAttached() override;
    bool setVideoSinkRectangle() override;
    bool setImmediateOutput() override;
    bool setShowVideoWindow() override;
    bool setLowLatency() override;
    bool setSync() override;
    bool setSyncOff() override;
    bool setStreamSyncMode(const MediaSourceType &type) override;
    bool setRenderFrame() override;
    bool setBufferingLimit() override;
    bool setEnableRateCorrection() override;
    void connectDecoderSignals(const MediaSourceType &mediaSourceType) override;
    bool setUseBuffering() override;
    void notifyNeedMediaData(const MediaSourceType mediaSource) override;
    GstBuffer *createBuffer(const IMediaPipeline::MediaSegment &mediaSegment) const override;
    void attachData(const firebolt::rialto::MediaSourceType mediaType) override;
    void updateAudioCaps(int32_t rate, int32_t channels, const std::shared_ptr<CodecData> &codecData) override;
    void updateVideoCaps(int32_t width, int32_t height, Fraction frameRate,
                         const std::shared_ptr<CodecData> &codecData) override;
    void addAudioClippingToBuffer(GstBuffer *buffer, uint64_t clippingStart, uint64_t clippingEnd) const override;
    GstStateChangeReturn changePipelineState(GstState newState) override;
    int64_t getPosition(GstElement *element) override;
    void startPositionReportingAndCheckAudioUnderflowTimer() override;
    void stopPositionReportingAndCheckAudioUnderflowTimer() override;
    void startNotifyPlaybackInfoTimer() override;
    void stopNotifyPlaybackInfoTimer() override;
    void startSubtitleClockResyncTimer() override;
    void stopSubtitleClockResyncTimer() override;
    void stopWorkerThread() override;
    void cancelUnderflow(firebolt::rialto::MediaSourceType mediaSource) override;
    void setPendingPlaybackRate() override;
    bool applyPlaybackRate(double rate) override;
    bool isAudioFadeSupported() const override;
    void audioFade(double target, uint32_t duration, firebolt::rialto::EaseType easeType) override;
    bool processAudioGap(GstElement *pipeline, int64_t position, uint32_t duration, int64_t discontinuityGap,
                         bool audioAac) override;
    void renderFrame() override;
    void handleBusMessage(GstMessage *message) override;
    void updatePlaybackGroup(GstElement *typefind, const GstCaps *caps) override;

    void pushSampleIfRequired(GstElement *source, const std::string &typeStr) override;
    bool reattachSource(const std::unique_ptr<IMediaPipeline::MediaSource> &source) override;
    bool hasSourceType(const MediaSourceType &mediaSourceType) const override;
    GstElement *getSink(const MediaSourceType &mediaSourceType) const override;
    void setSourceFlushed(const MediaSourceType &mediaSourceType) override;
    bool isAsync(const MediaSourceType &mediaSourceType) const;
    void notifyPlaybackInfo() override;
    void buildAudioChain(GstElement *source) override;
    void buildVideoChain(GstElement *source) override;

private:
    /**
     * @brief Initialises the MSE pipeline (plain pipeline container; per-stream chains are built
     *        explicitly in AttachSource). No playbin autoplugging.
     */
    void initMsePipeline();

    /**
     * @brief Callback on the explicit audio chain's decodebin pad-added. Called by the Gstreamer
     *        thread. Links the decoder's freshly-exposed src pad to the static audioconvert tail.
     *
     * @param[in] decodebin : the decodebin that exposed the pad.
     * @param[in] pad       : the decoder src pad to link downstream.
     * @param[in] self      : Reference to the calling object.
     */
    static void audioDecodebinPadAdded(GstElement *decodebin, GstPad *pad, GstGenericPlayer *self);

    /**
     * @brief Callback on the explicit video chain's decodebin pad-added. Called by the Gstreamer
     *        thread. Links the decoder's freshly-exposed src pad to the backend video sink.
     *
     * @param[in] decodebin : the decodebin that exposed the pad.
     * @param[in] pad       : the decoder src pad to link downstream.
     * @param[in] self      : Reference to the calling object.
     */
    static void videoDecodebinPadAdded(GstElement *decodebin, GstPad *pad, GstGenericPlayer *self);

    /**
     * @brief Enqueues a SetupAudioDecoder task. Called from the explicit audio chain's decodebin
     *        pad-added callback (Gstreamer thread) once the decoder has been autoplugged, so the
     *        pending audio-decoder properties are applied on the worker thread.
     */
    void scheduleSetupAudioDecoder();

    /**
     * @brief Enqueues a SetupVideoParser task. Called from the explicit video chain's decodebin
     *        pad-added callback (Gstreamer thread) once the parser has been autoplugged, so the
     *        pending video-parser property is applied on the worker thread.
     */
    void scheduleSetupVideoParser();

    /**
     * @brief Connects the underflow (and, for video, first-video-frame) telemetry callbacks on an
     *        explicit-construction element whose role and media type are already known. The
     *        explicit-path analogue of the SetupElement reactive wiring.
     *
     * @param[in] element         : The sink or decoder to wire.
     * @param[in] mediaSourceType : AUDIO or VIDEO.
     */
    void connectStreamSignals(GstElement *element, const MediaSourceType &mediaSourceType);

    /**
     * @brief Terminates the player pipeline.
     */
    void termPipeline();

    /**
     * @brief Shutdown and destroys the worker thread.
     */
    void resetWorkerThread();

    /**
     * @brief Sets codec_data in GstCaps if available
     *
     * @retval True if caps were changed
     */
    bool setCodecData(GstCaps *caps, const std::shared_ptr<CodecData> &codecData) const;

    /**
     * @brief Gets the decoder element for source type.
     *
     * @param[in] mediaSourceType : the source type to obtain the decoder for
     *
     * @retval The decoder, NULL if not found
     */
    GstElement *getDecoder(const MediaSourceType &mediaSourceType);

    /**
     * @brief Gets the parser element for source type.
     *
     * @param[in] mediaSourceType : the source type to obtain the parser for
     *
     * @retval The parser, NULL if not found
     */
    GstElement *getParser(const MediaSourceType &mediaSourceType);

    /**
     * @brief Gets the audio typefind element from within the audio decodebin (explicit-construction path).
     *
     * Located by name rather than factory type; scoped to m_curAudioDecodeBin so a per-stream video
     * decodebin's typefind is not matched.
     *
     * @retval The typefind, NULL if not found
     */
    GstElement *getAudioTypefind();

    /**
     * @brief Refreshes the audio decoder/parser/typefind playback-group handles from the live decodebin.
     *
     * Explicit-construction analogue of the playbin path's DeepElementAdded: there are no playbin signals
     * to populate the playback group that the audio-codec-switch machinery reads, so the per-switch
     * handles are refreshed from the graph just before a codec switch. Called by worker thread only.
     */
    void updateAudioPlaybackGroupHandles();

    /**
     * @brief Constructs new Audio Attributes structure based on MediaSource
     *        Called by worker thread only!
     *
     * @param[in] source : the media source, which contains params needed by Audio Attributes struct
     *
     * @retval The Audio Attributes structure on success, std::nullopt on failure
     */
    std::optional<firebolt::rialto::wrappers::AudioAttributesPrivate>
    createAudioAttributes(const std::unique_ptr<IMediaPipeline::MediaSource> &source) const;

    /**
     * @brief Sets text track position before pushing data
     *
     * @param[in] source : the subtitle media source
     */
    void setTextTrackPositionIfRequired(GstElement *source);

    /**
     * @brief GstAppSrc does not replace segment, if it's the same as previous one.
     *        It causes problems with position reporting on some platforms, so we need to push
     *        two segments with different reset time value.
     *
     * @param[in] source : the media source
     */
    void pushAdditionalSegmentIfRequired(GstElement *source);

private:
    /**
     * @brief The player context.
     */
    GenericPlayerContext m_context;

    /**
     * @brief The gstreamer player client.
     */
    IGstGenericPlayerClient *m_gstPlayerClient = nullptr;

    /**
     * @brief The gstreamer wrapper object.
     */
    std::shared_ptr<firebolt::rialto::wrappers::IGstWrapper> m_gstWrapper;

    /**
     * @brief The glib wrapper object.
     */
    std::shared_ptr<firebolt::rialto::wrappers::IGlibWrapper> m_glibWrapper;

    /**
     * @brief The SoC platform backend (sink/element creation behind a versioned ABI).
     *        May be null until the platform-backed construction path is wired.
     */
    std::shared_ptr<IPlatformBackend> m_platformBackend;

    /**
     * @brief Factory creating gst profilers
     */
    std::shared_ptr<IGstProfilerFactory> m_gstProfilerFactory;

    /**
     * @brief Thread for handling player tasks.
     */
    std::unique_ptr<IWorkerThread> m_workerThread;

    /**
     * @brief Thread for handling gst bus callbacks
     */
    std::unique_ptr<IGstDispatcherThread> m_gstDispatcherThread;

    /**
     * @brief Factory creating timers
     *
     */
    std::shared_ptr<common::ITimerFactory> m_timerFactory;

    /**
     * @brief Timer to trigger FinishSourceSetup
     */
    std::unique_ptr<firebolt::rialto::common::ITimer> m_finishSourceSetupTimer{nullptr};

    /**
     * @brief Timer checking audio underflow
     *
     * Variable can be used only in worker thread
     */
    std::unique_ptr<firebolt::rialto::common::ITimer> m_positionReportingAndCheckAudioUnderflowTimer{nullptr};

    /**
     * @brief Timer reporting playback information
     *
     * Variable can be used only in worker thread
     */
    std::unique_ptr<firebolt::rialto::common::ITimer> m_playbackInfoTimer{nullptr};

    /**
     * @brief Timer to resync subtitle clock with AV clock
     *
     * Variable can be used only in worker thread
     */
    std::unique_ptr<firebolt::rialto::common::ITimer> m_subtitleClockResyncTimer{nullptr};

    /**
     * @brief The GstGenericPlayer task factory
     */
    std::unique_ptr<IGenericPlayerTaskFactory> m_taskFactory;

    /**
     * @brief The protection metadata wrapper
     */
    std::unique_ptr<IGstProtectionMetadataHelper> m_protectionMetadataWrapper;

    /**
     * @brief The object used to check flushing state for all sources
     */
    std::unique_ptr<IFlushWatcher> m_flushWatcher;

    /**
     * @brief The explicit audio chain's audioconvert, held so the decodebin pad-added callback can
     *        link the decoder src pad to it. Set by buildAudioChain (explicit construction only).
     */
    GstElement *m_explicitAudioConvert{nullptr};

    /**
     * @brief The explicit video chain's backend video sink, held so the decodebin pad-added callback
     *        can link the decoder src pad to it. Set by buildVideoChain (explicit construction only).
     */
    GstElement *m_explicitVideoSink{nullptr};
};

} // namespace firebolt::rialto::server

#endif // FIREBOLT_RIALTO_SERVER_GST_GENERIC_PLAYER_H_
