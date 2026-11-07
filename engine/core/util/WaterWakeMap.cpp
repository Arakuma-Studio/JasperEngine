// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "WaterWakeMap.h"

#include <algorithm>
#include <cmath>

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

    uint8_t wakeUnorm8(float value){
        return (uint8_t)std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f);
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

    struct Crest{
        float offset;       // from the track
        float strength;
        float width;
        float wavelength;   // of the transverse waves
        float phase;        // of the transverse waves, 1 on a crest
    };
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

    // without the crests, only the last 0.6 s of the history (its breaking bow waves)
    size_t first = 1;
    if (!withCrests){
        first = track.history.size();
        while (first > 1 && track.time - track.history[first - 1].time < 0.6f)
            first--;
    }
    for (size_t s = first; s < track.history.size(); s++){
        const WaterWakeSample& a = track.history[s - 1];
        const WaterWakeSample& b = track.history[s];
        // a wake stays on the water it was made on (Doriax db794e641)
        if (a.body == 0 || a.body != b.body || (b.position - a.position).length() > std::max(3.0f, halfLength))
            continue;

        const Crest crestA = getCrest(a);
        const Crest crestB = getCrest(b);
        if (crestA.strength <= 0.0f && crestB.strength <= 0.0f)
            continue;

        const Vector2 acrossB(-b.direction.y, b.direction.x);
        if (withCrests){
            const Vector2 acrossA(-a.direction.y, a.direction.x);
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

            // the transverse waves of every fourth sample: a quarter of their wavelength wide, the
            // samples between overlap them whole
            if (crestB.phase > 0.0f && s % 4 == 0){
                const float strength = 0.45f * crestB.strength * crestB.phase * crestB.phase;
                stamp(crests, b.position - acrossB * crestB.offset, b.position + acrossB * crestB.offset,
                      std::max(0.25f * crestB.wavelength, texel), strength, strength, true, b.body);
            }
        }

        // the bow waves of a fast hull break as they form
        if (track.time - b.time < 0.6f){
            const float breaking = foamStrength * 0.8f * wakeSmoothstep(0.8f, 1.4f, b.speed / hullSpeed);
            for (float side : {-1.0f, 1.0f}){
                const Vector2 point = b.position + acrossB * (side * crestB.offset);
                stamp(foam, point, point, 0.6f * crestB.width, breaking, breaking, false, b.body);
            }
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
    strongest = 0.0f;
    const size_t count = foam.size();
    for (size_t t = 0; t < count; t++){
        strongest = std::max(strongest, std::max(foam[t], crests[t]));
        uint8_t* texel = &rgba[t * 4];
        texel[0] = wakeUnorm8(foam[t]);
        texel[1] = wakeUnorm8(crests[t]);
        texel[2] = tags[t];
        texel[3] = 255;
    }
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
