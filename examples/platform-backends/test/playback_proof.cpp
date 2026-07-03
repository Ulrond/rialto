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

// End-to-end playback proof: load a per-SoC shim through the real PlatformBackendLoader (the production
// dlopen + ABI-gate + destroy/dlclose path), hand it real gst/glib wrappers, and let it build and drive
// an actual GStreamer pipeline to EOS. This exercises the whole ABI contract on real elements, not mocks
// — buildVideoPath constructs the topology, the harness plays the engine's role (linking decodebin's
// dynamic pad to decoderLinkTarget), and the stream runs to completion.
//
// Headless-safe: GST_VIDEO_SINK=fakesink makes the backend's autovideosink resolve to fakesink so no
// display is needed. The linux profile has no SoC audio path, so a null rdk-gstreamer-utils wrapper is
// correct here. Usage: playback-proof <path-to-librialtoplatform-linux.so>

#include "GlibWrapper.h"
#include "GstWrapper.h"
#include "IPlatformBackend.h"
#include "PlatformBackendLoader.h"

#include <cstdio>
#include <cstdlib>
#include <gst/gst.h>
#include <memory>
#include <string>

namespace
{
// Engine role: when decodebin exposes its dynamic src pad, link it to the element the backend named as
// decoderLinkTarget (audioconvert on the reference audio path).
void onPadAdded(GstElement * /*decodebin*/, GstPad *pad, gpointer user)
{
    GstElement *target = static_cast<GstElement *>(user);
    GstPad *sinkpad = gst_element_get_static_pad(target, "sink");
    if (sinkpad)
    {
        if (!gst_pad_is_linked(sinkpad))
            gst_pad_link(pad, sinkpad);
        gst_object_unref(sinkpad);
    }
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <path-to-backend.so>\n", argv[0]);
        return 2;
    }

    // Set env before the loader so it picks our shim; surface the loader's "Loaded platform backend" line.
    setenv("RIALTO_PLATFORM_BACKEND", argv[1], 1);
    setenv("RIALTO_CONSOLE_LOG", "1", 1);
    setenv("RIALTO_DEBUG", "server:5", 1);

    gst_init(&argc, &argv);

    using firebolt::rialto::server::IPlatformBackend;
    using firebolt::rialto::server::PlatformBackendLoader;
    using firebolt::rialto::server::PlatformHostContext;
    using firebolt::rialto::server::PlatformMediaPath;

    auto gstWrapper = std::make_shared<firebolt::rialto::wrappers::GstWrapper>();
    auto glibWrapper = std::make_shared<firebolt::rialto::wrappers::GlibWrapper>();
    PlatformHostContext host{gstWrapper, glibWrapper, nullptr};

    // The production loader: discovers our shim via RIALTO_PLATFORM_BACKEND, ABI-gates it, and returns it
    // behind a deleter that destroys + dlcloses. (Falls back to the in-core reference only if load fails.)
    PlatformBackendLoader loader;
    std::shared_ptr<IPlatformBackend> backend = loader.load(host);
    if (!backend)
    {
        std::fprintf(stderr, "FAIL: loader returned no backend\n");
        return 1;
    }
    std::printf(">> loaded backend platformName = '%s'\n", backend->platformName());

    // A real, fully-decoded source: encoded Opus-in-Ogg into the backend's audio decode path. decodebin
    // demuxes+decodes to raw audio on its dynamic pad, which the harness (engine role) links to the tail.
    GstElement *pipeline = gst_pipeline_new("playback-proof");
    GstElement *src = gst_element_factory_make("audiotestsrc", "src");
    GstElement *conv = gst_element_factory_make("audioconvert", "srcconv");
    GstElement *enc = gst_element_factory_make("opusenc", "enc");
    GstElement *mux = gst_element_factory_make("oggmux", "mux");
    if (!pipeline || !src || !conv || !enc || !mux)
    {
        std::fprintf(stderr, "FAIL: could not create source elements (audiotestsrc/audioconvert/opusenc/oggmux)\n");
        return 1;
    }
    g_object_set(src, "num-buffers", 100, NULL);
    gst_bin_add_many(GST_BIN(pipeline), src, conv, enc, mux, NULL);
    gst_element_link_many(src, conv, enc, mux, NULL);

    // The backend builds the decode path from our source (the oggmux output) and owns the topology.
    PlatformMediaPath path = backend->buildAudioPath(pipeline, mux);
    if (!path.sink)
    {
        std::fprintf(stderr, "FAIL: buildAudioPath returned no sink\n");
        return 1;
    }
    std::printf(">> buildAudioPath: sink=%p decodebin=%p decoderLinkTarget=%p\n", (void *)path.sink,
                (void *)path.decodebin, (void *)path.decoderLinkTarget);
    if (path.decodebin && path.decoderLinkTarget)
        g_signal_connect(path.decodebin, "pad-added", G_CALLBACK(onPadAdded), path.decoderLinkTarget);

    // Light exercise of the rest of the ABI on the live backend.
    std::printf(">> isVideoMaster=%d\n", backend->isVideoMaster());
    {
        std::vector<std::string> want{"audio-fade", "async"};
        auto got = backend->getSupportedProperties(firebolt::rialto::MediaSourceType::AUDIO, want);
        std::printf(">> getSupportedProperties -> %zu of %zu\n", got.size(), want.size());
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        std::fprintf(stderr, "FAIL: pipeline could not go to PLAYING\n");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        return 1;
    }

    // A rate change is a live-graph op, applied while the pipeline is running.
    std::printf(">> applyPlaybackRate(1.0)=%d\n", backend->applyPlaybackRate(pipeline, 1.0));

    GstBus *bus = gst_element_get_bus(pipeline);
    bool eos = false;
    bool error = false;
    for (int i = 0; i < 100 && !eos && !error; ++i) // up to ~20s
    {
        GstMessage *msg = gst_bus_timed_pop_filtered(bus, 200 * GST_MSECOND,
                                                     static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!msg)
            continue;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
        {
            eos = true;
        }
        else
        {
            GError *err = nullptr;
            gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            std::fprintf(stderr, "FAIL: pipeline error: %s\n", err ? err->message : "(unknown)");
            if (err)
                g_error_free(err);
            g_free(dbg);
            error = true;
        }
        gst_message_unref(msg);
    }

    GstState state{GST_STATE_VOID_PENDING};
    gst_element_get_state(pipeline, &state, nullptr, 0);
    std::printf(">> reached state=%s, eos=%d\n", gst_element_state_get_name(state), eos);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    backend.reset(); // triggers the backend's destroy entrypoint + dlclose

    if (eos && !error)
    {
        std::printf("=== PASS: real playback ran to EOS through the dlopen'd backend ===\n");
        return 0;
    }
    std::fprintf(stderr, "=== FAIL: playback did not complete ===\n");
    return 1;
}
