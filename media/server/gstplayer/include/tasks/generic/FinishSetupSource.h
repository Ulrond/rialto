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

#ifndef FIREBOLT_RIALTO_SERVER_TASKS_GENERIC_FINISH_SETUP_SOURCE_H_
#define FIREBOLT_RIALTO_SERVER_TASKS_GENERIC_FINISH_SETUP_SOURCE_H_

#include "GenericPlayerContext.h"
#include "IGlibWrapper.h"
#include "IGstGenericPlayerClient.h"
#include "IGstGenericPlayerPrivate.h"
#include "IGstWrapper.h"
#include "IPlayerTask.h"
#include <gst/app/gstappsrc.h>
#include <memory>

namespace firebolt::rialto::server::tasks::generic
{
class FinishSetupSource : public IPlayerTask
{
public:
    FinishSetupSource(GenericPlayerContext &context,
                      const std::shared_ptr<firebolt::rialto::wrappers::IGstWrapper> &gstWrapper,
                      const std::shared_ptr<firebolt::rialto::wrappers::IGlibWrapper> &glibWrapper,
                      IGstGenericPlayerPrivate &player, IGstGenericPlayerClient *client);
    ~FinishSetupSource() override;
    void execute() const override;

private:
    /**
     * @brief Configures the chain appsrc and wires its data-flow callbacks directly (explicit
     *        construction). The appsrc is already in the pipeline (built by buildAudioChain), so
     *        this bypasses GstSrc::setupAndAddAppSrc and the rialtosrc bin entirely.
     *
     * @param[in] streamInfo : the stream whose appsrc is configured.
     * @param[in] callbacks  : the need/enough/seek callbacks to set on the appsrc.
     * @param[in] type       : the media source type (selects the appsrc max-bytes).
     */
    void configureExplicitAppSrc(StreamInfo &streamInfo, GstAppSrcCallbacks *callbacks,
                                 firebolt::rialto::MediaSourceType type) const;

    GenericPlayerContext &m_context;
    std::shared_ptr<firebolt::rialto::wrappers::IGstWrapper> m_gstWrapper;
    std::shared_ptr<firebolt::rialto::wrappers::IGlibWrapper> m_glibWrapper;
    IGstGenericPlayerPrivate &m_player;
    IGstGenericPlayerClient *m_gstPlayerClient;
};
} // namespace firebolt::rialto::server::tasks::generic

#endif // FIREBOLT_RIALTO_SERVER_TASKS_GENERIC_FINISH_SETUP_SOURCE_H_
