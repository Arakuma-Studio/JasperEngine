// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include "Export.h"
#include "VideoDemuxer.h"
#include "VideoTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct Dav1dContext;

namespace jasperengine {

    // One compressed video stream in, frames out (docs/html-ui/phase-6-video.md §3.6.1). Used by
    // one thread at a time (VideoStream's worker). A platform decoder would implement the same
    // interface; the engine has the software one, Av1Decoder.
    class JASPERENGINE_API VideoDecoder {
    public:
        virtual ~VideoDecoder() = default;

        virtual bool open(const VideoTrackInfo& track, std::string& error) = 0;
        // One frame's data (a WebM block). gotFrame tells whether a picture came out (one per
        // shown frame); false with error on corrupt data.
        virtual bool decode(const uint8_t* data, size_t size, int64_t ptsNs, VideoFrame& frame, bool& gotFrame, std::string& error) = 0;
        // forgets every reference: the next frame must be a keyframe (seeks)
        virtual void flush() = 0;
        // bytes of the decoder's picture buffers (pooled ones included)
        virtual size_t getPictureBytes() const = 0;
    };

    // AV1 on dav1d (C build, both bit depths): n_threads by the frame size (1 up to 640x360,
    // 2 up to 1280x720, else 4), max_frame_delay 1, and a picture allocator that pools and counts
    // its buffers. Pictures are converted to packed 8-bit planes.
    class JASPERENGINE_API Av1Decoder : public VideoDecoder {
    public:
        Av1Decoder();
        ~Av1Decoder() override;

        bool open(const VideoTrackInfo& track, std::string& error) override;
        bool decode(const uint8_t* data, size_t size, int64_t ptsNs, VideoFrame& frame, bool& gotFrame, std::string& error) override;
        void flush() override;
        size_t getPictureBytes() const override;

        int getThreadCount() const;
        // forced thread count for tests (0: by the frame size); applies to the next open()
        static void setThreadCountForTesting(int threads);
        // picture bytes of every Av1Decoder alive (leak tests)
        static size_t getLivePictureBytes();

        // the pool behind dav1d's picture callbacks (public for the C callbacks)
        struct PicturePool {
            std::mutex mutex;
            std::vector<std::pair<size_t, uint8_t*>> free;   // (size, buffer)
            std::atomic<size_t> bytes{0};                    // allocated, in use or free
            ~PicturePool();
        };

    private:
        void close();
        bool takePicture(VideoFrame& frame, bool& gotFrame, std::string& error);

        Dav1dContext* context = nullptr;
        std::unique_ptr<PicturePool> pool;
        int threads = 1;
        VideoColor containerColor;
    };

}

#endif //VIDEODECODER_H
