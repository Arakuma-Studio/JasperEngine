// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// World values right after decoded GameObjects arrive. Scene and prefab files hold no world
// values (scene format 4, docs/specs/S29-version-control.md R20): a decode leaves them to the
// transform pass, which runs when a Scene view draws. What reads them before that (Lua and MCP
// in Edit mode, picking, bounds, a bake) must not see zeros, so every place decoded transforms
// enter a registry (a scene's load, a Prefab's registry, an instantiated Prefab) opens a
// WorldTransformScope on it: when the outermost scope of the thread ends, the world values of
// every registry it named are computed (updateWorldTransforms: needUpdate stays set, so the
// render pass still does its work). Nested scopes (a scene's Prefab instances, nested Prefabs)
// run one pass, at the end.

#include "registry/EntityRegistry.h"
#include "subsystem/TransformPropagation.h"

#include <algorithm>
#include <vector>

namespace jasperengine::editor {

    class WorldTransformScope {
    public:
        explicit WorldTransformScope(EntityRegistry* registry) {
            State& state = current();
            state.depth++;
            if (registry && std::find(state.registries.begin(), state.registries.end(), registry) == state.registries.end()) {
                state.registries.push_back(registry);
            }
        }

        ~WorldTransformScope() {
            State& state = current();
            if (--state.depth > 0) return;
            std::vector<EntityRegistry*> registries;
            registries.swap(state.registries);
            for (EntityRegistry* registry : registries) updateWorldTransforms(*registry);
        }

        WorldTransformScope(const WorldTransformScope&) = delete;
        WorldTransformScope& operator=(const WorldTransformScope&) = delete;

    private:
        struct State {
            int depth = 0;
            std::vector<EntityRegistry*> registries;
        };

        static State& current() {
            static thread_local State state;
            return state;
        }
    };

}
