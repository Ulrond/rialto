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

#ifndef RIALTO_PLATFORM_BACKENDS_COMMON_PLATFORM_BACKEND_ENTRY_H_
#define RIALTO_PLATFORM_BACKENDS_COMMON_PLATFORM_BACKEND_ENTRY_H_

#include "GenericGstBackend.h"
#include "IPlatformBackend.h"
#include <new>

// Emits the versioned extern "C" loader ABI for a per-SoC backend .so, wiring a GenericGstBackend to the
// given SocProfile. Each per-SoC translation unit is then just: define its SocProfile, then
// RIALTO_DEFINE_PLATFORM_BACKEND(kProfile).
#define RIALTO_DEFINE_PLATFORM_BACKEND(PROFILE)                                                          \
    extern "C" uint32_t rialtoPlatformBackendAbiVersion(void)                                            \
    {                                                                                                   \
        return firebolt::rialto::server::kPlatformBackendAbiVersion;                                     \
    }                                                                                                   \
    extern "C" firebolt::rialto::server::IPlatformBackend *rialtoCreatePlatformBackend(                  \
        const firebolt::rialto::server::PlatformHostContext *host)                                       \
    {                                                                                                   \
        if (!host)                                                                                      \
            return nullptr;                                                                             \
        return new (std::nothrow) firebolt::rialto::server::backends::GenericGstBackend((PROFILE), *host); \
    }                                                                                                   \
    extern "C" void rialtoDestroyPlatformBackend(firebolt::rialto::server::IPlatformBackend *backend)    \
    {                                                                                                   \
        delete backend;                                                                                 \
    }

#endif // RIALTO_PLATFORM_BACKENDS_COMMON_PLATFORM_BACKEND_ENTRY_H_
