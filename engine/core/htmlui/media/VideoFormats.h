// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEOFORMATS_H
#define VIDEOFORMATS_H

#include "Export.h"
#include "VideoTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace jasperengine {

    // What a probe of a video file found (header only: EBML header, segment info and tracks)
    struct VideoProbe {
        bool playable = false;          // the engine can play it (AV1 and/or Opus in WebM/Matroska)
        bool readable = false;          // the file could be read
        std::string container;          // "webm", "matroska", "" (not EBML)
        std::string videoCodec;         // the first video track's Matroska codec id ("V_AV1", "V_VP9", ...)
        std::string audioCodec;         // the first audio track's ("A_OPUS", "A_VORBIS", ...)
        bool audioPlayable = true;      // false: an audio track the engine ignores (another codec)
        int width = 0;                  // display size
        int height = 0;
        int audioChannels = 0;
        double duration = 0.0;          // seconds (0: the segment does not say)
        MediaError error;               // why it cannot be played
    };

    // The formats of <video> (docs/html-ui/phase-6-video.md §3.4.3), shared by the element, the
    // compat layer, `check --ui` and the editor's importer.
    class JASPERENGINE_API VideoFormats {
    public:
        enum class CanPlay { NO, MAYBE, PROBABLY };

        // HTMLMediaElement.canPlayType: video/webm or audio/webm with codecs from {av01[.*], opus}
        // -> PROBABLY, video/webm (or audio/webm) without codecs -> MAYBE, anything else -> NO
        static CanPlay canPlayType(const std::string& mimeType);
        static const char* toString(CanPlay value);     // "probably", "maybe", ""

        // by the extension (case-insensitive, query and fragment ignored):
        static bool isVideoPath(const std::string& path);         // a video/media file of any kind
        static bool isPlayableExtension(const std::string& path); // .webm, .mkv (the probe decides)
        static bool isRejectedExtension(const std::string& path); // a container the engine never plays (.mp4, .mov, ...)

        // header-only probe through FileData (asset paths, absolute paths, the resource pack)
        static VideoProbe probeFile(const std::string& path);
        // the same over a file's bytes
        static VideoProbe probeMemory(const uint8_t* data, size_t size, const std::string& name);

        // "V_VP9" -> "VP9", "A_VORBIS" -> "Vorbis", "V_MPEG4/ISO/AVC" -> "H.264", ...
        static std::string codecName(const std::string& matroskaCodecId);
        // the fix to suggest for a file the engine cannot play
        static const char* conversionHint();
    };

}

#endif //VIDEOFORMATS_H
