// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FFMPEGBACKEND_H
#define FFMPEGBACKEND_H

#include "Export.h"
#include "media/PlatformVideoSource.h"

#include <memory>
#include <string>

namespace jasperengine {

    // The system's FFmpeg libraries, loaded with dlopen (docs/superpowers/specs/
    // 2026-10-31-video-mp4-design.md §6.6): the newest consistent set of libavformat,
    // libavcodec, libavutil and libswresample (libswscale optional) among FFmpeg 4.x to 8.x,
    // each major compiled against its vendored headers (engine/libs/ffmpeg-headers/<major>),
    // one translation unit per major (FFmpegBackend58.cpp ... FFmpegBackend62.cpp). Nothing is
    // linked or shipped; JASPERENGINE_FFMPEG_DIR points the loader at another directory (tests).
    struct FFmpegLibraries {
        void* avutil = nullptr;
        void* swresample = nullptr;
        void* swscale = nullptr;        // optional: other pixel formats
        void* avcodec = nullptr;
        void* avformat = nullptr;
        int avcodecMajor = 0;
        std::string version;            // "6.1 (avcodec 60.31)"
    };

    class JASPERENGINE_API FFmpegBackend {
    public:
        // loads the libraries once (thread-safe); false with the reason (what to install)
        static bool isAvailable(std::string* reason = nullptr);
        // the decoders of a video/audio stream (null when unavailable)
        static std::unique_ptr<PlatformDecoderBackend> create();
        // the libraries have a decoder for an FFmpeg codec name ("h264", "hevc", "vp9", "aac", ...)
        static bool hasDecoder(const std::string& codec);
        static std::string getVersion();
        // what the user should install (MEDIA_ERR_SRC_NOT_SUPPORTED's message)
        static const char* missingLibrariesMessage();
        // tests: forget the loaded set and load again (another JASPERENGINE_FFMPEG_DIR)
        static void reloadForTesting();
    };

    // one per major, in FFmpegBackend<major>.cpp
    namespace ffmpeg58 { std::unique_ptr<PlatformDecoderBackend> create(const FFmpegLibraries& libs, std::string& error); bool hasDecoder(const FFmpegLibraries& libs, const char* codec); }
    namespace ffmpeg59 { std::unique_ptr<PlatformDecoderBackend> create(const FFmpegLibraries& libs, std::string& error); bool hasDecoder(const FFmpegLibraries& libs, const char* codec); }
    namespace ffmpeg60 { std::unique_ptr<PlatformDecoderBackend> create(const FFmpegLibraries& libs, std::string& error); bool hasDecoder(const FFmpegLibraries& libs, const char* codec); }
    namespace ffmpeg61 { std::unique_ptr<PlatformDecoderBackend> create(const FFmpegLibraries& libs, std::string& error); bool hasDecoder(const FFmpegLibraries& libs, const char* codec); }
    namespace ffmpeg62 { std::unique_ptr<PlatformDecoderBackend> create(const FFmpegLibraries& libs, std::string& error); bool hasDecoder(const FFmpegLibraries& libs, const char* codec); }

}

#endif //FFMPEGBACKEND_H
