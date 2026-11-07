// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WaterWakeMap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace jasperengine;

namespace{

    const float WAKE_TWO_PI = 6.28318530718f;
    const float WAKE_GRAVITY = 9.81f;

    int wrapWakeTexel(int texel, int size){
        return ((texel % size) + size) % size;
    }

    float wakeSmoothstep(float edge0, float edge1, float x){
        const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // rounded by adding a half: std::lround is a library call per channel, this vectorizes
    uint8_t wakeUnorm8(float value){
        return (uint8_t)(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    }

}

void WaterWakeTrack::reset(){
    history.clear();
    velocity = Vector2(0.0f, 0.0f);
    tracked = false;
    sternValid = false;
    body = 0;
}

void WaterWakeFootprint::along(const Vector2& direction, float& halfLength, float& halfWidth) const{
    auto support = [&](const Vector2& d){
        const float a = d.dotProduct(axisX) * radiusX;
        const float b = d.dotProduct(axisZ) * radiusZ;
        return std::sqrt(a * a + b * b);
    };
    halfLength = 0.9f * support(direction);
    halfWidth = 0.7f * support(Vector2(-direction.y, direction.x));
}

float jasperengine::getWaterWakeHullSpeed(float length){
    return 0.4f * std::sqrt(WAKE_GRAVITY * std::max(length, 0.1f));
}

float jasperengine::getWaterWakeAngle(float speed, float length){
    const float froude = speed / std::sqrt(WAKE_GRAVITY * std::max(length, 0.1f));
    const float kelvin = 0.33984f; // asin(1 / 3)
    return (froude > 0.5f) ? kelvin * 0.5f / froude : kelvin;
}

void WaterWakeMap::reset(int size, float texel){
    this->size = std::max(size, 0);
    this->texel = texel;
    originX = 0;
    originZ = 0;
    const size_t count = (size_t)this->size * (size_t)this->size;
    foam.assign(count, 0.0f);
    crests.assign(count, 0.0f);
    tags.assign(count, 0);
    rgba.assign(count * 4, 0);
}

void WaterWakeMap::clear(){
    std::fill(foam.begin(), foam.end(), 0.0f);
    std::fill(crests.begin(), crests.end(), 0.0f);
    std::fill(tags.begin(), tags.end(), (uint8_t)0);
}

void WaterWakeMap::moveTo(int newOriginX, int newOriginZ){
    if (size <= 0)
        return;
    const int shiftX = newOriginX - originX;
    const int shiftZ = newOriginZ - originZ;
    if (std::abs(shiftX) >= size || std::abs(shiftZ) >= size){
        clear();
    }else{
        auto clearTexel = [&](size_t t){
            foam[t] = 0.0f;
            crests[t] = 0.0f;
            tags[t] = 0;
        };
        const int fromX = (shiftX > 0) ? originX + size : newOriginX;
        const int toX = (shiftX > 0) ? newOriginX + size : originX;
        for (int x = fromX; x < toX; x++){
            for (size_t t = (size_t)wrapWakeTexel(x, size); t < foam.size(); t += (size_t)size)
                clearTexel(t);
        }
        const int fromZ = (shiftZ > 0) ? originZ + size : newOriginZ;
        const int toZ = (shiftZ > 0) ? newOriginZ + size : originZ;
        for (int z = fromZ; z < toZ; z++){
            const size_t row = (size_t)wrapWakeTexel(z, size) * (size_t)size;
            for (int x = 0; x < size; x++)
                clearTexel(row + (size_t)x);
        }
    }
    originX = newOriginX;
    originZ = newOriginZ;
}

void WaterWakeMap::fade(float factor, bool clearCrests){
    for (float& value : foam){
        value = (value > 0.002f) ? value * factor : 0.0f;
    }
    if (clearCrests)
        std::fill(crests.begin(), crests.end(), 0.0f);
}

void WaterWakeMap::stamp(std::vector<float>& values, Vector2 a, Vector2 b, float radius, float strengthA, float strengthB, bool crest, uint8_t tag){
    if (size <= 0 || radius <= 0.0f || (strengthA <= 0.0f && strengthB <= 0.0f))
        return;

    const float inv = 1.0f / texel;
    const int minX = std::max((int)std::floor((std::min(a.x, b.x) - radius) * inv), originX);
    const int maxX = std::min((int)std::floor((std::max(a.x, b.x) + radius) * inv), originX + size - 1);
    const int minZ = std::max((int)std::floor((std::min(a.y, b.y) - radius) * inv), originZ);
    const int maxZ = std::min((int)std::floor((std::max(a.y, b.y) + radius) * inv), originZ + size - 1);
    const Vector2 ab = b - a;
    const float lengthSquared = ab.squaredLength();
    const float invLengthSquared = lengthSquared > 0.0f ? 1.0f / lengthSquared : 0.0f;
    const float invRadiusSquared = 1.0f / (radius * radius);
    const float strongest = std::max(strengthA, strengthB);
    const int firstColumn = wrapWakeTexel(minX, size);
    for (int z = minZ; z <= maxZ; z++){
        const size_t row = (size_t)wrapWakeTexel(z, size) * (size_t)size;
        const float pz = ((float)z + 0.5f) * texel - a.y;
        int column = firstColumn;
        for (int x = minX; x <= maxX; x++, column = (column + 1 == size) ? 0 : column + 1){
            const size_t index = row + (size_t)column;
            // a texel already at least the strongest this can give: nothing to do
            if (values[index] >= strongest)
                continue;
            const float px = ((float)x + 0.5f) * texel - a.x;
            const float t = std::clamp((px * ab.x + pz * ab.y) * invLengthSquared, 0.0f, 1.0f);
            const float dx = px - ab.x * t;
            const float dz = pz - ab.y * t;
            // the distance's square over the radius's: no root, no cosine
            const float d2 = (dx * dx + dz * dz) * invRadiusSquared;
            if (d2 >= 1.0f)
                continue;
            // a crest falls off as a smooth bump (close to Doriax's 0.5 + 0.5 cos(pi d)), foam keeps a
            // solid core to d = 0.45
            const float falloff = 1.0f - d2;
            const float shape = crest ? falloff * falloff : 1.0f - wakeSmoothstep(0.2f, 1.0f, d2);
            const float value = (strengthA + (strengthB - strengthA) * t) * shape;
            if (value > values[index]){
                values[index] = value;
                tags[index] = tag;
            }
        }
    }
}

void WaterWakeMap::stampTrack(WaterWakeTrack& track, const WaterWakeFootprint& footprint, float foamStrength, float waveStrength,
                              float fade, float step, bool withCrests){
    if (size <= 0 || track.history.empty())
        return;

    const float speed = track.velocity.length();
    const Vector2 direction = (speed > 0.001f) ? track.velocity / speed : track.history.back().direction;
    float halfLength;
    float halfWidth;
    footprint.along(direction, halfLength, halfWidth);
    const float hullSpeed = getWaterWakeHullSpeed(2.0f * halfLength);
    waveStrength = std::clamp(waveStrength, 0.0f, 2.0f);
    foamStrength = std::clamp(foamStrength, 0.0f, 2.0f);
    fade = std::max(fade, 0.1f);

    auto getCrest = [&](const WaterWakeSample& sample){
        const float age = track.time - sample.time;
        const float life = std::max(1.0f - age / fade, 0.0f);
        Crest crest;
        crest.offset = halfWidth + sample.speed * std::tan(getWaterWakeAngle(sample.speed, 2.0f * halfLength)) * age;
        crest.wavelength = std::max(WAKE_TWO_PI * sample.speed * sample.speed / WAKE_GRAVITY, 2.0f * texel);
        crest.phase = std::cos(WAKE_TWO_PI * sample.speed * age / crest.wavelength);
        // highest at the cusps, where the transverse waves meet it
        const float cusp = std::max(crest.phase, 0.0f);
        crest.strength = waveStrength * wakeSmoothstep(0.05f, 0.6f, sample.speed / hullSpeed) * life * std::sqrt(life) * (0.3f + 0.7f * cusp * cusp);
        // half the diverging wavelength at the cusps
        crest.width = std::clamp(2.1f * sample.speed * sample.speed / WAKE_GRAVITY, 2.0f * texel, std::max(halfLength, 2.0f * texel));
        return crest;
    };

    // two samples may join: the same body (a wake stays on the water it was made on, Doriax
    // db794e641), no jump between them
    const size_t count = track.history.size();
    auto joined = [&](size_t s){
        const WaterWakeSample& a = track.history[s - 1];
        const WaterWakeSample& b = track.history[s];
        return a.body != 0 && a.body == b.body && (b.position - a.position).length() <= std::max(3.0f, halfLength);
    };

    // The crests along segments of samples: a segment grows while the crests of the samples inside
    // it stay within half a texel of the straight line between its ends (in place, strength and
    // width), at most 16 samples. A stamp's capsule is far wider than a sample's step: one stamp per
    // straight run instead of one per pair visits a fraction of the texels.
    if (withCrests){
        crestScratch.resize(count);
        for (size_t s = 0; s < count; s++)
            crestScratch[s] = getCrest(track.history[s]);
        auto crestPoint = [&](size_t s, float offset){
            const WaterWakeSample& sample = track.history[s];
            return sample.position + Vector2(-sample.direction.y, sample.direction.x) * offset;
        };
        auto straight = [&](size_t i, size_t k){
            const Crest& from = crestScratch[i];
            const Crest& to = crestScratch[k];
            for (float side : {-1.0f, 1.0f}){
                const Vector2 a = crestPoint(i, side * from.offset);
                const Vector2 ab = crestPoint(k, side * to.offset) - a;
                const float lengthSquared = ab.squaredLength();
                for (size_t m = i + 1; m < k; m++){
                    const Crest& inside = crestScratch[m];
                    const Vector2 am = crestPoint(m, side * inside.offset) - a;
                    const float t = (lengthSquared > 0.0f) ? std::clamp(am.dotProduct(ab) / lengthSquared, 0.0f, 1.0f) : 0.0f;
                    if ((am - ab * t).length() > 0.5f * texel ||
                        std::abs(inside.strength - (from.strength + (to.strength - from.strength) * t)) > 0.03f ||
                        std::abs(inside.width - (from.width + (to.width - from.width) * t)) > 0.5f * texel)
                        return false;
                }
            }
            return true;
        };

        size_t i = 0;
        while (i + 1 < count){
            size_t j = i;
            while (j + 1 < count && j - i < 16 && joined(j + 1) && straight(i, j + 1))
                j++;
            if (j == i){
                i++;
                continue;
            }
            const WaterWakeSample& a = track.history[i];
            const WaterWakeSample& b = track.history[j];
            const Crest& crestA = crestScratch[i];
            const Crest& crestB = crestScratch[j];
            i = j;
            if (crestA.strength <= 0.0f && crestB.strength <= 0.0f)
                continue;

            const Vector2 acrossA(-a.direction.y, a.direction.x);
            const Vector2 acrossB(-b.direction.y, b.direction.x);
            const float width = 0.5f * (crestA.width + crestB.width);
            const float innerA = crestA.offset - 1.6f * crestA.width;
            const float innerB = crestB.offset - 1.6f * crestB.width;
            for (float side : {-1.0f, 1.0f}){
                stamp(crests, a.position + acrossA * (side * crestA.offset), b.position + acrossB * (side * crestB.offset),
                      width, crestA.strength, crestB.strength, true, b.body);
                if (innerA > halfWidth && innerB > halfWidth){
                    stamp(crests, a.position + acrossA * (side * innerA), b.position + acrossB * (side * innerB),
                          width, 0.5f * crestA.strength, 0.5f * crestB.strength, true, b.body);
                }
            }
        }

        // the transverse waves: one stamp a quarter of their wavelength wide on each crest, where the
        // samples' phase peaks (a parabola through three samples places it between them)
        for (size_t s = 1; s + 1 < count; s++){
            if (!joined(s) || !joined(s + 1))
                continue;
            const Crest& before = crestScratch[s - 1];
            const Crest& here = crestScratch[s];
            const Crest& after = crestScratch[s + 1];
            if (here.phase <= 0.0f || here.phase < before.phase || here.phase <= after.phase)
                continue;
            const float curve = before.phase - 2.0f * here.phase + after.phase;
            const float shift = (curve < 0.0f) ? std::clamp(0.5f * (before.phase - after.phase) / curve, -0.5f, 0.5f) : 0.0f;
            const WaterWakeSample& near = track.history[(shift < 0.0f) ? s - 1 : s + 1];
            const Crest& nearCrest = (shift < 0.0f) ? before : after;
            const float t = std::abs(shift);
            const WaterWakeSample& sample = track.history[s];
            const Vector2 position = sample.position + (near.position - sample.position) * t;
            const Vector2 direction = sample.direction + (near.direction - sample.direction) * t;
            const Vector2 across(-direction.y, direction.x);
            const float offset = here.offset + (nearCrest.offset - here.offset) * t;
            const float strength = 0.45f * (here.strength + (nearCrest.strength - here.strength) * t);
            stamp(crests, position - across * offset, position + across * offset, std::max(0.25f * here.wavelength, texel),
                  strength, strength, true, sample.body);
        }
    }

    // the bow waves of a fast hull break as they form: the samples of the last 0.6 s
    for (size_t s = count; s > 1 && track.time - track.history[s - 1].time < 0.6f; s--){
        if (!joined(s - 1))
            continue;
        const WaterWakeSample& b = track.history[s - 1];
        const float breaking = foamStrength * 0.8f * wakeSmoothstep(0.8f, 1.4f, b.speed / hullSpeed);
        const Crest crestB = getCrest(b);
        if (breaking <= 0.0f || crestB.strength <= 0.0f)
            continue;
        const Vector2 acrossB(-b.direction.y, b.direction.x);
        for (float side : {-1.0f, 1.0f}){
            const Vector2 point = b.position + acrossB * (side * crestB.offset);
            stamp(foam, point, point, 0.6f * crestB.width, breaking, breaking, false, b.body);
        }
    }

    if (track.body == 0 || speed <= 0.05f)
        return;

    const float ratio = speed / hullSpeed;
    const Vector2 center(track.lastPosition.x, track.lastPosition.z);
    const Vector2 across(-direction.y, direction.x);
    const Vector2 stern = center - direction * halfLength;
    const Vector2 from = (track.sternValid && (stern - track.lastStern).length() < 3.0f) ? track.lastStern : stern;
    const float churn = foamStrength * 0.9f * wakeSmoothstep(0.05f, 0.5f, ratio);
    stamp(foam, from, stern, halfWidth * (0.6f + 0.4f * std::min(ratio, 1.0f)), churn, churn, false, track.body);
    track.lastStern = stern;
    track.sternValid = true;

    // along the hull, from beam to beam past the bow
    const float spray = foamStrength * wakeSmoothstep(0.3f, 1.0f, ratio);
    const Vector2 moved = track.velocity * step;
    for (int i = -10; i <= 10; i++){
        const float angle = WAKE_TWO_PI * (10.0f * (float)i) / 360.0f;
        const Vector2 edge = center + direction * (halfLength * std::cos(angle)) + across * (halfWidth * std::sin(angle));
        const float strength = spray * (0.55f + 0.45f * std::cos(angle));
        stamp(foam, edge - moved, edge, std::max(1.2f * texel, 0.2f * halfWidth), strength, strength, false, track.body);
    }
}

void WaterWakeMap::encode(float& strongest){
    // a texel packed into one 32-bit store (RGBA in memory order on every target, all little
    // endian) and the strongest found in bytes: the loop vectorizes, a float max would not
    const size_t count = foam.size();
    uint8_t largest = 0;
    for (size_t t = 0; t < count; t++){
        const uint8_t foamByte = wakeUnorm8(foam[t]);
        const uint8_t crestByte = wakeUnorm8(crests[t]);
        largest = std::max(largest, std::max(foamByte, crestByte));
        const uint32_t texel = (uint32_t)foamByte | ((uint32_t)crestByte << 8) | ((uint32_t)tags[t] << 16) | 0xff000000u;
        std::memcpy(&rgba[t * 4], &texel, sizeof(texel));
    }
    strongest = (float)largest / 255.0f;
}

bool WaterWakeMap::sample(float x, float z, uint8_t body, float& foamOut, float& crestOut) const{
    foamOut = 0.0f;
    crestOut = 0.0f;
    if (size <= 0 || body == 0)
        return false;
    // texel centres at (i + 0.5) * texel: bilinear between the four around the point
    const float fx = x / texel - 0.5f;
    const float fz = z / texel - 0.5f;
    const int x0 = (int)std::floor(fx);
    const int z0 = (int)std::floor(fz);
    const float tx = fx - (float)x0;
    const float tz = fz - (float)z0;
    bool any = false;
    for (int dz = 0; dz < 2; dz++){
        for (int dx = 0; dx < 2; dx++){
            const int wx = x0 + dx;
            const int wz = z0 + dz;
            if (wx < originX || wx >= originX + size || wz < originZ || wz >= originZ + size)
                continue;
            const size_t index = (size_t)wrapWakeTexel(wz, size) * (size_t)size + (size_t)wrapWakeTexel(wx, size);
            if (tags[index] != body)
                continue;
            const float weight = (dx ? tx : 1.0f - tx) * (dz ? tz : 1.0f - tz);
            foamOut += foam[index] * weight;
            crestOut += crests[index] * weight;
            any = true;
        }
    }
    foamOut = std::min(foamOut, 1.0f);
    crestOut = std::min(crestOut, 1.0f);
    return any;
}
