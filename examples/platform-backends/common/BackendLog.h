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

#ifndef RIALTO_PLATFORM_BACKENDS_COMMON_BACKEND_LOG_H_
#define RIALTO_PLATFORM_BACKENDS_COMMON_BACKEND_LOG_H_

#include <cstdio>

// Dependency-free logging for the standalone prototype .so — it must not pull the core's
// RialtoServerLogging (which would need linking the core). When these backends relocate to
// comcast-sky/rialto-platform-backends this maps onto the platform's real logger.
#define BE_LOG_ERROR(fmt, ...) std::fprintf(stderr, "[rialto-backend][E] " fmt "\n", ##__VA_ARGS__)
#define BE_LOG_WARN(fmt, ...) std::fprintf(stderr, "[rialto-backend][W] " fmt "\n", ##__VA_ARGS__)
#ifdef RIALTO_BACKEND_VERBOSE
#define BE_LOG_DEBUG(fmt, ...) std::fprintf(stderr, "[rialto-backend][D] " fmt "\n", ##__VA_ARGS__)
#else
#define BE_LOG_DEBUG(fmt, ...) \
    do                         \
    {                          \
    } while (0)
#endif

#endif // RIALTO_PLATFORM_BACKENDS_COMMON_BACKEND_LOG_H_
