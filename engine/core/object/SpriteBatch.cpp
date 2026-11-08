// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "SpriteBatch.h"

#include "Log.h"
#include "component/SpriteBatchComponent.h"
#include "component/SpriteComponent.h"
#include "render/sprite/SpriteInstanceBatch.h"
#include "sokol/SokolSystem.h"
#include "sprite/SpriteSheet.h"
#include "sprite/SpriteSheetPool.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

SpriteBatch::SpriteBatch(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (!scene->findComponent<SpriteComponent>(entity)){
        Log::error("A Sprite Batch goes on a GameObject with a Sprite (add a Sprite first)");
    }
    if (!scene->findComponent<SpriteBatchComponent>(entity)){
        scene->addComponent<SpriteBatchComponent>(entity, {});
    }
}

SpriteBatch::~SpriteBatch(){
}

SpriteBatch::SpriteBatch(const SpriteBatch& rhs) = default;
SpriteBatch& SpriteBatch::operator=(const SpriteBatch& rhs) = default;

void SpriteBatch::setEnabled(bool enabled){
    getComponent<SpriteBatchComponent>().enabled = enabled;
    markChanged<SpriteBatchComponent>();
}

bool SpriteBatch::isEnabled() const{
    return getComponent<SpriteBatchComponent>().enabled;
}

void SpriteBatch::setMode(SpriteBatchMode mode){
    getComponent<SpriteBatchComponent>().mode = mode;
    markChanged<SpriteBatchComponent>();
}

SpriteBatchMode SpriteBatch::getMode() const{
    return getComponent<SpriteBatchComponent>().mode;
}

void SpriteBatch::setSpace(SpriteBatchSpace space){
    SpriteBatchComponent& component = getComponent<SpriteBatchComponent>();
    component.space = space;
    if (component.state)
        component.state->revision++;
    markChanged<SpriteBatchComponent>();
}

SpriteBatchSpace SpriteBatch::getSpace() const{
    return getComponent<SpriteBatchComponent>().space;
}

void SpriteBatch::setCapacity(uint32_t capacity){
    getComponent<SpriteBatchComponent>().capacity = std::max<uint32_t>(capacity, 1);
    markChanged<SpriteBatchComponent>();
}

uint32_t SpriteBatch::getCapacity() const{
    return getComponent<SpriteBatchComponent>().capacity;
}

void SpriteBatch::setSortInstances(bool sortInstances){
    SpriteBatchComponent& component = getComponent<SpriteBatchComponent>();
    component.sortInstances = sortInstances;
    if (component.state)
        component.state->revision++;
    markChanged<SpriteBatchComponent>();
}

bool SpriteBatch::isSortInstances() const{
    return getComponent<SpriteBatchComponent>().sortInstances;
}

uint32_t SpriteBatch::getCount() const{
    const SpriteBatchComponent& component = getComponent<SpriteBatchComponent>();
    return component.state ? component.state->count : 0;
}

int SpriteBatch::getFrameCount() const{
    const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
    if (!sprite)
        return 0;
    if (!sprite->sheet.empty()){
        std::shared_ptr<const SpriteSheetData> sheet = (sprite->sheetData && sprite->loadedSheet == sprite->sheet.path)
            ? sprite->sheetData : SpriteSheetPool::get(sprite->sheet.path);
        return sheet ? sheet->frameCount() : -1;
    }
    // a legacy sprite: its frames, else the one texture rect it shows
    return std::max<int>((int)sprite->numFramesRect, 1);
}

int SpriteBatch::frameId(const std::string& name) const{
    const SpriteComponent* sprite = scene->findComponent<SpriteComponent>(entity);
    if (!sprite)
        return -1;
    if (!sprite->sheet.empty()){
        std::shared_ptr<const SpriteSheetData> sheet = (sprite->sheetData && sprite->loadedSheet == sprite->sheet.path)
            ? sprite->sheetData : SpriteSheetPool::get(sprite->sheet.path);
        return sheet ? sheet->findFrame(name) : -1;
    }
    for (unsigned int i = 0; i < sprite->numFramesRect; i++){
        if (sprite->framesRect[i].name == name)
            return (int)i;
    }
    return -1;
}

int SpriteBatch::fxSlot(const std::string& name){
    if (name == "palette") return (int)SpriteFxSlot::PALETTE;
    if (name == "flash") return (int)SpriteFxSlot::FLASH;
    if (name == "fade") return (int)SpriteFxSlot::FADE;
    if (name == "dissolve") return (int)SpriteFxSlot::DISSOLVE;
    return -1;
}

SpriteBatchState& SpriteBatch::writeState(){
    SpriteBatchComponent& component = getComponent<SpriteBatchComponent>();
    if (!component.state)
        component.state = std::make_shared<SpriteBatchState>();
    SpriteBatchState& state = *component.state;
    // Clear Each Frame: the first write after a drawn frame starts this frame's instances
    if (component.mode == SpriteBatchMode::CLEAR_EACH_FRAME && state.drawnFrame != 0 && !state.writtenSinceDraw &&
            SokolSystem::getCommitCount() > state.drawnFrame){
        state.clear();
    }
    return state;
}

void SpriteBatch::written(SpriteBatchState& state){
    state.markWritten();
    getComponent<SpriteBatchComponent>().count = state.count;
}

std::string SpriteBatch::checkFrames(std::span<const int32_t> frames) const{
    const int count = getFrameCount();
    if (count < 0)
        return ""; // the sheet loads later: the packer clamps
    for (int32_t frame : frames){
        if (frame < 0 || frame >= count)
            return "frame_range: frame " + std::to_string(frame) + " is out of range (the Sprite has " + std::to_string(count) +
                   " frame" + (count == 1 ? "" : "s") + ", 0 to " + std::to_string(std::max(count - 1, 0)) + ")";
    }
    return "";
}

std::string SpriteBatch::setPositions(std::span<const float> values, int stride){
    if (stride != 2 && stride != 3)
        return "stride: positions are x, y pairs (stride 2) or x, y, z triples (stride 3)";
    if (values.size() % (size_t)stride != 0)
        return "length: " + std::to_string(values.size()) + " numbers is not a whole number of " + (stride == 2 ? "x, y pairs" : "x, y, z triples");
    SpriteBatchState& state = writeState();
    const uint32_t count = (uint32_t)(values.size() / (size_t)stride);
    state.resize(count);
    for (uint32_t i = 0; i < count; i++){
        const float* p = values.data() + (size_t)i * (size_t)stride;
        state.positions[i] = Vector3(p[0], p[1], stride == 3 ? p[2] : 0.0f);
    }
    written(state);
    return "";
}

std::string SpriteBatch::setRotations(std::span<const float> radians){
    SpriteBatchState& state = writeState();
    if (radians.size() < state.count)
        return "length: " + std::to_string(radians.size()) + " rotations for " + std::to_string(state.count) + " instances";
    std::copy(radians.begin(), radians.begin() + state.count, state.rotations.begin());
    written(state);
    return "";
}

std::string SpriteBatch::setScales(std::span<const float> values, int stride){
    if (stride != 1 && stride != 2)
        return "stride: scales are uniform (stride 1) or x, y pairs (stride 2)";
    SpriteBatchState& state = writeState();
    if (values.size() < (size_t)state.count * (size_t)stride)
        return "length: " + std::to_string(values.size()) + " numbers for " + std::to_string(state.count) + " instances of stride " + std::to_string(stride);
    for (uint32_t i = 0; i < state.count; i++){
        const float* s = values.data() + (size_t)i * (size_t)stride;
        state.scales[i] = Vector2(s[0], stride == 2 ? s[1] : s[0]);
    }
    written(state);
    return "";
}

std::string SpriteBatch::setFrames(std::span<const int32_t> frames){
    SpriteBatchState& state = writeState();
    if (frames.size() < state.count)
        return "length: " + std::to_string(frames.size()) + " frames for " + std::to_string(state.count) + " instances";
    const std::string error = checkFrames(frames.first(state.count));
    if (!error.empty())
        return error;
    std::copy(frames.begin(), frames.begin() + state.count, state.frames.begin());
    written(state);
    return "";
}

std::string SpriteBatch::setColors(std::span<const float> rgba){
    SpriteBatchState& state = writeState();
    if (rgba.size() < (size_t)state.count * 4)
        return "length: " + std::to_string(rgba.size()) + " numbers for " + std::to_string(state.count) + " r, g, b, a colours";
    for (uint32_t i = 0; i < state.count; i++){
        const float* c = rgba.data() + (size_t)i * 4;
        state.colors[i] = Vector4(c[0], c[1], c[2], c[3]);
    }
    written(state);
    return "";
}

std::string SpriteBatch::setFx(const std::string& slot, std::span<const float> amounts){
    const int index = fxSlot(slot);
    if (index < 0)
        return "fx_slot: \"" + slot + "\" is not an FX slot (palette, flash, fade or dissolve)";
    SpriteBatchState& state = writeState();
    if (amounts.size() < state.count)
        return "length: " + std::to_string(amounts.size()) + " amounts for " + std::to_string(state.count) + " instances";
    if (!state.hasFx){
        state.hasFx = true;
        state.fx.assign(state.count, Vector4(0.0f, 0.0f, 0.0f, 0.0f));
    }
    // (x, y, z, w are consecutive floats)
    for (uint32_t i = 0; i < state.count; i++)
        (&state.fx[i].x)[index] = amounts[i];
    written(state);
    return "";
}

int SpriteBatch::add(float x, float y, int frame){
    const int32_t frames[1] = {frame};
    const std::string error = checkFrames(frames);
    if (!error.empty()){
        Log::error("SpriteBatch:add: %s", error.c_str());
        return -1;
    }
    SpriteBatchState& state = writeState();
    const uint32_t index = state.count;
    state.resize(index + 1);
    state.positions[index] = Vector3(x, y, 0.0f);
    state.frames[index] = frame;
    written(state);
    return (int)index;
}

bool SpriteBatch::set(uint32_t index, float x, float y){
    SpriteBatchState& state = writeState();
    if (index >= state.count)
        return false;
    state.positions[index].x = x;
    state.positions[index].y = y;
    written(state);
    return true;
}

bool SpriteBatch::setFrame(uint32_t index, int frame){
    const int32_t frames[1] = {frame};
    if (!checkFrames(frames).empty())
        return false;
    SpriteBatchState& state = writeState();
    if (index >= state.count)
        return false;
    state.frames[index] = frame;
    written(state);
    return true;
}

bool SpriteBatch::remove(uint32_t index){
    SpriteBatchState& state = writeState();
    if (index >= state.count)
        return false;
    const uint32_t last = state.count - 1;
    if (index != last){
        state.positions[index] = state.positions[last];
        state.rotations[index] = state.rotations[last];
        state.scales[index] = state.scales[last];
        state.frames[index] = state.frames[last];
        state.colors[index] = state.colors[last];
        if (state.hasFx)
            state.fx[index] = state.fx[last];
    }
    state.resize(last);
    written(state);
    return true;
}

void SpriteBatch::clear(){
    SpriteBatchState& state = writeState();
    state.clear();
    written(state);
}
