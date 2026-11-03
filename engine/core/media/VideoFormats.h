// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEOFORMATS_H
#define VIDEOFORMATS_H

#include "Export.h"
#include "VideoTypes.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace jasperengine {

    // What a probe of a video file found (header only: WebM's EBML header, segment info and
    // tracks; an MP4's moov box)
    struct VideoProbe {
        bool playable = false;          // the engine can play it here: WebM AV1/Opus everywhere (software),
                                        // other codecs where this platform's decoders have them (VideoSourceFactory)
        bool readable = false;          // the file could be read
        std::string container;          // "webm", "matroska", "mp4", "mov", "3gp", "" (neither)
        std::string videoCodec;         // the first video track's codec id: Matroska ("V_AV1", "V_VP9") or the MP4 sample entry ("avc1", "hvc1")
        std::string audioCodec;         // the first audio track's ("A_OPUS", "A_VORBIS", "mp4a", ...)
        std::string videoCodecString;   // RFC 6381 where known ("avc1.42E01E"), else videoCodec
        std::string audioCodecString;   // ("mp4a.40.2")
        bool audioPlayable = true;      // false: an audio track the engine ignores (another codec)
        int width = 0;                  // display size (pixel aspect and rotation applied)
        int height = 0;
        int audioChannels = 0;
        double duration = 0.0;          // seconds (0: the file does not say)
        double frameRate = 0.0;         // MP4: of the sample table (0: unknown)
        int rotation = 0;               // MP4: clockwise degrees the picture is turned to show
        bool fastStart = false;         // MP4: the moov box precedes the media data (progressive start from URLs)
        bool fragmented = false;        // MP4: movie fragments
        bool encrypted = false;         // MP4: DRM-protected (never playable)
        std::string backend;            // what would play it here: "webm", "ffmpeg", "mediacodec", ... ("" none)
        MediaError error;               // why it cannot be played (here)
    };

    // The formats of <video> (docs/html-ui/phase-6-video.md §3.4.3), shared by the element, the
    // compat layer, `check --ui` and the editor's importer.
    class JASPERENGINE_API VideoFormats {
    public:
        enum class CanPlay { NO, MAYBE, PROBABLY };

        // HTMLMediaElement.canPlayType: video/webm or audio/webm with codecs from {av01[.*], opus}
        // -> PROBABLY (software, every platform), video/webm (or audio/webm) without codecs ->
        // MAYBE; video/mp4, audio/mp4, video/quicktime and other WebM codecs by what this
        // platform's decoders have (VideoSourceFactory::codecSupport): PROBABLY when every codec
        // is decoded, MAYBE when one depends on the device or no codecs are given, else NO
        static CanPlay canPlayType(const std::string& mimeType);
        // by a path's extension or a URL's (the probe decides for files): "probably" for .webm,
        // .mp4/.m4v/.mov where the platform has an H.264 decoder ("maybe" when that depends on
        // the device), "" for anything else
        static CanPlay canPlayExtension(const std::string& pathOrUrl);
        static const char* toString(CanPlay value);     // "probably", "maybe", ""

        // by the extension (case-insensitive, query and fragment ignored):
        static bool isVideoPath(const std::string& path);         // a video/media file of any kind
        static bool isPlayableExtension(const std::string& path); // .webm, .mkv, .mp4, .m4v, .mov (the probe decides)
        static bool isRejectedExtension(const std::string& path); // a container the engine never plays (.avi, .ogv, ...)
        static bool isMp4Extension(const std::string& path);      // .mp4, .m4v, .mov

        // header-only probe through FileData (asset paths, absolute paths, the resource pack)
        static VideoProbe probeFile(const std::string& path);
        // the same over a file's bytes
        static VideoProbe probeMemory(const uint8_t* data, size_t size, const std::string& name);
        // the same over an open stdio file (owned; closed when done): host files outside the
        // engine's file system, e.g. the editor's importer (no System needed)
        static VideoProbe probeStdio(FILE* handle, const std::string& name);

        // "V_VP9" -> "VP9", "A_VORBIS" -> "Vorbis", "V_MPEG4/ISO/AVC" -> "H.264", MP4 sample
        // entries ("avc1" -> "H.264", "mp4a" -> "AAC") and FFmpeg's names ("h264", "aac"), ...
        static std::string codecName(const std::string& codecId);
        // the same ids (and RFC 6381 strings "avc1.42E01E") as FFmpeg's codec name: "h264",
        // "hevc", "vp8", "vp9", "av1", "aac", "mp3", "opus", "vorbis", "flac", ... ("" unknown)
        static std::string decoderName(const std::string& codecId);
        // the fix to suggest for a file the engine cannot play
        static const char* conversionHint();
    };

}

#endif //VIDEOFORMATS_H
