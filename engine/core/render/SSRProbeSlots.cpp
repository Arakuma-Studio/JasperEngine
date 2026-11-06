// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SSRProbeSlots.h"

#include "math/Vector4.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

uint8_t SSRCode::probe(int slot, float weight){
    const int clampedSlot = std::clamp(slot, 0, SSR_PROBE_SLOTS - 1);
    const float w = std::clamp(weight, 0.0f, 1.0f);
    const int step = std::max((int)std::lround(w * (float)SSR_PROBE_WEIGHT_STEPS), 1);
    return (uint8_t)(PROBE_BASE + clampedSlot * SSR_PROBE_WEIGHT_STEPS + (step - 1));
}

bool SSRCode::decodeProbe(uint8_t code, int& slot, int& step){
    if (code < PROBE_BASE)
        return false;
    const int index = (int)code - PROBE_BASE;
    if (index >= SSR_PROBE_SLOTS * SSR_PROBE_WEIGHT_STEPS)
        return false;
    slot = index / SSR_PROBE_WEIGHT_STEPS;
    step = index % SSR_PROBE_WEIGHT_STEPS + 1;
    return true;
}

float jasperengine::screenCoverage(const AABB& box, const Matrix4& viewProjection){
    if (!box.isFinite())
        return box.isInfinite() ? 1.0f : 0.0f;

    // the box clipped to the space in front of the eye (clip w above a small epsilon): its corners
    // there, and where its edges cross that plane, so a mesh reaching behind the camera covers what
    // its front part covers instead of the whole screen
    constexpr float NEAR_W = 1e-4f;
    const Vector3* corners = box.getCorners();
    Vector4 clip[8];
    for (int i = 0; i < 8; i++)
        clip[i] = viewProjection * Vector4(corners[i], 1.0f);

    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    bool any = false;
    const auto include = [&](const Vector4& c){
        const float x = c.x / c.w;
        const float y = c.y / c.w;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
        any = true;
    };
    for (int i = 0; i < 8; i++){
        if (clip[i].w > NEAR_W)
            include(clip[i]);
    }
    // the 12 edges of a box: corners differing in one coordinate (AABB::getCorners' order is not
    // assumed, the pairs are found by their positions)
    for (int a = 0; a < 8; a++){
        for (int b = a + 1; b < 8; b++){
            int same = 0;
            same += corners[a].x == corners[b].x ? 1 : 0;
            same += corners[a].y == corners[b].y ? 1 : 0;
            same += corners[a].z == corners[b].z ? 1 : 0;
            if (same != 2)
                continue;
            const bool inA = clip[a].w > NEAR_W;
            const bool inB = clip[b].w > NEAR_W;
            if (inA == inB)
                continue;
            const float t = (NEAR_W - clip[a].w) / (clip[b].w - clip[a].w);
            include(clip[a] + (clip[b] - clip[a]) * t);
        }
    }
    if (!any)
        return 0.0f;

    // the part of the projected rectangle on screen (ndc -1..1 on both axes, area 4)
    const float width = std::max(0.0f, std::min(maxX, 1.0f) - std::max(minX, -1.0f));
    const float height = std::max(0.0f, std::min(maxY, 1.0f) - std::max(minY, -1.0f));
    return std::min(width * height * 0.25f, 1.0f);
}

int SSRProbeAssignment::slotOf(Entity probe) const{
    if (probe == NULL_ENTITY)
        return -1;
    for (int s = 0; s < SSR_PROBE_SLOTS; s++){
        if (slots[s] == probe)
            return s;
    }
    return -1;
}

int SSRProbeAssignment::count() const{
    int n = 0;
    for (int s = 0; s < SSR_PROBE_SLOTS; s++){
        if (slots[s] != NULL_ENTITY)
            n++;
    }
    return n;
}

void jasperengine::assignSSRProbeSlots(const std::vector<SSRProbeCandidate>& candidates,
                                       const SSRProbeAssignment& previous, SSRProbeAssignment& out){
    out = SSRProbeAssignment();

    // the winners: the SSR_PROBE_SLOTS best by (held-boosted) coverage, then the lower entity
    struct Ranked{
        Entity probe;
        float score;
    };
    Ranked ranked[SSR_PROBE_SLOTS];
    int rankedCount = 0;
    for (const SSRProbeCandidate& candidate : candidates){
        if (candidate.probe == NULL_ENTITY)
            continue;
        const float score = candidate.coverage * (previous.slotOf(candidate.probe) >= 0 ? SSR_PROBE_HYSTERESIS : 1.0f);
        const auto better = [](const Ranked& a, const Ranked& b){
            return a.score > b.score || (a.score == b.score && a.probe < b.probe);
        };
        const Ranked entry{candidate.probe, score};
        if (rankedCount < SSR_PROBE_SLOTS){
            ranked[rankedCount++] = entry;
        }else if (better(entry, ranked[rankedCount - 1])){
            ranked[rankedCount - 1] = entry;
        }else{
            continue;
        }
        std::sort(ranked, ranked + rankedCount, better);
    }

    // a winner that held a slot keeps its index; the others take the free ones in rank order
    bool placed[SSR_PROBE_SLOTS] = {false, false, false, false};
    for (int r = 0; r < rankedCount; r++){
        const int held = previous.slotOf(ranked[r].probe);
        if (held >= 0){
            out.slots[held] = ranked[r].probe;
            placed[r] = true;
        }
    }
    for (int r = 0; r < rankedCount; r++){
        if (placed[r])
            continue;
        for (int s = 0; s < SSR_PROBE_SLOTS; s++){
            if (out.slots[s] == NULL_ENTITY){
                out.slots[s] = ranked[r].probe;
                break;
            }
        }
    }
}
