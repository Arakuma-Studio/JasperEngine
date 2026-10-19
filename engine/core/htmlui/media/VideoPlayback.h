// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VIDEOPLAYBACK_H
#define VIDEOPLAYBACK_H

#include "Export.h"

#include <cstdint>
#include <vector>

namespace jasperengine {

    // The media clock of one <video> (docs/html-ui/phase-6-video.md §3.5): where playback is at a
    // given UI-clock time, with HTMLMediaElement's paused/ended/seeking states, playbackRate, loop and
    // the events they fire. Pure (no RmlUi, no decoding, no threads): the element feeds it
    // HtmlUIClock::now() and shows the frame of getPosition().
    //
    // While playing (not paused, not ended, not seeking, not stalled) the position advances by
    // rate x (now - last update). At the duration it wraps (loop: SEEKING, TIMEUPDATE, SEEKED, and
    // the iteration counts up) or ends (TIMEUPDATE, PAUSE, ENDED: paused and ended become true).
    // TIMEUPDATE fires at most every 0.25 s of UI time while the position moves (HTML: 15-250 ms),
    // from update(); the control calls fire their own (pause: TIMEUPDATE, PAUSE).
    // A seek fires SEEKING at once and TIMEUPDATE, SEEKED when the element calls completeSeek (the
    // frame is shown). play() fires PLAY, and PLAYING once the element calls notifyReady.
    class JASPERENGINE_API VideoPlayback {
    public:
        enum class Event : uint8_t { PLAY, PLAYING, PAUSE, WAITING, SEEKING, SEEKED, TIMEUPDATE, ENDED, RATECHANGE };

        static constexpr double TIMEUPDATE_INTERVAL = 0.25;
        static constexpr double MIN_RATE = 0.0625;
        static constexpr double MAX_RATE = 16.0;

        // a new resource: position 0, paused, no duration; the playback rate goes back to the
        // default one (HTML's load algorithm). loop and the default rate are kept
        void reset();
        // the metadata: seconds > 0 (else the media has no duration and nothing plays)
        void setDuration(double seconds);
        bool hasDuration() const;
        double getDuration() const;       // NaN before setDuration

        void setLoop(bool loop);
        bool getLoop() const;

        // 0, or [MIN_RATE, MAX_RATE]: anything else is refused (false). RATECHANGE when it changes
        static bool isValidRate(double rate);
        bool setPlaybackRate(double rate, double now, std::vector<Event>& events);
        double getPlaybackRate() const;
        bool setDefaultPlaybackRate(double rate, std::vector<Event>& events);
        double getDefaultPlaybackRate() const;

        // control (HTML's play() and pause()); events are appended
        void play(double now, std::vector<Event>& events);
        void pause(double now, std::vector<Event>& events);
        // Seeks to `seconds`, clamped to [0, duration] (before the metadata: kept as the start
        // position); SEEKING now, the rest at completeSeek
        void seek(double seconds, double now, std::vector<Event>& events);
        void completeSeek(double now, std::vector<Event>& events);
        // the frame for the position is shown: PLAYING after play() or a stall, once
        void notifyReady(double now, std::vector<Event>& events);
        // real time without the frame for the position: the clock stops (WAITING once, while
        // playing) until it is false again (then notifyReady gives PLAYING)
        void setStalled(bool stalled, double now, std::vector<Event>& events);

        // advances to `now`
        void update(double now, std::vector<Event>& events);

        double getPosition() const;       // seconds into the media [0, duration]
        int getIteration() const;         // loops completed since the last reset or seek
        bool isPaused() const;
        bool isEnded() const;             // HTML: at the end, not looping
        bool isSeeking() const;
        bool isStalled() const;
        // the clock moves (playing, not ended, not seeking, not stalled, rate > 0)
        bool isAdvancing() const;

    private:
        // periodic: TIMEUPDATE at its cadence (update() only; control calls fire their own events)
        void advance(double now, std::vector<Event>& events, bool periodic = false);

        double duration = -1.0;
        double position = 0.0;
        double rate = 1.0;
        double defaultRate = 1.0;
        bool loop = false;
        bool paused = true;
        bool ended = false;
        bool seeking = false;
        bool stalled = false;
        bool playingPending = false;      // PLAYING waits for notifyReady
        int iteration = 0;
        double lastTime = 0.0;
        double lastTimeupdate = 0.0;
        double lastTimeupdatePosition = 0.0;
    };

}

#endif //VIDEOPLAYBACK_H
