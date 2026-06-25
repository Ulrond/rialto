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

#include "PlatformBackendLoader.h"
#include "LinuxPlatformBackend.h"
#include "RialtoServerLogging.h"
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <new>
#include <string>

// Compile-time default directory the loader scans for a vendor backend .so when
// RIALTO_PLATFORM_DIR is not set. Overridable via the build; falls back to a sane
// install path so the loader is well-defined even without the define.
#ifndef RIALTO_DEFAULT_PLATFORM_BACKEND_DIR
#define RIALTO_DEFAULT_PLATFORM_BACKEND_DIR "/usr/lib/rialto/platform"
#endif

namespace
{
constexpr char kBackendEnvVar[]{"RIALTO_PLATFORM_BACKEND"};
constexpr char kBackendDirEnvVar[]{"RIALTO_PLATFORM_DIR"};
constexpr char kBackendPrefix[]{"librialtoplatform-"};
constexpr char kBackendSuffix[]{".so"};

using AbiVersionFn = uint32_t (*)(void);
using CreateBackendFn = firebolt::rialto::server::IPlatformBackend *(*)(const firebolt::rialto::server::PlatformHostContext *);
using DestroyBackendFn = void (*)(firebolt::rialto::server::IPlatformBackend *);

std::string envOrEmpty(const char *name)
{
    const char *value = std::getenv(name);
    return (value != nullptr) ? std::string{value} : std::string{};
}

bool matchesBackendName(const std::string &filename)
{
    const std::string prefix{kBackendPrefix};
    const std::string suffix{kBackendSuffix};
    return filename.size() > prefix.size() + suffix.size() &&
           filename.compare(0, prefix.size(), prefix) == 0 &&
           filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0;
}
} // namespace

namespace firebolt::rialto::server
{
std::shared_ptr<IPlatformBackend> PlatformBackendLoader::load(const PlatformHostContext &host) const
{
    const std::string path{discoverBackendObject()};
    if (!path.empty())
    {
        std::shared_ptr<IPlatformBackend> backend{loadExternal(path, host)};
        if (backend)
            return backend;
        // loadExternal already logged the reason; fall through to the reference backend.
    }

    return referenceBackend(host);
}

std::string PlatformBackendLoader::discoverBackendObject() const
{
    // 1. Explicit override: an absolute path to a specific .so (bring-up / override).
    const std::string explicitPath{envOrEmpty(kBackendEnvVar)};
    if (!explicitPath.empty())
    {
        RIALTO_SERVER_LOG_INFO("Platform backend override: %s=%s", kBackendEnvVar, explicitPath.c_str());
        return explicitPath;
    }

    // 2. A single librialtoplatform-*.so in the configured directory (else the default).
    std::string dir{envOrEmpty(kBackendDirEnvVar)};
    if (dir.empty())
        dir = RIALTO_DEFAULT_PLATFORM_BACKEND_DIR;

    std::error_code ec;
    std::filesystem::directory_iterator it{dir, ec};
    if (ec)
    {
        RIALTO_SERVER_LOG_INFO("No platform backend directory '%s' (%s); using reference backend", dir.c_str(),
                               ec.message().c_str());
        return {};
    }

    std::string found;
    for (const std::filesystem::directory_entry &entry : it)
    {
        if (!matchesBackendName(entry.path().filename().string()))
            continue;
        if (!found.empty())
        {
            RIALTO_SERVER_LOG_ERROR("Multiple platform backends in '%s' (e.g. '%s' and '%s'); ambiguous, using "
                                    "reference backend",
                                    dir.c_str(), found.c_str(), entry.path().string().c_str());
            return {};
        }
        found = entry.path().string();
    }

    if (!found.empty())
        RIALTO_SERVER_LOG_INFO("Platform backend discovered: %s", found.c_str());
    return found;
}

std::shared_ptr<IPlatformBackend> PlatformBackendLoader::loadExternal(const std::string &path,
                                                                      const PlatformHostContext &host) const
{
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        RIALTO_SERVER_LOG_ERROR("dlopen('%s') failed: %s; using reference backend", path.c_str(), dlerror());
        return nullptr;
    }

    auto abiVersionFn = reinterpret_cast<AbiVersionFn>(dlsym(handle, "rialtoPlatformBackendAbiVersion"));
    auto createFn = reinterpret_cast<CreateBackendFn>(dlsym(handle, "rialtoCreatePlatformBackend"));
    auto destroyFn = reinterpret_cast<DestroyBackendFn>(dlsym(handle, "rialtoDestroyPlatformBackend"));
    if (!abiVersionFn || !createFn || !destroyFn)
    {
        RIALTO_SERVER_LOG_ERROR("Platform backend '%s' is missing a required entrypoint; using reference backend",
                                path.c_str());
        dlclose(handle);
        return nullptr;
    }

    const uint32_t reported{abiVersionFn()};
    if (reported != kPlatformBackendAbiVersion)
    {
        RIALTO_SERVER_LOG_ERROR("Platform backend '%s' ABI version %u != core %u; refusing it, using reference backend",
                                path.c_str(), reported, kPlatformBackendAbiVersion);
        dlclose(handle);
        return nullptr;
    }

    IPlatformBackend *backend{createFn(&host)};
    if (!backend)
    {
        RIALTO_SERVER_LOG_ERROR("Platform backend '%s' failed to create; using reference backend", path.c_str());
        dlclose(handle);
        return nullptr;
    }

    RIALTO_SERVER_LOG_INFO("Loaded platform backend '%s' (%s, ABI v%u)", backend->platformName(), path.c_str(),
                           reported);

    // The deleter keeps the .so mapped exactly as long as the backend lives: destroy the
    // backend through its own entrypoint, then dlclose the handle (Decision 4).
    return std::shared_ptr<IPlatformBackend>(backend,
                                             [handle, destroyFn](IPlatformBackend *b)
                                             {
                                                 destroyFn(b);
                                                 dlclose(handle);
                                             });
}

std::shared_ptr<IPlatformBackend> PlatformBackendLoader::referenceBackend(const PlatformHostContext &host) const
{
    // Decision 3: the reference backend is compiled in and used directly — no dlopen, no
    // dlclose — so native playback is a can't-fail path. Plain deleter.
    return std::shared_ptr<IPlatformBackend>(new (std::nothrow) LinuxPlatformBackend(host));
}

} // namespace firebolt::rialto::server
