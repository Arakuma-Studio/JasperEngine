// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlayerLayerActions.h"

#include "Project.h"
#include "animation/AnimationPlayerRuntime.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    // a list that is the single default Base Layer again is stored empty (as a new player)
    std::vector<AnimationPlayerLayer> stored(std::vector<AnimationPlayerLayer> layers){
        if (layers.size() == 1 && layers[0] == AnimationPlayerLayer{anim::BASE_LAYER_NAME, 1.0f, AnimationLayerBlend::Override, "", ""}){
            layers.clear();
        }
        return layers;
    }

    bool nameTaken(const std::vector<AnimationPlayerLayer>& layers, const std::string& name, int except){
        for (size_t i = 0; i < layers.size(); i++){
            if ((int)i != except && layers[i].name == name) return true;
        }
        return false;
    }

    bool push(Project* project, uint32_t sceneId, Entity entity, const PlayerLayerActions::Result& result, std::string* error){
        SceneProject* scene = project ? project->getScene(sceneId) : nullptr;
        AnimationPlayerComponent* player = scene && scene->scene ? scene->scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
        if (!player) return fail(error, "the GameObject has no Animation Player");
        const ComponentType type = ComponentType::AnimationPlayerComponent;
        MultiPropertyCmd* command = new MultiPropertyCmd();
        command->addPropertyCmd<std::vector<AnimationPlayerLayer>>(project, sceneId, entity, type, "layers", result.layers);
        if (result.clips != player->clips){
            command->addPropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneId, entity, type, "clips", result.clips);
        }
        command->setNoMerge();
        CommandHandle::get(sceneId)->addCommand(command);
        return true;
    }

}

bool PlayerLayerActions::computeAdd(const AnimationPlayerComponent& player, const AnimationPlayerLayer& layer, Result& out, std::string* error){
    if (layer.name.empty()) return fail(error, "a layer needs a name");
    std::vector<AnimationPlayerLayer> layers = anim::playerLayers(player);
    if (nameTaken(layers, layer.name, -1)) return fail(error, "\"" + layer.name + "\" is already a layer of the player");
    AnimationPlayerLayer added = layer;
    added.weight = std::isfinite(added.weight) ? std::min(std::max(added.weight, 0.0f), 1.0f) : 1.0f;
    layers.push_back(added);
    out.index = (int)layers.size() - 1;
    out.layers = stored(layers);
    out.clips = player.clips;
    return true;
}

bool PlayerLayerActions::computeRemove(const AnimationPlayerComponent& player, int layer, Result& out, std::string* error){
    std::vector<AnimationPlayerLayer> layers = anim::playerLayers(player);
    if (layer == 0) return fail(error, "the base layer cannot be removed");
    if (layer < 0 || (size_t)layer >= layers.size()) return fail(error, "the player has no layer " + std::to_string(layer));
    layers.erase(layers.begin() + layer);
    out.clips = player.clips;
    for (AnimationPlayerClip& entry : out.clips){
        if (entry.layer == layer) entry.layer = 0;
        else if (entry.layer > layer) entry.layer--;
    }
    out.layers = stored(layers);
    out.index = -1;
    return true;
}

bool PlayerLayerActions::computeRename(const AnimationPlayerComponent& player, int layer, const std::string& name, Result& out, std::string* error){
    std::vector<AnimationPlayerLayer> layers = anim::playerLayers(player);
    if (layer < 0 || (size_t)layer >= layers.size()) return fail(error, "the player has no layer " + std::to_string(layer));
    if (name.empty()) return fail(error, "a layer needs a name");
    if (nameTaken(layers, name, layer)) return fail(error, "\"" + name + "\" is already a layer of the player");
    layers[(size_t)layer].name = name;
    out.layers = stored(layers);
    out.clips = player.clips;
    out.index = layer;
    return true;
}

bool PlayerLayerActions::computeMove(const AnimationPlayerComponent& player, int from, int to, Result& out, std::string* error){
    std::vector<AnimationPlayerLayer> layers = anim::playerLayers(player);
    const int count = (int)layers.size();
    if (from < 0 || from >= count) return fail(error, "the player has no layer " + std::to_string(from));
    if (to < 0 || to >= count) return fail(error, "the player has no layer " + std::to_string(to));
    if (from == 0 || to == 0) return fail(error, "the base layer stays first (index 0)");
    // the old index of the layer at each new index
    std::vector<int> order(count);
    for (int i = 0; i < count; i++) order[i] = i;
    order.erase(order.begin() + from);
    order.insert(order.begin() + to, from);
    std::vector<int> newIndex(count);
    std::vector<AnimationPlayerLayer> moved(count);
    for (int i = 0; i < count; i++){
        moved[i] = layers[(size_t)order[i]];
        newIndex[(size_t)order[i]] = i;
    }
    out.clips = player.clips;
    for (AnimationPlayerClip& entry : out.clips){
        if (entry.layer >= 0 && entry.layer < count) entry.layer = newIndex[(size_t)entry.layer];
    }
    out.layers = stored(moved);
    out.index = to;
    return true;
}

namespace {

    const AnimationPlayerComponent* playerOf(Project* project, uint32_t sceneId, Entity entity){
        SceneProject* scene = project ? project->getScene(sceneId) : nullptr;
        return scene && scene->scene ? scene->scene->findComponent<AnimationPlayerComponent>(entity) : nullptr;
    }

}

bool PlayerLayerActions::addLayer(Project* project, uint32_t sceneId, Entity entity, const AnimationPlayerLayer& layer, std::string* error, int* index){
    const AnimationPlayerComponent* player = playerOf(project, sceneId, entity);
    if (!player) return fail(error, "the GameObject has no Animation Player");
    Result result;
    if (!computeAdd(*player, layer, result, error) || !push(project, sceneId, entity, result, error)) return false;
    if (index) *index = result.index;
    return true;
}

bool PlayerLayerActions::removeLayer(Project* project, uint32_t sceneId, Entity entity, int layer, std::string* error){
    const AnimationPlayerComponent* player = playerOf(project, sceneId, entity);
    if (!player) return fail(error, "the GameObject has no Animation Player");
    Result result;
    return computeRemove(*player, layer, result, error) && push(project, sceneId, entity, result, error);
}

bool PlayerLayerActions::renameLayer(Project* project, uint32_t sceneId, Entity entity, int layer, const std::string& name, std::string* error){
    const AnimationPlayerComponent* player = playerOf(project, sceneId, entity);
    if (!player) return fail(error, "the GameObject has no Animation Player");
    Result result;
    return computeRename(*player, layer, name, result, error) && push(project, sceneId, entity, result, error);
}

bool PlayerLayerActions::moveLayer(Project* project, uint32_t sceneId, Entity entity, int from, int to, std::string* error){
    const AnimationPlayerComponent* player = playerOf(project, sceneId, entity);
    if (!player) return fail(error, "the GameObject has no Animation Player");
    Result result;
    return computeMove(*player, from, to, result, error) && push(project, sceneId, entity, result, error);
}
