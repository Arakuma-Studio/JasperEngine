// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PLATFORMVIDEOSOURCE_H
#define PLATFORMVIDEOSOURCE_H

#include "Export.h"
#include "IVideoSource.h"
#include "MediaByteSource.h"
#include "Mp4Probe.h"
#include "VideoAudio.h"
#include "VideoTypes.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace jasperengine {

    // One OS decoder of one stream (docs/superpowers/specs/2026-10-31-video-mp4-design.md §6.3):
    // FFmpeg (Linux), MediaCodec (Android), AVFoundation (Apple), Media Foundation (Windows). A
    // PlatformVideoSource opens one for the video and another for the audio of the same bytes,
    // so each stream seeks on its own (audio resyncs never disturb the picture). Pull model, the
    // source's worker thread only: decodeNext() is called until it gives a frame, an audio chunk,
    // or the end; seek() goes back to a keyframe and the source decodes forward from there.
    class JASPERENGINE_API PlatformDecoderBackend {
    public:
        enum class Stream { VIDEO, AUDIO };
        enum class Step {
            FRAME,      // video: `frame` holds a picture (I420 8-bit planes, ptsNs, colour)
            AUDIO,      // audio: `pcm` holds 48 kHz interleaved stereo float from `pcmStart` seconds
            AGAIN,      // input was consumed, nothing came out yet
            END,        // no more (until a seek)
            FAILURE     // `error` says why
        };

        virtual ~PlatformDecoderBackend() = default;

        // The first video (or audio) track of `source`. VIDEO fills the video fields of `info`
        // (coded and display size, rotation, frameRate, duration, videoCodec as a fourcc or
        // Matroska id) and AUDIO the audio ones (audioChannels, audioCodec, duration). False with
        // `error`: SRC_NOT_SUPPORTED (no decoder for it: the message names the codec and the fix),
        // DECODE (the decoder failed), NETWORK (the bytes could not be read)
        virtual bool open(MediaByteSource& source, Stream stream, VideoInfo& info, MediaError& error) = 0;
        // the keyframe at or before `seconds` (video) or about `seconds` (audio: chunks carry
        // their start, earlier samples are dropped by the source)
        virtual bool seek(double seconds) = 0;
        virtual Step decodeNext(VideoFrame& frame, std::vector<float>& pcm, double& pcmStart, MediaError& error) = 0;
        virtual void close() = 0;
        // the open failed because the device's decoder instances are all in use (Android caps
        // them): the source retries once after 100 ms
        virtual bool wasResourceLimited() const { return false; }
        virtual int getDecoderThreads() const { return 0; }
    };

    using PlatformBackendFactory = std::function<std::unique_ptr<PlatformDecoderBackend>()>;

    // Plane copies for the backends: decoders give NV12, I420, P010 with strides; the engine
    // keeps tightly packed 8-bit I420 (VideoFrame), so the Y/U/V upload and the Yuv pass of the
    // WebM path draw them unchanged (§5.4).
    namespace VideoFramePlanes {
        // frame.width/height/chroma shifts set to 4:2:0 of width x height
        JASPERENGINE_API void prepare(VideoFrame& frame, int width, int height);
        JASPERENGINE_API void copyI420(VideoFrame& frame, int width, int height, const uint8_t* y, int yStride,
                                       const uint8_t* u, int uStride, const uint8_t* v, int vStride);
        // semi-planar: UV interleaved (vu: VU, NV21)
        JASPERENGINE_API void copyNV12(VideoFrame& frame, int width, int height, const uint8_t* y, int yStride,
                                       const uint8_t* uv, int uvStride, bool vu = false);
        // 16-bit samples: `bits` significant ones at the top (P010: msb) or the bottom (yuv420p10le)
        JASPERENGINE_API void copyI420x16(VideoFrame& frame, int width, int height, int bits, bool msb, const uint8_t* y, int yStride,
                                          const uint8_t* u, int uStride, const uint8_t* v, int vStride);
        JASPERENGINE_API void copyP01x(VideoFrame& frame, int width, int height, int bits, const uint8_t* y, int yStride,
                                       const uint8_t* uv, int uvStride);
        // turns the planes clockwise by 90, 180 or 270 degrees (rotated phone videos)
        JASPERENGINE_API void rotate(VideoFrame& frame, int degrees);
    }

    // PCM to 48 kHz interleaved stereo float for the backends whose decoders give other rates
    // or formats (MediaCodec, Media Foundation): linear interpolation, continuous across calls
    class JASPERENGINE_API AudioResampler48k {
    public:
        void reset(int inputRate, int inputChannels);
        void process(const float* interleaved, size_t frames, std::vector<float>& out);
        void processInt16(const int16_t* interleaved, size_t frames, std::vector<float>& out);
        // forgets the carried sample (after a seek)
        void flush();
        int getInputRate() const { return rate; }
        int getInputChannels() const { return channels; }

    private:
        int rate = 48000;
        int channels = 2;
        double position = 0.0;          // in input frames, relative to the next chunk's first
        float last[2] = {0.0f, 0.0f};   // the last input frame of the previous chunk (stereo)
        bool hasLast = false;
        std::vector<float> stereo;
    };

    // The IVideoSource of the platform decoders (§6.3): one worker thread runs a video and an
    // audio PlatformDecoderBackend over the same bytes, with VideoStream's queue of decoded frames,
    // seeks, loops and audio ring, so the players cannot tell it from the WebM path. The frame
    // index is the file's own (an MP4's sample tables: exact frame numbers, keyframes) or, where
    // there is none, an estimate from the frame rate: floor(t x frameRate), frameCount =
    // round(duration x frameRate), as Unity reports.
    class JASPERENGINE_API PlatformVideoSource : public IVideoSource {
    public:
        static constexpr size_t QUEUE_FRAMES = 4;
        static constexpr double AUDIO_AHEAD = 0.5;
        static constexpr int SEEK_AHEAD_FRAMES = 8;

        // bytes: the file (shared by both decoders); probe: its MP4 probe when it is one (the
        // frame index is built from it on the worker); name: the backendName()
        PlatformVideoSource(std::shared_ptr<MediaByteSource> bytes, const Mp4ProbeResult* probe, PlatformBackendFactory factory,
                            const char* name, bool audio);
        ~PlatformVideoSource() override;
        PlatformVideoSource(const PlatformVideoSource&) = delete;
        PlatformVideoSource& operator=(const PlatformVideoSource&) = delete;

        State getState() const override;
        State waitOpen() override;
        const VideoInfo& getInfo() const override;
        MediaError getError() const override;
        void setLoop(bool loop) override;
        FrameResult acquireFrame(int iteration, double seconds, bool wait, int64_t shownSequence, VideoFrame& frame) override;
        int64_t sequenceFor(int iteration, double seconds) const override;
        int frameIndexFor(double seconds) const override;
        double keyframeTimeAtOrBefore(double seconds) const override;
        void syncAudio(int64_t frames) override;
        bool isAudioSyncPending() const override;
        void waitAudioSync() override;
        std::shared_ptr<VideoAudioRing> getAudioRing() const override;
        bool pump(int steps) override;
        VideoStats getStats() const override;
        double getBufferedSeconds() const override;
        bool isFrameExact() const override;
        const char* backendName() const override;

        // the frame index is the file's (true) or estimated from the frame rate
        bool hasExactIndex() const;
        size_t getQueuedFrames() const;
        static int getLiveCount();

    private:
        // NONE: nothing to do (wait for work); DONE: progress; IDLE: the decoder had nothing yet
        // (back off a little instead of spinning)
        enum class Work { NONE, DONE, IDLE };

        void threadMain();
        Work step();
        void openNow();
        Work decodeVideo(std::unique_lock<std::mutex>& lock);
        Work decodeAudio(std::unique_lock<std::mutex>& lock);
        void handleSeek(std::unique_lock<std::mutex>& lock);
        void handleAudioSync(std::unique_lock<std::mutex>& lock);
        void requestSeekLocked(int64_t targetSequence);
        bool hasWorkLocked() const;
        void failLocked(MediaErrorCode code, const std::string& message);
        bool openBackend(PlatformDecoderBackend::Stream stream, std::unique_ptr<PlatformDecoderBackend>& out, VideoInfo& found, MediaError& error);
        void pushFrame(VideoFrame& frame, int64_t sequence);
        bool restartLocked(std::unique_lock<std::mutex>& lock, uint64_t generation);

        // index (set once at open, read by both threads afterwards)
        int indexOf(int64_t ptsNs) const;
        int64_t ptsOf(int index) const;
        int keyframeAtOrBefore(int index) const;
        int keyframeAfter(int index) const;
        int64_t audioFramesPerIteration() const;

        std::shared_ptr<MediaByteSource> bytes;
        Mp4ProbeResult probe;
        bool hasProbe = false;
        PlatformBackendFactory factory;
        const char* name;
        bool wantAudio;

        // worker-owned
        std::unique_ptr<PlatformDecoderBackend> video;
        std::unique_ptr<PlatformDecoderBackend> audio;
        std::vector<float> pcm;
        int decodeIteration = 0;
        int64_t discardBelow = 0;
        int64_t lastPushed = -1;            // the newest sequence queued (or discarded) since the last seek
        int64_t ptsBias = 0;                // the decoder's first pts less the index's
        int audioIteration = 0;
        bool audioRestart = true;
        bool queuedSinceSeek = true;        // a frame was queued since the last seek
        VideoFrame lastDiscarded;           // the last frame decoded on the way to a seek target
        bool restartPending = false;        // loop switched on after the end: the next iteration starts

        // the index
        Mp4VideoIndex index;
        bool exactIndex = false;

        // shared, under mutex
        mutable std::mutex mutex;
        std::condition_variable workerWake;
        std::condition_variable frameReady;
        std::atomic<State> state{State::OPENING};
        std::atomic<bool> opened{false};
        bool lateWarned = false;
        VideoInfo info;
        MediaError error;
        std::deque<VideoFrame> queue;
        int64_t nextSequence = 0;
        bool videoEnd = false;
        bool loop = false;
        int64_t pendingSeek = -1;
        uint64_t seekGeneration = 0;
        int64_t pendingAudioSync = -1;
        uint64_t audioGeneration = 0;
        bool audioEnd = false;
        bool quit = false;
        std::shared_ptr<VideoAudioRing> ring;
        VideoStats stats;
        std::thread worker;
    };

}

#endif //PLATFORMVIDEOSOURCE_H
