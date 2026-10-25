// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShadowCasterRegistry.h"

using namespace jasperengine;

bool ShadowCasterRegistry::same(const ShadowCasterState& a, const ShadowCasterState& b){
    return a.bounds == b.bounds && a.revision == b.revision && a.layerBit == b.layerBit &&
        a.isStatic == b.isStatic && a.cameraDependent == b.cameraDependent;
}

void ShadowCasterRegistry::beginUpdate(){
    updateCount++;
    changes.clear();
    bounds = ShadowBounds();
    staticBounds = ShadowBounds();
    dynamicBounds = ShadowBounds();
    staticLayers = 0;
    dynamicLayers = 0;
    cameraLayers = 0;
}

void ShadowCasterRegistry::addChange(const ShadowCasterState* oldState, const ShadowCasterState* newState){
    ShadowCasterChange change;
    if (oldState){
        change.oldBounds = oldState->bounds;
        change.layerBits |= oldState->layerBit;
        change.staticLayer = change.staticLayer || oldState->isStatic;
        change.dynamicLayer = change.dynamicLayer || !oldState->isStatic;
        change.cameraDependent = change.cameraDependent || oldState->cameraDependent;
    }
    if (newState){
        change.newBounds = newState->bounds;
        change.layerBits |= newState->layerBit;
        change.staticLayer = change.staticLayer || newState->isStatic;
        change.dynamicLayer = change.dynamicLayer || !newState->isStatic;
        change.cameraDependent = change.cameraDependent || newState->cameraDependent;
    }
    changes.push_back(change);
}

bool ShadowCasterRegistry::update(uint32_t entity, const ShadowCasterState& state){
    if (entity >= records.size()){
        records.resize((size_t)entity + 1 + records.size() / 2);
    }
    Record& record = records[entity];

    bool changed;
    if (!record.live){
        addChange(nullptr, &state);
        record.live = true;
        liveEntities.push_back(entity);
        liveCount++;
        changed = true;
    }else{
        changed = !same(record.state, state) || state.animated;
        if (changed)
            addChange(&record.state, &state);
    }
    record.state = state;
    record.seen = updateCount;

    bounds.merge(state.bounds);
    if (state.isStatic){
        staticBounds.merge(state.bounds);
        staticLayers |= state.layerBit;
    }else{
        dynamicBounds.merge(state.bounds);
        dynamicLayers |= state.layerBit;
    }
    if (state.cameraDependent)
        cameraLayers |= state.layerBit;
    return changed;
}

void ShadowCasterRegistry::endUpdate(){
    size_t kept = 0;
    for (size_t i = 0; i < liveEntities.size(); i++){
        const uint32_t entity = liveEntities[i];
        Record& record = records[entity];
        if (record.seen != updateCount){
            addChange(&record.state, nullptr);
            record.live = false;
            liveCount--;
            continue;
        }
        liveEntities[kept++] = entity;
    }
    liveEntities.resize(kept);
}

void ShadowCasterRegistry::clear(){
    records.clear();
    liveEntities.clear();
    changes.clear();
    bounds = ShadowBounds();
    staticBounds = ShadowBounds();
    dynamicBounds = ShadowBounds();
    staticLayers = 0;
    dynamicLayers = 0;
    cameraLayers = 0;
    liveCount = 0;
}
