// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The importance ranking, sticky slots with a challenge margin and cooldown follow OpenEngine's
// PointShadowAtlasPlanner (Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT
// License, NOTICE.md); the code is our own.

#include "ShadowSlotPlanner.h"

#include <algorithm>
#include <limits>

using namespace jasperengine;

bool ShadowAtlasLayout::operator==(const ShadowAtlasLayout& o) const{
    return width == o.width && height == o.height && rects == o.rects;
}

unsigned int ShadowSlotPlanner::powerOfTwoResolution(unsigned int resolution, unsigned int minimum, unsigned int maximum){
    unsigned int value = 1;
    while (value * 2 <= resolution && value < (1u << 30))
        value *= 2;
    unsigned int low = 1;
    while (low * 2 <= minimum)
        low *= 2;
    unsigned int high = 1;
    while (high * 2 <= maximum)
        high *= 2;
    if (high < low)
        high = low;
    return std::clamp(value, low, high);
}

namespace {
    struct Block {
        int x, y;
        unsigned int size;
    };

    // Buddy allocation of power-of-two squares sorted largest first into blocks of the largest
    // size laid in rows of `columns`: a block splits into four quadrants, the lowest free one is
    // taken first, so the atlas grows upwards only as much as it must. Returns the height.
    unsigned int buddyPack(const std::vector<std::pair<unsigned int, int>>& items, unsigned int columns,
                           std::vector<ShadowRect>* rects){
        const unsigned int largest = items.front().first;
        std::vector<Block> freeBlocks;
        unsigned int blocks = 0;
        unsigned int height = 0;
        for (const auto& item : items){
            const unsigned int size = item.first;
            // the lowest (then leftmost) free block of the smallest size that holds it
            int best = -1;
            for (int i = 0; i < (int)freeBlocks.size(); i++){
                const Block& b = freeBlocks[i];
                if (b.size < size)
                    continue;
                if (best < 0){
                    best = i;
                    continue;
                }
                const Block& o = freeBlocks[best];
                if (b.size < o.size || (b.size == o.size && (b.y < o.y || (b.y == o.y && b.x < o.x))))
                    best = i;
            }
            Block block;
            if (best < 0){
                block = Block{(int)((blocks % columns) * largest), (int)((blocks / columns) * largest), largest};
                blocks++;
            }else{
                block = freeBlocks[best];
                freeBlocks.erase(freeBlocks.begin() + best);
            }
            while (block.size > size){
                const unsigned int half = block.size / 2;
                freeBlocks.push_back(Block{block.x + (int)half, block.y, half});
                freeBlocks.push_back(Block{block.x, block.y + (int)half, half});
                freeBlocks.push_back(Block{block.x + (int)half, block.y + (int)half, half});
                block.size = half;
            }
            height = std::max(height, (unsigned int)block.y + size);
            if (rects)
                (*rects)[item.second] = ShadowRect{block.x, block.y, (int)size, (int)size};
        }
        return height;
    }
}

bool ShadowSlotPlanner::pack(const std::vector<unsigned int>& sizes, unsigned int maxSide, ShadowAtlasLayout& layout){
    layout.width = 0;
    layout.height = 0;
    layout.rects.assign(sizes.size(), ShadowRect());

    std::vector<std::pair<unsigned int, int>> items;
    for (int i = 0; i < (int)sizes.size(); i++){
        if (sizes[i] > 0)
            items.push_back({sizes[i], i});
    }
    if (items.empty())
        return true;
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b){
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });

    const unsigned int largest = items.front().first;
    if (largest > maxSide)
        return false;

    // the column count giving the smallest area, then the squarest
    unsigned int bestColumns = 0;
    uint64_t bestArea = std::numeric_limits<uint64_t>::max();
    unsigned int bestSkew = std::numeric_limits<unsigned int>::max();
    for (unsigned int columns = 1; columns * largest <= maxSide; columns++){
        const unsigned int width = columns * largest;
        const unsigned int height = buddyPack(items, columns, nullptr);
        if (height > maxSide)
            continue;
        const uint64_t area = (uint64_t)width * (uint64_t)height;
        const unsigned int skew = (width > height) ? width - height : height - width;
        if (area < bestArea || (area == bestArea && skew < bestSkew)){
            bestArea = area;
            bestSkew = skew;
            bestColumns = columns;
        }
        // one row holds every block: more columns only widen it
        if (height <= largest)
            break;
    }
    if (bestColumns == 0)
        return false;

    layout.width = bestColumns * largest;
    layout.height = buddyPack(items, bestColumns, &layout.rects);
    // used columns only: a row of fewer blocks than columns leaves nothing on its right
    unsigned int usedWidth = 0;
    for (const ShadowRect& rect : layout.rects){
        if (!rect.empty())
            usedWidth = std::max(usedWidth, (unsigned int)(rect.x + rect.width));
    }
    layout.width = usedWidth;
    return true;
}

void ShadowSlotPlanner::reset(){
    holders.clear();
    current = ShadowPlan();
}

namespace {
    bool projectiveKind(ShadowLightKind kind){
        return kind != ShadowLightKind::POINT;
    }

    // the first run of `length` free slots starting at a multiple of `alignment`, -1 for none
    int findRun(const std::vector<bool>& used, int length, int alignment){
        for (int start = 0; start + length <= (int)used.size(); start += alignment){
            bool free = true;
            for (int s = start; s < start + length && free; s++){
                free = !used[s];
            }
            if (free)
                return start;
        }
        return -1;
    }

    void mark(std::vector<bool>& used, int start, int length, bool value){
        for (int s = start; s < start + length; s++){
            used[s] = value;
        }
    }
}

const ShadowPlan& ShadowSlotPlanner::plan(const std::vector<ShadowLightRequest>& requests, const Limits& limits, uint64_t frame){
    ShadowPlan next;
    next.lights.assign(requests.size(), ShadowLightAssignment());

    // priority: directional lights first (the sun), then importance, then a stable id order
    std::vector<int> order(requests.size());
    for (int i = 0; i < (int)requests.size(); i++){
        order[i] = i;
    }
    auto ahead = [&](int a, int b){
        const ShadowLightRequest& ra = requests[a];
        const ShadowLightRequest& rb = requests[b];
        const bool da = ra.kind == ShadowLightKind::DIRECTIONAL;
        const bool db = rb.kind == ShadowLightKind::DIRECTIONAL;
        if (da != db)
            return da;
        if (ra.importance != rb.importance)
            return ra.importance > rb.importance;
        return ra.id < rb.id;
    };
    std::sort(order.begin(), order.end(), ahead);

    std::vector<bool> usedProjective(std::max(limits.projectiveSlots, 0), false);
    std::vector<bool> usedPoint(std::max(limits.pointSlots, 0), false);
    auto usedOf = [&](ShadowLightKind kind) -> std::vector<bool>& {
        return projectiveKind(kind) ? usedProjective : usedPoint;
    };

    // holders that still ask for the same slots keep them
    std::vector<Holder> kept;
    std::vector<int> holderOf(requests.size(), -1);
    for (const Holder& holder : holders){
        for (int i = 0; i < (int)requests.size(); i++){
            const ShadowLightRequest& request = requests[i];
            if (holderOf[i] < 0 && request.id == holder.id && request.kind == holder.kind && request.slots == holder.slots){
                std::vector<bool>& used = usedOf(holder.kind);
                if (holder.firstSlot >= 0 && holder.firstSlot + (int)holder.slots <= (int)used.size()){
                    holderOf[i] = (int)kept.size();
                    kept.push_back(holder);
                    mark(used, holder.firstSlot, holder.slots, true);
                }
                break;
            }
        }
    }

    auto importanceOf = [&](int holderIndex){
        for (int i = 0; i < (int)requests.size(); i++){
            if (holderOf[i] == holderIndex)
                return i;
        }
        return -1;
    };

    for (int index : order){
        if (holderOf[index] >= 0)
            continue;
        const ShadowLightRequest& request = requests[index];
        if (request.slots == 0)
            continue;
        std::vector<bool>& used = usedOf(request.kind);
        const int alignment = (request.kind == ShadowLightKind::POINT) ? (int)request.slots : 1;
        int start = findRun(used, (int)request.slots, alignment);

        if (start < 0){
            // challenge the weakest holders of the same atlas
            std::vector<int> candidates;
            for (int h = 0; h < (int)kept.size(); h++){
                const Holder& holder = kept[h];
                if (holder.firstSlot < 0 || projectiveKind(holder.kind) != projectiveKind(request.kind))
                    continue;
                const int holderRequest = importanceOf(h);
                if (holderRequest < 0)
                    continue;
                const ShadowLightRequest& held = requests[holderRequest];
                const bool sunOverLocal = request.kind == ShadowLightKind::DIRECTIONAL && held.kind != ShadowLightKind::DIRECTIONAL;
                const bool beats = !ahead(holderRequest, index) && request.importance > held.importance * limits.margin &&
                    frame >= holder.since + limits.cooldownFrames;
                if (sunOverLocal || beats)
                    candidates.push_back(h);
            }
            std::sort(candidates.begin(), candidates.end(), [&](int a, int b){
                return ahead(importanceOf(b), importanceOf(a));
            });
            std::vector<int> evicted;
            for (int h : candidates){
                mark(used, kept[h].firstSlot, kept[h].slots, false);
                evicted.push_back(h);
                start = findRun(used, (int)request.slots, alignment);
                if (start >= 0)
                    break;
            }
            if (start < 0){
                for (int h : evicted){
                    mark(used, kept[h].firstSlot, kept[h].slots, true);
                }
            }else{
                for (int h : evicted){
                    const int holderRequest = importanceOf(h);
                    if (holderRequest >= 0)
                        holderOf[holderRequest] = -1;
                    kept[h].firstSlot = -1;
                }
            }
        }

        if (start < 0 && request.kind != ShadowLightKind::POINT){
            // enough free slots, but no run of them: lay the projective holders out again
            int free = 0;
            for (bool u : used)
                free += u ? 0 : 1;
            if (free >= (int)request.slots){
                std::vector<int> projectiveHolders;
                for (int h = 0; h < (int)kept.size(); h++){
                    if (kept[h].firstSlot >= 0 && projectiveKind(kept[h].kind))
                        projectiveHolders.push_back(h);
                }
                std::fill(used.begin(), used.end(), false);
                int cursor = 0;
                for (int h : projectiveHolders){
                    kept[h].firstSlot = cursor;
                    mark(used, cursor, kept[h].slots, true);
                    cursor += (int)kept[h].slots;
                }
                start = findRun(used, (int)request.slots, 1);
            }
        }

        if (start >= 0){
            mark(used, start, request.slots, true);
            Holder holder;
            holder.id = request.id;
            holder.kind = request.kind;
            holder.firstSlot = start;
            holder.slots = request.slots;
            holder.since = frame;
            holderOf[index] = (int)kept.size();
            kept.push_back(holder);
        }
    }

    holders.clear();
    for (int i = 0; i < (int)requests.size(); i++){
        if (holderOf[i] >= 0 && kept[holderOf[i]].firstSlot >= 0){
            holders.push_back(kept[holderOf[i]]);
            next.lights[i].firstSlot = kept[holderOf[i]].firstSlot;
            const unsigned int resolution = powerOfTwoResolution(requests[i].resolution, limits.minResolution, limits.maxResolution);
            for (unsigned int s = 0; s < requests[i].slots && s < SHADOW_LIGHT_MAX_SLOTS; s++){
                next.lights[i].resolution[s] = resolution;
            }
        }
    }

    // resolutions within the texture size and the memory budget
    const unsigned int minResolution = powerOfTwoResolution(limits.minResolution, 1, limits.maxResolution);
    std::vector<unsigned int> projectiveSizes(usedProjective.size(), 0);
    std::vector<unsigned int> pointSizes(usedPoint.size(), 0);
    auto fillSizes = [&](){
        std::fill(projectiveSizes.begin(), projectiveSizes.end(), 0u);
        std::fill(pointSizes.begin(), pointSizes.end(), 0u);
        for (int i = 0; i < (int)requests.size(); i++){
            const ShadowLightAssignment& light = next.lights[i];
            if (light.firstSlot < 0)
                continue;
            std::vector<unsigned int>& sizes = projectiveKind(requests[i].kind) ? projectiveSizes : pointSizes;
            for (unsigned int s = 0; s < requests[i].slots && s < SHADOW_LIGHT_MAX_SLOTS; s++){
                sizes[light.firstSlot + s] = light.resolution[s];
            }
        }
    };
    // the least important light that can still give up resolution (or, at the minimum, its slots)
    auto weakest = [&](bool dropping) -> int{
        for (auto it = order.rbegin(); it != order.rend(); ++it){
            const ShadowLightAssignment& light = next.lights[*it];
            if (light.firstSlot < 0)
                continue;
            if (dropping)
                return *it;
            for (unsigned int s = 0; s < requests[*it].slots && s < SHADOW_LIGHT_MAX_SLOTS; s++){
                if (light.resolution[s] > minResolution)
                    return *it;
            }
        }
        return -1;
    };
    // halves the largest of a light's maps, the farthest (last) of equals first
    auto halve = [&](int index){
        ShadowLightAssignment& light = next.lights[index];
        int slot = -1;
        for (unsigned int s = 0; s < requests[index].slots && s < SHADOW_LIGHT_MAX_SLOTS; s++){
            if (light.resolution[s] > minResolution && (slot < 0 || light.resolution[s] >= light.resolution[slot]))
                slot = (int)s;
        }
        if (slot >= 0)
            light.resolution[slot] /= 2;
    };
    while (true){
        fillSizes();
        const bool fits = pack(projectiveSizes, limits.maxTextureSize, next.projective) &&
                          pack(pointSizes, limits.maxTextureSize, next.point);
        const uint64_t bytes = next.projective.bytes() + next.point.bytes();
        if (fits && (limits.memoryBytes == 0 || bytes <= limits.memoryBytes))
            break;
        int light = weakest(false);
        if (light >= 0){
            halve(light);
            next.reducedResolution = true;
            continue;
        }
        light = weakest(true);
        if (light < 0)
            break;
        next.lights[light] = ShadowLightAssignment();
        next.reducedResolution = true;
        holders.erase(std::remove_if(holders.begin(), holders.end(), [&](const Holder& h){
            return h.id == requests[light].id && h.kind == requests[light].kind;
        }), holders.end());
    }

    // static layers, most important first, while they fit
    std::vector<unsigned int> projectiveStatic(usedProjective.size(), 0);
    std::vector<unsigned int> pointStatic(usedPoint.size(), 0);
    const uint64_t atlasBytes = next.projective.bytes() + next.point.bytes();
    for (int index : order){
        ShadowLightAssignment& light = next.lights[index];
        if (light.firstSlot < 0 || !requests[index].wantsStatic)
            continue;
        std::vector<unsigned int>& sizes = projectiveKind(requests[index].kind) ? projectiveStatic : pointStatic;
        for (unsigned int s = 0; s < requests[index].slots && s < SHADOW_LIGHT_MAX_SLOTS; s++){
            sizes[light.firstSlot + s] = light.resolution[s];
        }
        ShadowAtlasLayout projectiveLayout, pointLayout;
        const bool fits = pack(projectiveStatic, limits.maxTextureSize, projectiveLayout) &&
                          pack(pointStatic, limits.maxTextureSize, pointLayout);
        const uint64_t bytes = atlasBytes + projectiveLayout.bytes() + pointLayout.bytes();
        if (fits && (limits.memoryBytes == 0 || bytes <= limits.memoryBytes)){
            light.staticLayer = true;
            next.projectiveStatic = projectiveLayout;
            next.pointStatic = pointLayout;
        }else{
            for (unsigned int s = 0; s < requests[index].slots && s < SHADOW_LIGHT_MAX_SLOTS; s++){
                sizes[light.firstSlot + s] = 0;
            }
        }
    }
    if (next.projectiveStatic.rects.empty())
        next.projectiveStatic.rects.assign(usedProjective.size(), ShadowRect());
    if (next.pointStatic.rects.empty())
        next.pointStatic.rects.assign(usedPoint.size(), ShadowRect());

    next.memoryBytes = next.projective.bytes() + next.point.bytes() + next.projectiveStatic.bytes() + next.pointStatic.bytes();
    current = next;
    return current;
}
