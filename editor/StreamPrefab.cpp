// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Prefab instance serialization: the per-instance data written next to a prefab root
// (prefabOverrides, prefabRemovedGameObjects, prefabLocalEntities), the decode hook
// that rebuilds an instance from it, and the PrefabComponent codec.

#include "Stream.h"
#include "Catalog.h"

#include <algorithm>
#include <unordered_set>

using namespace jasperengine;

namespace {
    // Removed GameObject overrides of an instance and of the instances nested in it, as
    // `prefabRemovedGameObjects` entries relative to it: {registryEntity} for its own, and
    // for a nested instance's the nested root's prefabPath and prefabRootRegistryEntity,
    // with the inner entry under `nested` once more than one level down.
    void encodeRemovedGameObjects(const editor::Project* project, uint32_t sceneId, const EntityRegistry* registry,
                                  const editor::Prefab::Instance& instance, YAML::Node& entries, size_t depth = 0) {
        std::vector<Entity> removed;
        removed.reserve(instance.removed.size());
        for (const auto& [registryEntity, members] : instance.removed) {
            removed.push_back(registryEntity);
        }
        std::sort(removed.begin(), removed.end());
        for (Entity registryEntity : removed) {
            YAML::Node entry;
            entry["registryEntity"] = registryEntity;
            entries.push_back(entry);
        }

        if (depth > 64) {
            return; // a nesting cycle, which importPrefab refuses anyway
        }
        for (const editor::Prefab::EntityMember& member : instance.members) {
            if (!registry->isEntityCreated(member.localEntity) ||
                !registry->getSignature(member.localEntity).test(registry->getComponentId<PrefabComponent>())) {
                continue;
            }
            const PrefabComponent* component = &registry->getComponent<PrefabComponent>(member.localEntity);
            const editor::Prefab* nestedPrefab = component && !component->path.empty() ? project->getPrefab(component->path) : nullptr;
            const editor::Prefab::Instance* nestedInstance = nestedPrefab ? nestedPrefab->getInstance(sceneId, member.localEntity) : nullptr;
            if (!nestedInstance || nestedInstance->rootEntity != member.localEntity) {
                continue;
            }
            YAML::Node nestedEntries(YAML::NodeType::Sequence);
            encodeRemovedGameObjects(project, sceneId, registry, *nestedInstance, nestedEntries, depth + 1);
            for (const YAML::Node& nestedEntry : nestedEntries) {
                YAML::Node entry;
                if (nestedEntry["prefabPath"]) {
                    entry["prefabPath"] = component->path;
                    entry["prefabRootRegistryEntity"] = member.registryEntity;
                    entry["nested"] = nestedEntry;
                } else {
                    entry["registryEntity"] = nestedEntry["registryEntity"];
                    entry["prefabPath"] = component->path;
                    entry["prefabRootRegistryEntity"] = member.registryEntity;
                }
                entries.push_back(entry);
            }
        }
    }
}

void editor::Stream::encodePrefabInstance(YAML::Node& entityNode, const Entity entity, const EntityRegistry* registry,
        const Project* project, const SceneProject* sceneProject, const Prefab* prefab) {
    auto sceneIt = prefab->instances.find(sceneProject->id);
    if (sceneIt != prefab->instances.end()) {
        for (const auto& instance : sceneIt->second) {
            if (instance.rootEntity != entity) continue;

            // Encode component overrides keyed by registryEntity
            YAML::Node overridesNode;
            for (const auto& member : instance.members) {
                auto overrideIt = instance.overrides.find(member.localEntity);
                if (overrideIt != instance.overrides.end() && overrideIt->second != 0) {
                    YAML::Node entry;
                    entry["registryEntity"] = member.registryEntity;
                    Signature sig = Catalog::componentMaskToSignature(registry, overrideIt->second);
                    YAML::Node overrideComps = encodeComponents(member.localEntity, registry, sig);
                    if (overrideComps.IsMap() && overrideComps.size() > 0) {
                        entry["components"] = overrideComps;
                    }
                    overridesNode.push_back(entry);
                }
            }

            // Encode nested prefab overrides (tagged with prefabPath)
            for (const auto& member : instance.members) {
                if (!registry->getSignature(member.localEntity).test(registry->getComponentId<PrefabComponent>())) continue;
                const PrefabComponent& nestedBC = registry->getComponent<PrefabComponent>(member.localEntity);
                if (nestedBC.path.empty()) continue;

                const Prefab* nestedPrefab = project->getPrefab(nestedBC.path);
                if (!nestedPrefab) continue;

                auto nestedSceneIt = nestedPrefab->instances.find(sceneProject->id);
                if (nestedSceneIt == nestedPrefab->instances.end()) continue;

                for (const auto& nestedInst : nestedSceneIt->second) {
                    if (nestedInst.rootEntity != member.localEntity) continue;

                    for (const auto& nestedMember : nestedInst.members) {
                        auto nOverrideIt = nestedInst.overrides.find(nestedMember.localEntity);
                        if (nOverrideIt != nestedInst.overrides.end() && nOverrideIt->second != 0) {
                            YAML::Node entry;
                            entry["registryEntity"] = nestedMember.registryEntity;
                            entry["prefabPath"] = nestedBC.path;
                            entry["prefabRootRegistryEntity"] = member.registryEntity;
                            Signature sig = Catalog::componentMaskToSignature(registry, nOverrideIt->second);
                            YAML::Node overrideComps = encodeComponents(nestedMember.localEntity, registry, sig);
                            if (overrideComps.IsMap() && overrideComps.size() > 0) {
                                entry["components"] = overrideComps;
                            }
                            overridesNode.push_back(entry);
                        }
                    }
                    break;
                }
            }

            if (overridesNode.size() > 0) {
                entityNode["prefabOverrides"] = overridesNode;
            }

            // Prefab GameObjects this instance (or one nested in it) left out
            YAML::Node removedNode(YAML::NodeType::Sequence);
            encodeRemovedGameObjects(project, sceneProject->id, registry, instance, removedNode);
            if (removedNode.size() > 0) {
                entityNode["prefabRemovedGameObjects"] = removedNode;
            }

            // Encode scene-specific local entities (children of prefab members/root not in members list)
            YAML::Node localEntsNode;
            std::unordered_set<Entity> memberSet;
            memberSet.insert(instance.rootEntity);
            for (const auto& m : instance.members) {
                memberSet.insert(m.localEntity);

                // Also add nested prefab members to memberSet
                if (registry->getSignature(m.localEntity).test(registry->getComponentId<PrefabComponent>())) {
                    const PrefabComponent& nbc = registry->getComponent<PrefabComponent>(m.localEntity);
                    if (!nbc.path.empty()) {
                        const Prefab* nb = project->getPrefab(nbc.path);
                        if (nb) {
                            auto nsi = nb->instances.find(sceneProject->id);
                            if (nsi != nb->instances.end()) {
                                for (const auto& ni : nsi->second) {
                                    if (ni.rootEntity == m.localEntity) {
                                        for (const auto& nm : ni.members) {
                                            memberSet.insert(nm.localEntity);
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Check root and each member for non-member children
            // Skip nested prefab roots (their local entities are handled separately)
            std::vector<std::pair<Entity, Entity>> parentCandidates; // {parentLocal, parentRegistry}
            parentCandidates.push_back({instance.rootEntity, NULL_ENTITY});
            for (const auto& m : instance.members) {
                if (registry->getSignature(m.localEntity).test(registry->getComponentId<PrefabComponent>())) {
                    continue;
                }
                parentCandidates.push_back({m.localEntity, m.registryEntity});
            }

            for (const auto& [parentLocal, parentReg] : parentCandidates) {
                if (!registry->getSignature(parentLocal).test(registry->getComponentId<Transform>())) continue;
                auto transforms = registry->getComponentArray<Transform>();
                size_t parentIdx = transforms->getIndex(parentLocal);
                size_t childPos = 0;
                for (size_t ti = parentIdx + 1; ti < transforms->size(); ti++) {
                    Transform& t = transforms->getComponentFromIndex(ti);
                    Entity childEnt = transforms->getEntity(ti);
                    if (t.parent != parentLocal) {
                        if (t.parent == NULL_ENTITY) break;
                        // Skip deeper descendants
                        continue;
                    }
                    if (memberSet.find(childEnt) == memberSet.end()) {
                        // This is a local entity
                        YAML::Node localEntNode = encodeEntity(childEnt, registry, project, sceneProject);
                        localEntNode["parentRegistryEntity"] = parentReg;
                        localEntNode["childIndex"] = childPos;
                        localEntsNode.push_back(localEntNode);
                    }
                    childPos++;
                }
            }

            // Encode nested prefab local entities (tagged with prefabPath)
            for (const auto& member : instance.members) {
                if (!registry->getSignature(member.localEntity).test(registry->getComponentId<PrefabComponent>())) continue;
                const PrefabComponent& nestedBCLoc = registry->getComponent<PrefabComponent>(member.localEntity);
                if (nestedBCLoc.path.empty()) continue;

                const Prefab* nestedPrefab = project->getPrefab(nestedBCLoc.path);
                if (!nestedPrefab) continue;

                auto nestedSceneIt = nestedPrefab->instances.find(sceneProject->id);
                if (nestedSceneIt == nestedPrefab->instances.end()) continue;

                for (const auto& nestedInst : nestedSceneIt->second) {
                    if (nestedInst.rootEntity != member.localEntity) continue;

                    std::unordered_set<Entity> nestedMemberSet;
                    nestedMemberSet.insert(nestedInst.rootEntity);
                    for (const auto& nm : nestedInst.members) {
                        nestedMemberSet.insert(nm.localEntity);
                    }

                    std::vector<std::pair<Entity, Entity>> nestedParentCandidates;
                    nestedParentCandidates.push_back({nestedInst.rootEntity, NULL_ENTITY});
                    for (const auto& nm : nestedInst.members) {
                        nestedParentCandidates.push_back({nm.localEntity, nm.registryEntity});
                    }

                    for (const auto& [nParentLocal, nParentReg] : nestedParentCandidates) {
                        if (!registry->getSignature(nParentLocal).test(registry->getComponentId<Transform>())) continue;
                        auto transforms = registry->getComponentArray<Transform>();
                        size_t nParentIdx = transforms->getIndex(nParentLocal);
                        size_t nChildPos = 0;
                        for (size_t ti = nParentIdx + 1; ti < transforms->size(); ti++) {
                            Transform& t = transforms->getComponentFromIndex(ti);
                            Entity childEnt = transforms->getEntity(ti);
                            if (t.parent != nParentLocal) {
                                if (t.parent == NULL_ENTITY) break;
                                continue;
                            }
                            if (nestedMemberSet.find(childEnt) == nestedMemberSet.end()) {
                                YAML::Node localEntNode = encodeEntity(childEnt, registry, project, sceneProject);
                                localEntNode["parentRegistryEntity"] = nParentReg;
                                localEntNode["childIndex"] = nChildPos;
                                localEntNode["prefabPath"] = nestedBCLoc.path;
                                localEntNode["prefabRootRegistryEntity"] = member.registryEntity;
                                localEntsNode.push_back(localEntNode);
                            }
                            nChildPos++;
                        }
                    }
                    break;
                }
            }

            if (localEntsNode.size() > 0) {
                entityNode["prefabLocalEntities"] = localEntsNode;
            }

            break;
        }
    }
}

void editor::Stream::decodePrefabInstance(const YAML::Node& entityNode, Entity entity, EntityRegistry* registry,
        std::vector<Entity>* entities, Project* project, SceneProject* sceneProject,
        std::unordered_map<Entity, Entity>* entityRemap, std::vector<Entity>& allEntities) {
    // If entity has PrefabComponent, import prefab children from its path
    if (project && sceneProject) {
        PrefabComponent* prefabComp = registry->findComponent<PrefabComponent>(entity);
        if (prefabComp && !prefabComp->path.empty()) {
            std::vector<Entity> prefabEntities = project->importPrefab(
                sceneProject, entities, prefabComp->path, entity, false,
                entityNode["prefabOverrides"], entityNode["prefabLocalEntities"], entityRemap,
                entityNode["prefabRemovedGameObjects"]);
            allEntities.insert(allEntities.end(), prefabEntities.begin(), prefabEntities.end());
        }
    }
}

// ==============================
// PrefabComponent
// ==============================

YAML::Node editor::Stream::encodePrefabComponent(const PrefabComponent& prefab) {
    YAML::Node node;
    node["name"] = prefab.name;
    node["path"] = prefab.path;
    return node;
}

PrefabComponent editor::Stream::decodePrefabComponent(const YAML::Node& node, const PrefabComponent* oldPrefab) {
    PrefabComponent prefab;

    if (oldPrefab) {
        prefab = *oldPrefab;
    }

    if (node["name"]) prefab.name = node["name"].as<std::string>();
    if (node["path"]) prefab.path = node["path"].as<std::string>();

    return prefab;
}
