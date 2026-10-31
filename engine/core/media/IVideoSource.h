// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef IVIDEOSOURCE_H
#define IVIDEOSOURCE_H

#include "Export.h"
#include "VideoTypes.h"

#include <cstdint>
#include <memory>

namespace jasperengine {

    class Framebuffer;
    class VideoAudioRing;

    // One opened video for a player (the HTML UI <video>, the Video Player component): the pull
    // pipeline of docs/html-ui/phase-6-video.md §3.6 behind one interface
    // (docs/superpowers/specs/2026-10-31-video-mp4-design.md §6.2). Implemented by VideoStream
    // (WebM AV1/Opus in software, every platform, frame-exact), PlatformVideoSource (each OS's own
    // decoders: FFmpeg on Linux, MediaCodec, AVFoundation, Media Foundation) and, on the Web,
    // WebVideoSource (the browser's <video>, which decodes and plays its own audio).
    //
    // The player owns the clock: every update it asks for the frame of its media time (loop
    // `iteration`, `seconds` into the media) and keeps the audio ring in sync with it. Frames are
    // numbered by a sequence, iteration x frameCount + index, so a loop is one continuous stream.
    // VideoSourceFactory picks the implementation by the file's content.
    //
    // Main thread: every method but the audio ring's reader side (the mixer's).
    class JASPERENGINE_API IVideoSource {
    public:
        // BUFFERING: opened from a file still downloading, waiting for its bytes (progressive start)
        enum class State { OPENING, BUFFERING, READY, FAILED };

        enum class FrameResult {
            SAME,         // the shown frame is still the one for the time
            NEW,          // `frame` holds the frame for the time (empty planes: the stream has no
                          // picture for it, the previous one stays; rendersToTexture sources:
                          // always empty, uploadTo draws it)
            STALE,        // real time: `frame` holds the newest decoded one, older than the time's
            NOT_READY,    // nothing newer than the shown frame yet
            FAILED        // the stream failed (getError): no more frames
        };

        virtual ~IVideoSource() = default;

        virtual State getState() const = 0;
        // blocks until the file is open (READY) or failed (a bounded wait: OPENING or BUFFERING
        // after it); the work happens inline without a worker thread
        virtual State waitOpen() = 0;
        virtual const VideoInfo& getInfo() const = 0;      // READY only
        virtual MediaError getError() const = 0;

        virtual void setLoop(bool loop) = 0;

        // The frame for media time `seconds` of loop `iteration`. wait: block until it is decoded
        // (frame-exact mode). shownSequence: the frame on screen (-1 for none). A target the
        // decoder cannot reach by decoding on starts a seek; real time never waits (frames late
        // by more than half a second are skipped to the next keyframe).
        virtual FrameResult acquireFrame(int iteration, double seconds, bool wait, int64_t shownSequence, VideoFrame& frame) = 0;
        // the sequence of the frame for a time (to compare with the shown one); -1 before open
        virtual int64_t sequenceFor(int iteration, double seconds) const = 0;
        virtual int frameIndexFor(double seconds) const = 0;
        // the keyframe at or before the frame of `seconds`, as a time (fastSeek)
        virtual double keyframeTimeAtOrBefore(double seconds) const = 0;

        // the audio of media position `frames` (sample frames at 48 kHz from the start of
        // iteration 0) follows: the ring is refilled from there (seeks, loops, a large drift)
        virtual void syncAudio(int64_t frames) = 0;
        virtual bool isAudioSyncPending() const = 0;
        virtual void waitAudioSync() = 0;
        // the decoded audio (48 kHz) the player's voice reads; null without audio or for
        // sources that play their own (playsOwnAudio)
        virtual std::shared_ptr<VideoAudioRing> getAudioRing() const = 0;

        // inline mode: up to `steps` units of work; returns whether there is more to do. A
        // threaded source does nothing here
        virtual bool pump(int steps) = 0;

        virtual VideoStats getStats() const = 0;
        // seconds of media decodable ahead of the start: the whole duration for complete files,
        // what has been downloaded of a growing one (progressive start)
        virtual double getBufferedSeconds() const = 0;
        // 0..1 of the bytes needed before it can open (a URL's download; 1 for local files),
        // -1 while the size is unknown
        virtual double getDownloadProgress() const { return 1.0; }
        // every frame of a time is exactly the one the index says (false: the browser's)
        virtual bool isFrameExact() const = 0;
        // the source draws its frames itself (Web: the browser's element straight into an RGBA
        // target with texSubImage2D, no YUV planes)
        virtual bool rendersToTexture() const { return false; }
        // draws the current frame into `target` (rendersToTexture sources); false when it could not
        virtual bool uploadTo(Framebuffer& target) { (void)target; return false; }
        // sources that decode and play their own audio (Web): the player's volume (0..1, mute
        // and global volume applied) and its clock, which they follow
        virtual bool playsOwnAudio() const { return false; }
        virtual void setOutputVolume(float volume) { (void)volume; }
        virtual void setClock(bool advancing, double rate) { (void)advancing; (void)rate; }
        // "webm", "ffmpeg", "mediacodec", "avfoundation", "mediafoundation", "browser"
        virtual const char* backendName() const = 0;
    };

}

#endif //IVIDEOSOURCE_H
