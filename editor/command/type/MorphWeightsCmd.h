// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "command/Command.h"
#include "command/CommandNames.h"
#include "command/PrefabEditUndo.h"
#include "Project.h"
#include "Catalog.h"
#include "animation/ClipRecordHooks.h"
#include "component/MeshComponent.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace jasperengine::editor{

    // The Inspector's Blend Shapes rows (animation clips T3c): the morph target weights of several
    // GameObjects' meshes in one undo step per slider drag (mergeWith). Header only.
    // - Its writes notify the channel write listeners (ClipRecordHooks::before/afterComponentWrite):
    //   a clip preview puts the weights back to their rest first and takes the new ones as its
    //   rest after. The new weights were read from what the Scene view shows: a weight the edit
    //   leaves as shown keeps its rest (a previewed weight is never baked into the scene, T3c
    //   review C1); decided on the first execute and kept for redo.
    // - Prefab instances: each changed weight is an override ("morphWeights[i]", a Catalog
    //   property) and undo restores the overrides.
    class MorphWeightsCmd : public Command{
    public:
        using Weights = std::array<float, MAX_MORPHTARGETS>;
        struct Target{
            Entity entity = NULL_ENTITY;
            Weights before{};
            Weights after{};
        };

        MorphWeightsCmd(Project* project, uint32_t sceneId, std::vector<Target> targets)
            : project(project), sceneId(sceneId), targets(std::move(targets)){}

        // the weights the edit sets on purpose (the row's weight): the new rest of a clip
        // preview even when equal to what it shows; the others follow "left as shown"
        void setEditedWeights(std::vector<size_t> weights){ edited = std::move(weights); }

        bool execute() override{
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject || !sceneProject->scene) return false;
            if (!executed) wasModified = sceneProject->isModified;
            Scene* scene = sceneProject->scene;
            // what the Scene view shows (a preview's weights), before the rest is put back
            std::vector<Weights> shown(targets.size());
            for (size_t t = 0; t < targets.size(); t++){
                if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(targets[t].entity)){
                    std::copy(mesh->morphWeights, mesh->morphWeights + MAX_MORPHTARGETS, shown[t].begin());
                }
            }
            for (const Target& target : targets){
                ClipRecordHooks::beforeComponentWrite(sceneId, target.entity, ComponentType::MeshComponent, "");
            }
            for (size_t t = 0; t < targets.size(); t++){
                Target& target = targets[t];
                MeshComponent* mesh = scene->findComponent<MeshComponent>(target.entity);
                if (!mesh) continue;
                if (!executed){
                    // the weights it replaces are the rest
                    std::copy(mesh->morphWeights, mesh->morphWeights + MAX_MORPHTARGETS, target.before.begin());
                    for (size_t i = 0; i < (size_t)MAX_MORPHTARGETS; i++){
                        if (std::find(edited.begin(), edited.end(), i) != edited.end()) continue;
                        if (target.after[i] == shown[t][i] && target.before[i] != shown[t][i]) target.after[i] = target.before[i];
                    }
                }
                prefabUndo.capture(project, sceneId, target.entity);
                std::copy(target.after.begin(), target.after.end(), mesh->morphWeights);
                std::vector<std::string> changed;
                for (size_t i = 0; i < (size_t)MAX_MORPHTARGETS; i++){
                    if (target.after[i] != target.before[i]) changed.push_back("morphWeights[" + std::to_string(i) + "]");
                }
                if (!changed.empty()) project->recordPrefabEdit(sceneId, target.entity, ComponentType::MeshComponent, changed);
            }
            {
                ClipRecordHooks::ExplicitWrite write;
                write.components = edited;
                ClipRecordHooks::ExplicitWriteScope scope(write);
                for (const Target& target : targets){
                    ClipRecordHooks::afterComponentWrite(sceneId, target.entity, ComponentType::MeshComponent, "");
                }
            }
            executed = true;
            sceneProject->isModified = true;
            return true;
        }

        void undo() override{
            SceneProject* sceneProject = project->getScene(sceneId);
            if (!sceneProject || !sceneProject->scene) return;
            Scene* scene = sceneProject->scene;
            for (const Target& target : targets){
                ClipRecordHooks::beforeComponentWrite(sceneId, target.entity, ComponentType::MeshComponent, "");
            }
            for (const Target& target : targets){
                if (MeshComponent* mesh = scene->findComponent<MeshComponent>(target.entity)){
                    std::copy(target.before.begin(), target.before.end(), mesh->morphWeights);
                }
            }
            prefabUndo.restore(project, sceneId);
            // undo writes the weights it replaced: they are the rest again
            ClipRecordHooks::ExplicitWrite write;
            write.all = true;
            ClipRecordHooks::ExplicitWriteScope scope(write);
            for (const Target& target : targets){
                ClipRecordHooks::afterComponentWrite(sceneId, target.entity, ComponentType::MeshComponent, "");
            }
            sceneProject->isModified = wasModified;
        }

        bool mergeWith(Command* other) override{
            MorphWeightsCmd* older = dynamic_cast<MorphWeightsCmd*>(other);
            if (!older || older->project != project || older->sceneId != sceneId || older->targets.size() != targets.size()) return false;
            for (size_t i = 0; i < targets.size(); i++){
                if (older->targets[i].entity != targets[i].entity) return false;
            }
            // the older command holds the weights (and overrides) a single undo restores
            for (size_t i = 0; i < targets.size(); i++) targets[i].before = older->targets[i].before;
            for (size_t weight : older->edited){
                if (std::find(edited.begin(), edited.end(), weight) == edited.end()) edited.push_back(weight);
            }
            prefabUndo.mergeOlder(older->prefabUndo);
            wasModified = older->wasModified;
            return true;
        }

        bool affectsStructure() const override{ return false; }

        // "Set Blend Shapes (Hero)" (S24)
        std::string describe() const override{
            std::vector<Entity> entities;
            for (const Target& target : targets) entities.push_back(target.entity);
            return "Set Blend Shapes (" + CommandNames::entities(project, sceneId, entities) + ")";
        }
        const char* typeName() const override{ return "MorphWeightsCmd"; }

        const std::vector<Target>& getTargets() const{ return targets; }

    private:
        Project* project;
        uint32_t sceneId;
        std::vector<Target> targets;
        bool wasModified = false;
        bool executed = false;
        std::vector<size_t> edited;
        PrefabEditUndo prefabUndo;
    };

}
