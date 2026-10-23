// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetRefEditor.h"

#include "Out.h"
#include "Project.h"
#include "PrefabStage.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetRefSchema.h"
#include "util/Util.h"
#include "util/EntityClipboard.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // "a.b[2].c" -> {"a", "b[2]", "c"}
    std::vector<std::string> fieldParts(const std::string& field) {
        std::vector<std::string> parts;
        size_t begin = 0;
        while (begin <= field.size() && !field.empty()) {
            const size_t dot = field.find('.', begin);
            parts.push_back(field.substr(begin, dot == std::string::npos ? std::string::npos : dot - begin));
            if (dot == std::string::npos) break;
            begin = dot + 1;
        }
        return parts;
    }

    // "name[3]" -> name, 3; "name" -> name, -1
    std::string splitIndex(const std::string& part, long& index) {
        index = -1;
        const size_t open = part.find('[');
        if (open == std::string::npos || part.back() != ']') return part;
        index = std::strtol(part.c_str() + open + 1, nullptr, 10);
        return part.substr(0, open);
    }

    YAML::Node findEntityNode(const YAML::Node& list, uint64_t id) {
        if (!list || !list.IsSequence()) return YAML::Node();
        for (const YAML::Node& entity : list) {
            if (!entity.IsMap()) continue;
            if (entity["entity"] && entity["entity"].as<uint64_t>(0) == id) return entity;
            if (YAML::Node found = findEntityNode(entity["children"], id)) return found;
            if (const YAML::Node block = entity["prefabInstance"]) {
                if (YAML::Node found = findEntityNode(block["addedGameObjects"], id)) return found;
            }
        }
        return YAML::Node();
    }

    YAML::Node findOverride(const YAML::Node& block, const std::string& registryEntity) {
        if (!block || !block.IsMap()) return YAML::Node();
        if (const YAML::Node overrides = block["overrides"]) {
            for (const YAML::Node& entry : overrides) {
                if (entry["registryEntity"] && entry["registryEntity"].as<std::string>() == registryEntity) return entry;
            }
        }
        if (const YAML::Node nested = block["nested"]) {
            for (const YAML::Node& entry : nested) {
                if (YAML::Node found = findOverride(entry, registryEntity)) return found;
            }
        }
        return YAML::Node();
    }

    // The new value of one file of a shader reference
    void replaceShaderFile(std::string& value, std::string& guids, uint32_t ordinal, const std::string& hint, const Guid& guid) {
        std::string vert, frag;
        AssetRefSchema::shaderFiles(value, vert, frag);
        const size_t bar = guids.find('|');
        Guid vertGuid = Guid::parse(bar == std::string::npos ? guids : guids.substr(0, bar));
        Guid fragGuid = bar == std::string::npos ? Guid() : Guid::parse(guids.substr(bar + 1));
        if (hint.empty()) {
            value.clear();
            guids.clear();
            return;
        }
        if (ordinal == 0) { vert = hint; vertGuid = guid; } else { frag = hint; fragGuid = guid; }
        value = vert.empty() || frag.empty() ? hint : Util::makeCustomShader(vert, frag);
        guids = vertGuid.isNull() && fragGuid.isNull() ? std::string() : vertGuid.str() + "|" + fragGuid.str();
    }

    // Edits one field of a closed document's YAML; the new reference is project-relative
    bool editYaml(YAML::Node root, const AssetRefTarget& target, const AssetRef& newRef, const AssetIndex::Roots& roots,
                  std::string& error) {
        const size_t slash = target.locator.find('/');
        const std::string scope = target.locator.substr(0, slash);
        std::string rest = slash == std::string::npos ? std::string() : target.locator.substr(slash + 1);

        YAML::Node base;
        const AssetRefField* row = nullptr;
        if (scope.compare(0, 7, "entity:") == 0) {
            const uint64_t id = std::strtoull(scope.c_str() + 7, nullptr, 10);
            YAML::Node entity = findEntityNode(root["entities"], id);
            if (!entity) entity = findEntityNode(root["members"], id);
            if (!entity && root["components"]) entity = root; // a version 1 prefab
            if (!entity) {
                error = "no GameObject " + std::to_string(id) + " in the file";
                return false;
            }
            YAML::Node components = entity["components"];
            if (rest.compare(0, 15, "prefabInstance[") == 0) {
                const size_t close = rest.find(']');
                const YAML::Node entry = findOverride(entity["prefabInstance"], rest.substr(15, close - 15));
                if (!entry) {
                    error = "no override record " + rest.substr(0, close + 1);
                    return false;
                }
                components = entry["components"];
                rest = rest.substr(std::min(rest.size(), close + 2));
            }
            const size_t dot = rest.find('.');
            const std::string componentName = rest.substr(0, dot);
            ComponentType component;
            if (!AssetRefSchema::componentByName(componentName, component) || !components) {
                error = "no " + componentName + " on GameObject " + std::to_string(id);
                return false;
            }
            base = components[Catalog::getComponentName(component, true)];
            rest = dot == std::string::npos ? std::string("path") : rest.substr(dot + 1);
            row = AssetRefSchema::findRow(AssetRefScope::Component, component, rest);
        } else if (scope == "settings") {
            base = root["scene"];
            row = AssetRefSchema::findRow(AssetRefScope::SceneSettings, ComponentType::Transform, rest);
        } else if (scope == "material") {
            base = root;
            row = AssetRefSchema::findRow(AssetRefScope::Material, ComponentType::MeshComponent, rest);
        } else if (scope == "scene") {
            base = root;
            row = AssetRefSchema::findRow(AssetRefScope::SceneFile, ComponentType::Transform, rest);
        } else if (scope == "project") {
            base = root;
            row = AssetRefSchema::findRow(AssetRefScope::Project, ComponentType::Transform, rest);
        }
        if (!row || !base) {
            error = "the field " + target.locator + " is not an asset reference of the file";
            return false;
        }
        if (row->kind == DepEdgeKind::ChildScene) {
            error = "child scenes are referenced by scene id: change them in the scene's settings";
            return false;
        }

        // to the node holding the last part
        std::vector<std::string> parts = fieldParts(rest);
        YAML::Node parent = base;
        std::string key;
        long index = -1;
        if (row->shape == AssetRefShape::PrefabRef && rest == "path") {
            parts.clear();
        }
        for (size_t i = 0; i < parts.size(); i++) {
            long partIndex = -1;
            const std::string name = splitIndex(parts[i], partIndex);
            const bool last = i + 1 == parts.size();
            if (last) {
                key = name;
                index = partIndex;
                break;
            }
            parent = partIndex >= 0 ? parent[name][static_cast<size_t>(partIndex)] : parent[name];
            if (!parent) {
                error = "the file has no " + target.locator;
                return false;
            }
        }

        const AssetPathBase base_ = row->base;
        AssetPathBase hintBase = base_;
        // Lua script entries are relative to the Lua directory
        if (row->path == "scripts[].path") {
            long scriptIndex = -1;
            splitIndex(parts.empty() ? std::string() : parts[0], scriptIndex);
            if (scriptIndex >= 0 && base["scripts"][static_cast<size_t>(scriptIndex)]["type"] &&
                base["scripts"][static_cast<size_t>(scriptIndex)]["type"].as<std::string>() == "lua") {
                hintBase = AssetPathBase::Lua;
            }
        }
        const std::string hint = AssetIndex::fromProjectRelative(roots, hintBase, newRef.path);
        const bool clear = newRef.path.empty();

        switch (row->shape) {
            case AssetRefShape::Texture: {
                YAML::Node texture = index >= 0 ? parent[key][static_cast<size_t>(index)] : parent[key];
                if (clear) {
                    if (index >= 0) parent[key][static_cast<size_t>(index)] = YAML::Node(YAML::NodeType::Map);
                    else parent[key] = YAML::Node(YAML::NodeType::Map);
                    break;
                }
                if (!texture || !texture.IsMap() || !texture["textureType"]) {
                    YAML::Node fresh(YAML::NodeType::Map);
                    fresh["textureType"] = "2D";
                    fresh["source"] = "path";
                    fresh["path"] = hint;
                    if (!newRef.guid.isNull()) fresh["guid"] = newRef.guid.str();
                    if (index >= 0) parent[key][static_cast<size_t>(index)] = fresh; else parent[key] = fresh;
                    break;
                }
                if (texture["paths"] && texture["paths"].IsSequence()) {
                    texture["paths"][static_cast<size_t>(target.ordinal)] = hint;
                    YAML::Node guids = texture["guids"];
                    if (!guids || !guids.IsSequence() || guids.size() != texture["paths"].size()) {
                        guids = YAML::Node(YAML::NodeType::Sequence);
                        guids.SetStyle(YAML::EmitterStyle::Flow);
                        for (size_t f = 0; f < texture["paths"].size(); f++) guids.push_back(std::string());
                    }
                    guids[static_cast<size_t>(target.ordinal)] = newRef.guid.str();
                    texture["guids"] = guids;
                } else {
                    texture["path"] = hint;
                    if (!newRef.guid.isNull()) texture["guid"] = newRef.guid.str(); else texture.remove("guid");
                }
                break;
            }
            case AssetRefShape::Asset: {
                parent[key] = hint;
                if (!newRef.guid.isNull()) parent[key + "Guid"] = newRef.guid.str(); else parent.remove(key + "Guid");
                break;
            }
            case AssetRefShape::Shader: {
                std::string value = parent[key] ? parent[key].as<std::string>() : std::string();
                std::string guids = parent[key + "Guid"] ? parent[key + "Guid"].as<std::string>() : std::string();
                replaceShaderFile(value, guids, target.ordinal, hint, newRef.guid);
                parent[key] = value;
                if (!guids.empty()) parent[key + "Guid"] = guids; else parent.remove(key + "Guid");
                break;
            }
            case AssetRefShape::AssetList: {
                YAML::Node list = parent[key];
                if (!list || !list.IsSequence() || index < 0 || static_cast<size_t>(index) >= list.size()) {
                    error = "the file has no " + target.locator;
                    return false;
                }
                list[static_cast<size_t>(index)] = hint;
                YAML::Node guids = parent[key + "Guid"];
                if (!guids || !guids.IsSequence() || guids.size() != list.size()) {
                    guids = YAML::Node(YAML::NodeType::Sequence);
                    guids.SetStyle(YAML::EmitterStyle::Flow);
                    for (size_t i = 0; i < list.size(); i++) guids.push_back(std::string());
                }
                guids[static_cast<size_t>(index)] = newRef.guid.str();
                parent[key + "Guid"] = guids;
                break;
            }
            case AssetRefShape::PrefabRef: {
                YAML::Node map = parts.empty() ? parent : (index >= 0 ? parent[key][static_cast<size_t>(index)] : parent[key]);
                if (clear) {
                    map["path"] = std::string();
                    map.remove("guid");
                } else {
                    map["path"] = hint;
                    map["guid"] = newRef.guid.str();
                    if (map["name"]) map["name"] = fs::path(hint).stem().string();
                }
                break;
            }
            case AssetRefShape::ScriptProperty: {
                YAML::Node property = parent[key][static_cast<size_t>(std::max(index, 0L))];
                YAML::Node value(YAML::NodeType::Map);
                if (!clear) {
                    if (!newRef.guid.isNull()) value["guid"] = newRef.guid.str();
                    value["path"] = hint;
                }
                property["value"] = value;
                break;
            }
        }
        return true;
    }

    // The loaded document a file is, if any: a scene, a prefab stage, a prefab asset
    struct LoadedOwner {
        SceneProject* scene = nullptr;
        Prefab* prefab = nullptr;
        EntityRegistry* registry() const { return scene ? scene->scene : prefab ? prefab->registry.get() : nullptr; }
    };

    LoadedOwner findLoaded(Project& project, const std::string& owner) {
        LoadedOwner loaded;
        for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            const PrefabStage* stage = project.getPrefabStage(sceneProject->id);
            const fs::path path = stage ? stage->assetPath : sceneProject->filepath;
            if (!path.empty() && project.normalizeToProjectRelative(path).generic_string() == owner) {
                loaded.scene = sceneProject;
                return loaded;
            }
        }
        if (Util::isPrefabFile(owner)) {
            Prefab* prefab = project.getPrefab(fs::path(owner));
            if (prefab && prefab->registry) loaded.prefab = prefab;
        }
        return loaded;
    }

    const ShaderType defaultShaderTypes[5] = {ShaderType::MESH, ShaderType::UI, ShaderType::SKYBOX, ShaderType::POINTS, ShaderType::LINES};

    // Sets the fields of a loaded document, after a snapshot of what they are in (the
    // components touched, the scene settings) for the undo
    bool editMemory(Project& project, const LoadedOwner& loaded, const std::vector<AssetRefTarget>& targets,
                    const AssetRef& newRef, AssetRefEditor::Undo& undo, std::string& error) {
        EntityRegistry* registry = loaded.registry();
        undo.wasModified = loaded.scene ? loaded.scene->isModified : loaded.prefab->isModified;
        for (const AssetRefTarget& target : targets) {
            const FieldLocatorParts parts = parseFieldLocator(target.locator);
            if (parts.hasEntity) {
                ComponentType type;
                if (!AssetRefSchema::componentByName(parts.component, type)) {
                    error = "no field " + target.locator + " in " + target.owner;
                    return false;
                }
                const Entity entity = static_cast<Entity>(parts.entity);
                const bool captured = std::any_of(undo.components.begin(), undo.components.end(), [&](const AssetRefEditor::ComponentSnapshot& snapshot) {
                    return snapshot.entity == entity && snapshot.type == type;
                });
                if (!captured) {
                    undo.components.push_back({entity, type, EntityClipboard::encodeComponent(registry, entity, type)});
                }
            } else if (parts.scope == "settings" && loaded.scene && loaded.scene->scene && !undo.hasSettings) {
                Scene* scene = loaded.scene->scene;
                undo.hasSettings = true;
                for (int i = 0; i < 5; i++) undo.defaultShaders[i] = scene->getDefaultShaderRef(defaultShaderTypes[i]);
                undo.passes = scene->getPostProcessPasses();
                undo.postProcessing = scene->getPostProcessing();
            }
        }

        const AssetIndex::Roots roots = project.getAssetRoots();
        std::vector<bool> done(targets.size(), false);
        auto visitor = [&](AssetRef& ref, const AssetRefSite& site, const std::string& scope) {
            const std::string locator = site.locator(scope);
            for (size_t i = 0; i < targets.size(); i++) {
                if (done[i] || targets[i].locator != locator || targets[i].ordinal != site.ordinal) continue;
                done[i] = true;
                ref = newRef.path.empty() ? AssetRef()
                    : AssetRef(AssetIndex::fromProjectRelative(roots, site.base, newRef.path), newRef.guid);
            }
        };
        AssetRefSchema::forEachAssetRef(*registry, [&](AssetRef& ref, const AssetRefSite& site) { visitor(ref, site, std::string()); });
        if (loaded.scene && loaded.scene->scene) {
            AssetRefSchema::forEachSceneSettingsRef(*loaded.scene->scene, [&](AssetRef& ref, const AssetRefSite& site) { visitor(ref, site, "settings"); });
        }
        for (size_t i = 0; i < targets.size(); i++) {
            if (!done[i]) {
                error = "no field " + targets[i].locator + " in " + targets[i].owner;
                return false;
            }
        }
        if (loaded.scene) {
            loaded.scene->isModified = true;
            loaded.scene->needUpdateRender = true;
        }
        if (loaded.prefab) {
            loaded.prefab->isModified = true;
        }
        return true;
    }

    void restoreMemory(const LoadedOwner& loaded, const AssetRefEditor::Undo& undo) {
        EntityRegistry* registry = loaded.registry();
        for (auto it = undo.components.rbegin(); it != undo.components.rend(); ++it) {
            if (it->before.IsMap()) {
                EntityClipboard::applyComponentValue(registry, it->entity, it->type, it->before);
            }
        }
        if (undo.hasSettings && loaded.scene && loaded.scene->scene) {
            Scene* scene = loaded.scene->scene;
            for (int i = 0; i < 5; i++) scene->setDefaultShaderRef(defaultShaderTypes[i], undo.defaultShaders[i]);
            scene->setPostProcessPasses(undo.passes);
            scene->setPostProcessing(undo.postProcessing);
        }
        if (loaded.scene) {
            loaded.scene->isModified = undo.wasModified;
            loaded.scene->needUpdateRender = true;
        }
        if (loaded.prefab) {
            loaded.prefab->isModified = undo.wasModified;
        }
    }

}

bool AssetRefEditor::apply(Project& project, const std::vector<AssetRefTarget>& targets, const AssetRef& newRef,
                           std::vector<Undo>& undo, std::vector<std::string>& filesWritten, std::string& error) {
    std::map<std::string, std::vector<AssetRefTarget>> byOwner;
    for (const AssetRefTarget& target : targets) {
        byOwner[target.owner].push_back(target);
    }
    const AssetIndex::Roots roots = project.getAssetRoots();
    for (const auto& [owner, ownerTargets] : byOwner) {
        Undo entry;
        entry.owner = owner;
        const LoadedOwner loaded = findLoaded(project, owner);
        if (loaded.registry()) {
            if (!editMemory(project, loaded, ownerTargets, newRef, entry, error)) {
                restoreMemory(loaded, entry);
                revert(project, undo);
                return false;
            }
            undo.push_back(std::move(entry));
            continue;
        }
        const fs::path full = project.getProjectPath() / owner;
        entry.closedFile = true;
        if (!assetio::readFile(full, entry.bytes)) {
            error = "cannot read " + owner;
            revert(project, undo);
            return false;
        }
        try {
            YAML::Node root = YAML::Load(entry.bytes);
            for (const AssetRefTarget& target : ownerTargets) {
                if (!editYaml(root, target, newRef, roots, error)) {
                    error = owner + ": " + error;
                    revert(project, undo);
                    return false;
                }
            }
            std::string writeError;
            if (!assetio::writeFile(full, YAML::Dump(root) + "\n", assetio::Durability::Document, &writeError)) {
                error = "Couldn't save \"" + owner + "\": " + writeError;
                revert(project, undo);
                return false;
            }
        } catch (const std::exception& e) {
            error = owner + ": " + e.what();
            revert(project, undo);
            return false;
        }
        filesWritten.push_back(owner);
        project.getAssetDatabase().importAssets(full);
        project.getAssetIndex().updateReferrer(owner);
        undo.push_back(std::move(entry));
    }
    return true;
}

void AssetRefEditor::revert(Project& project, const std::vector<Undo>& undo) {
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
        const Undo& entry = *it;
        if (entry.closedFile) {
            const fs::path full = project.getProjectPath() / entry.owner;
            std::string error;
            if (!assetio::writeFile(full, entry.bytes, assetio::Durability::Document, &error)) {
                Out::error("Couldn't restore \"%s\": %s", entry.owner.c_str(), error.c_str());
                continue;
            }
            project.getAssetDatabase().importAssets(full);
            project.getAssetIndex().updateReferrer(entry.owner);
            continue;
        }
        const LoadedOwner loaded = findLoaded(project, entry.owner);
        if (loaded.registry()) {
            restoreMemory(loaded, entry);
        }
    }
}

bool AssetRefEditor::migrateFile(Project& project, const std::string& relativePath, bool& written, std::string& error) {
    written = false;
    const fs::path full = project.getProjectPath() / relativePath;
    std::string bytes;
    if (!assetio::readFile(full, bytes)) {
        error = "cannot read " + relativePath;
        return false;
    }
    try {
        YAML::Node root = YAML::Load(bytes);
        const AssetIndex::Roots roots = project.getAssetRoots();
        std::vector<std::pair<AssetRefTarget, AssetRef>> updates;
        auto consider = [&](const AssetRefFound& found) {
            if (found.sceneId != 0 || found.site.kind == DepEdgeKind::ChildScene) return;
            const std::string hint = AssetIndex::toProjectRelative(roots, found.site.base, found.ref.path);
            const AssetDatabase::Resolution resolved = project.getAssetDatabase().resolve(
                found.ref.guid, hint, found.site.expected, found.site.expected != AssetType::Prefab);
            if (!resolved.found() || resolved.by == AssetDatabase::ResolvedBy::PathFallback) return;
            const bool needsGuid = found.ref.guid.isNull();
            const bool staleHint = hint != resolved.path;
            if (!needsGuid && !staleHint) return;
            // a reference by a former GUID keeps it: the redirect is what makes it resolve
            const Guid guid = resolved.by == AssetDatabase::ResolvedBy::Former ? found.ref.guid : resolved.guid;
            updates.push_back({{relativePath, found.site.locator(found.scope), found.site.ordinal}, AssetRef(resolved.path, guid)});
        };
        if (relativePath == "project.yaml") {
            AssetRefSchema::extractProjectRefs(root, consider);
        } else if (Util::isMaterialFile(relativePath)) {
            AssetRefSchema::extractMaterialFileRefs(root, consider);
        } else {
            AssetRefSchema::extractDocumentRefs(root, Util::isPrefabFile(relativePath), consider);
        }
        if (updates.empty()) {
            return true;
        }
        for (const auto& [target, ref] : updates) {
            if (!editYaml(root, target, ref, roots, error)) {
                error = relativePath + ": " + error;
                return false;
            }
        }
        std::string writeError;
        bool changed = false;
        if (!assetio::writeFile(full, YAML::Dump(root) + "\n", assetio::Durability::Document, &writeError, &changed)) {
            error = "Couldn't save \"" + relativePath + "\": " + writeError;
            return false;
        }
        written = changed;
    } catch (const std::exception& e) {
        error = relativePath + ": " + e.what();
        return false;
    }
    if (written) {
        project.getAssetDatabase().importAssets(full);
        project.getAssetIndex().updateReferrer(relativePath);
    }
    return true;
}
