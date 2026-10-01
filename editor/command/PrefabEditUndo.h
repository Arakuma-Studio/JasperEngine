// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"

#include <map>
#include <optional>

namespace jasperengine::editor{

    // The prefab overrides of the GameObjects an edit command touches, as they were before
    // its first run: Unity's instance edits record overrides (Project::recordPrefabEdit and
    // friends), and their undo puts the overrides back with the values. Merged commands keep
    // the oldest state (mergeOlder), like their old values.
    class PrefabEditUndo{
    private:
        // nullopt: not a GameObject of a prefab instance when captured
        std::map<Entity, std::optional<Prefab::ObjectOverrides>> saved;

    public:
        // Before the edit; a later call for the same GameObject keeps the first state
        void capture(Project* project, uint32_t sceneId, Entity entity){
            if (saved.count(entity)){
                return;
            }
            Prefab::ObjectOverrides overrides;
            if (project->getPrefabEditOverrides(sceneId, entity, overrides)){
                saved[entity] = overrides;
            }else{
                saved[entity] = std::nullopt;
            }
        }

        // After the edit was undone
        void restore(Project* project, uint32_t sceneId) const{
            for (const auto& [entity, overrides] : saved){
                if (overrides){
                    project->setPrefabEditOverrides(sceneId, entity, *overrides);
                }
            }
        }

        void restore(Project* project, uint32_t sceneId, Entity entity) const{
            auto it = saved.find(entity);
            if (it != saved.end() && it->second){
                project->setPrefabEditOverrides(sceneId, entity, *it->second);
            }
        }

        // `older` ran before this command: its states are the ones from before both
        void mergeOlder(const PrefabEditUndo& older){
            for (const auto& [entity, overrides] : older.saved){
                saved[entity] = overrides;
            }
        }

        void clear(){
            saved.clear();
        }
    };

}
