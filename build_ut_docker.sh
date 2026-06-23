#!/usr/bin/env bash
# ceres: run rialto native unit-test build via sc (sc-integrated rialto-build:local image).
# sc launches the container AS the host user and binds the home dir transparently, so artifacts are
# host-owned and paths match the host. This is the native mocked-unit-test inner loop ONLY -- it is
# NOT the production build (production = official rdk-kirkstone/Yocto cross-build, which builds its own
# gstreamer + deps). Args pass straight to build_ut.py:
#   ./build_ut_docker.sh -s servergstplayer servermain
# Note: sc space-joins the post-"--" command, so avoid shell-glob chars in a -gf filter (quote-safe
# filters like FooTest.* are fine; *Foo* may glob-expand against the cwd).
set -e
cd "$(dirname "$0")"
exec sc docker run -l -t local rialto-build -- env PROFILER_ENABLED=true python3 build_ut.py "$@"
