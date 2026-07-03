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

// Loader smoke test: dlopen a per-SoC backend .so exactly as PlatformBackendLoader does, resolve the
// three extern "C" entrypoints, and check the reported ABI version against the expected value. Proves
// each shim is loadable and version-correct without needing the GStreamer/rdk wrappers a live backend
// would be handed. Usage: abi-smoke <expected-abi> <path-to-.so>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s <expected-abi> <path-to-.so>\n", argv[0]);
        return 2;
    }
    const auto expected = static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 10));
    const char *path = argv[2];

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        std::fprintf(stderr, "FAIL dlopen %s: %s\n", path, dlerror());
        return 1;
    }

    auto abiVersion = reinterpret_cast<std::uint32_t (*)()>(dlsym(handle, "rialtoPlatformBackendAbiVersion"));
    void *create = dlsym(handle, "rialtoCreatePlatformBackend");
    void *destroy = dlsym(handle, "rialtoDestroyPlatformBackend");
    if (!abiVersion || !create || !destroy)
    {
        std::fprintf(stderr, "FAIL %s: missing loader entrypoint(s)\n", path);
        dlclose(handle);
        return 1;
    }

    const std::uint32_t got = abiVersion();
    if (got != expected)
    {
        std::fprintf(stderr, "FAIL %s: ABI version %u, expected %u\n", path, got, expected);
        dlclose(handle);
        return 1;
    }

    std::printf("OK  %s  (ABI v%u, all entrypoints resolved)\n", path, got);
    dlclose(handle);
    return 0;
}
