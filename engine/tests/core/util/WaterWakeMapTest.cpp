// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Water Wakes' map without a scene (util/WaterWakeMap.h,
// docs/superpowers/specs/2026-11-04-water-boats-design.md): the Kelvin wake angle, the toroidal
// map moving with the camera, the foam's fade, and the wakes staying on the water they were made on.

#include "util/WaterWakeMap.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;

namespace water_wake_map_test {

    // a 2 x 6 m hull moving along +X at `speed`, its history over `seconds`, on body `body`
    WaterWakeTrack track(float speed, float seconds, uint8_t body){
        WaterWakeTrack track;
        track.tracked = true;
        track.velocity = Vector2(speed, 0.0f);
        track.body = body;
        const float step = 1.0f / 30.0f;
        for (float t = 0.0f; t <= seconds; t += step){
            WaterWakeSample sample;
            sample.position = Vector2(-10.0f + speed * t, 0.0f);
            sample.direction = Vector2(1.0f, 0.0f);
            sample.speed = speed;
            sample.time = t;
            sample.body = body;
            track.history.push_back(sample);
        }
        track.time = seconds;
        track.lastPosition = Vector3(-10.0f + speed * seconds - 2.7f, 0.0f, 0.0f);
        return track;
    }

    // a hull at `position` moving along +X at 4 m/s on a body, with one sample of history
    WaterWakeTrack churning(const Vector3& position, uint8_t body){
        WaterWakeTrack track;
        track.tracked = true;
        track.velocity = Vector2(4.0f, 0.0f);
        track.body = body;
        track.lastPosition = position;
        WaterWakeSample sample;
        sample.position = Vector2(position.x + 1.8f, position.z);
        sample.direction = Vector2(1.0f, 0.0f);
        sample.speed = 4.0f;
        sample.body = body;
        track.history.push_back(sample);
        return track;
    }

    // a footprint of radius r along both axes
    WaterWakeFootprint square(float r){
        WaterWakeFootprint footprint;
        footprint.radiusX = r;
        footprint.radiusZ = r;
        return footprint;
    }

    WaterWakeFootprint hull(){
        WaterWakeFootprint footprint;
        footprint.axisX = Vector2(0.0f, 1.0f);   // its width across the motion
        footprint.axisZ = Vector2(1.0f, 0.0f);   // its length along it
        footprint.radiusX = 1.0f;
        footprint.radiusZ = 3.0f;
        return footprint;
    }

}

using namespace water_wake_map_test;

TEST(WaterWakeMap, KelvinAngle){
    // slow: Kelvin's 19.47 degrees whatever the speed
    EXPECT_NEAR(getWaterWakeAngle(1.0f, 10.0f) * 57.29578f, 19.47f, 0.05f);
    // past a length Froude number of 0.5 it narrows as 1 / Froude
    const float length = 10.0f;
    const float fast = 2.0f * std::sqrt(9.81f * length);
    EXPECT_NEAR(getWaterWakeAngle(fast, length), 0.33984f * 0.25f, 1e-4f);
    // the hull speed of a 10 m hull (Froude 0.4)
    EXPECT_NEAR(getWaterWakeHullSpeed(length), 0.4f * std::sqrt(98.1f), 1e-4f);
}

TEST(WaterWakeMap, MoveClearsOnlyUncovered){
    WaterWakeMap map;
    map.reset(16, 1.0f);
    // a 4 x 4 m hull at (8, 8) moving along +X on body 1: its churn behind the stern, at (6.2, 8)
    WaterWakeTrack wake = churning(Vector3(8.0f, 0.0f, 8.0f), 1);
    map.stampTrack(wake, square(2.0f), 2.0f, 0.0f, 8.0f, 1.0f / 30.0f);
    float foam = 0.0f;
    float crest = 0.0f;
    ASSERT_TRUE(map.sample(6.5f, 8.5f, 1, foam, crest));
    ASSERT_GT(foam, 0.5f);
    const float kept = foam;

    // three columns along +X move into view: those are empty, the rest keeps its foam
    map.moveTo(3, 0);
    EXPECT_TRUE(map.sample(6.5f, 8.5f, 1, foam, crest));
    EXPECT_FLOAT_EQ(foam, kept);
    EXPECT_FALSE(map.sample(17.5f, 8.5f, 1, foam, crest));
    EXPECT_EQ(foam, 0.0f);
    // a jump of a whole map clears it
    map.moveTo(100, 0);
    EXPECT_FALSE(map.sample(108.5f, 8.5f, 1, foam, crest));
}

TEST(WaterWakeMap, FoamDecay){
    WaterWakeMap map;
    map.reset(8, 1.0f);
    // the churn of a 4 x 4 m hull at (4, 4), weak enough to stay under 1
    WaterWakeTrack wake = churning(Vector3(4.0f, 0.0f, 4.0f), 1);
    map.stampTrack(wake, square(2.0f), 0.5f, 0.0f, 8.0f, 1.0f / 30.0f);
    float before = 0.0f;
    float crest = 0.0f;
    ASSERT_TRUE(map.sample(2.5f, 4.5f, 1, before, crest));
    ASSERT_GT(before, 0.1f);
    ASSERT_LT(before, 1.0f);

    // a fade of 2 s, run in 60 steps of 1/30 s: exp(-2) of the foam left
    const float fade = 2.0f;
    const float dt = 1.0f / 30.0f;
    for (int i = 0; i < 60; i++)
        map.fade(std::exp(-2.0f * dt / fade));
    float after = 0.0f;
    map.sample(2.5f, 4.5f, 1, after, crest);
    EXPECT_NEAR(after / before, std::exp(-2.0f), 0.01f);
}

TEST(WaterWakeMap, StaysOnItsWater){
    // a hull on body 1 near the edge of body 2's part of the map: its waves spread as Kelvin's,
    // but only texels of its own body hold them
    WaterWakeMap map;
    map.reset(128, 0.25f);
    map.moveTo(-64, -64);
    WaterWakeTrack wake = track(5.0f, 3.0f, 1);
    map.stampTrack(wake, hull(), 1.0f, 1.0f, 8.0f, 1.0f / 30.0f);
    float strongest = 0.0f;
    map.encode(strongest);
    ASSERT_GT(strongest, 0.1f);

    const std::vector<uint8_t>& texels = map.getTexels();
    bool any = false;
    for (size_t t = 0; t < texels.size(); t += 4){
        if (texels[t] > 0 || texels[t + 1] > 0){
            any = true;
            EXPECT_EQ(texels[t + 2], 1) << "a wake texel of another body";
        }
    }
    EXPECT_TRUE(any);
    // a place of the wake: read on its own body it holds the wake, on another body nothing
    // (Doriax db794e641: wakes stay on the water they were made on)
    float foam = 0.0f;
    float crest = 0.0f;
    bool found = false;
    Vector2 place;
    for (float x = -8.0f; x < 6.0f && !found; x += 0.25f){
        for (float z = -6.0f; z < 6.0f && !found; z += 0.25f){
            if (map.sample(x, z, 1, foam, crest) && crest > 0.2f){
                found = true;
                place = Vector2(x, z);
            }
        }
    }
    ASSERT_TRUE(found);
    EXPECT_FALSE(map.sample(place.x, place.y, 2, foam, crest));
    EXPECT_EQ(crest, 0.0f);
}
