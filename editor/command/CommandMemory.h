// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "util/YamlFootprint.h"

#include <cstddef>

namespace jasperengine::editor{

    // Command::memoryBytes estimates of the prefab bookkeeping commands keep for undo: the
    // YAML trees they hold (the bulk) plus their records
    namespace CommandMemory{

        inline size_t of(const NodeRecovery& recovery){
            size_t bytes = 0;
            for (const auto& [key, entry] : recovery){
                bytes += sizeof(entry) + key.size() + yamlFootprint(entry.node) +
                    entry.members.size() * sizeof(Prefab::EntityMember) +
                    entry.overrides.size() * sizeof(Prefab::ObjectOverrides);
            }
            return bytes;
        }

        inline size_t of(const PrefabUndoState& state){
            size_t bytes = 0;
            for (const PrefabUndoState::Asset& asset : state.assets){
                bytes += sizeof(asset) + yamlFootprint(asset.node);
            }
            for (const PrefabUndoState::InstanceState& instance : state.instances){
                bytes += sizeof(instance) + yamlFootprint(instance.rootNode);
            }
            return bytes;
        }

        inline size_t of(const PrefabBranchRecovery& recovery){
            return sizeof(recovery) + yamlFootprint(recovery.node) +
                recovery.members.size() * sizeof(Prefab::EntityMember) +
                recovery.overrides.size() * sizeof(Prefab::ObjectOverrides);
        }

    }

}
