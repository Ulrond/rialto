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

#include "tasks/generic/FinishSetupSource.h"
#include "GenericPlayerContext.h"
#include "IGstGenericPlayerClient.h"
#include "IGstGenericPlayerPrivate.h"
#include "RialtoServerLogging.h"
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <unordered_map>

namespace
{
/**
 * @brief Callback for need-data event from gstreamer. Called by the Gstreamer thread.
 *
 * @param[in] src       : the appsrc element that emitted the signal
 * @param[in] length    : the amount of bytes needed.
 * @param[in] user_data : The data to be passed with the message.
 *
 */
void appSrcNeedData(GstAppSrc *src, guint length, gpointer user_data)
{
    firebolt::rialto::server::IGstGenericPlayerPrivate *self =
        static_cast<firebolt::rialto::server::IGstGenericPlayerPrivate *>(user_data);
    self->scheduleNeedMediaData(src);
}

/**
 * @brief Callback for enough-data event from gstreamer. Called by the Gstreamer thread.
 *
 * @param[in] src       : the appsrc element that emitted the signal
 * @param[in] user_data : The data to be passed with the message.
 *
 */
void appSrcEnoughData(GstAppSrc *src, gpointer user_data)
{
    firebolt::rialto::server::IGstGenericPlayerPrivate *self =
        static_cast<firebolt::rialto::server::IGstGenericPlayerPrivate *>(user_data);
    self->scheduleEnoughData(src);
}

/**
 * @brief Callback for seek-data event from gstreamer. Called by the Gstreamer thread.
 *
 * @param[in] src       : the appsrc element that emitted the signal
 * @param[in] offset    : the offset to seek to
 * @param[in] user_data : The data to be passed with the message.
 *
 * @retval true if the handling of the message is successful, false otherwise.
 */
gboolean appSrcSeekData(GstAppSrc *src, guint64 offset, gpointer user_data)
{
    appSrcEnoughData(src, user_data);
    return TRUE;
}
} // namespace

namespace firebolt::rialto::server::tasks::generic
{
FinishSetupSource::FinishSetupSource(GenericPlayerContext &context,
                                     const std::shared_ptr<firebolt::rialto::wrappers::IGstWrapper> &gstWrapper,
                                     const std::shared_ptr<firebolt::rialto::wrappers::IGlibWrapper> &glibWrapper,
                                     IGstGenericPlayerPrivate &player, IGstGenericPlayerClient *client)
    : m_context{context}, m_gstWrapper{gstWrapper}, m_glibWrapper{glibWrapper}, m_player{player}, m_gstPlayerClient{client}
{
    RIALTO_SERVER_LOG_DEBUG("Constructing FinishSetupSource");
}

FinishSetupSource::~FinishSetupSource()
{
    RIALTO_SERVER_LOG_DEBUG("FinishSetupSource finished");
}

void FinishSetupSource::execute() const
{
    RIALTO_SERVER_LOG_DEBUG("Executing FinishSetupSource");
    m_context.wereAllSourcesAttached = true;

    GstAppSrcCallbacks callbacks = {appSrcNeedData, appSrcEnoughData, appSrcSeekData, {nullptr}};

    // Each stream's appsrc is already in the pipeline (built by buildAudioChain/buildVideoChain/
    // buildSubtitleChain) and there is no rialtosrc. Wire its data-flow callbacks directly, bypassing
    // GstSrc::setupAndAddAppSrc.
    for (auto &elem : m_context.streamInfo)
    {
        const firebolt::rialto::MediaSourceType sourceType = elem.first;
        if (sourceType == firebolt::rialto::MediaSourceType::UNKNOWN)
        {
            RIALTO_SERVER_LOG_WARN("Unknown media segment type");
            continue;
        }

        StreamInfo &streamInfo = elem.second;
        configureExplicitAppSrc(streamInfo, &callbacks, sourceType);
        streamInfo.isDataNeeded = true;
        m_player.notifyNeedMediaData(sourceType);
    }

    // Notify GstPlayerClient of Idle state once setup has finished
    if (m_gstPlayerClient)
        m_gstPlayerClient->notifyPlaybackState(PlaybackState::IDLE);

    m_context.setupSourceFinished = true;

    RIALTO_SERVER_LOG_MIL("All sources attached.");
    auto recordId = m_context.gstProfiler->createRecord("All Sources Attached");
    if (recordId)
        m_context.gstProfiler->logRecord(recordId.value());
}

void FinishSetupSource::configureExplicitAppSrc(StreamInfo &streamInfo, GstAppSrcCallbacks *callbacks,
                                                firebolt::rialto::MediaSourceType type) const
{
    m_glibWrapper->gObjectSet(streamInfo.appSrc, "block", FALSE, "format", GST_FORMAT_TIME, "stream-type",
                              GST_APP_STREAM_TYPE_STREAM, "min-percent", 20, "handle-segment-change", TRUE, nullptr);
    m_gstWrapper->gstAppSrcSetCallbacks(GST_APP_SRC(streamInfo.appSrc), callbacks, &m_player, nullptr);

    const std::unordered_map<firebolt::rialto::MediaSourceType, uint32_t> queueSize =
        {{firebolt::rialto::MediaSourceType::VIDEO, 8 * 1024 * 1024},
         {firebolt::rialto::MediaSourceType::AUDIO, 512 * 1024},
         {firebolt::rialto::MediaSourceType::SUBTITLE, 256 * 1024}};

    auto sizeIt = queueSize.find(type);
    if (sizeIt != queueSize.end())
    {
        m_gstWrapper->gstAppSrcSetMaxBytes(GST_APP_SRC(streamInfo.appSrc), sizeIt->second);
    }
    else
    {
        RIALTO_SERVER_LOG_WARN("Could not find max-bytes value for appsrc");
    }

    m_gstWrapper->gstAppSrcSetStreamType(GST_APP_SRC(streamInfo.appSrc), GST_APP_STREAM_TYPE_SEEKABLE);
}
} // namespace firebolt::rialto::server::tasks::generic
