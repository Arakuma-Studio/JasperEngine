// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The clip-time helpers of KeyframeSampling (spec 2.1 wrap modes, 9.R16 events): wrapClipTime
// for every wrap, and collectCrossedEvents forward, backward, across loop and ping-pong
// boundaries, with the start rule, and at low frame rates (each event at most once per update).

#include <gtest/gtest.h>

#include "animation/AnimationClip.h"
#include "animation/KeyframeSampling.h"

using namespace jasperengine;

namespace {

    // a 1 s clip with events at 0, 0.25, 0.5 and 1 (the end)
    std::shared_ptr<const AnimationClip> eventClip(){
        ClipData data;
        data.length = 1.0f;
        for (float time : {0.0f, 0.25f, 0.5f, 1.0f}){
            ClipEventData event;
            event.time = time;
            event.function = "at" + std::to_string((int)(time * 100));
            data.events.push_back(event);
        }
        return AnimationClip::build(data);
    }

    // the names of what crossed
    std::vector<std::string> crossed(const AnimationClip& clip, ClipWrapMode wrap, float from, float to, bool start = false){
        std::vector<size_t> indices;
        anim::collectCrossedEvents(clip, wrap, from, to, start, indices);
        std::vector<std::string> names;
        for (size_t index : indices) names.push_back(clip.getEvents()[index].function);
        return names;
    }

    using Names = std::vector<std::string>;

}

TEST(EventCrossing, WrapClipTime){
    EXPECT_FLOAT_EQ(anim::wrapClipTime(0.4f, 1.0f, ClipWrapMode::Once), 0.4f);
    EXPECT_FLOAT_EQ(anim::wrapClipTime(1.4f, 1.0f, ClipWrapMode::Once), 1.0f);
    EXPECT_FLOAT_EQ(anim::wrapClipTime(-0.4f, 1.0f, ClipWrapMode::Once), 0.0f);
    EXPECT_FLOAT_EQ(anim::wrapClipTime(1.4f, 1.0f, ClipWrapMode::Default), 1.0f);
    EXPECT_FLOAT_EQ(anim::wrapClipTime(7.5f, 1.0f, ClipWrapMode::ClampForever), 1.0f);

    EXPECT_NEAR(anim::wrapClipTime(1.25f, 1.0f, ClipWrapMode::Loop), 0.25f, 1e-6f);
    EXPECT_NEAR(anim::wrapClipTime(-0.25f, 1.0f, ClipWrapMode::Loop), 0.75f, 1e-6f);
    EXPECT_NEAR(anim::wrapClipTime(2.0f, 1.0f, ClipWrapMode::Loop), 0.0f, 1e-6f);
    EXPECT_NEAR(anim::wrapClipTime(1000000.5f, 1.0f, ClipWrapMode::Loop), 0.5f, 1e-6f);

    EXPECT_NEAR(anim::wrapClipTime(0.25f, 1.0f, ClipWrapMode::PingPong), 0.25f, 1e-6f);
    EXPECT_NEAR(anim::wrapClipTime(1.25f, 1.0f, ClipWrapMode::PingPong), 0.75f, 1e-6f);
    EXPECT_NEAR(anim::wrapClipTime(1.0f, 1.0f, ClipWrapMode::PingPong), 1.0f, 1e-6f);
    EXPECT_NEAR(anim::wrapClipTime(2.25f, 1.0f, ClipWrapMode::PingPong), 0.25f, 1e-6f);
    EXPECT_NEAR(anim::wrapClipTime(-0.25f, 1.0f, ClipWrapMode::PingPong), 0.25f, 1e-6f);

    // degenerate lengths and times
    EXPECT_EQ(anim::wrapClipTime(3.0f, 0.0f, ClipWrapMode::Loop), 0.0f);
    EXPECT_EQ(anim::wrapClipTime(NAN, 1.0f, ClipWrapMode::Loop), 0.0f);
}

TEST(EventCrossing, OnceForwardAndBackward){
    std::shared_ptr<const AnimationClip> clip = eventClip();
    // forward fires from < t <= to; a state that just started also fires at its start
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Once, 0.0f, 0.3f), (Names{"at25"}));
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Once, 0.0f, 0.3f, true), (Names{"at0", "at25"}));
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Once, 0.25f, 0.5f), (Names{"at50"}));
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Once, 0.6f, 5.0f), (Names{"at100"})) << "past the end: up to the end";
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Once, 0.3f, 0.3f), (Names{}));
    // backward fires to <= t < from, in reverse order; started at its length it fires there
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Once, 1.0f, 0.2f), (Names{"at50", "at25"}));
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Once, 1.0f, 0.2f, true), (Names{"at100", "at50", "at25"}));
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Once, 0.5f, -1.0f), (Names{"at25", "at0"}));
    // clamp_forever holding at its end crosses nothing more
    EXPECT_EQ(crossed(*clip, ClipWrapMode::ClampForever, 1.0f, 1.5f), (Names{}));
}

TEST(EventCrossing, LoopBoundariesSplitTheInterval){
    std::shared_ptr<const AnimationClip> clip = eventClip();
    // the end of a cycle, then the start of the next
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Loop, 0.9f, 1.3f), (Names{"at100", "at0", "at25"}));
    // reaching the boundary exactly fires both (the next update starts past them)
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Loop, 0.6f, 1.0f), (Names{"at100", "at0"}));
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Loop, 1.0f, 1.2f), (Names{})) << "at the boundary already: both fired";
    // a loop just started at 0 fires its start event, not the end of a cycle before it
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Loop, 0.0f, 0.1f, true), (Names{"at0"}));
    // backward: the start of the cycle, then the end of the previous one
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Loop, 1.3f, 0.9f), (Names{"at25", "at0", "at100"}));
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Loop, 1.0f, 0.9f, true), (Names{"at100"}));
    // low frame rate: an update spanning several cycles fires each event once, in order
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Loop, 0.3f, 3.6f), (Names{"at50", "at100", "at0", "at25"}));
    EXPECT_EQ(crossed(*clip, ClipWrapMode::Loop, 3.6f, 0.3f), (Names{"at50", "at25", "at0", "at100"}));
}

TEST(EventCrossing, PingPongCrossesEachEventOnTheWayBack){
    std::shared_ptr<const AnimationClip> clip = eventClip();
    // 0.9 -> 1.6 is clip time 0.9 -> 1 -> 0.4: the end, then 0.5 on the way back
    EXPECT_EQ(crossed(*clip, ClipWrapMode::PingPong, 0.9f, 1.6f), (Names{"at100", "at50"}));
    EXPECT_EQ(crossed(*clip, ClipWrapMode::PingPong, 1.6f, 2.1f), (Names{"at25", "at0"}));
    // several legs in one update: once each
    EXPECT_EQ(crossed(*clip, ClipWrapMode::PingPong, 0.1f, 5.0f), (Names{"at25", "at50", "at100", "at0"}));
}

TEST(EventCrossing, PositionsOrderEventsOnTheTimeLine){
    std::shared_ptr<const AnimationClip> clip = eventClip();
    std::vector<anim::CrossedEvent> events;
    anim::collectCrossedEvents(*clip, ClipWrapMode::Loop, 0.9f, 2.3f, false, events);
    ASSERT_EQ(events.size(), 4u);
    EXPECT_NEAR(events[0].position, 1.0, 1e-6);    // at100, end of cycle 0
    EXPECT_NEAR(events[1].position, 1.0, 1e-6);    // at0, start of cycle 1
    EXPECT_NEAR(events[2].position, 1.25, 1e-6);
    EXPECT_NEAR(events[3].position, 1.5, 1e-6);
    // events beyond an explicit shorter length never fire
    ClipData data;
    data.length = 0.4f;
    ClipEventData late;
    late.time = 0.5f;
    late.function = "late";
    data.events.push_back(late);
    std::shared_ptr<const AnimationClip> cut = AnimationClip::build(data);
    EXPECT_EQ(crossed(*cut, ClipWrapMode::Loop, 0.0f, 3.0f, true), (Names{}));
}
