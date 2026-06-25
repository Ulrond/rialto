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

#ifndef FIREBOLT_RIALTO_SERVER_PLATFORM_BACKEND_LOADER_H_
#define FIREBOLT_RIALTO_SERVER_PLATFORM_BACKEND_LOADER_H_

#include "IPlatformBackendLoader.h"
#include <memory>
#include <string>

namespace firebolt::rialto::server
{
/**
 * @brief Reference platform-backend loader.
 *
 * Discovery order (Decision 1): the explicit path in RIALTO_PLATFORM_BACKEND, else a
 * single librialtoplatform-*.so in RIALTO_PLATFORM_DIR (or the compile-time default
 * directory), else the built-in reference backend. Exactly one external backend is
 * loaded per process; an ABI-version mismatch or a missing entrypoint is refused and
 * falls back to the reference backend (never degrades).
 */
class PlatformBackendLoader : public IPlatformBackendLoader
{
public:
    PlatformBackendLoader() = default;
    ~PlatformBackendLoader() override = default;

    std::shared_ptr<IPlatformBackend> load(const PlatformHostContext &host) const override;

private:
    /**
     * @brief Resolves the path of the backend .so to load, honouring the env overrides.
     *
     * @retval the .so path, or an empty string if no external object is configured/present.
     */
    std::string discoverBackendObject() const;

    /**
     * @brief dlopen + version-check + create the backend at @p path.
     *
     * @retval the loaded backend (deleter unloads the .so), or nullptr to fall back.
     */
    std::shared_ptr<IPlatformBackend> loadExternal(const std::string &path, const PlatformHostContext &host) const;

    /**
     * @brief The guaranteed in-process reference backend (autoaudiosink / autovideosink).
     */
    std::shared_ptr<IPlatformBackend> referenceBackend(const PlatformHostContext &host) const;
};

} // namespace firebolt::rialto::server

#endif // FIREBOLT_RIALTO_SERVER_PLATFORM_BACKEND_LOADER_H_
