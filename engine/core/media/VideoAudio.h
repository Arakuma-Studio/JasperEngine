// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEOAUDIO_H
#define VIDEOAUDIO_H

#include "Export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace jasperengine {

    // The decoded audio of a <video> between the decode worker (writer) and the mixer (reader),
    // with the media position of both ends in sample frames at 48 kHz, counted from the start of
    // the first loop iteration (docs/html-ui/phase-6-video.md §3.7). Thread-safe (a mutex).
    // - A read with nothing buffered is silence and still moves the read position: a gap or
    //   the end of the audio sounds as silence and the position stays on the media clock. Only
    //   writes (and reset) move the write position: data that arrives late, for positions already
    //   read, is dropped by write().
    // - Corrections (the sync controller): a positive one inserts that many frames of silence
    //   without consuming (audio was ahead), a negative one drops frames (audio was behind); at
    //   most MAX_CORRECTION_PER_READ frames per read, so a correction is never a jump.
    class JASPERENGINE_API VideoAudioRing {
    public:
        static constexpr int SAMPLE_RATE = 48000;
        static constexpr size_t MAX_CORRECTION_PER_READ = 480;   // 10 ms

        VideoAudioRing(int channels, size_t capacityFrames);

        int getChannels() const;
        size_t getCapacity() const;

        // writer: frames for positions from getWritePosition() on; returns the frames taken
        // (those before the read position count as taken and are dropped)
        size_t write(const float* interleaved, size_t frames);
        size_t getFreeFrames() const;
        int64_t getWritePosition() const;

        // reader: always fills `frames` interleaved sample frames
        void read(float* interleaved, size_t frames);
        int64_t getReadPosition() const;
        size_t getBufferedFrames() const;

        // empties it; both ends at `position`
        void reset(int64_t position);
        void requestCorrection(int64_t frames);
        int64_t getPendingCorrection() const;
        uint64_t getUnderrunFrames() const;    // silence read for want of data

    private:
        mutable std::mutex mutex;
        int channels;
        std::vector<float> buffer;
        size_t capacity;
        size_t head = 0;          // index (in frames) of the next frame to read
        size_t count = 0;         // frames buffered
        int64_t readPosition = 0;
        int64_t writePosition = 0;
        int64_t correction = 0;
        uint64_t underrunFrames = 0;
    };

    // When the audio has drifted from the picture: the audible position (read position less the
    // output latency) against the media clock (docs/html-ui/phase-6-video.md §3.7). Pure.
    class JASPERENGINE_API VideoAudioSync {
    public:
        static constexpr double DEAD_BAND = 0.015;   // seconds: no correction inside it
        static constexpr double RESYNC = 0.2;        // seconds: beyond it, refill from the media time

        enum class Action { NONE, CORRECT, RESYNC };
        struct Decision {
            Action action = Action::NONE;
            int64_t frames = 0;       // CORRECT: the ring correction; RESYNC: the position to restart at
            double drift = 0.0;       // seconds, audible audio minus picture (positive: audio ahead)
        };

        // mediaFrames: the media clock in sample frames; latencyFrames: what the output holds
        static Decision decide(int64_t readPosition, int64_t pendingCorrection, int64_t mediaFrames, int64_t latencyFrames);
    };

    // The SoLoud audio source of one <video>: a voice reads the shared ring (the instance holds it
    // by shared_ptr, so the mixer thread never sees a freed ring). 48 kHz, the ring's channels.
    class JASPERENGINE_INTERNAL VideoAudioSource {
    public:
        explicit VideoAudioSource(std::shared_ptr<VideoAudioRing> ring);
        ~VideoAudioSource();   // stops the voice
        VideoAudioSource(const VideoAudioSource&) = delete;
        VideoAudioSource& operator=(const VideoAudioSource&) = delete;

        // AudioSystem::init() and a paused voice; false without audio output
        bool start(float volume);
        bool isPlaying() const;    // a valid voice exists (SoLoud may have been torn down)
        void setPaused(bool paused);
        void setVolume(float volume);
        void setSpeed(float speed);
        void stop();
        // frames the output holds ahead of what is heard (SoLoud's backend buffer, at 48 kHz)
        int64_t getLatencyFrames() const;

    private:
        class Source;
        std::unique_ptr<Source> source;
        unsigned int handle = 0;
        bool paused = true;
    };

}

#endif //VIDEOAUDIO_H
