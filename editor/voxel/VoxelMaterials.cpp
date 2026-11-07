// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelMaterials.h"
#include "component/VoxelVolumeComponent.h"

#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/PropertyCmd.h"
#include "command/type/VoxelEditCmd.h"
#include "subsystem/VoxelSystem.h"

#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    using VoxelSlotList = std::vector<VoxelMaterialSlot>;

    VoxelVolumeComponent* slotOwnerComponent(Project* project, uint32_t sceneId, Entity owner, std::string* error) {
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        VoxelVolumeComponent* component = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<VoxelVolumeComponent>(owner) : nullptr;
        if (!component && error) *error = "That GameObject has no Voxel Volume.";
        return component;
    }

    PropertyCmd<VoxelSlotList>* slotsCommand(Project* project, uint32_t sceneId, Entity owner, const VoxelSlotList& slots) {
        return new PropertyCmd<VoxelSlotList>(project, sceneId, owner, ComponentType::VoxelVolumeComponent, "materialSlots", slots);
    }

    bool validSlotIndex(const VoxelVolumeComponent& component, int index, std::string* error) {
        if (index >= 0 && index < static_cast<int>(component.materialSlots.size())) return true;
        if (error) *error = "There is no material slot " + std::to_string(index) + ".";
        return false;
    }

    // slots stay where they are: the last goes, with any empty ones it leaves at the end
    void clearSlotKeepingIndices(VoxelSlotList& slots, int index) {
        slots[static_cast<size_t>(index)] = VoxelMaterialSlot{};
        while (!slots.empty() && isVoxelSlotEmpty(slots.back())) slots.pop_back();
    }

}

uint16_t editor::voxelSlotsInUse(const voxel::Store& store) {
    const voxel::Int3 count = store.blockCount(0);
    uint16_t used = 0;
    const auto addMaterial = [&used](uint16_t material) {
        used = static_cast<uint16_t>(used | (1u << voxel::materialSlotA(material)));
        if (voxel::materialWeightB(material)) used = static_cast<uint16_t>(used | (1u << voxel::materialSlotB(material)));
    };
    for (int z = 0; z < count.z; z++) {
        for (int y = 0; y < count.y; y++) {
            for (int x = 0; x < count.x; x++) {
                const voxel::BlockContent content = store.readBlock(voxel::BlockKey{0, x, y, z});
                if (content.sdfIsUniform() && content.sdfValueIfUniform() >= voxel::SDF_UNITS_PER_VOXEL) continue;
                if (content.materialIsUniform()) {
                    addMaterial(content.materialValueIfUniform());
                    continue;
                }
                for (int i = 0; i < voxel::BLOCK_SAMPLES; i++) {
                    // within a voxel of the surface a mesh vertex can read it
                    if (content.sdfAt(i) < voxel::SDF_UNITS_PER_VOXEL) addMaterial(content.materialAt(i));
                }
            }
        }
    }
    return used;
}

bool editor::isVoxelSlotEmpty(const VoxelMaterialSlot& slot) {
    return slot.name.empty() && slot.linkedTerrainLayer == -1 && slot.layer == TerrainSurfaceLayer{};
}

int editor::addVoxelMaterialSlot(Project* project, uint32_t sceneId, Entity owner, const std::string& name, std::string* error) {
    const VoxelVolumeComponent* component = slotOwnerComponent(project, sceneId, owner, error);
    if (!component) return -1;
    VoxelSlotList slots = component->materialSlots;
    int index = -1;
    for (size_t i = 0; i < slots.size(); i++) {
        if (isVoxelSlotEmpty(slots[i])) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index < 0) {
        if (slots.size() >= static_cast<size_t>(voxel::MAX_SLOTS)) {
            if (error) *error = "A volume has at most 16 material slots.";
            return -1;
        }
        index = static_cast<int>(slots.size());
        slots.emplace_back();
    }
    slots[static_cast<size_t>(index)].name = name;
    CommandHandle::get(sceneId)->addCommandNoMerge(slotsCommand(project, sceneId, owner, slots));
    return index;
}

bool editor::removeVoxelMaterialSlot(Project* project, uint32_t sceneId, Entity owner, int index, std::string* error) {
    const VoxelVolumeComponent* component = slotOwnerComponent(project, sceneId, owner, error);
    if (!component || !validSlotIndex(*component, index, error)) return false;
    std::shared_ptr<VoxelSystem> system = project->getScene(sceneId)->scene->findSystem<VoxelSystem>();
    const voxel::Store* store = system ? system->getStore(owner) : nullptr;
    if (store && (voxelSlotsInUse(*store) & (1u << index))) {
        if (error) *error = "Slot " + std::to_string(index) + " is in use in this volume. Use Replace With to move those voxels to another slot first.";
        return false;
    }
    VoxelSlotList slots = component->materialSlots;
    clearSlotKeepingIndices(slots, index);
    CommandHandle::get(sceneId)->addCommandNoMerge(slotsCommand(project, sceneId, owner, slots));
    return true;
}

bool editor::replaceVoxelMaterialSlot(Project* project, uint32_t sceneId, Entity owner, int index, int replaceWith, std::string* error) {
    const VoxelVolumeComponent* component = slotOwnerComponent(project, sceneId, owner, error);
    if (!component || !validSlotIndex(*component, index, error) || !validSlotIndex(*component, replaceWith, error)) return false;
    if (index == replaceWith) {
        if (error) *error = "A slot cannot replace itself.";
        return false;
    }
    std::shared_ptr<VoxelSystem> system = project->getScene(sceneId)->scene->findSystem<VoxelSystem>();
    voxel::Store* store = system ? system->getStore(owner) : nullptr;
    if (!store) {
        if (error) *error = "The volume is not loaded.";
        return false;
    }

    const auto replaced = [&](uint16_t material) {
        int a = voxel::materialSlotA(material), b = voxel::materialSlotB(material), weight = voxel::materialWeightB(material);
        if (a == index) a = replaceWith;
        if (b == index) b = replaceWith;
        if (a == b) weight = 0;
        return voxel::packMaterial(a, weight ? b : 0, weight);
    };

    // Every level holds its own samples, so coarse meshes change with the fine ones
    std::vector<voxel::SlotChange> delta;
    for (int level = 0; level < store->levels(); level++) {
        const voxel::Int3 count = store->blockCount(level);
        for (int z = 0; z < count.z; z++) {
            for (int y = 0; y < count.y; y++) {
                for (int x = 0; x < count.x; x++) {
                    const voxel::BlockKey key{level, x, y, z};
                    const voxel::BlockContent content = store->readBlock(key);
                    voxel::Slot after;
                    if (content.materialIsUniform()) {
                        const uint16_t material = content.materialValueIfUniform();
                        const uint16_t changed = replaced(material);
                        if (changed == material) continue;
                        if (content.isFullyUniform()) {
                            voxel::BlockContent next = content;
                            next.materialUniform = changed;
                            after.setContent(next, true, true);
                        }
                    }
                    if (!content.isFullyUniform()) {
                        std::vector<int16_t> sdf(voxel::BLOCK_SAMPLES);
                        std::vector<uint16_t> material(voxel::BLOCK_SAMPLES);
                        bool any = false;
                        for (int i = 0; i < voxel::BLOCK_SAMPLES; i++) {
                            sdf[static_cast<size_t>(i)] = content.sdfAt(i);
                            const uint16_t value = content.materialAt(i);
                            material[static_cast<size_t>(i)] = replaced(value);
                            any = any || material[static_cast<size_t>(i)] != value;
                        }
                        if (!any) continue;
                        after.setContent(voxel::makeContent(sdf.data(), material.data()), true, true);
                    }
                    delta.push_back(voxel::SlotChange{key, store->slot(key), std::move(after)});
                }
            }
        }
    }

    VoxelSlotList slots = component->materialSlots;
    clearSlotKeepingIndices(slots, index);
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t mark = history->mark();
    if (!delta.empty()) {
        system->applyDelta(owner, delta, false);
        history->addCommandNoMerge(new VoxelEditCmd(project, sceneId, owner, "Replace Material Slot", std::move(delta)));
    }
    history->addCommandNoMerge(slotsCommand(project, sceneId, owner, slots));
    history->groupSince(mark, "Replace Material Slot");
    return true;
}

bool editor::linkVoxelMaterialSlot(Project* project, uint32_t sceneId, Entity owner, int index, int terrainLayer, std::string* error) {
    const VoxelVolumeComponent* component = slotOwnerComponent(project, sceneId, owner, error);
    if (!component || !validSlotIndex(*component, index, error)) return false;
    VoxelSlotList slots = component->materialSlots;
    slots[static_cast<size_t>(index)].linkedTerrainLayer = terrainLayer;
    CommandHandle::get(sceneId)->addCommandNoMerge(slotsCommand(project, sceneId, owner, slots));
    return true;
}

bool editor::setVoxelMaterialSlot(Project* project, uint32_t sceneId, Entity owner, int index, const VoxelMaterialSlot& value, bool merge, std::string* error) {
    const VoxelVolumeComponent* component = slotOwnerComponent(project, sceneId, owner, error);
    if (!component || !validSlotIndex(*component, index, error)) return false;
    VoxelSlotList slots = component->materialSlots;
    slots[static_cast<size_t>(index)] = value;
    CommandHistory* history = CommandHandle::get(sceneId);
    if (merge) history->addCommand(slotsCommand(project, sceneId, owner, slots));
    else history->addCommandNoMerge(slotsCommand(project, sceneId, owner, slots));
    return true;
}
