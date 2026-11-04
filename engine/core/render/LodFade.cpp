// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LodFade.h"

#include <algorithm>
#include <cmath>
#include <utility>

using namespace jasperengine;

namespace{
    LodDraw single(unsigned int level){
        LodDraw draw;
        draw.from = draw.to = (uint8_t)level;
        return draw;
    }

    // from keeps k of the 64ths: a k that rounds to all or nothing is one draw
    LodDraw fade(unsigned int from, unsigned int to, float shareOfFrom){
        const long k = std::lround(LOD_FADE_STEPS * shareOfFrom);
        if (k <= 0)
            return single(to);
        if (k >= LOD_FADE_STEPS)
            return single(from);
        LodDraw draw;
        draw.from = (uint8_t)from;
        draw.to = (uint8_t)to;
        draw.k = (int8_t)k;
        return draw;
    }

    unsigned int forcedLevel(const LodFadeInput& input){
        return std::min((unsigned int)input.forced, std::max(input.numLods, 1u) - 1);
    }

    float screenShare(const LodFadeInput& input){
        return input.screenHeight / std::max(input.viewHeight, 1.0f);
    }
}

unsigned int jasperengine::lodSelect(const float* errors, unsigned int numLods, float allowed){
    unsigned int level = 0;
    for (unsigned int l = 1; l < numLods; l++){
        if (errors[l] > allowed)
            break;
        level = l;
    }
    return level;
}

unsigned int jasperengine::lodSelectStable(const LodFadeInput& input, unsigned int current){
    if (input.forced >= 0)
        return forcedLevel(input);
    if (input.cullBelow > 0.0f){
        const float share = screenShare(input);
        if (share < input.cullBelow || (current == LOD_CULLED && share < input.cullBelow * (1.0f + LOD_HYSTERESIS)))
            return LOD_CULLED;
    }
    unsigned int level = lodSelect(input.errors, input.numLods, input.allowed);
    if (level < current && current < input.numLods && input.errors[current] <= input.allowed * (1.0f + LOD_HYSTERESIS))
        level = current;
    return level;
}

LodDraw jasperengine::lodFadeUpdate(MeshLodFade& state, const LodFadeInput& input){
    const unsigned int selected = lodSelectStable(input, state.target);

    // a camera cut, teleport, zoom or switch of camera: the budget jumped
    const bool cut = state.lastAllowed > 0.0f && (input.allowed > 2.0f * state.lastAllowed || input.allowed < 0.5f * state.lastAllowed);
    const bool snap = !input.fadeAllowed || input.forced >= 0 ||
                      state.frame == 0 || state.frame + 1 != input.frame ||   // first frame, or not drawn last frame
                      input.screenHeight < LOD_FADE_MIN_PIXELS || cut;
    state.frame = input.frame;
    state.lastAllowed = input.allowed;

    if (snap){
        state.level = state.target = (uint8_t)selected;
        state.progress = 1.0f;
        return single(selected);
    }

    if (state.level == state.target){
        if (selected != state.level){
            // starts, whatever the distance between the levels
            state.target = (uint8_t)selected;
            state.progress = 0.0f;
        }
    }else if (selected == state.level){
        // back to where it came from: reverses from where it is, no pop
        std::swap(state.level, state.target);
        state.progress = 1.0f - state.progress;
    }
    // (a third level waits for the fade's end)

    if (state.level != state.target){
        state.progress += input.deltaTime / std::max(input.duration, 0.001f);
        if (state.progress >= 1.0f){
            state.level = state.target;
            state.progress = 1.0f;
        }
    }
    if (state.level == state.target)
        return single(state.level);
    return fade(state.level, state.target, 1.0f - state.progress);
}

LodDraw jasperengine::lodDistanceFade(const LodFadeInput& input){
    if (input.forced >= 0)
        return single(forcedLevel(input));
    const float width = std::clamp(input.transitionWidth, 0.0f, 1.0f);
    const bool fades = input.fadeAllowed && width > 0.0f && input.screenHeight >= LOD_FADE_MIN_PIXELS;
    const unsigned int level = lodSelect(input.errors, input.numLods, input.allowed);

    // Cull Below: out over the last `width` of the height above the cull height
    if (input.cullBelow > 0.0f){
        const float share = screenShare(input);
        if (share < input.cullBelow)
            return single(LOD_CULLED);
        if (fades && share < input.cullBelow * (1.0f + width))
            return fade(level, LOD_CULLED, (share - input.cullBelow) / (input.cullBelow * width));
    }

    if (!fades || level + 1 >= input.numLods)
        return single(level);
    // the band of this level's budget, from its own error to the next level's (perspective
    // budgets grow linearly with the distance)
    const float own = (level == 0) ? 0.0f : input.errors[level];
    const float next = input.errors[level + 1];
    const float start = next - width * (next - own);
    if (next <= own || input.allowed <= start)
        return single(level);
    return fade(level, level + 1, (next - input.allowed) / (next - start));
}
