// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// LottiePlayback (p1-vector): the pure state machine behind <lottie>. Frames for a given
// time, speed, direction, bounce, loop count and segment; marker resolution; the play,
// pause, loop and complete events.

#include "vector/LottiePlayback.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace jasperengine;

namespace {

    using Event = LottiePlayback::Event;

    // 60 frames at 30 fps: 2 s per pass, frame n shows during [n / 30, (n + 1) / 30)
    LottiePlayback make(){
        LottiePlayback playback;
        playback.setAnimation(60.0f, 30.0f);
        return playback;
    }

    size_t count(const std::vector<Event>& events, Event type){
        return (size_t)std::count(events.begin(), events.end(), type);
    }

    float frameAt(LottiePlayback& playback, double time){
        std::vector<Event> events;
        return playback.update(time, events);
    }

}

TEST(LottiePlaybackTest, EmptyAnimation){
    LottiePlayback playback;
    EXPECT_FALSE(playback.hasAnimation());
    playback.setAnimation(0.0f, 30.0f);
    EXPECT_FALSE(playback.hasAnimation());
    playback.setAnimation(60.0f, -1.0f);
    EXPECT_FALSE(playback.hasAnimation());
    std::vector<Event> events;
    EXPECT_EQ(playback.update(1.0, events), 0.0f);
    EXPECT_EQ(playback.getDuration(), 0.0f);
}

TEST(LottiePlaybackTest, StartsPausedAtTheFirstFrame){
    LottiePlayback playback = make();
    EXPECT_TRUE(playback.hasAnimation());
    EXPECT_FALSE(playback.isPlaying());
    EXPECT_EQ(playback.getCurrentFrame(), 0.0f);
    EXPECT_EQ(frameAt(playback, 5.0), 0.0f);
    EXPECT_FLOAT_EQ(playback.getDuration(), 2.0f);
    EXPECT_FLOAT_EQ(playback.getTotalFrames(), 60.0f);
    EXPECT_FLOAT_EQ(playback.getFrameRate(), 30.0f);
    EXPECT_EQ(playback.getTimeToNextFrame(), 0.0);
}

TEST(LottiePlaybackTest, FrameForTime){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.play(10.0, events);
    ASSERT_EQ(events, std::vector<Event>{Event::PLAY});

    EXPECT_EQ(frameAt(playback, 10.0), 0.0f);
    EXPECT_EQ(frameAt(playback, 10.5), 15.0f);
    EXPECT_EQ(frameAt(playback, 11.0), 30.0f);
    EXPECT_EQ(frameAt(playback, 11.99), 59.0f);
    // the next frame boundary: 1/30 s after 11.99 is past 12, the cycle end
    EXPECT_NEAR(playback.getTimeToNextFrame(), 0.01, 1e-6);
}

TEST(LottiePlaybackTest, Speed){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.setSpeed(2.0f);
    playback.play(0.0, events);
    EXPECT_EQ(frameAt(playback, 0.25), 15.0f);
    EXPECT_EQ(frameAt(playback, 0.5), 30.0f);
    EXPECT_NEAR(playback.getTimeToNextFrame(), 1.0 / 60.0, 1e-9);
    // a change takes effect from the last update
    playback.setSpeed(0.5f);
    EXPECT_EQ(frameAt(playback, 1.5), 45.0f);
    // invalid speeds are ignored
    playback.setSpeed(0.0f);
    playback.setSpeed(-1.0f);
    EXPECT_FLOAT_EQ(playback.getSpeed(), 0.5f);
}

TEST(LottiePlaybackTest, Direction){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.setDirection(-1);
    EXPECT_EQ(playback.getCurrentFrame(), 59.0f);
    playback.play(0.0, events);
    EXPECT_EQ(frameAt(playback, 0.0), 59.0f);
    EXPECT_EQ(frameAt(playback, 0.5), 44.0f);
    EXPECT_EQ(frameAt(playback, 1.99), 0.0f);
    // flipping keeps the frame and reverses the motion
    LottiePlayback flip = make();
    flip.play(0.0, events);
    EXPECT_EQ(frameAt(flip, 0.5), 15.0f);
    flip.setDirection(-1);
    EXPECT_EQ(flip.getCurrentFrame(), 15.0f);
    EXPECT_EQ(frameAt(flip, 0.6), 12.0f);
    // invalid values are ignored
    flip.setDirection(0);
    flip.setDirection(2);
    EXPECT_EQ(flip.getDirection(), -1);
}

TEST(LottiePlaybackTest, Bounce){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.setMode(LottiePlayback::Mode::BOUNCE);
    playback.setLoop(true);
    playback.play(0.0, events);
    EXPECT_EQ(frameAt(playback, 1.0), 30.0f);
    EXPECT_EQ(frameAt(playback, 1.99), 59.0f);
    // on the way back
    EXPECT_EQ(frameAt(playback, 2.5), 44.0f);
    EXPECT_EQ(frameAt(playback, 3.99), 0.0f);
    // one iteration is there and back: 4 s
    events.clear();
    playback.update(4.5, events);
    EXPECT_EQ(count(events, Event::LOOP), 1u);
    EXPECT_EQ(playback.getCurrentFrame(), 15.0f);
}

TEST(LottiePlaybackTest, BounceOnceCompletesAtTheStart){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.setMode(LottiePlayback::Mode::BOUNCE);
    playback.play(0.0, events);
    events.clear();
    playback.update(5.0, events);
    EXPECT_EQ(events, std::vector<Event>{Event::COMPLETE});
    EXPECT_FALSE(playback.isPlaying());
    EXPECT_TRUE(playback.isComplete());
    EXPECT_EQ(playback.getCurrentFrame(), 0.0f);
}

TEST(LottiePlaybackTest, PlayOnceCompletesOnTheLastFrame){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.play(0.0, events);
    events.clear();
    EXPECT_EQ(playback.update(1.99, events), 59.0f);
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(playback.update(2.0, events), 59.0f);
    EXPECT_EQ(events, std::vector<Event>{Event::COMPLETE});
    EXPECT_FALSE(playback.isPlaying());
    // stays there
    events.clear();
    EXPECT_EQ(playback.update(3.0, events), 59.0f);
    EXPECT_TRUE(events.empty());
    // play after complete starts over
    playback.play(3.0, events);
    EXPECT_EQ(events, std::vector<Event>{Event::PLAY});
    EXPECT_EQ(playback.getCurrentFrame(), 0.0f);
    EXPECT_EQ(frameAt(playback, 3.5), 15.0f);
}

TEST(LottiePlaybackTest, EventCountsOverThreeAndAHalfLoops){
    // simulated at 60 Hz for 3.5 passes (7 s)
    auto simulate = [](LottiePlayback& playback, std::vector<Event>& events){
        playback.play(0.0, events);
        for (int i = 1; i <= 420; i++)
            playback.update(i / 60.0, events);
    };

    LottiePlayback forever = make();
    forever.setLoop(true);
    std::vector<Event> events;
    simulate(forever, events);
    EXPECT_EQ(count(events, Event::PLAY), 1u);
    EXPECT_EQ(count(events, Event::LOOP), 3u);
    EXPECT_EQ(count(events, Event::COMPLETE), 0u);
    EXPECT_TRUE(forever.isPlaying());
    EXPECT_EQ(forever.getIterations(), 3);
    EXPECT_EQ(forever.getCurrentFrame(), 30.0f);

    LottiePlayback twice = make();
    twice.setLoopCount(2);
    events.clear();
    simulate(twice, events);
    EXPECT_EQ(count(events, Event::LOOP), 1u);
    EXPECT_EQ(count(events, Event::COMPLETE), 1u);
    EXPECT_EQ(events.back(), Event::COMPLETE);
    EXPECT_FALSE(twice.isPlaying());
    EXPECT_EQ(twice.getCurrentFrame(), 59.0f);

    LottiePlayback once = make();
    events.clear();
    simulate(once, events);
    EXPECT_EQ(count(events, Event::LOOP), 0u);
    EXPECT_EQ(count(events, Event::COMPLETE), 1u);

    LottiePlayback four = make();
    four.setLoopCount(4);
    events.clear();
    simulate(four, events);
    EXPECT_EQ(count(events, Event::LOOP), 3u);
    EXPECT_EQ(count(events, Event::COMPLETE), 0u);
    EXPECT_TRUE(four.isPlaying());
}

TEST(LottiePlaybackTest, ClockJumpCountsEveryWrap){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.setLoopCount(3);
    playback.play(0.0, events);
    events.clear();
    // 2 loops then complete in one update
    playback.update(100.0, events);
    EXPECT_EQ(count(events, Event::LOOP), 2u);
    EXPECT_EQ(count(events, Event::COMPLETE), 1u);
    EXPECT_EQ(playback.getIterations(), 3);

    // an infinite loop over a huge jump: the iterations count, the events are capped
    LottiePlayback forever = make();
    forever.setLoop(true);
    forever.play(0.0, events);
    events.clear();
    forever.update(100000.0, events);
    EXPECT_EQ(forever.getIterations(), 50000);
    EXPECT_EQ(count(events, Event::LOOP), 64u);
}

TEST(LottiePlaybackTest, Segment){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.setSegment(10.0f, 20.0f);
    EXPECT_EQ(playback.getCurrentFrame(), 10.0f);
    playback.setLoop(true);
    playback.play(0.0, events);
    EXPECT_EQ(frameAt(playback, 0.1), 13.0f);
    EXPECT_EQ(frameAt(playback, 0.3), 19.0f);
    EXPECT_EQ(frameAt(playback, 0.35), 10.0f);  // wrapped after 10 frames (1/3 s)
    // reversed bounds are swapped, out-of-range ones clamped
    playback.setSegment(50.0f, 40.0f);
    EXPECT_FLOAT_EQ(playback.getSegmentStart(), 40.0f);
    EXPECT_FLOAT_EQ(playback.getSegmentEnd(), 50.0f);
    playback.setSegment(-5.0f, 500.0f);
    EXPECT_FLOAT_EQ(playback.getSegmentStart(), 0.0f);
    EXPECT_FLOAT_EQ(playback.getSegmentEnd(), 60.0f);
    // a segment of one frame: an infinite loop holds it, a finite one completes at once
    playback.setSegment(25.0f, 25.0f);
    EXPECT_EQ(frameAt(playback, 5.0), 25.0f);
    EXPECT_TRUE(playback.isPlaying());
    playback.setLoop(false);
    events.clear();
    playback.update(5.1, events);
    EXPECT_EQ(events, std::vector<Event>{Event::COMPLETE});
    EXPECT_EQ(playback.getCurrentFrame(), 25.0f);
    playback.clearSegment();
    EXPECT_FLOAT_EQ(playback.getSegmentEnd(), 60.0f);
}

TEST(LottiePlaybackTest, SegmentReversed){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.setSegment(10.0f, 20.0f);
    playback.setDirection(-1);
    EXPECT_EQ(playback.getCurrentFrame(), 19.0f);
    playback.play(0.0, events);
    EXPECT_EQ(frameAt(playback, 0.1), 16.0f);
    events.clear();
    playback.update(1.0, events);
    EXPECT_EQ(events, std::vector<Event>{Event::COMPLETE});
    EXPECT_EQ(playback.getCurrentFrame(), 10.0f);
}

TEST(LottiePlaybackTest, Markers){
    LottiePlayback playback = make();
    playback.setMarkers({{"intro", 0.0f, 30.0f}, {"loop", 30.0f, 60.0f}});
    ASSERT_NE(playback.findMarker("loop"), nullptr);
    EXPECT_EQ(playback.findMarker("missing"), nullptr);
    EXPECT_TRUE(playback.setMarker("loop"));
    EXPECT_FLOAT_EQ(playback.getSegmentStart(), 30.0f);
    EXPECT_FLOAT_EQ(playback.getSegmentEnd(), 60.0f);
    EXPECT_EQ(playback.getCurrentFrame(), 30.0f);
    // an unknown marker leaves the segment alone
    EXPECT_FALSE(playback.setMarker("missing"));
    EXPECT_FLOAT_EQ(playback.getSegmentStart(), 30.0f);
    // a new animation forgets the markers
    playback.setAnimation(10.0f, 10.0f);
    EXPECT_TRUE(playback.getMarkers().empty());
    EXPECT_FLOAT_EQ(playback.getSegmentEnd(), 10.0f);
}

TEST(LottiePlaybackTest, PauseResumeStop){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.play(0.0, events);
    playback.update(0.5, events);
    playback.pause(0.5, events);
    EXPECT_EQ(events, (std::vector<Event>{Event::PLAY, Event::PAUSE}));
    EXPECT_FALSE(playback.isPlaying());
    // paused time does not count
    EXPECT_EQ(frameAt(playback, 3.0), 15.0f);
    events.clear();
    playback.play(3.0, events);
    EXPECT_EQ(frameAt(playback, 3.5), 30.0f);
    // a second play/pause is no event
    playback.play(3.5, events);
    EXPECT_EQ(count(events, Event::PLAY), 1u);
    events.clear();
    playback.stop(3.6, events);
    EXPECT_EQ(events, std::vector<Event>{Event::PAUSE});
    EXPECT_EQ(playback.getCurrentFrame(), 0.0f);
    events.clear();
    playback.stop(3.7, events);
    playback.pause(3.7, events);
    EXPECT_TRUE(events.empty());
}

TEST(LottiePlaybackTest, PauseAfterTheEndCompletes){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.play(0.0, events);
    events.clear();
    playback.pause(10.0, events);
    EXPECT_EQ(events, std::vector<Event>{Event::COMPLETE});
}

TEST(LottiePlaybackTest, SeekAndSetFrame){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.seek(1.0, 0.0);
    EXPECT_EQ(playback.getCurrentFrame(), 30.0f);
    playback.setFrame(45.0f, 0.0);
    EXPECT_EQ(playback.getCurrentFrame(), 45.0f);
    playback.setFrame(500.0f, 0.0);
    EXPECT_EQ(playback.getCurrentFrame(), 59.0f);
    playback.setFrame(-3.0f, 0.0);
    EXPECT_EQ(playback.getCurrentFrame(), 0.0f);
    // playing continues from the seek point
    playback.play(0.0, events);
    playback.seek(1.0, 1.0);
    EXPECT_EQ(frameAt(playback, 1.5), 45.0f);
    // reversed: the frame asked for is shown
    playback.setDirection(-1);
    playback.setFrame(20.0f, 2.0);
    EXPECT_EQ(playback.getCurrentFrame(), 20.0f);
    EXPECT_EQ(frameAt(playback, 2.1), 17.0f);
    // inside a segment: seconds count from its start
    playback.setDirection(1);
    playback.setSegment(30.0f, 60.0f);
    playback.seek(0.5, 3.0);
    EXPECT_EQ(playback.getCurrentFrame(), 45.0f);
}

TEST(LottiePlaybackTest, SeekAfterCompleteReplaysTheLastIteration){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.setLoopCount(2);
    playback.play(0.0, events);
    playback.update(10.0, events);
    ASSERT_TRUE(playback.isComplete());
    playback.seek(1.0, 10.0);
    EXPECT_FALSE(playback.isComplete());
    EXPECT_EQ(playback.getCurrentFrame(), 30.0f);
    events.clear();
    playback.play(10.0, events);
    // from the seek point, not from the start
    EXPECT_EQ(frameAt(playback, 10.5), 45.0f);
    events.clear();
    playback.update(11.0, events);
    EXPECT_EQ(events, std::vector<Event>{Event::COMPLETE});
}

TEST(LottiePlaybackTest, ClockGoingBackwardsDoesNotRewind){
    LottiePlayback playback = make();
    std::vector<Event> events;
    playback.play(5.0, events);
    EXPECT_EQ(frameAt(playback, 5.5), 15.0f);
    // HtmlUIClock::reset (tests) or a fixed time set back: no negative step
    EXPECT_EQ(frameAt(playback, 0.0), 15.0f);
    EXPECT_EQ(frameAt(playback, 0.5), 30.0f);
}
