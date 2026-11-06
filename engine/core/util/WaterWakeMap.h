// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WATER_WAKE_MAP_H
#define WATER_WAKE_MAP_H

#include "Export.h"
#include "math/Vector2.h"
#include "math/Vector3.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace jasperengine{

    // The wakes of Water Wakes (component/WaterWakeComponent.h) without a scene: a toroidal map of
    // the world around the main camera and the Kelvin wake model that stamps into it (from Doriax
    // 755e7d723 and its fixes). WaterInteractionSystem drives them; library:water reads the map
    // (includes/water_interaction.glsl).

    // the height of a full wake crest (metres): what the map's crest channel 1 is
    constexpr float WATER_WAKE_CREST_HEIGHT = 0.4f;

    // a place the bow passed, which diverging waves spread from
    struct JASPERENGINE_API WaterWakeSample{
        Vector2 position;
        Vector2 direction;
        float speed = 0.0f;
        float time = 0.0f;
        uint8_t body = 0;               // the tag of the water body it was on (0: none)
    };

    // what the model remembers of one emitter
    struct JASPERENGINE_API WaterWakeTrack{
        std::deque<WaterWakeSample> history;
        Vector3 lastPosition;
        Vector2 velocity;               // smoothed, world XZ m/s
        Vector2 lastStern;
        float time = 0.0f;              // the emitter's own clock (its steps' deltas)
        bool tracked = false;
        bool sternValid = false;
        uint8_t body = 0;               // the body it crosses now (0: not on water)

        void reset();
    };

    // The waterline footprint along a direction of motion: an ellipse inside the box of the
    // emitter (its local X and Z axes over world XZ, their half extents in metres)
    struct JASPERENGINE_API WaterWakeFootprint{
        Vector2 axisX = Vector2(1.0f, 0.0f);
        Vector2 axisZ = Vector2(0.0f, 1.0f);
        float radiusX = 0.5f;
        float radiusZ = 0.5f;

        // half its length along the direction (0.9 of the ellipse's reach) and half its width across (0.7)
        void along(const Vector2& direction, float& halfLength, float& halfWidth) const;
    };

    // the speed of a hull of that length at a length Froude number of 0.4
    JASPERENGINE_API float getWaterWakeHullSpeed(float length);
    // Kelvin's half angle (radians), narrowing as 1 / Froude past 0.5 (Rabaud and Moisy, 2013)
    JASPERENGINE_API float getWaterWakeAngle(float speed, float length);

    class JASPERENGINE_API WaterWakeMap{
    private:
        int size = 0;
        float texel = 0.0f;             // world size of a texel
        int originX = 0;                // world texel at the map's corner
        int originZ = 0;
        std::vector<float> foam;
        std::vector<float> crests;
        std::vector<uint8_t> tags;
        std::vector<uint8_t> rgba;

        // keeps the larger value along a capsule from a to b (its strength lerped along it); a crest
        // falls off as a cosine, foam keeps a solid core; a raised texel takes the tag
        void stamp(std::vector<float>& values, Vector2 a, Vector2 b, float radius, float strengthA, float strengthB, bool crest, uint8_t tag);

    public:
        // an empty map of size x size texels, each texel world units wide
        void reset(int size, float texel);
        void clear();
        // moves the corner to a world texel: the texels it uncovers are cleared, the rest kept
        // (each world texel is at its position modulo the size)
        void moveTo(int originX, int originZ);
        // foam times factor (under 0.002: none); every crest cleared, they are stamped again each step
        void fade(float factor);

        // The wake one emitter makes on the map this step: from each pair of its history samples on
        // the same body, diverging crests at the wake angle of their speed (an inner one at half
        // strength), transverse waves between and breaking bow waves past hull speed; while it
        // crosses a water (track.body), churn behind the stern and spray along the hull.
        // waves and foam scale the strengths (0..2), fade is the scene's wake fade (seconds).
        void stampTrack(WaterWakeTrack& track, const WaterWakeFootprint& footprint, float foamStrength, float waveStrength,
                        float fade, float step);

        // the RGBA8 texels (R foam, G crest, B the body tag, A 255) and the largest value
        void encode(float& strongest);
        const std::vector<uint8_t>& getTexels() const { return rgba; }

        // the foam and crest at a world XZ on a body (bilinear between texels of that body; 0 elsewhere)
        bool sample(float x, float z, uint8_t body, float& foam, float& crest) const;

        bool isAllocated() const { return size > 0; }
        int getSize() const { return size; }
        float getTexel() const { return texel; }
        int getOriginX() const { return originX; }
        int getOriginZ() const { return originZ; }
        // world XZ of the corner
        Vector2 getCorner() const { return Vector2((float)originX * texel, (float)originZ * texel); }
    };

}

#endif //WATER_WAKE_MAP_H
