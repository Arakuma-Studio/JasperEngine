// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEODEMUXER_H
#define VIDEODEMUXER_H

#include "Export.h"
#include "VideoTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mkvparser {
    class Segment;
}

namespace jasperengine {

    class FileData;

    struct VideoTrackInfo {
        std::string codecId;            // Matroska codec id ("V_AV1")
        int width = 0;                  // coded size
        int height = 0;
        int displayWidth = 0;           // the size to show (DisplayWidth/Height in pixels, else the coded size)
        int displayHeight = 0;
        int64_t defaultDurationNs = 0;  // 0: not signalled
        std::vector<uint8_t> codecPrivate;
        VideoColor color;               // the container's Colour element (the bitstream's wins when set)
    };

    struct AudioTrackInfo {
        std::string codecId;            // "A_OPUS"
        int channels = 0;
        double sampleRate = 0.0;
        int64_t codecDelayNs = 0;
        int64_t seekPreRollNs = 0;
        std::vector<uint8_t> codecPrivate;
    };

    // One frame of a track: where its bytes are and when it shows
    struct VideoPacketRef {
        int64_t offset = 0;
        uint32_t size = 0;
        int64_t ptsNs = 0;
        int64_t discardPaddingNs = 0;   // audio: samples to drop at the end (the last packet)
        bool key = false;
    };

    // The WebM/Matroska reader of <video> (docs/html-ui/phase-6-video.md §3.6.2): libwebm's
    // mkvparser over a FileData handle (files, the resource pack, Android assets, the Web file
    // system) with a block cache. open() parses the headers and every cluster's block headers into
    // a frame index of the first AV1 video track and the first Opus audio track; the compressed
    // data stays in the file and is read per frame. One thread at a time.
    class JASPERENGINE_API VideoDemuxer {
    public:
        VideoDemuxer();
        ~VideoDemuxer();
        VideoDemuxer(const VideoDemuxer&) = delete;
        VideoDemuxer& operator=(const VideoDemuxer&) = delete;

        // false with error (and errorCode) set. headersOnly: no cluster is read (probe); the
        // index stays empty and the duration is the segment's (0 when it has none)
        bool open(const std::string& path, bool headersOnly = false);
        void close();

        const MediaError& getError() const;
        // the EBML DocType: "webm" or "matroska"
        const std::string& getDocType() const;
        // the tracks found (whatever their codec; hasVideo/hasAudio: the ones the engine plays)
        const std::string& getVideoCodecId() const;
        const std::string& getAudioCodecId() const;
        bool hasVideo() const;
        bool hasAudio() const;
        const VideoTrackInfo& getVideoTrack() const;
        const AudioTrackInfo& getAudioTrack() const;

        const std::vector<VideoPacketRef>& getVideoPackets() const;
        const std::vector<VideoPacketRef>& getAudioPackets() const;
        // seconds: the segment's Duration, else the end of the last frame of either track
        double getDuration() const;
        int64_t getDurationNs() const;

        // the last video frame whose pts <= ptsNs (0 before the first); -1 without frames
        int videoFrameAt(int64_t ptsNs) const;
        // the keyframe at or before frame `index`
        int keyframeAtOrBefore(int index) const;
        // the first keyframe after frame `index` (-1 if none)
        int keyframeAfter(int index) const;
        // the last audio packet whose pts <= ptsNs (0 before the first); -1 without packets
        int audioPacketAt(int64_t ptsNs) const;

        // a frame's bytes; false (with a NETWORK error) when the file cannot be read any more
        bool read(const VideoPacketRef& packet, std::vector<uint8_t>& out);

        // bytes the reader's cache holds
        size_t getCacheBytes() const;

    private:
        class Reader;
        bool fail(MediaErrorCode code, const std::string& message);
        bool buildIndex();

        std::unique_ptr<Reader> reader;
        std::unique_ptr<mkvparser::Segment> segment;
        MediaError error;
        std::string path;
        std::string docType;
        std::string videoCodecId;
        std::string audioCodecId;
        long videoTrackNumber = -1;
        long audioTrackNumber = -1;
        VideoTrackInfo videoTrack;
        AudioTrackInfo audioTrack;
        std::vector<VideoPacketRef> videoPackets;
        std::vector<VideoPacketRef> audioPackets;
        int64_t durationNs = 0;
    };

}

#endif //VIDEODEMUXER_H
