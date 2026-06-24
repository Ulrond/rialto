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

#ifndef FIREBOLT_RIALTO_SERVER_TASKS_GENERIC_SETUP_VIDEO_PARSER_H_
#define FIREBOLT_RIALTO_SERVER_TASKS_GENERIC_SETUP_VIDEO_PARSER_H_

#include "GenericPlayerContext.h"
#include "IGstGenericPlayerPrivate.h"
#include "IPlayerTask.h"

namespace firebolt::rialto::server::tasks::generic
{
/**
 * @brief Applies the pending video-parser property once the explicit video chain's decodebin has
 *        autoplugged the parser. This is the explicit-construction analogue of the playbin path's
 *        reactive SetupElement parser branch: the stream-sync-mode setter reaches the parser inside
 *        decodebin via getParser(VIDEO).
 */
class SetupVideoParser : public IPlayerTask
{
public:
    SetupVideoParser(GenericPlayerContext &context, IGstGenericPlayerPrivate &player);
    ~SetupVideoParser() override;
    void execute() const override;

private:
    GenericPlayerContext &m_context;
    IGstGenericPlayerPrivate &m_player;
};
} // namespace firebolt::rialto::server::tasks::generic

#endif // FIREBOLT_RIALTO_SERVER_TASKS_GENERIC_SETUP_VIDEO_PARSER_H_
