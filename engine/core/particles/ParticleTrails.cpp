// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ParticleTrails.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::particles;

void ParticleTrails::configure(uint32_t ringCount, uint32_t pointCount){
    rings = ringCount;
    points = std::max(pointCount, 2u);
    const size_t n = (size_t)rings * points;
    xs.assign(n, 0.0f);
    ys.assign(n, 0.0f);
    zs.assign(n, 0.0f);
    times.assign(n, 0.0f);
    heads.assign(rings, 0u);
    counts.assign(rings, 0u);
    state.assign(rings, Free);
    lifetimes.assign(rings, 1.0f);
    colors.assign(rings, 0xffffffffu);
    widths.assign(rings, 1.0f);
    freeList.clear();
    freeList.reserve(rings);
    for (uint32_t r = rings; r > 0; r--)
        freeList.push_back(r - 1);
    used = 0;
}

void ParticleTrails::clear(){
    configure(rings, points);
}

uint32_t ParticleTrails::acquire(){
    if (freeList.empty())
        return kNoTrail;
    const uint32_t ring = freeList.back();
    freeList.pop_back();
    state[ring] = Owned;
    counts[ring] = 0;
    heads[ring] = 0;
    used++;
    return ring;
}

void ParticleTrails::release(uint32_t ring, bool fade){
    if (ring >= rings || state[ring] == Free)
        return;
    if (fade && counts[ring] > 0){
        state[ring] = Orphan;
        return;
    }
    state[ring] = Free;
    counts[ring] = 0;
    freeList.push_back(ring);
    used--;
}

void ParticleTrails::record(uint32_t ring, float x, float y, float z, float time, float minDistance){
    if (ring >= rings || state[ring] != Owned)
        return;
    const size_t base = (size_t)ring * points;
    uint32_t& count = counts[ring];
    uint32_t& head = heads[ring];
    if (count >= 2){
        // the head follows the particle while it is within the spacing of the point before
        const uint32_t prev = (head + points - 1) % points;
        const float dx = x - xs[base + prev], dy = y - ys[base + prev], dz = z - zs[base + prev];
        if (dx * dx + dy * dy + dz * dz < minDistance * minDistance){
            xs[base + head] = x;
            ys[base + head] = y;
            zs[base + head] = z;
            times[base + head] = time;
            return;
        }
    }
    head = count == 0 ? 0 : (head + 1) % points;
    xs[base + head] = x;
    ys[base + head] = y;
    zs[base + head] = z;
    times[base + head] = time;
    if (count < points)
        count++;
}

void ParticleTrails::expire(float time){
    for (uint32_t ring = 0; ring < rings; ring++){
        if (state[ring] == Free)
            continue;
        const size_t base = (size_t)ring * points;
        // the oldest points are at the tail: head - count + 1
        while (counts[ring] > 0){
            const uint32_t tail = (heads[ring] + points - (counts[ring] - 1)) % points;
            if (time - times[base + tail] <= lifetimes[ring])
                break;
            counts[ring]--;
        }
        if (state[ring] == Orphan && counts[ring] == 0){
            state[ring] = Free;
            freeList.push_back(ring);
            used--;
        }
    }
}

void ParticleTrails::setLifetime(uint32_t ring, float lifetime){
    if (ring < rings)
        lifetimes[ring] = lifetime;
}

void ParticleTrails::setStyle(uint32_t ring, uint32_t color, float width){
    if (ring < rings){
        colors[ring] = color;
        widths[ring] = width;
    }
}

void ParticleTrails::point(uint32_t ring, uint32_t k, float& x, float& y, float& z, float& time) const{
    const size_t slot = (size_t)ring * points + (heads[ring] + points - k) % points;
    x = xs[slot];
    y = ys[slot];
    z = zs[slot];
    time = times[slot];
}
