// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreatePrefabCmd.h"
#include "project/PrefabUnpackRecord.h"
#include "editor/Out.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "Stream.h"

using namespace jasperengine;

namespace {

    // The root GameObject of a selection node: the one with a Transform, else the first
    YAML::Node selectionRoot(const YAML::Node& node) {
        if (!node || !node.IsMap()) {
            return YAML::Node();
        }
        if (!node["members"] || !node["members"].IsSequence()) {
            return node;
        }
        const std::string transformKey = editor::Catalog::getComponentName(editor::ComponentType::Transform, true);
        for (const YAML::Node& member : node["members"]) {
            const YAML::Node components = member["components"];
            if (components && components.IsMap() && components[transformKey]) {
                return member;
            }
        }
        return node["members"].size() > 0 ? node["members"][0] : YAML::Node(YAML::NodeType::Undefined);
    }

    // Entity nodes of a selection that are prefab instance roots (nested ones stay opaque)
    void collectInstanceRoots(const YAML::Node& node, std::vector<Entity>& roots) {
        if (!node || !node.IsMap()) {
            return;
        }
        if (node["members"] && node["members"].IsSequence()) {
            for (const YAML::Node& member : node["members"]) {
                collectInstanceRoots(member, roots);
            }
            return;
        }
        const YAML::Node components = node["components"];
        if (node["entity"] && components && components.IsMap() &&
            components[editor::Catalog::getComponentName(editor::ComponentType::PrefabComponent, true)]) {
            roots.push_back(node["entity"].as<Entity>());
        }
        if (node["children"] && node["children"].IsSequence()) {
            for (const YAML::Node& child : node["children"]) {
                collectInstanceRoots(child, roots);
            }
        }
    }

}

editor::CreatePrefabCmd::CreatePrefabCmd(Project* project, uint32_t sceneId, const fs::path& filepath, const YAML::Node& prefabNode)
    : CreatePrefabCmd(project, sceneId, std::vector<std::pair<fs::path, YAML::Node>>{{filepath, prefabNode}}) {
}

editor::CreatePrefabCmd::CreatePrefabCmd(Project* project, uint32_t sceneId, const std::vector<std::pair<fs::path, YAML::Node>>& prefabs, bool variants){
    this->project = project;
    this->sceneId = sceneId;
    this->wasModified = project->getScene(sceneId)->isModified;
    for (const auto& [filepath, node] : prefabs) {
        Item item;
        item.filepath = filepath;
        item.prefabNode = YAML::Clone(node);
        item.variant = variants;
        items.push_back(std::move(item));
    }
}

bool editor::CreatePrefabCmd::execute(){
    SceneProject* sceneProject = project->getScene(sceneId);

    if (!sceneProject || !sceneProject->scene){
        editor::Out::error("Invalid scene ID: %u", sceneId);
        return false;
    }
    Scene* scene = sceneProject->scene;

    // Store current selection for undo
    lastSelected = project->getSelectedEntities(sceneId);

    bool any = false;
    for (Item& item : items) {
        item.created = false;
        item.unpacked.clear();
        item.nestedRoots.clear();

        if (!item.filepath.is_relative()) {
            editor::Out::error("Prefab filepath must be relative: %s", item.filepath.string().c_str());
            continue;
        }
        if (project->getPrefab(item.filepath)) {
            editor::Out::error("Prefab already exists at %s", item.filepath.string().c_str());
            continue;
        }

        YAML::Node node = YAML::Clone(item.prefabNode);
        const YAML::Node rootNode = selectionRoot(node);
        if (!rootNode || !rootNode["entity"]) {
            editor::Out::error("Nothing to make a prefab of at %s", item.filepath.string().c_str());
            continue;
        }
        const Entity root = rootNode["entity"].as<Entity>();
        if (!scene->isEntityCreated(root)) {
            continue;
        }

        // Unity's Prefab Variant: the instance stays one, of the new variant of its prefab
        if (item.variant) {
            item.variantRoot = root;
            item.variantRootNode.reset(Stream::encodeEntity(root, scene, project, sceneProject));
            if (project->createPrefabVariant(sceneId, item.filepath, root, item.guid)) {
                item.guid = project->getPrefabGuid(item.filepath);
                item.created = true;
                any = true;
            }
            continue;
        }

        // A prefab instance root becomes the root of an original prefab: unpacked first, its
        // GameObjects read again with its nested instances as instances
        PrefabObjectRef ref;
        if (project->findPrefabObject(sceneId, root, ref)) {
            if (!ref.isRoot) {
                editor::Out::error("'%s' is a GameObject of a prefab instance: unpack the instance to make a prefab of it",
                    scene->getEntityName(root).c_str());
                continue;
            }
            item.unpacked = project->unpackPrefabInstances(sceneId, {root}, false);
            YAML::Node unpackedNode = Stream::encodeEntity(root, scene, project, sceneProject);
            if (node["members"] && node["members"].IsSequence()) {
                YAML::Node members(YAML::NodeType::Sequence);
                for (YAML::Node member : node["members"]) {
                    members.push_back(member["entity"] && member["entity"].as<Entity>() == root ? unpackedNode : member);
                }
                node["members"] = members;
            } else {
                node = unpackedNode;
            }
        }

        // The instances nested in it, as they are now: their overrides go into the new prefab
        std::vector<Entity> nestedRoots;
        collectInstanceRoots(node, nestedRoots);
        for (Entity nested : nestedRoots) {
            if (nested != root && project->isPrefabInstanceRoot(sceneId, nested)) {
                item.nestedRoots.emplace_back(nested, Stream::encodeEntity(nested, scene, project, sceneProject));
            }
        }

        if (project->createPrefab(sceneId, item.filepath, node, item.guid)) {
            item.guid = project->getPrefabGuid(item.filepath);
            item.created = true;
            any = true;
            editor::Out::info("Created Prefab at '%s'", item.filepath.string().c_str());
        } else if (!item.unpacked.empty()) {
            project->repackPrefabInstances(sceneId, std::move(item.unpacked));
            item.unpacked.clear();
        }
    }

    return any;
}

void editor::CreatePrefabCmd::undo(){
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject) {
        return;
    }

    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        Item& item = *it;
        if (!item.created) {
            continue;
        }

        // The file it made, wherever it was moved since
        if (!item.guid.empty()) {
            fs::path current = project->resolvePrefabPath(item.guid, item.filepath);
            if (!current.empty()) {
                item.filepath = current;
            }
        }

        // Delete the prefab file from disk if it exists
        try {
            fs::path fullPath = project->getProjectPath() / item.filepath;
            if (fs::exists(fullPath)) {
                // the entry itself (in a write scope: beneath its root)
                std::error_code removeError;
                if (FileUtils::removePath(fullPath, removeError)) {
                    editor::Out::info("Removed prefab file: %s", FileUtils::pathToGenericUtf8(fullPath).c_str());
                } else {
                    undoProblem += (undoProblem.empty() ? "" : "; ") + std::string("could not remove ") + FileUtils::pathToGenericUtf8(fullPath) +
                                   (safewrite::lastScopedError().empty() ? std::string() : ": " + safewrite::lastScopedError());
                }
            }
            project->getAssetDatabase().removeAssets(fullPath);
        } catch (const std::exception& e) {
            editor::Out::warning("Failed to remove prefab file: %s", e.what());
        }

        if (item.variant) {
            // the instance is one of the base again, as it was
            project->removePrefabVariantInstance(sceneId, item.filepath, item.variantRoot, item.variantRootNode);
            item.created = false;
            continue;
        }

        // The prefab and its link on the root go
        project->removePrefab(item.filepath);

        // The nested instances take back their overrides, the unpacked one its link
        for (const auto& [nested, node] : item.nestedRoots) {
            project->reimportPrefabInstance(sceneProject, nested, node);
        }
        if (!item.unpacked.empty()) {
            project->repackPrefabInstances(sceneId, std::move(item.unpacked));
            item.unpacked.clear();
        }
        item.created = false;
    }

    // Restore previous selection
    if (!lastSelected.empty()) {
        project->replaceSelectedEntities(sceneId, lastSelected);
    }

    // Restore modified state
    sceneProject->isModified = wasModified;
}

std::vector<editor::Command::Touched> editor::CreatePrefabCmd::touchedPaths(bool undo) const{
    std::vector<Touched> out;
    for (const Item& item : items){
        if (item.filepath.empty()) continue;
        // where the prefab is now (moved since: its GUID knows), as the undo will find it
        fs::path current = item.filepath;
        if (!item.guid.empty()){
            const fs::path moved = project->resolvePrefabPath(item.guid, item.filepath);
            if (!moved.empty()) current = moved;
        }
        const fs::path full = current.is_absolute() ? current : project->getProjectPath() / current;
        // made again (a write) or removed (the entry)
        out.push_back({full, undo});
    }
    return out;
}
