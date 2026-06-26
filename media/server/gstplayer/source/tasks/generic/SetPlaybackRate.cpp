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

#include "tasks/generic/SetPlaybackRate.h"
#include "RialtoServerLogging.h"
#include <gst/gst.h>

namespace firebolt::rialto::server::tasks::generic
{
SetPlaybackRate::SetPlaybackRate(GenericPlayerContext &context, IGstGenericPlayerPrivate &player, double rate)
    : m_context{context}, m_player{player}, m_rate{rate}
{
    RIALTO_SERVER_LOG_DEBUG("Constructing SetPlaybackRate");
}

SetPlaybackRate::~SetPlaybackRate()
{
    RIALTO_SERVER_LOG_DEBUG("SetPlaybackRate finished");
}

void SetPlaybackRate::execute() const
{
    RIALTO_SERVER_LOG_DEBUG("Executing SetPlaybackRate");
    if (m_context.playbackRate == m_rate)
    {
        RIALTO_SERVER_LOG_DEBUG("No need to change playback rate - it is already %lf", m_rate);
        return;
    }

    if (!m_context.pipeline)
    {
        RIALTO_SERVER_LOG_INFO("Postponing set playback rate to %lf. Pipeline is NULL", m_rate);
        m_context.pendingPlaybackRate = m_rate;
        return;
    }

    if (GST_STATE(m_context.pipeline) < GST_STATE_PLAYING)
    {
        RIALTO_SERVER_LOG_INFO("Postponing set playback rate to %lf. Pipeline state is below PLAYING", m_rate);
        m_context.pendingPlaybackRate = m_rate;
        return;
    }
    m_context.pendingPlaybackRate = kNoPendingPlaybackRate;

    // The platform decides how a rate change is signalled (instant-rate event vs sink-pad
    // new-segment): the backend applies it so the engine names no SoC.
    if (m_player.applyPlaybackRate(m_rate))
    {
        RIALTO_SERVER_LOG_MIL("Playback rate set to: %lf", m_rate);
        m_context.playbackRate = m_rate;
    }
}
} // namespace firebolt::rialto::server::tasks::generic
