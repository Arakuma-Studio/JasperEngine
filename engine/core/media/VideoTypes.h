// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEOTYPES_H
#define VIDEOTYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    // How a frame's YUV values become RGB (docs/html-ui/phase-6-video.md §3.6.5)
    struct VideoColor {
        enum class Matrix : uint8_t { BT601, BT709, BT2020, IDENTITY, UNSPECIFIED };
        Matrix matrix = Matrix::UNSPECIFIED;
        bool fullRange = false;
        // ISO/IEC 23091-4 code points as signalled (2 = unspecified): reported, the transfer is not converted
        int primaries = 2;
        int transfer = 2;

        // the matrix to use: the signalled one, else BT.601 under 720 lines and BT.709 from 720 (FFmpeg, browsers)
        Matrix resolved(int height) const;
        bool isHdr() const;   // PQ (16) or HLG (18) transfer
    };

    // One decoded picture as tightly packed 8-bit planes (10/12-bit streams are rounded to 8 bits).
    // u/v are empty for monochrome streams.
    struct VideoFrame {
        int64_t ptsNs = 0;
        int64_t durationNs = 0;
        int index = -1;            // in the video track's frame index
        int64_t sequence = -1;     // iteration * frameCount + index (loops; VideoStream)
        int width = 0;
        int height = 0;
        int chromaShiftX = 1;      // 4:2:0 -> 1,1; 4:2:2 -> 1,0; 4:4:4 -> 0,0
        int chromaShiftY = 1;
        int bitDepth = 8;          // of the stream
        VideoColor color;
        std::vector<uint8_t> y, u, v;

        int chromaWidth() const { return (width + (1 << chromaShiftX) - 1) >> chromaShiftX; }
        int chromaHeight() const { return (height + (1 << chromaShiftY) - 1) >> chromaShiftY; }
        bool hasChroma() const { return !u.empty(); }
        size_t bytes() const { return y.size() + u.size() + v.size(); }
    };

    // What an opened video source found (IVideoSource::getInfo, READY only)
    struct VideoInfo {
        bool hasVideo = false;
        bool hasAudio = false;
        int width = 0, height = 0;                    // coded (decoded) size
        int displayWidth = 0, displayHeight = 0;      // the size shown (pixel aspect and rotation applied)
        int frameCount = 0;                           // the index's, else round(duration x frameRate)
        int64_t durationNs = 0;
        double duration = 0.0;
        double frameRate = 0.0;                       // frames per second (0: unknown)
        int rotation = 0;                             // clockwise degrees the picture is turned to show (0/90/180/270)
        int audioChannels = 0;                        // of the stream (the ring has 1 or 2)
        int decoderThreads = 0;
        std::string container;                        // "webm", "matroska", "mp4", "mov", "" (the browser's)
        std::string videoCodec;                       // the container's id: Matroska ("V_AV1") or ISO-BMFF ("avc1")
        std::string audioCodec;                       // "A_OPUS", "mp4a", ...
        bool audioIgnored = false;                    // an audio track of another codec
    };

    // Counters of a video source's pipeline (tooling, tests)
    struct VideoStats {
        uint64_t framesDecoded = 0;
        uint64_t framesDropped = 0;     // decoded, never shown (the main thread was ahead of them)
        uint64_t seeks = 0;
        uint64_t decodeNs = 0;          // time inside the video decoder
        uint64_t audioPackets = 0;
        size_t pictureBytes = 0;        // the decoder's picture pool
        size_t queueBytes = 0;          // decoded frames waiting
        size_t cacheBytes = 0;          // the file reader's cache
    };

    // HTMLMediaElement's MediaError codes
    enum class MediaErrorCode : int { NONE = 0, ABORTED = 1, NETWORK = 2, DECODE = 3, SRC_NOT_SUPPORTED = 4 };

    struct MediaError {
        MediaErrorCode code = MediaErrorCode::NONE;
        std::string message;
        explicit operator bool() const { return code != MediaErrorCode::NONE; }
    };

}

#endif //VIDEOTYPES_H
