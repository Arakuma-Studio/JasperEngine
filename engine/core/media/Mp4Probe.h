// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MP4PROBE_H
#define MP4PROBE_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    class MediaByteSource;

    // One track of an MP4/QuickTime file as its moov box describes it
    struct JASPERENGINE_API Mp4Track {
        enum class Kind { NONE, VIDEO, AUDIO, OTHER };
        Kind kind = Kind::NONE;
        uint32_t id = 0;
        bool enabled = true;
        std::string fourcc;             // the sample entry: "avc1", "avc3", "hvc1", "hev1", "mp4a", "Opus", "vp09", "av01", ...
        std::string codecString;        // RFC 6381 where the probe knows it ("avc1.42E01E", "mp4a.40.2"), else the fourcc
        bool encrypted = false;         // encv/enca (DRM): never playable
        int width = 0, height = 0;      // video: the sample entry's coded size
        double presentationWidth = 0.0; // video: tkhd's size (pixel aspect applied, before the rotation)
        double presentationHeight = 0.0;
        int rotation = 0;               // video: clockwise degrees of the track matrix (0/90/180/270)
        int bitDepth = 8;               // video: luma bits (avcC high profiles, hvcC, vpcC, av1C)
        int channels = 0;               // audio
        int sampleRate = 0;
        uint32_t timescale = 0;         // mdhd
        uint64_t durationTicks = 0;
        double duration = 0.0;          // seconds
        uint32_t sampleCount = 0;       // stts total (0 in a fragmented file's moov)
        double frameRate = 0.0;         // video: samples per second of the sample table
        int64_t mediaTime = 0;          // the first edit's media_time (ticks; 0 without an edit list)

        // where the sample tables are (buildVideoIndex)
        int64_t sttsOffset = -1, sttsSize = 0;
        int64_t cttsOffset = -1, cttsSize = 0;
        int64_t stssOffset = -1, stssSize = 0;
    };

    // What the header boxes of an ISO-BMFF file (MP4, M4V, MOV, 3GP) say
    struct JASPERENGINE_API Mp4ProbeResult {
        bool isoBmff = false;           // an ftyp or a moov/mdat at the start: this format
        bool ok = false;                // the moov box was read
        bool needMoreData = false;      // the bytes ended before the moov (a file still downloading)
        std::string error;
        std::string majorBrand;
        std::vector<std::string> brands;
        std::string container;          // "mp4", "mov", "3gp"
        bool fastStart = false;         // the moov box precedes the media data (playable while downloading)
        bool fragmented = false;        // movie fragments (moof): the moov has no sample tables
        int64_t moovOffset = -1;
        int64_t mdatOffset = -1;
        double duration = 0.0;          // seconds (mvhd, else the longest track)
        int trackCount = 0;
        Mp4Track video;                 // the first enabled video track (kind NONE: none)
        Mp4Track audio;                 // the first enabled audio track
    };

    // The presentation times of a video track's samples in display order and its sync samples
    struct JASPERENGINE_API Mp4VideoIndex {
        std::vector<int64_t> ptsNs;     // ascending; [0] is the first frame shown (edit list applied)
        std::vector<int> keyframes;     // indices into ptsNs, ascending
        double frameRate = 0.0;

        // the last frame whose pts <= ns (0 before the first); -1 when empty
        int frameAt(int64_t ns) const;
        int keyframeAtOrBefore(int index) const;
        int keyframeAfter(int index) const;     // -1 if none
    };

    // The ISO base media file format reader of the video probe (VideoFormats::probeFile) and the
    // platform sources (frame index, fast start): box headers are read on demand through
    // positional reads, the media data and the large sample tables (stsz, stco) never.
    class JASPERENGINE_API Mp4Probe {
    public:
        // the first bytes look like ISO-BMFF (an ftyp, moov, mdat, free, skip or wide box)
        static bool sniff(const uint8_t* data, size_t size);
        static Mp4ProbeResult probe(MediaByteSource& source);
        // the presentation index of the result's video track (stts, ctts, stss, the edit list)
        static bool buildVideoIndex(MediaByteSource& source, const Mp4ProbeResult& result, Mp4VideoIndex& out, std::string& error);
        // "avc1" -> "H.264", "hvc1" -> "HEVC", "mp4a" -> "AAC", ... (the fourcc when unknown)
        static std::string codecName(const std::string& fourcc);

        // "Optimize for streaming": rewrites `input` with its moov box before the media data
        // (chunk offsets moved), into `output`; a file already in that order is copied. False
        // with `error` for fragmented or unreadable files
        static bool rewriteFastStart(const std::string& inputSystemPath, const std::string& outputSystemPath, std::string& error);
    };

}

#endif //MP4PROBE_H
