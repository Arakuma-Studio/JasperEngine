// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PLATFORMVIDEOBACKEND_H
#define PLATFORMVIDEOBACKEND_H

#include "Export.h"
#include "PlatformVideoSource.h"

#include <memory>
#include <string>

namespace jasperengine::platformvideo {

    // The platform's own decoders behind PlatformVideoSource (VideoSourceFactory asks here). Each
    // platform's backend file defines these (engine/CMakeLists.txt compiles one):
    // platform/linux/FFmpegBackend.cpp (Linux, FreeBSD: the system FFmpeg), platform/android/
    // MediaCodecBackend.cpp, platform/apple/AVFoundationBackend.mm, platform/win/
    // MediaFoundationBackend.cpp; platform/emscripten/WebVideoSource.cpp has none (the browser's
    // element is a source of its own).

    enum class Support { NO, MAYBE, YES };

    // "ffmpeg", "mediacodec", "avfoundation", "mediafoundation"; "" without one
    JASPERENGINE_API const char* backendName();
    // the decoders can be used (Linux: the FFmpeg libraries were found); false with what to do
    JASPERENGINE_API bool isAvailable(std::string* reason);
    JASPERENGINE_API std::unique_ptr<PlatformDecoderBackend> createBackend();
    // a codec by its FFmpeg name: "h264", "hevc", "vp8", "vp9", "av1", "aac", "mp3", "opus",
    // "vorbis", "flac", "ac3", "eac3", "alac" (asked of the OS at first use, then cached)
    JASPERENGINE_API Support supports(const std::string& codec);
    // containers the decoders read: "mp4" (MP4, MOV, M4V, 3GP), "webm" (WebM, Matroska)
    JASPERENGINE_API bool readsContainer(const std::string& container);
    // a file still downloading can be played (a decoder reading through MediaByteSource)
    JASPERENGINE_API bool supportsGrowingSources();
    // what makes `codec` play here, for the messages (Windows: the HEVC Video Extensions, ...)
    JASPERENGINE_API std::string unsupportedHint(const std::string& codec);

#if defined(__EMSCRIPTEN__)
    // the Web: the browser's own <video> (platform/emscripten/WebVideoSource.cpp) plays everything
    // but WebM AV1/Opus; `url`: an http(s) URL the browser streams, else a file of the engine's
    // file system (played from a Blob of its bytes)
    JASPERENGINE_API std::unique_ptr<IVideoSource> createBrowserSource(const std::string& pathOrUrl, bool url, bool audio);
    // the browser's canPlayType for a codec in a container ("mp4", "webm")
    JASPERENGINE_API Support browserSupports(const std::string& codec, const std::string& container);
#endif

}

#endif //PLATFORMVIDEOBACKEND_H
