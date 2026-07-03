#!/usr/bin/env bash
#
# Copyright 2026 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Build + smoke-test the per-SoC backend shims. One entrypoint, either context:
#   - already inside rialto-build:local  -> runs the tests directly
#   - on the host                        -> relaunches itself in the image via sc, then runs them
# Just run it:  ./examples/platform-backends/test.sh
set -e

SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$SELF")"

run_tests()
{
    rm -rf build
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build -j"$(nproc)"

    echo "=== built .so artifacts ==="
    find build -name 'librialtoplatform-*.so' | sort

    local abi_header="../../media/server/platform/interface/IPlatformBackend.h"
    local expected_abi
    expected_abi="$(grep -oE 'kPlatformBackendAbiVersion *= *[0-9]+' "${abi_header}" | grep -oE '[0-9]+')"

    echo "=== dlopen smoke test (expect ABI v${expected_abi}) ==="
    local fail=0
    local so
    for so in build/librialtoplatform-*.so; do
        build/abi-smoke "${expected_abi}" "${so}" || fail=1
    done

    # The mocked-unit image has core+base GStreamer but no vendor/autodetect sinks, so the proof drives a
    # real opus/ogg decode pipeline through the fakesink-leaf fixture backend (same loader + GenericGstBackend
    # path as a shipped SoC). The five real shims are covered by the dlopen smoke test above.
    echo "=== end-to-end playback proof (fixture shim via the real loader) ==="
    build/playback-proof build/librialtoplatform-fakesink.so || fail=1

    if [ "${fail}" = 0 ]; then
        echo "=== ALL PASS ==="
    else
        echo "=== FAILURES ==="
        return 1
    fi
}

# /.dockerenv exists in any container; RIALTO_IN_DOCKER is the belt-and-braces override we set on relaunch.
if [ -f /.dockerenv ] || [ -n "${RIALTO_IN_DOCKER}" ]; then
    run_tests
else
    echo "Not in docker -> relaunching in rialto-build:local via sc ..."
    exec sc docker run -l -t local rialto-build -- env RIALTO_IN_DOCKER=1 bash "${SELF}"
fi
