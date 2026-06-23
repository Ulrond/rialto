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

#include "tasks/generic/SetupAudioDecoder.h"
#include "RialtoServerLogging.h"

namespace firebolt::rialto::server::tasks::generic
{
SetupAudioDecoder::SetupAudioDecoder(GenericPlayerContext &context, IGstGenericPlayerPrivate &player)
    : m_context{context}, m_player{player}
{
    RIALTO_SERVER_LOG_DEBUG("Constructing SetupAudioDecoder");
}

SetupAudioDecoder::~SetupAudioDecoder()
{
    RIALTO_SERVER_LOG_DEBUG("SetupAudioDecoder finished");
}

void SetupAudioDecoder::execute() const
{
    RIALTO_SERVER_LOG_DEBUG("Executing SetupAudioDecoder");

    if (!m_context.pipeline)
    {
        return;
    }

    // The explicit audio chain's decodebin has autoplugged the audio decoder, so the pending
    // audio-decoder properties can now be applied. Each setter is guarded by its own pending state
    // and reaches the decoder inside decodebin via getDecoder(AUDIO).
    m_player.setSyncOff();
    m_player.setStreamSyncMode(MediaSourceType::AUDIO);
    m_player.setBufferingLimit();
    m_player.setEnableRateCorrection();

    // Wire underflow telemetry on the autoplugged audio decoder (the sink was wired in buildAudioChain).
    m_player.connectDecoderSignals(MediaSourceType::AUDIO);
}
} // namespace firebolt::rialto::server::tasks::generic
