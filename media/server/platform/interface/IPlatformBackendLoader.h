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

#ifndef FIREBOLT_RIALTO_SERVER_I_PLATFORM_BACKEND_LOADER_H_
#define FIREBOLT_RIALTO_SERVER_I_PLATFORM_BACKEND_LOADER_H_

#include "IPlatformBackend.h"
#include <memory>

namespace firebolt::rialto::server
{
/**
 * @brief Acquires the SoC platform backend at engine startup.
 *
 * The loader discovers a per-SoC backend shared object, dlopen()s it, resolves the
 * versioned extern "C" entrypoints (rialtoPlatformBackendAbiVersion /
 * rialtoCreatePlatformBackend / rialtoDestroyPlatformBackend), and rejects any object
 * whose reported ABI version differs from kPlatformBackendAbiVersion. When no vendor
 * object resolves, it returns the built-in reference Linux backend (autoaudiosink /
 * autovideosink) in-process, so Rialto-for-Linux always plays with zero configuration.
 *
 * This is the production acquisition seam. The GstGenericPlayer constructor's
 * platformBackend parameter remains the test injection point, so unit and component
 * tests bypass the loader entirely by supplying a fake IPlatformBackend.
 */
class IPlatformBackendLoader
{
public:
    virtual ~IPlatformBackendLoader() = default;

    /**
     * @brief Discovers, loads and version-checks the platform backend, or falls back
     *        to the reference backend.
     *
     * @param[in] host : Services (gst/glib wrappers) handed to the backend at creation.
     *
     * @retval a shared_ptr owning the backend; for a loaded object the deleter calls
     *         rialtoDestroyPlatformBackend then dlclose, so the .so stays mapped exactly
     *         as long as the backend lives. Never returns nullptr (falls back to the
     *         reference backend).
     */
    virtual std::shared_ptr<IPlatformBackend> load(const PlatformHostContext &host) const = 0;
};

} // namespace firebolt::rialto::server

#endif // FIREBOLT_RIALTO_SERVER_I_PLATFORM_BACKEND_LOADER_H_
