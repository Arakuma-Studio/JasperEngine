// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LOTTIEPLAYBACK_H
#define LOTTIEPLAYBACK_H

#include "Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    // The playback state of one <lottie> element: which frame shows at a given UI-clock time,
    // with speed, direction, bounce, loop count, segment and markers. Pure (no RmlUi, no
    // ThorVG): the element feeds it HtmlUIClock::now() and renders the frame it returns.
    //
    // Frames follow lottie-web: an animation of N frames shows frames 0 .. N-1 for 1/fps
    // seconds each, so one pass lasts N / fps seconds. A segment [start, end) shows frames
    // start .. end-1. A pass forward goes start -> end-1; direction -1 goes end-1 -> start.
    // In bounce mode one iteration is a pass and its way back (2 x the segment length).
    // Loop counts are iterations in total: loop count 3 plays three times, with two `loop`
    // events and one `complete`; an infinite loop never completes.
    class JASPERENGINE_API LottiePlayback {
    public:
        enum class Mode : uint8_t { NORMAL, BOUNCE };
        enum class Event : uint8_t { PLAY, PAUSE, COMPLETE, LOOP };

        struct Marker {
            std::string name;
            float start;      // first frame
            float end;        // one past the last frame
        };

        // A new animation: totalFrames > 0 and fps > 0 (else the playback stays empty).
        // Stops, resets the segment to the whole animation and forgets the markers.
        void setAnimation(float totalFrames, float frameRate);
        bool hasAnimation() const;

        // settings; they keep the frame on screen where they can
        void setSpeed(float speed);                 // > 0; anything else is ignored (false)
        float getSpeed() const;
        void setDirection(int direction);           // 1 or -1; anything else is ignored
        int getDirection() const;
        void setMode(Mode mode);
        Mode getMode() const;
        void setLoop(bool loop);                    // true: forever; false: once
        void setLoopCount(int count);               // iterations in total; <= 0: forever
        int getLoopCount() const;                   // 0 = forever
        // [start, end) clamped to the animation; start == end shows one frame; reversed
        // bounds are swapped. Goes to the first frame of the segment (playing state kept).
        void setSegment(float start, float end);
        void clearSegment();                        // the whole animation
        float getSegmentStart() const;
        float getSegmentEnd() const;

        void setMarkers(std::vector<Marker> markers);
        const std::vector<Marker>& getMarkers() const;
        // the marker's segment; false (segment unchanged) for an unknown name
        bool setMarker(const std::string& name);
        const Marker* findMarker(const std::string& name) const;

        // control; events are appended to `events` (PLAY when it starts, PAUSE when a playing
        // animation pauses or stops)
        void play(double now, std::vector<Event>& events);     // after `complete`: from the start
        void pause(double now, std::vector<Event>& events);
        void stop(double now, std::vector<Event>& events);     // paused at the first frame
        void seek(double seconds, double now);                 // time into the segment
        void setFrame(float frame, double now);                // absolute frame, clamped to the segment

        // Advances to `now` (seconds of UI time) and returns the frame to show; appends LOOP
        // for every wrap and COMPLETE at the end of the last iteration
        float update(double now, std::vector<Event>& events);

        bool isPlaying() const;
        bool isComplete() const;
        float getCurrentFrame() const;   // the frame shown (an integer, absolute)
        float getTotalFrames() const;
        float getFrameRate() const;
        float getDuration() const;       // seconds of the whole animation
        int getIterations() const;       // completed iterations since play/stop/seek
        // seconds until the shown frame changes while playing (0 = not playing)
        double getTimeToNextFrame() const;

    private:
        float segmentLength() const;     // frames of one pass
        float cycleLength() const;       // frames of one iteration
        float frameAt(double position) const;
        void syncFrame();
        void advance(double now, std::vector<Event>& events);
        void finish(std::vector<Event>& events, int loops);

        float totalFrames = 0.0f;
        float frameRate = 0.0f;
        float speed = 1.0f;
        int direction = 1;
        Mode mode = Mode::NORMAL;
        int loopCount = 1;               // 0 = forever
        float segmentStart = 0.0f;
        float segmentEnd = 0.0f;
        std::vector<Marker> markers;

        bool playing = false;
        bool complete = false;
        double position = 0.0;           // frames into the current iteration [0, cycleLength]
        int iterations = 0;
        double lastTime = 0.0;
        float currentFrame = 0.0f;
    };

}

#endif //LOTTIEPLAYBACK_H
