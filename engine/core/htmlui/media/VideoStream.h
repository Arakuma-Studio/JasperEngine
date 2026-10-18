// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEOSTREAM_H
#define VIDEOSTREAM_H

#include "Export.h"
#include "VideoAudio.h"
#include "VideoDecoder.h"
#include "VideoDemuxer.h"
#include "OpusAudioDecoder.h"
#include "VideoTypes.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace jasperengine {

    // The decode pipeline of one <video> (docs/html-ui/phase-6-video.md §3.6): a worker thread
    // opens and indexes the file, decodes video frames ahead into a queue of at most QUEUE_FRAMES
    // and audio ahead into a VideoAudioRing, seeks, and continues from the start after the last
    // frame while looping. Frames are numbered by a sequence, iteration x frameCount + index, so a
    // loop is one continuous stream. Without threads (Web without pthreads, or `threaded` false)
    // the same steps run inline in the calling thread: pump() and the waiting calls do the work.
    //
    // Main thread: every public method but the audio ring's reader side (the mixer's).
    class JASPERENGINE_API VideoStream {
    public:
        static constexpr size_t QUEUE_FRAMES = 4;
        static constexpr double AUDIO_AHEAD = 0.5;       // seconds decoded ahead of the read position
        static constexpr int SEEK_AHEAD_FRAMES = 8;      // a target further ahead than this seeks

        struct Info {
            bool hasVideo = false;
            bool hasAudio = false;
            int width = 0, height = 0;                    // coded
            int displayWidth = 0, displayHeight = 0;
            int frameCount = 0;
            int64_t durationNs = 0;
            double duration = 0.0;
            int audioChannels = 0;                        // of the stream (the ring has 1 or 2)
            int decoderThreads = 0;
            std::string videoCodec;
            std::string audioCodec;
            bool audioIgnored = false;                    // an audio track of another codec
        };

        struct Stats {
            uint64_t framesDecoded = 0;
            uint64_t framesDropped = 0;     // decoded, never shown (the main thread was ahead of them)
            uint64_t seeks = 0;
            uint64_t decodeNs = 0;          // time inside the video decoder
            uint64_t audioPackets = 0;
            size_t pictureBytes = 0;        // the decoder's picture pool
            size_t queueBytes = 0;          // decoded frames waiting
            size_t cacheBytes = 0;          // the file reader's cache
        };

        enum class State { OPENING, READY, FAILED };

        enum class FrameResult {
            SAME,         // the shown frame is still the one for the time
            NEW,          // `frame` holds the frame for the time (empty planes: the stream has no
                          // picture for it, the previous one stays)
            STALE,        // real time: `frame` holds the newest decoded one, older than the time's
            NOT_READY,    // nothing newer than the shown frame yet
            FAILED        // the stream failed (getError): no more frames
        };

        // threaded: a worker thread decodes (where the platform has threads); audio: decode the
        // audio track into getAudioRing()
        VideoStream(const std::string& path, bool threaded, bool audio);
        ~VideoStream();
        VideoStream(const VideoStream&) = delete;
        VideoStream& operator=(const VideoStream&) = delete;

        static bool platformHasThreads();

        State getState() const;
        // blocks until the file is open (READY) or failed; the work happens inline without a thread
        State waitOpen();
        const Info& getInfo() const;      // READY only
        MediaError getError() const;

        void setLoop(bool loop);

        // The frame for media time `seconds` of loop `iteration`. wait: block until it is decoded
        // (frame-exact mode). shownSequence: the frame on screen (-1 for none). A target the
        // decoder cannot reach by decoding on (behind the queue, or SEEK_AHEAD_FRAMES past it)
        // starts a seek. Real time: a target behind by more than half a second skips to the next
        // keyframe (frames are dropped, as a browser does).
        FrameResult acquireFrame(int iteration, double seconds, bool wait, int64_t shownSequence, VideoFrame& frame);
        // the sequence of the frame for a time (to compare with the shown one)
        int64_t sequenceFor(int iteration, double seconds) const;
        int frameIndexFor(double seconds) const;
        // the keyframe at or before the frame of `seconds`, as a time (fastSeek)
        double keyframeTimeAtOrBefore(double seconds) const;

        // the audio of media position `frames` (sample frames from the start of iteration 0)
        // follows: the ring is refilled from there (seeks, loops, a large drift)
        void syncAudio(int64_t frames);
        // a sync the worker has not done yet (the ring still holds what was before it)
        bool isAudioSyncPending() const;
        // waits until it is done (frame-exact mode: decisions on the new positions only)
        void waitAudioSync();
        std::shared_ptr<VideoAudioRing> getAudioRing() const;

        // inline mode: up to `steps` units of work (opening, one frame, one audio packet); returns
        // whether there is more to do. A threaded stream does nothing here
        bool pump(int steps);

        Stats getStats() const;
        // the decoded queue, for tests: sequences waiting
        size_t getQueuedFrames() const;

        // tests: every decode takes at least this long (real-time mode: stalls, dropped frames)
        static void setDecodeDelayForTesting(int milliseconds);
        // streams alive (leak tests)
        static int getLiveCount();

    private:
        enum class Work { NONE, DONE, BLOCKED };

        void threadMain();
        // one unit of work; NONE: nothing to do now
        Work step();
        bool openNow();
        Work decodeVideo(std::unique_lock<std::mutex>& lock);
        Work decodeAudio(std::unique_lock<std::mutex>& lock);
        void handleSeek(std::unique_lock<std::mutex>& lock);
        void handleAudioSync(std::unique_lock<std::mutex>& lock);
        void requestSeekLocked(int64_t targetSequence);
        bool hasWorkLocked() const;
        void failLocked(MediaErrorCode code, const std::string& message);
        int64_t audioFramesPerIteration() const;

        std::string path;
        bool threaded;
        bool wantAudio;

        // worker-owned
        VideoDemuxer demuxer;
        std::unique_ptr<VideoDecoder> decoder;
        std::unique_ptr<OpusAudioDecoder> audioDecoder;
        std::vector<uint8_t> packetBuffer;
        std::vector<float> pcm;
        int decodeIndex = 0;            // the next video frame to decode
        int decodeIteration = 0;
        int64_t discardBelow = 0;       // sequences below it are decoded but not queued (seeks)
        int audioIndex = 0;             // the next audio packet
        int audioIteration = 0;
        bool audioRestart = true;       // the next packet is placed by its timestamp (start, syncs)
        bool flushPending = false;      // the decoder is flushed before the next frame (seeks, loops)

        // shared, under mutex
        mutable std::mutex mutex;
        std::condition_variable workerWake;
        std::condition_variable frameReady;
        std::atomic<State> state{State::OPENING};
        std::atomic<bool> opened{false};    // the index and info are there (also after a later FAILED)
        Info info;
        MediaError error;
        std::deque<VideoFrame> queue;
        int64_t nextSequence = 0;       // the sequence decodeVideo makes next
        bool videoEnd = false;          // the last frame was decoded and the stream does not loop
        bool loop = false;
        int64_t pendingSeek = -1;       // a seek to this sequence waits for the worker
        uint64_t seekGeneration = 0;
        int64_t pendingAudioSync = -1;
        uint64_t audioGeneration = 0;
        bool audioEnd = false;
        bool quit = false;
        std::shared_ptr<VideoAudioRing> ring;
        Stats stats;
        std::thread worker;
    };

}

#endif //VIDEOSTREAM_H
