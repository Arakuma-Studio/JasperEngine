// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// 2D sorting (docs/specs/S52-sprite-instancing.md R1-R9, D1, D2): Sorting Layers, Order in Layer,
// Sorting Groups and the Transparency Sort Axis. While every renderer keeps the defaults, no
// Sorting Group is enabled and the mode is Default (sortingActive false) the passes draw as
// before. Otherwise a pass without distance sort draws its hierarchy in the order gathered here
// once per frame: cut into runs at UI subtrees (they keep their place and order: scissors, Canvas
// Groups and FLATTEN layers depend on it), each run's renderers stable-sorted by (layer value,
// Order in Layer, axis distance), the hierarchy index breaking ties; a Sorting Group is one item
// whose members sort among themselves. The transparent queue of 3D passes compares the same keys
// first (TransparentRenderComparison; TransparentGroupComparison with a Sorting Group in it).
// Every draw carries its bucket to the batcher (BatchItem::sortKey): layer value | order |
// outermost group's sequence + 1.

#include "RenderSystem.h"

#include "Scene.h"
#include "component/SortingGroupComponent.h"
#include "registry/SortingLayers.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;

uint64_t RenderSystem::makeSortKey(uint16_t layerValue, int32_t order, bool axis, float distance){
    const int32_t clamped = std::clamp<int32_t>(order, -32768, 32767);
    uint64_t key = ((uint64_t)layerValue << 48) | ((uint64_t)(uint32_t)(clamped + 32768) << 32);
    if (axis){
        // the float's bits made order-preserving (negatives reversed), then inverted: a larger
        // distance sorts first
        uint32_t bits;
        const float value = distance == 0.0f ? 0.0f : distance; // -0 sorts as 0
        std::memcpy(&bits, &value, sizeof(bits));
        bits = (bits & 0x80000000u) ? ~bits : (bits | 0x80000000u);
        key |= (uint64_t)(~bits);
    }
    return key;
}

void RenderSystem::updateSortingActive(){
    sortFrame++;
    bool active = SortingLayers::getTransparencySortMode() == TransparencySortMode::CUSTOM_AXIS;
    auto check = [&active](auto array){
        for (size_t i = 0; i < array->size() && !active; i++){
            const auto& component = array->getComponentFromIndex(i);
            active = component.sortingLayer != 0 || component.sortingOrder != 0;
        }
    };
    if (!active)
        check(scene->getComponentArray<MeshComponent>());
    if (!active)
        check(scene->getComponentArray<ParticleSystemComponent>());
    if (!active)
        check(scene->getComponentArray<PointsComponent>());
    if (!active)
        check(scene->getComponentArray<LineRendererComponent>());
    if (!active)
        check(scene->getComponentArray<TrailRendererComponent>());
    if (!active && scene->isComponentRegistered<SortingGroupComponent>()){
        auto groups = scene->getComponentArray<SortingGroupComponent>();
        for (size_t i = 0; i < groups->size() && !active; i++)
            active = groups->getComponentFromIndex(i).enabled;
    }
    sortingActive = active;
}

bool RenderSystem::rendererSortFields(Entity entity, Signature signature, uint16_t& layerValue, int32_t& order, Vector3& point){
    int32_t layer = 0;
    order = 0;
    bool found = true;
    if (signature.test(scene->getComponentId<MeshComponent>())){
        const MeshComponent& mesh = scene->getComponent<MeshComponent>(entity);
        layer = mesh.sortingLayer;
        order = mesh.sortingOrder;
        // a Sprite measured at its bounds' centre (Sprite Sort Point Center), else at its pivot
        if (signature.test(scene->getComponentId<SpriteComponent>()) &&
                scene->getComponent<SpriteComponent>(entity).spriteSortPoint == SpriteSortPoint::CENTER &&
                !mesh.worldAABB.isNull() && mesh.worldAABB != AABB::ZERO && mesh.worldAABB.isFinite()){
            point = mesh.worldAABB.getCenter();
        }
    }else if (signature.test(scene->getComponentId<ParticleSystemComponent>())){
        const ParticleSystemComponent& particles = scene->getComponent<ParticleSystemComponent>(entity);
        layer = particles.sortingLayer;
        order = particles.sortingOrder;
    }else if (signature.test(scene->getComponentId<PointsComponent>())){
        const PointsComponent& points = scene->getComponent<PointsComponent>(entity);
        layer = points.sortingLayer;
        order = points.sortingOrder;
    }else if (signature.test(scene->getComponentId<LineRendererComponent>())){
        const LineRendererComponent& lines = scene->getComponent<LineRendererComponent>(entity);
        layer = lines.sortingLayer;
        order = lines.sortingOrder;
    }else if (signature.test(scene->getComponentId<TrailRendererComponent>())){
        const TrailRendererComponent& trail = scene->getComponent<TrailRendererComponent>(entity);
        layer = trail.sortingLayer;
        order = trail.sortingOrder;
    }else{
        found = false;
    }
    layerValue = SortingLayers::valueOf(layer);
    return found;
}

uint64_t RenderSystem::entitySortKey(Entity entity, Signature signature, bool axis){
    const Transform& transform = scene->getComponent<Transform>(entity);
    Vector3 point = transform.worldPosition;
    uint16_t layerValue = 0;
    int32_t order = 0;
    rendererSortFields(entity, signature, layerValue, order, point);
    float distance = 0.0f;
    if (axis)
        distance = SortingLayers::getTransparencySortAxis().dotProduct(point);
    return makeSortKey(layerValue, order, axis, distance);
}

void RenderSystem::gatherSortRange(size_t first, size_t end, size_t depth, uint32_t outerSequence, uint64_t outerKey, int32_t skipGroup){
    auto transforms = scene->getComponentArray<Transform>();
    const bool axis = SortingLayers::getTransparencySortMode() == TransparencySortMode::CUSTOM_AXIS;
    const ComponentId rectId = scene->getComponentId<RectTransformComponent>();
    const bool groups = scene->isComponentRegistered<SortingGroupComponent>() && scene->getComponentArray<SortingGroupComponent>()->size() > 0;
    const ComponentId groupId = groups ? scene->getComponentId<SortingGroupComponent>() : ComponentId();
    if (sortItemStack.size() <= depth)
        sortItemStack.resize(depth + 1);
    sortItemStack[depth].clear();

    auto bucketOf = [&](uint64_t key){
        return outerSequence ? ((outerKey & 0xFFFFFFFF00000000ull) | outerSequence) : (key & 0xFFFFFFFF00000000ull);
    };
    // the run so far, sorted, into the order (a group item gathers its members in its place)
    auto flush = [&](){
        std::vector<SortItem>& items = sortItemStack[depth];
        auto byKey = [](const SortItem& a, const SortItem& b){ return a.key < b.key; };
        // (appended in hierarchy order: the stable sort keeps it for equal keys)
        if (!std::is_sorted(items.begin(), items.end(), byKey))
            std::stable_sort(items.begin(), items.end(), byKey);
        for (size_t n = 0; n < sortItemStack[depth].size(); n++){
            const SortItem item = sortItemStack[depth][n];
            if (item.group){
                // nested groups take the outermost group's place in the batcher
                const uint32_t sequence = outerSequence ? outerSequence : ++sortGroupCounter;
                gatherSortRange(item.index, (size_t)item.last + 1, depth + 1, sequence, outerSequence ? outerKey : item.key, (int32_t)item.index);
            }else{
                sortedOrder.push_back({item.index, bucketOf(item.key), item.part});
            }
        }
        sortItemStack[depth].clear();
    };

    size_t i = first;
    while (i < end){
        const Entity entity = transforms->getEntity(i);
        const Signature signature = scene->getSignature(entity);
        if (signature.test(rectId)){
            // a UI subtree keeps its place and its order (a run boundary)
            flush();
            const size_t last = std::min(scene->findBranchLastIndex(entity), end - 1);
            for (size_t k = i; k <= last; k++)
                sortedOrder.push_back({(uint32_t)k, UI_SORT_BUCKET, -1});
            i = last + 1;
            continue;
        }
        if (groups && (int32_t)i != skipGroup && signature.test(groupId)){
            const SortingGroupComponent& group = scene->getComponent<SortingGroupComponent>(entity);
            if (group.enabled){
                const Transform& transform = scene->getComponent<Transform>(entity);
                const float distance = axis ? SortingLayers::getTransparencySortAxis().dotProduct(transform.worldPosition) : 0.0f;
                SortItem item;
                item.key = makeSortKey(SortingLayers::valueOf(group.sortingLayer), group.sortingOrder, axis, distance);
                item.index = (uint32_t)i;
                item.last = (uint32_t)std::min(scene->findBranchLastIndex(entity), end - 1);
                item.group = true;
                sortItemStack[depth].push_back(item);
                i = (size_t)item.last + 1;
                continue;
            }
        }
        // S50 R32: a Tilemap Layer in Individual mode sorts each row with the scene's other items
        if (SpriteInstanceChunkList* chunks = spriteInstanceChunksOf(entity); chunks && !chunks->chunks.empty() && chunks->chunks[0].sortItem){
            const Transform& transform = scene->getComponent<Transform>(entity);
            uint16_t layerValue = 0;
            int32_t order = 0;
            Vector3 point = transform.worldPosition;
            rendererSortFields(entity, signature, layerValue, order, point);
            for (size_t part = 0; part < chunks->chunks.size(); part++){
                const float distance = axis ? SortingLayers::getTransparencySortAxis().dotProduct(transform.modelMatrix * chunks->chunks[part].sortPoint) : 0.0f;
                SortItem item;
                item.key = makeSortKey(layerValue, order, axis, distance);
                item.index = (uint32_t)i;
                item.part = (int32_t)part;
                sortItemStack[depth].push_back(item);
            }
            i++;
            continue;
        }
        SortItem item;
        item.key = entitySortKey(entity, signature, axis);
        item.index = (uint32_t)i;
        sortItemStack[depth].push_back(item);
        i++;
    }
    flush();
}

const std::vector<RenderSystem::SortedDraw>& RenderSystem::getSorted2DOrder(){
    if (sortOrderFrame == sortFrame)
        return sortedOrder;
    sortOrderFrame = sortFrame;
    sortedOrder.clear();
    sortGroupCounter = 0;
    if (!sortingActive)
        return sortedOrder;
    sortGathers++;
    auto transforms = scene->getComponentArray<Transform>();
    sortedOrder.reserve(transforms->size());
    gatherSortRange(0, transforms->size(), 0, 0, 0, -1);
    return sortedOrder;
}

bool RenderSystem::applyTransparentSortKeys(std::vector<TransparentRenderData>& renders, const Vector3& cameraPosition){
    if (!sortingActive || renders.empty())
        return false;
    auto transforms = scene->getComponentArray<Transform>();
    const size_t count = transforms->size();
    // the outermost enabled Sorting Group over each transform (the array is in hierarchy order:
    // a branch is contiguous after its root)
    sortOuterGroup.assign(count, -1);
    sortGroupSequence.assign(count, 0);
    const bool groups = scene->isComponentRegistered<SortingGroupComponent>() && scene->getComponentArray<SortingGroupComponent>()->size() > 0;
    if (groups){
        const ComponentId groupId = scene->getComponentId<SortingGroupComponent>();
        uint32_t sequence = 0;
        for (size_t i = 0; i < count; i++){
            if (sortOuterGroup[i] >= 0)
                continue;
            const Entity entity = transforms->getEntity(i);
            if (!scene->getSignature(entity).test(groupId) || !scene->getComponent<SortingGroupComponent>(entity).enabled)
                continue;
            const size_t last = scene->findBranchLastIndex(entity);
            sequence++;
            for (size_t k = i; k <= last && k < count; k++){
                sortOuterGroup[k] = (int32_t)i;
                sortGroupSequence[k] = sequence;
            }
        }
    }
    bool grouped = false;
    for (TransparentRenderData& render : renders){
        const Signature signature = scene->getSignature(render.entity);
        uint16_t layerValue = 0;
        int32_t order = 0;
        Vector3 point;
        rendererSortFields(render.entity, signature, layerValue, order, point);
        const uint64_t own = makeSortKey(layerValue, order, false, 0.0f);
        const size_t index = transforms->getIndex(render.entity);
        const int32_t group = index < count ? sortOuterGroup[index] : -1;
        if (group >= 0){
            const Entity groupEntity = transforms->getEntity((size_t)group);
            const SortingGroupComponent& component = scene->getComponent<SortingGroupComponent>(groupEntity);
            render.sortKey = makeSortKey(SortingLayers::valueOf(component.sortingLayer), component.sortingOrder, false, 0.0f);
            render.sortGroup = sortGroupSequence[index];
            render.memberKey = own;
            render.sortBucket = (render.sortKey & 0xFFFFFFFF00000000ull) | render.sortGroup;
            // the group is one unit: at its own distance, after every item alone of its distance
            render.unitPriority = 0;
            render.unitDistance = (scene->getComponent<Transform>(groupEntity).worldPosition - cameraPosition).length();
            render.unitSeq = (1ull << 32) | render.sortGroup;
            grouped = true;
        }else{
            render.sortKey = own;
            render.sortGroup = 0;
            render.memberKey = 0;
            render.sortBucket = own & 0xFFFFFFFF00000000ull;
            render.unitPriority = render.sortPriority;
            render.unitDistance = render.waterSortDistance;
            render.unitSeq = render.order;
        }
    }
    return grouped;
}
