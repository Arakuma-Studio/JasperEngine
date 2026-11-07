// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "EntityClipboard.h"
#include "component/ActionComponent.h"
#include "component/Transform.h"
#include "component/UIComponent.h"
#include "component/PrefabComponent.h"
#include "component/MeshComponent.h"
#include "pose/PoseEditSession.h"

#include "Project.h"
#include "Stream.h"
#include "Out.h"
#include "command/CommandHandle.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/PasteEntitiesCmd.h"
#include "util/CameraTextureLink.h"
#include "util/ProjectUtils.h"
#include "util/BackgroundRelease.h"

#include "imgui.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <cstring>
#include <chrono>
#include <optional>
#include <unordered_set>

using namespace jasperengine;

namespace {

    using Kind = editor::EntityClipboard::Kind;

    constexpr int CLIPBOARD_VERSION = 1;

    // Parsed clipboard text of one kind
    struct Entry {
        std::string text;
        Kind kind = Kind::None;
        std::string project;
        editor::ComponentType componentType = editor::ComponentType::Transform;
        std::shared_ptr<const editor::GameObjectsClip> objects;
        std::shared_ptr<const editor::ComponentClip> component;
    };

    // What this editor last wrote, and the last text of ours it read from the system
    // clipboard (another instance's copy). Kept so neither is parsed twice.
    Entry ownEntry;
    Entry readEntry;

    // What the system clipboard held when last probed
    Kind probedKind = Kind::None;
    std::string probedProject;
    editor::ComponentType probedComponentType = editor::ComponentType::Transform;
    std::shared_ptr<const editor::ComponentClip> probedComponent;
    bool probedOnce = false;

    bool writingClipboard = false;
    bool appFocusLost = false;
    void (*originalSetClipboardText)(ImGuiContext*, const char*) = nullptr;
    // Bumped by every clipboard write that is not ours
    uint64_t foreignWrites = 0;

    // The text of the last copy, emitted by a Background job: YAML text of a thousand GameObjects
    // takes as long to write as they take to encode. Until it lands, the copy is kept in
    // ownEntry and is the clipboard for this editor.
    struct PendingText {
        jobs::Task<std::string> text;
        uint64_t foreignWrites = 0;
    };
    std::optional<PendingText> pendingText;

    // Replaces an entry, the copy it held freed on the background thread
    void replaceEntry(Entry& entry, Entry replacement) {
        if (entry.objects) {
            editor::BackgroundRelease::release(std::move(entry.objects));
        }
        if (entry.component) {
            editor::BackgroundRelease::release(std::move(entry.component));
        }
        entry = std::move(replacement);
    }

    // Any other SetClipboardText (a text field, the Console) replaces our data
    void setClipboardTextHook(ImGuiContext* ctx, const char* text) {
        if (!writingClipboard) {
            probedKind = Kind::None;
            probedComponent.reset();
            foreignWrites++;
        }
        if (originalSetClipboardText) {
            originalSetClipboardText(ctx, text);
        }
    }

    void setProbed(const Entry* entry) {
        probedOnce = true;
        if (!entry || entry->kind == Kind::None) {
            probedKind = Kind::None;
            probedProject.clear();
            probedComponent.reset();
            return;
        }
        probedKind = entry->kind;
        probedProject = entry->project;
        probedComponentType = entry->componentType;
        probedComponent = entry->component;
    }

    bool sameText(const Entry& entry, const char* text, size_t length) {
        return entry.kind != Kind::None && entry.text.size() == length &&
            std::memcmp(entry.text.data(), text, length) == 0;
    }

    std::unordered_map<Entity, std::string> decodeExternalNames(const YAML::Node& node) {
        std::unordered_map<Entity, std::string> names;
        if (node && node.IsMap()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                names[it->first.as<Entity>()] = it->second.as<std::string>();
            }
        }
        return names;
    }

    YAML::Node encodeExternalNames(const std::unordered_map<Entity, std::string>& names) {
        // Sorted, so the same copy always gives the same text
        std::vector<std::pair<Entity, std::string>> sorted(names.begin(), names.end());
        std::sort(sorted.begin(), sorted.end());
        YAML::Node node(YAML::NodeType::Map);
        for (const auto& [entity, name] : sorted) {
            node[entity] = name;
        }
        return node;
    }

    // Parses clipboard text of ours; kind None when it is not, or is damaged
    Entry parseText(const char* text, size_t length) {
        Entry entry;
        entry.text.assign(text, length);
        try {
            YAML::Node doc = YAML::Load(entry.text);
            if (!doc.IsMap() || !doc["jasperengine_clipboard"] ||
                doc["jasperengine_clipboard"].as<int>() != CLIPBOARD_VERSION) {
                return entry;
            }
            const std::string kind = doc["kind"] ? doc["kind"].as<std::string>() : std::string();
            const std::string project = doc["project"] ? doc["project"].as<std::string>() : std::string();
            const uint32_t sceneId = doc["scene"] ? doc["scene"].as<uint32_t>() : 0;

            if (kind == "gameobjects") {
                auto clip = std::make_shared<editor::GameObjectsClip>();
                clip->project = project;
                clip->sceneId = sceneId;
                clip->sceneName = doc["scene_name"] ? doc["scene_name"].as<std::string>() : std::string();
                clip->roots = doc["roots"];
                if (!clip->roots || !clip->roots.IsSequence() || clip->roots.size() == 0) {
                    return entry;
                }
                clip->selectedRoots = std::min<size_t>(clip->roots.size(),
                    doc["selected_roots"] ? doc["selected_roots"].as<size_t>() : clip->roots.size());
                if (doc["prefab_instances"] && doc["prefab_instances"].IsSequence()) {
                    for (const YAML::Node& instanceNode : doc["prefab_instances"]) {
                        editor::ClipboardPrefabInstance instance;
                        instance.path = instanceNode["path"].as<std::string>();
                        instance.root = instanceNode["root"].as<Entity>();
                        for (const YAML::Node& member : instanceNode["members"]) {
                            instance.members.emplace_back(member[0].as<Entity>(), member[1].as<Entity>());
                        }
                        clip->prefabInstances.push_back(std::move(instance));
                    }
                }
                clip->externalNames = decodeExternalNames(doc["external"]);
                entry.project = project;
                entry.objects = std::move(clip);
                entry.kind = Kind::GameObjects;
            } else if (kind == "component") {
                const std::string componentName = doc["component"] ? doc["component"].as<std::string>() : std::string();
                const editor::ComponentType type = editor::Catalog::getComponentType(componentName);
                if (componentName.empty() || editor::Catalog::getComponentName(type, true) != componentName ||
                    !doc["value"] || !doc["value"].IsMap()) {
                    return entry;
                }
                auto clip = std::make_shared<editor::ComponentClip>();
                clip->project = project;
                clip->sceneId = sceneId;
                clip->type = type;
                clip->entity = doc["entity"] ? doc["entity"].as<Entity>() : NULL_ENTITY;
                clip->value = doc["value"];
                clip->externalNames = decodeExternalNames(doc["external"]);
                entry.project = project;
                entry.componentType = type;
                entry.component = std::move(clip);
                entry.kind = Kind::Component;
            }
        } catch (const std::exception& e) {
            editor::Out::warning("Ignoring damaged JasperEngine clipboard data: %s", e.what());
            entry.kind = Kind::None;
            entry.objects.reset();
            entry.component.reset();
        }
        return entry;
    }

    // Puts the text of the last copy on the system clipboard once the worker wrote it;
    // `wait` blocks for it (someone is about to read the clipboard or the copy's YAML)
    void flushPendingText(bool wait) {
        if (!pendingText) {
            return;
        }
        if (!wait && !pendingText->text.isReady()) {
            return;
        }
        std::string text = pendingText->text.get();
        const bool stillOurs = pendingText->foreignWrites == foreignWrites;
        pendingText.reset();
        if (!stillOurs) {
            // Something else was copied meanwhile and is the clipboard now
            replaceEntry(ownEntry, Entry());
            return;
        }
        ownEntry.text = std::move(text);
        writingClipboard = true;
        ImGui::SetClipboardText(ownEntry.text.c_str());
        writingClipboard = false;
    }

    // The clipboard as parsed now; null when it holds nothing of ours
    const Entry* readClipboard() {
        flushPendingText(true);
        const char* text = ImGui::GetClipboardText();
        const Entry* result = nullptr;
        if (text) {
            const size_t headerLength = std::strlen(editor::EntityClipboard::HEADER);
            if (std::strncmp(text, editor::EntityClipboard::HEADER, headerLength) == 0) {
                const size_t length = std::strlen(text);
                if (sameText(ownEntry, text, length)) {
                    result = &ownEntry;
                } else if (readEntry.text.size() == length && std::memcmp(readEntry.text.data(), text, length) == 0) {
                    result = readEntry.kind != Kind::None ? &readEntry : nullptr;
                } else {
                    replaceEntry(readEntry, parseText(text, length));
                    result = readEntry.kind != Kind::None ? &readEntry : nullptr;
                }
            }
        }
        setProbed(result);
        return result;
    }

    // The doc shares its nodes with the copy's snapshot, which the main thread does not
    // change until flushPendingText has joined the worker (every paste goes through
    // readClipboard). A Cut's delete snapshot is the same nodes, only read meanwhile: const
    // reads of yaml-cpp nodes change nothing the emitter reads.
    void writeClipboard(Entry entry, YAML::Node doc) {
        flushPendingText(true);

        PendingText pending;
        pending.foreignWrites = foreignWrites;
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "clipboard-emit";
        pending.text = jobs::submit([doc]() {
            YAML::Emitter out;
            out << doc;
            std::string text;
            text.reserve(out.size() + std::strlen(editor::EntityClipboard::HEADER) + 2);
            text = editor::EntityClipboard::HEADER;
            text += '\n';
            text.append(out.c_str(), out.size());
            text += '\n';
            return text;
        }, options);
        pendingText = std::move(pending);

        replaceEntry(ownEntry, std::move(entry));
        setProbed(&ownEntry);
    }

    // Hierarchy order: the flat (Transform-less) GameObjects first, as the Hierarchy lists
    // them, then Transform order
    void sortAsHierarchy(Scene* scene, const std::vector<Entity>& tracked, std::vector<Entity>& entities) {
        std::unordered_map<Entity, size_t> order;
        order.reserve(entities.size());
        std::unordered_set<Entity> wanted(entities.begin(), entities.end());
        size_t flatIndex = 0;
        for (Entity entity : tracked) {
            if (wanted.count(entity) && !scene->findComponent<Transform>(entity)) {
                order[entity] = flatIndex;
            }
            flatIndex++;
        }
        auto transforms = scene->getComponentArray<Transform>();
        for (Entity entity : entities) {
            if (scene->findComponent<Transform>(entity)) {
                order[entity] = tracked.size() + transforms->getIndex(entity);
            }
        }
        std::stable_sort(entities.begin(), entities.end(), [&order](Entity a, Entity b) {
            return order[a] < order[b];
        });
    }

    // An entity and its Transform descendants, which follow it in the Transform array
    void collectBranch(Scene* scene, Entity root, std::vector<Entity>& out) {
        out.push_back(root);
        if (!scene->findComponent<Transform>(root)) {
            return;
        }
        auto transforms = scene->getComponentArray<Transform>();
        std::unordered_set<Entity> branch{root};
        for (size_t i = transforms->getIndex(root) + 1; i < transforms->size(); i++) {
            if (!branch.count(transforms->getComponentFromIndex(i).parent)) {
                break;
            }
            Entity entity = transforms->getEntity(i);
            branch.insert(entity);
            out.push_back(entity);
        }
    }

    const editor::Prefab::Instance* clipboardInstanceRootedAt(editor::Project* project, Scene* scene, uint32_t sceneId, Entity entity, std::filesystem::path* path = nullptr) {
        const PrefabComponent* component = scene->findComponent<PrefabComponent>(entity);
        if (!component || component->path.empty()) {
            return nullptr;
        }
        const editor::Prefab* prefab = project->getPrefab(component->path);
        const editor::Prefab::Instance* instance = prefab ? prefab->getInstance(sceneId, entity) : nullptr;
        if (!instance || instance->rootEntity != entity) {
            return nullptr;
        }
        if (path) {
            *path = component->path;
        }
        return instance;
    }

    // Everything a copy root brings along: its Transform branch and the members of the prefab
    // instances in it
    std::vector<Entity> collectCoverage(editor::Project* project, Scene* scene, uint32_t sceneId, Entity root) {
        std::vector<Entity> coverage;
        collectBranch(scene, root, coverage);
        std::unordered_set<Entity> seen(coverage.begin(), coverage.end());
        for (size_t i = 0; i < coverage.size(); i++) {
            if (!clipboardInstanceRootedAt(project, scene, sceneId, coverage[i])) {
                continue;
            }
            // every level rooted there (an instance of a Prefab Variant roots its bases' too)
            for (const editor::PrefabInstanceLevel& level : project->getInstanceLevels(project->getScene(sceneId), coverage[i])) {
                for (const editor::Prefab::EntityMember& member : level.instance->members) {
                    if (scene->isEntityCreated(member.localEntity) && seen.insert(member.localEntity).second) {
                        coverage.push_back(member.localEntity);
                    }
                }
            }
        }
        return coverage;
    }

    bool isLocalReference(uint32_t referenceSceneId, uint32_t sceneId) {
        return referenceSceneId == 0 || referenceSceneId == sceneId;
    }

    // Names of the entities outside `covered` that the components of `entity` reference
    void collectExternalNames(Scene* scene, uint32_t sceneId, Entity entity, const std::vector<editor::ComponentType>& types,
                              const std::unordered_set<Entity>& covered, std::unordered_map<Entity, std::string>& names) {
        for (editor::ComponentType type : types) {
            if (!editor::Catalog::mayHoldReferences(scene, entity, type, true, true)) {
                continue;
            }
            for (auto& [propertyName, property] : editor::Catalog::findEntityProperties(scene, entity, type)) {
                if (!property.ref) {
                    continue;
                }
                Entity target = NULL_ENTITY;
                if (property.type == editor::PropertyType::Entity) {
                    target = *static_cast<Entity*>(property.ref);
                } else if (property.type == editor::PropertyType::EntityReference) {
                    const EntityReference* reference = static_cast<EntityReference*>(property.ref);
                    if (!isLocalReference(reference->sceneId, sceneId)) {
                        continue;
                    }
                    target = reference->entity;
                } else if (property.type == editor::PropertyType::Texture) {
                    // Render to texture: the texture names its camera (or its fluid)
                    target = editor::CameraTextureLink::parseLink(static_cast<Texture*>(property.ref)->getId());
                } else {
                    continue;
                }
                if (target != NULL_ENTITY && !covered.count(target) && scene->isEntityCreated(target)) {
                    names.emplace(target, scene->getEntityName(target));
                }
            }
        }
    }

}

std::string editor::EntityClipboard::projectKey(const Project* project) {
    return project ? project->getProjectPath().lexically_normal().generic_string() : std::string();
}

YAML::Node editor::EntityClipboard::encodeComponent(EntityRegistry* registry, Entity entity, ComponentType type) {
    // A generated or model-backed mesh is saved without its geometry because its source
    // rebuilds it, but a copy may land where nothing does, so it always carries it
    if (type == ComponentType::MeshComponent) {
        return Stream::encodeMeshComponent(registry->getComponent<MeshComponent>(entity));
    }
    if (type == ComponentType::UIComponent) {
        return Stream::encodeUIComponent(registry->getComponent<UIComponent>(entity));
    }
    YAML::Node components = Stream::encodeComponents(entity, registry, Catalog::componentTypeToSignature(registry, type));
    YAML::Node value = components[Catalog::getComponentName(type, true)];
    return value ? value : YAML::Node(YAML::NodeType::Map);
}

YAML::Node editor::EntityClipboard::encodeDefaultComponent(ComponentType type){
    EntityRegistry scratch;
    Entity entity = scratch.createUserEntity();
    std::vector<Entity> tracked{entity};
    ProjectUtils::addEntityComponent(&scratch, entity, type, tracked);
    return encodeComponent(&scratch, entity, type);
}

void editor::EntityClipboard::applyComponentValue(EntityRegistry* registry, Entity entity, ComponentType type, YAML::Node value){
    value = YAML::Clone(value);
    if (!value.IsMap()) {
        value = YAML::Node(YAML::NodeType::Map);
    }

    // The decoder keeps a field whose key is absent, and the encoder leaves out some
    // fields at their default (an empty list, no custom shader). Such a key the live
    // component writes gets its empty value, so the result is exactly `value`.
    const YAML::Node current = encodeComponent(registry, entity, type);
    const YAML::Node& constValue = value;
    YAML::Node defaults;
    bool defaultsLoaded = false;
    for (auto it = current.begin(); it != current.end(); ++it) {
        const std::string key = it->first.as<std::string>();
        if (constValue[key]) {
            continue;
        }
        if (it->second.IsSequence()) {
            value[key] = YAML::Node(YAML::NodeType::Sequence);
        } else if (it->second.IsMap()) {
            value[key] = YAML::Node(YAML::NodeType::Map);
        } else if (it->second.IsScalar()) {
            if (!defaultsLoaded) {
                defaults = encodeDefaultComponent(type);
                defaultsLoaded = true;
            }
            const YAML::Node& constDefaults = defaults;
            value[key] = constDefaults[key] ? YAML::Clone(constDefaults[key]) : YAML::Node(std::string());
        }
    }

    YAML::Node components;
    components[Catalog::getComponentName(type, true)] = value;
    Transform* transform = registry->findComponent<Transform>(entity);
    Stream::decodeComponents(entity, transform ? transform->parent : NULL_ENTITY, registry, components);

    // The decoder flags the properties that changed. Data beside them (geometry, frames,
    // tiles) is not a property, so the component is refreshed as if each had been edited.
    uint64_t updateFlags = 0;
    for (const auto& [propertyName, property] : Catalog::findEntityProperties(registry, entity, type)) {
        updateFlags |= property.updateFlags;
    }
    Catalog::updateEntity(registry, entity, updateFlags & ~static_cast<uint64_t>(UpdateFlags_SceneWide));
}

std::vector<Entity> editor::EntityClipboard::getCopyableSelection(Project* project, uint32_t sceneId) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return {};
    }
    Scene* scene = sceneProject->scene;
    std::unordered_set<Entity> tracked(sceneProject->entities.begin(), sceneProject->entities.end());
    std::vector<Entity> selection;
    for (Entity entity : sceneProject->selectedEntities) {
        // Only authored GameObjects: what a running scene made is gone after Stop
        if (tracked.count(entity) && scene->isEntityCreated(entity)) {
            selection.push_back(entity);
        }
    }
    sortAsHierarchy(scene, sceneProject->entities, selection);
    return selection;
}

bool editor::EntityClipboard::copyGameObjects(Project* project, uint32_t sceneId, const std::vector<Entity>& requested) {
    return copyGameObjects(project, sceneId, requested, nullptr);
}

std::shared_ptr<editor::GameObjectsClip> editor::EntityClipboard::snapshotGameObjects(Project* project, uint32_t sceneId,
                                                                                     const std::vector<Entity>& entities) {
    std::shared_ptr<GameObjectsClip> clip;
    if (!copyGameObjects(project, sceneId, entities, nullptr, &clip, false)) {
        return nullptr;
    }
    return clip;
}

bool editor::EntityClipboard::copyGameObjects(Project* project, uint32_t sceneId, const std::vector<Entity>& requested,
                                              std::unordered_map<Entity, YAML::Node>* encodedRoots,
                                              std::shared_ptr<GameObjectsClip>* clipOut, bool write) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return false;
    }
    // a posed character goes back to its rest bones first: they are what gets copied
    PoseEditSession::suspendActiveFor(sceneId, requested);
    Scene* scene = sceneProject->scene;

    std::unordered_set<Entity> tracked(sceneProject->entities.begin(), sceneProject->entities.end());
    std::vector<Entity> ordered;
    ordered.reserve(requested.size());
    {
        std::unordered_set<Entity> seen;
        for (Entity entity : requested) {
            if (tracked.count(entity) && scene->isEntityCreated(entity) && seen.insert(entity).second) {
                ordered.push_back(entity);
            }
        }
    }
    if (ordered.empty()) {
        return false;
    }
    sortAsHierarchy(scene, sceneProject->entities, ordered);

    // A selected child is copied with its selected parent. The same goes for what a
    // selected prefab instance holds, whatever order the two were selected in.
    std::vector<Entity> candidates = Project::getTopLevelEntities(scene, ordered);
    std::vector<std::vector<Entity>> coverages;
    coverages.reserve(candidates.size());
    std::unordered_map<Entity, size_t> coveringCount;
    for (Entity candidate : candidates) {
        coverages.push_back(collectCoverage(project, scene, sceneId, candidate));
        for (Entity entity : coverages.back()) {
            coveringCount[entity]++;
        }
    }

    std::vector<Entity> roots;
    std::vector<Entity> covered;
    std::unordered_set<Entity> coveredSet;
    for (size_t i = 0; i < candidates.size(); i++) {
        if (coveringCount[candidates[i]] > 1) {
            continue; // Brought along by another copied GameObject
        }
        roots.push_back(candidates[i]);
        for (Entity entity : coverages[i]) {
            if (coveredSet.insert(entity).second) {
                covered.push_back(entity);
            }
        }
    }

    // What a model or a UI control generated is copied only with its owner (Select All
    // takes a model's animations along with the model)
    for (Entity root : roots) {
        Entity modelOwner = ProjectUtils::getModelBranchOwner(scene, root);
        if (modelOwner != NULL_ENTITY && !coveredSet.count(modelOwner)) {
            Out::warning("Cannot copy '%s': it is part of an imported model. Copy the whole model instead.",
                scene->getEntityName(root).c_str());
            return false;
        }
        Entity owner = ProjectUtils::getLockedEntityParent(scene, root);
        if (owner != NULL_ENTITY && !coveredSet.count(owner)) {
            Out::warning("Cannot copy '%s': it belongs to '%s'. Copy '%s' instead.",
                scene->getEntityName(root).c_str(), scene->getEntityName(owner).c_str(), scene->getEntityName(owner).c_str());
            return false;
        }
    }

    // The actions animating copied GameObjects (listed under them in the Hierarchy) and the
    // entities copied components own go along, each as one more root
    const size_t userRoots = roots.size();
    auto actions = scene->getComponentArray<ActionComponent>();
    size_t scanned = 0;
    bool grew = true;
    while (grew) {
        grew = false;
        auto addRoot = [&](Entity entity) {
            if (entity == NULL_ENTITY || coveredSet.count(entity) || !tracked.count(entity) || !scene->isEntityCreated(entity)) {
                return;
            }
            roots.push_back(entity);
            for (Entity coveredEntity : collectCoverage(project, scene, sceneId, entity)) {
                if (coveredSet.insert(coveredEntity).second) {
                    covered.push_back(coveredEntity);
                }
            }
            grew = true;
        };
        for (size_t i = 0; i < actions->size(); i++) {
            Entity action = actions->getEntity(i);
            if (coveredSet.count(action)) {
                continue;
            }
            Entity target = ProjectUtils::getVirtualParent(scene, action);
            if (target != NULL_ENTITY && coveredSet.count(target)) {
                addRoot(action);
            }
        }
        for (; scanned < covered.size(); scanned++) {
            Entity entity = covered[scanned];
            for (ComponentType type : {ComponentType::ActionComponent, ComponentType::AnimationComponent}) {
                for (Entity owned : ProjectUtils::getOwnedCascadeEntities(scene, entity, type)) {
                    addRoot(owned);
                }
            }
        }
    }

    YAML::Node doc;
    doc["jasperengine_clipboard"] = CLIPBOARD_VERSION;
    doc["kind"] = "gameobjects";
    doc["project"] = projectKey(project);
    doc["scene"] = sceneId;
    doc["scene_name"] = sceneProject->name;

    YAML::Node rootsNode(YAML::NodeType::Sequence);
    for (Entity root : roots) {
        // A prefab member copied on its own is a plain GameObject, the prefab instances
        // below it still paste as instances
        const bool member = !clipboardInstanceRootedAt(project, scene, sceneId, root) &&
            !project->findPrefabPathFor(sceneId, root).empty();
        YAML::Node encoded = member ?
            Stream::encodeEntity(root, scene, project, sceneProject, true) :
            Stream::encodeEntity(root, scene, project, sceneProject);
        if (encodedRoots && !member) {
            encodedRoots->emplace(root, encoded);
        }
        rootsNode.push_back(encoded);
    }
    doc["selected_roots"] = userRoots;
    doc["roots"] = rootsNode;

    auto clip = std::make_shared<GameObjectsClip>();
    clip->project = projectKey(project);
    clip->sceneId = sceneId;
    clip->sceneName = sceneProject->name;
    clip->roots = rootsNode;
    clip->selectedRoots = userRoots;

    YAML::Node instancesNode(YAML::NodeType::Sequence);
    std::vector<std::tuple<Entity, std::filesystem::path, const Prefab::Instance*>> levels;
    for (Entity entity : covered) {
        if (!clipboardInstanceRootedAt(project, scene, sceneId, entity)) {
            continue;
        }
        // outer levels first (a variant's, then its bases')
        for (const PrefabInstanceLevel& level : project->getInstanceLevels(sceneProject, entity)) {
            levels.emplace_back(entity, level.path, level.instance);
        }
    }
    for (const auto& [entity, path, instance] : levels) {
        ClipboardPrefabInstance snapshot;
        snapshot.path = path;
        snapshot.root = entity;
        YAML::Node instanceNode;
        instanceNode["path"] = path.generic_string();
        instanceNode["root"] = entity;
        YAML::Node membersNode(YAML::NodeType::Sequence);
        for (const Prefab::EntityMember& member : instance->members) {
            snapshot.members.emplace_back(member.localEntity, member.registryEntity);
            YAML::Node pair(YAML::NodeType::Sequence);
            pair.SetStyle(YAML::EmitterStyle::Flow);
            pair.push_back(member.localEntity);
            pair.push_back(member.registryEntity);
            membersNode.push_back(pair);
        }
        instanceNode["members"] = membersNode;
        instancesNode.push_back(instanceNode);
        clip->prefabInstances.push_back(std::move(snapshot));
    }
    if (instancesNode.size() > 0) {
        doc["prefab_instances"] = instancesNode;
    }

    for (Entity entity : covered) {
        collectExternalNames(scene, sceneId, entity, Catalog::findComponents(scene, entity), coveredSet, clip->externalNames);
    }
    if (!clip->externalNames.empty()) {
        doc["external"] = encodeExternalNames(clip->externalNames);
    }

    if (clipOut) {
        *clipOut = clip;
    }
    if (!write) {
        return true;
    }

    Entry entry;
    entry.kind = Kind::GameObjects;
    entry.project = clip->project;
    entry.objects = clip;
    writeClipboard(std::move(entry), doc);

    const size_t extra = roots.size() - userRoots;
    if (extra > 0) {
        Out::info("Copied %zu GameObject%s (and %zu animation GameObject%s they need)", userRoots, userRoots == 1 ? "" : "s",
            extra, extra == 1 ? "" : "s");
    } else {
        Out::info("Copied %zu GameObject%s", userRoots, userRoots == 1 ? "" : "s");
    }
    return true;
}

bool editor::EntityClipboard::cutGameObjects(Project* project, uint32_t sceneId, const std::vector<Entity>& entities) {
    std::unordered_map<Entity, YAML::Node> encodedRoots;
    if (!copyGameObjects(project, sceneId, entities, &encodedRoots)) {
        return false;
    }
    // The delete command handles what the copy took along (branches, instances, actions).
    // What it snapshots was just encoded for the copy, unchanged since: it takes those.
    std::vector<Entity> roots = Project::getTopLevelEntities(project->getScene(sceneId)->scene, entities);
    DeleteEntityCmd* deleteCmd = new DeleteEntityCmd(project, sceneId, roots);
    deleteCmd->setPreEncoded(std::move(encodedRoots));
    CommandHandle::get(sceneId)->addCommandNoMerge(deleteCmd);
    return true;
}

bool editor::EntityClipboard::pasteGameObjects(Project* project, uint32_t sceneId, bool asChild) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return false;
    }
    Scene* scene = sceneProject->scene;

    // The bottom-most selected GameObject in Hierarchy order anchors the paste
    Entity anchor = NULL_ENTITY;
    auto transforms = scene->getComponentArray<Transform>();
    std::unordered_set<Entity> tracked(sceneProject->entities.begin(), sceneProject->entities.end());
    size_t anchorIndex = 0;
    for (Entity entity : sceneProject->selectedEntities) {
        if (!tracked.count(entity) || !scene->findComponent<Transform>(entity)) {
            continue;
        }
        size_t index = transforms->getIndex(entity);
        if (anchor == NULL_ENTITY || index > anchorIndex) {
            anchor = entity;
            anchorIndex = index;
        }
    }

    if (asChild) {
        if (anchor == NULL_ENTITY) {
            Out::warning("Paste As Child needs a selected GameObject with a Transform");
            return false;
        }
        return pasteGameObjects(project, sceneId, Placement::Into, anchor);
    }
    return pasteGameObjects(project, sceneId, anchor == NULL_ENTITY ? Placement::Root : Placement::After, anchor);
}

bool editor::EntityClipboard::pasteGameObjects(Project* project, uint32_t sceneId, Placement placement, Entity anchor) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene) {
        return false;
    }

    const Entry* entry = readClipboard();
    if (!entry || entry->kind != Kind::GameObjects) {
        return false;
    }
    if (entry->project != projectKey(project)) {
        Out::warning("The copied GameObjects belong to another project and cannot be pasted here");
        return false;
    }

    CommandHandle::get(sceneId)->addCommandNoMerge(new PasteEntitiesCmd(project, sceneId, entry->objects, placement, anchor));
    return true;
}

bool editor::EntityClipboard::copyComponent(Project* project, uint32_t sceneId, Entity entity, ComponentType type) {
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject || !sceneProject->scene || type == ComponentType::PrefabComponent) {
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (!scene->isEntityCreated(entity) || !scene->getSignature(entity).test(Catalog::getComponentId(scene, type))) {
        return false;
    }

    auto clip = std::make_shared<ComponentClip>();
    clip->project = projectKey(project);
    clip->sceneId = sceneId;
    clip->type = type;
    clip->entity = entity;
    clip->value = encodeComponent(scene, entity, type);
    collectExternalNames(scene, sceneId, entity, {type}, {entity}, clip->externalNames);

    YAML::Node doc;
    doc["jasperengine_clipboard"] = CLIPBOARD_VERSION;
    doc["kind"] = "component";
    doc["project"] = clip->project;
    doc["scene"] = sceneId;
    doc["component"] = Catalog::getComponentName(type, true);
    doc["entity"] = entity;
    if (!clip->externalNames.empty()) {
        doc["external"] = encodeExternalNames(clip->externalNames);
    }
    doc["value"] = clip->value;

    Entry entry;
    entry.kind = Kind::Component;
    entry.project = clip->project;
    entry.componentType = type;
    entry.component = clip;
    writeClipboard(std::move(entry), doc);
    return true;
}

editor::EntityClipboard::Kind editor::EntityClipboard::getKind(const Project* project) {
    if (!probedOnce) {
        refresh();
    }
    if (probedKind == Kind::None || probedProject != projectKey(project)) {
        return Kind::None;
    }
    return probedKind;
}

bool editor::EntityClipboard::hasComponent(const Project* project, ComponentType* type) {
    if (getKind(project) != Kind::Component) {
        return false;
    }
    if (type) {
        *type = probedComponentType;
    }
    return true;
}

void editor::EntityClipboard::finishPendingWrite() {
    flushPendingText(true);
}

void editor::EntityClipboard::refresh() {
    readClipboard();
}

void editor::EntityClipboard::update() {
    flushPendingText(false);

    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    if (platformIO.Platform_SetClipboardTextFn && platformIO.Platform_SetClipboardTextFn != &setClipboardTextHook) {
        originalSetClipboardText = platformIO.Platform_SetClipboardTextFn;
        platformIO.Platform_SetClipboardTextFn = &setClipboardTextHook;
    }

    // Back from another application or editor instance: the clipboard may have changed
    const bool focusLost = ImGui::GetIO().AppFocusLost;
    if (appFocusLost && !focusLost) {
        refresh();
    }
    appFocusLost = focusLost;
}

std::shared_ptr<const editor::GameObjectsClip> editor::EntityClipboard::readGameObjects(const Project* project) {
    const Entry* entry = readClipboard();
    if (!entry || entry->kind != Kind::GameObjects || entry->project != projectKey(project)) {
        return nullptr;
    }
    return entry->objects;
}

std::shared_ptr<const editor::ComponentClip> editor::EntityClipboard::peekComponent(const Project* project) {
    if (getKind(project) != Kind::Component) {
        return nullptr;
    }
    return probedComponent;
}

std::shared_ptr<const editor::ComponentClip> editor::EntityClipboard::readComponent(const Project* project) {
    const Entry* entry = readClipboard();
    if (!entry || entry->kind != Kind::Component || entry->project != projectKey(project)) {
        return nullptr;
    }
    return entry->component;
}

uint64_t editor::EntityClipboard::remapReferences(Scene* scene, Entity entity, ComponentType type, const ClipboardRemap& remap,
                                                  const std::vector<std::string>& skip) {
    uint64_t updateFlags = 0;

    // A reference to an entity outside the copy survives only in the scene it was copied
    // from, and only while that entity is still the one copied (same id, same name)
    auto resolveLocal = [&](Entity value) -> Entity {
        if (value == NULL_ENTITY) {
            return NULL_ENTITY;
        }
        if (remap.map) {
            auto it = remap.map->find(value);
            if (it != remap.map->end()) {
                return it->second;
            }
        }
        if (!remap.sameScene || !scene->isEntityCreated(value) || !remap.externalNames) {
            return NULL_ENTITY;
        }
        auto name = remap.externalNames->find(value);
        if (name == remap.externalNames->end() || name->second != scene->getEntityName(value)) {
            return NULL_ENTITY;
        }
        return value;
    };

    if (!Catalog::mayHoldReferences(scene, entity, type, true, true)) {
        return 0;
    }
    for (auto& [propertyName, property] : Catalog::findEntityProperties(scene, entity, type)) {
        if (!property.ref) {
            continue;
        }
        bool skipped = false;
        for (const std::string& name : skip) {
            // An aggregate name also skips its indexed fields ("actions" covers "actions[0].action")
            if (propertyName.compare(0, name.size(), name) == 0 &&
                (propertyName.size() == name.size() || propertyName[name.size()] == '[' || propertyName[name.size()] == '.')) {
                skipped = true;
                break;
            }
        }
        if (skipped) {
            continue;
        }

        bool changed = false;
        if (property.type == PropertyType::Entity) {
            Entity& value = *static_cast<Entity*>(property.ref);
            Entity resolved = resolveLocal(value);
            changed = resolved != value;
            value = resolved;
        } else if (property.type == PropertyType::EntityReference) {
            EntityReference& reference = *static_cast<EntityReference*>(property.ref);
            if (isLocalReference(reference.sceneId, remap.sourceSceneId)) {
                EntityReference resolved{resolveLocal(reference.entity), 0};
                // Keep the explicit scene id form it was stored with, now naming this scene
                if (resolved.entity != NULL_ENTITY && reference.sceneId != 0) {
                    resolved.sceneId = remap.targetSceneId;
                }
                changed = resolved.entity != reference.entity || resolved.sceneId != reference.sceneId;
                reference = resolved;
            } else if (reference.sceneId == remap.targetSceneId && reference.entity != NULL_ENTITY &&
                       !scene->isEntityCreated(reference.entity)) {
                // Pointed into this scene from another one, at an entity that is gone
                reference = EntityReference{NULL_ENTITY, 0};
                changed = true;
            }
            // Any other scene id names a third scene, which the paste does not change
        } else if (property.type == PropertyType::Texture) {
            // A render-to-texture link names its camera (or its fluid) like any other reference
            Texture& texture = *static_cast<Texture*>(property.ref);
            Entity linked = CameraTextureLink::parseLink(texture.getId());
            if (linked != NULL_ENTITY) {
                Entity resolved = resolveLocal(linked);
                if (resolved == NULL_ENTITY) {
                    texture = Texture();
                    changed = true;
                } else {
                    // Bound to the framebuffer of the camera (or fluid output) it names now
                    Framebuffer* framebuffer = CameraTextureLink::linkFramebuffer(scene, texture.getId(), resolved);
                    if (resolved != linked || texture.getFramebuffer() != framebuffer) {
                        CameraTextureLink::retarget(scene, texture, resolved); // keeps the kind of link
                        changed = true;
                    }
                }
            }
        }
        if (changed) {
            updateFlags |= property.updateFlags;
        }
    }

    return updateFlags;
}

uint64_t editor::EntityClipboard::bindCameraTextures(EntityRegistry* registry, Entity entity, ComponentType type) {
    uint64_t updateFlags = 0;
    if (!Catalog::mayHoldReferences(registry, entity, type, false, true)) {
        return updateFlags;
    }
    for (auto& [propertyName, property] : Catalog::findEntityProperties(registry, entity, type)) {
        if (property.type != PropertyType::Texture || !property.ref) {
            continue;
        }
        Texture* texture = static_cast<Texture*>(property.ref);
        Entity linked = CameraTextureLink::parseLink(texture->getId());
        if (linked == NULL_ENTITY) {
            continue;
        }
        // a camera's framebuffer, or a fluid's output
        Framebuffer* framebuffer = CameraTextureLink::linkFramebuffer(registry, texture->getId(), linked);
        if (texture->getFramebuffer() != framebuffer) {
            const std::string id = texture->getId();
            texture->setFramebuffer(framebuffer); // clears the id
            texture->setId(id);
            updateFlags |= property.updateFlags;
        }
    }
    return updateFlags;
}
