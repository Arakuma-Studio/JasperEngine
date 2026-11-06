// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Asset identity on the Project's side (docs/specs/S04-asset-identity.md): resolving the
// references of loaded documents by GUID, the identity keys written into scenes, prefabs and
// materials, and the in-memory edges of documents with unsaved edits.

#include "Project.h"
#include "component/MeshComponent.h"

#include "Out.h"
#include "DocumentFile.h"
#include "util/FileUtils.h"
#include "Stream.h"
#include "PrefabStage.h"
#include "asset/AssetFileWrite.h"
#include "util/EntityPath.h"

#include <algorithm>

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    bool isPrefabExpected(const editor::AssetRefSite& site) {
        return site.expected == AssetType::Prefab;
    }

}

editor::AssetIndex::Roots editor::Project::getAssetRoots() const {
    auto relative = [&](const fs::path& root) {
        fs::path path = normalizeToProjectRelative(root);
        const std::string text = path.generic_string();
        return text == "." ? std::string() : text;
    };
    return {relative(getAssetsPath()), relative(getLuaPath())};
}

std::string editor::Project::assetHintToProjectPath(const std::string& hint, AssetPathBase base) const {
    return AssetIndex::toProjectRelative(getAssetRoots(), base, hint);
}

std::string editor::Project::projectPathToAssetHint(const std::string& projectRelative, AssetPathBase base) const {
    return AssetIndex::fromProjectRelative(getAssetRoots(), base, projectRelative);
}

AssetRef editor::Project::makeAssetRef(const std::string& hint, AssetPathBase base) const {
    if (hint.empty()) {
        return AssetRef();
    }
    const AssetDatabase::Resolution found = assetDatabase.lookupPath(assetHintToProjectPath(hint, base));
    return AssetRef(hint, found.found() ? found.guid : Guid());
}

// Paths as references hold them: relative to the assets directory for asset types, to the
// project otherwise (the form assetmap.bin has in builds)
bool editor::Project::EditorAssetResolver::findPath(const Guid& guid, std::string& path, AssetType& type) const {
    const AssetDatabase::Resolution found = project->getAssetDatabase().resolve(guid, std::string(), AssetType::Unknown, false);
    if (!found.found()) {
        return false;
    }
    type = found.type;
    std::string container, key;
    const bool sub = splitSubAssetPath(found.path, container, key);
    const AssetTypeInfo* info = AssetTypes::find(sub ? container : found.path, project->getAssetRoots().assetsDir);
    path = info ? project->projectPathToAssetHint(found.path, info->base == AssetPathBase::Lua ? AssetPathBase::Project : info->base) : found.path;
    return true;
}

Guid editor::Project::EditorAssetResolver::findGuid(const std::string& path) const {
    const AssetDatabase& database = project->getAssetDatabase();
    const AssetIndex::Roots roots = project->getAssetRoots();
    // an asset-type path is relative to the assets directory; any other to the project
    const AssetDatabase::Resolution inAssets = database.lookupPath(AssetIndex::toProjectRelative(roots, AssetPathBase::Assets, path));
    if (inAssets.found() && (inAssets.type == AssetType::SubAsset || AssetTypes::info(inAssets.type).base == AssetPathBase::Assets)) {
        return inAssets.guid;
    }
    const AssetDatabase::Resolution inProject = database.lookupPath(path);
    return inProject.found() ? inProject.guid : Guid();
}

editor::MissingAssetTracker& editor::Project::getMissingAssets() {
    missingAssets.update(assetDatabase, assetIndex);
    return missingAssets;
}

bool editor::Project::resolveRegistryAssetRefs(EntityRegistry* registry, bool loading) {
    if (!registry || !assetDatabase.isOpen()) {
        return false;
    }
    const AssetIndex::Roots roots = getAssetRoots();
    bool hintChanged = false;
    AssetRefSchema::forEachAssetRef(*registry, [&](AssetRef& ref, const AssetRefSite& site) {
        // instance roots resolve through the prefab code (PREFABS.md §3), which owns their
        // Missing Prefab state
        if (isPrefabExpected(site) && site.kind != DepEdgeKind::ScriptProperty) {
            return;
        }
        const std::string hint = AssetIndex::toProjectRelative(roots, site.base, ref.path);
        const AssetDatabase::Resolution found = assetDatabase.resolve(ref.guid, hint, site.expected,
                                                                      !isPrefabExpected(site), !loading);
        if (!found.found()) {
            return; // Missing: the reference stays as it is (R15)
        }
        const std::string current = AssetIndex::fromProjectRelative(roots, site.base, found.path);
        if (found.by == AssetDatabase::ResolvedBy::PathFallback) {
            // R11: listed under "Resolved by path" until the owner saves with the file's GUID
            if (!loading) ref.guid = found.guid;
            return;
        }
        if (ref.guid.isNull()) {
            ref.guid = found.guid;
        }
        if (ref.path != current) {
            ref.path = current;
            hintChanged = true;
        }
    });
    return hintChanged;
}

void editor::Project::bindLoadedAssetRefs() {
    if (!assetDatabase.isOpen()) {
        return;
    }
    for (SceneProject* sceneProject : getScenesAndPrefabStages()) {
        if (sceneProject->scene && sceneProject->playState == ScenePlayState::STOPPED) {
            resolveSceneAssetRefs(*sceneProject, false);
        }
    }
    for (auto& [path, prefab] : prefabAssets) {
        if (prefab.registry) {
            resolveRegistryAssetRefs(prefab.registry.get(), false);
        }
    }
}

bool editor::Project::resolveSceneAssetRefs(SceneProject& sceneProject, bool loading) {
    if (!sceneProject.scene || !assetDatabase.isOpen()) {
        return false;
    }
    bool hintChanged = resolveRegistryAssetRefs(sceneProject.scene, loading);
    const AssetIndex::Roots roots = getAssetRoots();
    AssetRefSchema::forEachSceneSettingsRef(*sceneProject.scene, [&](AssetRef& ref, const AssetRefSite& site) {
        const std::string hint = AssetIndex::toProjectRelative(roots, site.base, ref.path);
        const AssetDatabase::Resolution found = assetDatabase.resolve(ref.guid, hint, site.expected, true, !loading);
        if (!found.found()) {
            return;
        }
        if (found.by == AssetDatabase::ResolvedBy::PathFallback) {
            if (!loading) ref.guid = found.guid;
            return;
        }
        const std::string current = AssetIndex::fromProjectRelative(roots, site.base, found.path);
        if (ref.guid.isNull()) {
            ref.guid = found.guid;
        }
        if (ref.path != current) {
            ref.path = current;
            hintChanged = true;
        }
    });
    if (hintChanged) {
        // the links of submeshes whose .material file moved follow it
        for (auto& [key, link] : materialFileLinks) {
            if (std::get<0>(key) != sceneProject.id) continue;
            MeshComponent* mesh = sceneProject.scene->findComponent<MeshComponent>(std::get<1>(key));
            if (mesh && std::get<2>(key) < mesh->numSubmeshes) {
                link.filePath = mesh->submeshes[std::get<2>(key)].material.name.path;
            }
        }
    }
    return hintChanged;
}

void editor::Project::bindMaterialAssetRefs(Material& material) {
    if (!assetDatabase.isOpen()) {
        return;
    }
    const AssetIndex::Roots roots = getAssetRoots();
    AssetRefSchema::forEachMaterialRef(material, std::string(), [&](AssetRef& ref, const AssetRefSite& site) {
        // name: in a .material file, the file itself (not a reference)
        if (!ref.guid.isNull() || site.kind == DepEdgeKind::MeshMaterialFile) {
            return;
        }
        // a path picked on Windows holds '\': a separator, as on every platform (Stream::portablePath)
        const std::string hint = AssetIndex::toProjectRelative(roots, site.base, Stream::portablePath(ref.path));
        AssetDatabase::Resolution found = assetDatabase.resolve(ref.guid, hint, site.expected, true, true);
        std::error_code ec;
        if (!found.found() && !hint.empty() && fs::is_regular_file(projectPath / hint, ec)) {
            // written a moment ago (a fork_shader, a tool's file) and not seen by a refresh yet;
            // what else the database learns waits for the next refresh, as resolvePrefabPath does
            std::vector<AssetDatabase::Change> changes = assetDatabase.importAssets(hint);
            pendingAssetChanges.insert(pendingAssetChanges.end(), changes.begin(), changes.end());
            found = assetDatabase.resolve(ref.guid, hint, site.expected, true, true);
        }
        if (found.found()) {
            // the GUID, and the path in the database's form, as the scene resolver writes it
            // (resolveAssetRefs): the file opens anywhere, not only where it was picked
            ref.guid = found.guid;
            ref.path = AssetIndex::fromProjectRelative(roots, site.base, found.path);
        }
    });
}

void editor::Project::refreshAssetHints() {
    for (SceneProject* sceneProject : getScenesAndPrefabStages()) {
        if (sceneProject->scene && resolveSceneAssetRefs(*sceneProject, false)) {
            sceneProject->isModified = true;
            sceneProject->needUpdateRender = true;
        }
    }
    // prefab files get the new hints when they are saved anyway (PREFABS.md §3)
    for (auto& [prefabPath, prefab] : prefabAssets) {
        if (prefab.registry) {
            resolveRegistryAssetRefs(prefab.registry.get(), false);
        }
    }
}

Guid editor::Project::prepareDocumentIdentity(YAML::Node& root, const fs::path& relativePath) {
    const std::string relative = normalizeToProjectRelative(relativePath).generic_string();
    AssetDatabase::AssetInfo info;
    Guid guid;
    std::vector<Guid> formerGuids;
    if (assetDatabase.getInfo(fs::path(relative), info)) {
        guid = info.guid;
        formerGuids = info.formerGuids;
    }
    // a prefab carries its own GUID (Prefab::guid); a new file gets a fresh one
    if (root["guid"] && Guid::isValidText(root["guid"].as<std::string>())) {
        const Guid written = Guid::parse(root["guid"].as<std::string>());
        if (written != guid) {
            guid = written;
            formerGuids.clear();
        }
    }
    if (guid.isNull()) {
        guid = Guid::parse(AssetDatabase::generateGuid());
        while (!assetDatabase.getPath(guid).empty()) {
            guid = Guid::parse(AssetDatabase::generateGuid());
        }
    }

    // guid (then formerGuids) after the leading header keys (formatVersion, type, version),
    // the rest in its order: readInFileIdentity finds formerGuids right after guid
    YAML::Node ordered(YAML::NodeType::Map);
    bool placed = false;
    // S25 R22: the asset's labels, from the label store (the file's own until the store has
    // read them), so a save keeps them and a label edit needs no save
    std::vector<std::string> labels;
    if (assetLabels.isReady()) {
        labels = assetLabels.labelsOf(guid);
    } else {
        AssetLabels::readFileLabels(projectPath / relative, true, labels);
    }
    auto place = [&]() {
        ordered["guid"] = guid.str();
        if (!formerGuids.empty()) {
            YAML::Node list(YAML::NodeType::Sequence);
            list.SetStyle(YAML::EmitterStyle::Flow);
            for (const Guid& former : formerGuids) list.push_back(former.str());
            ordered["formerGuids"] = list;
        }
        if (!labels.empty()) {
            YAML::Node list(YAML::NodeType::Sequence);
            list.SetStyle(YAML::EmitterStyle::Flow);
            for (const std::string& label : labels) list.push_back(label);
            ordered["labels"] = list;
        }
        placed = true;
    };
    for (const auto& entry : root) {
        const std::string key = entry.first.as<std::string>();
        if (key == "guid" || key == "formerGuids" || key == "labels") {
            if (!placed && key == "guid") place();
            continue;
        }
        if (!placed && key != "formatVersion" && key != "type" && key != "version") {
            place();
        }
        ordered[key] = entry.second;
    }
    if (!placed) {
        place();
    }
    root = ordered;
    return guid;
}

std::string editor::Project::materialFileText(const fs::path& file, const Material& material) {
    Material bound = material;
    bindMaterialAssetRefs(bound);
    YAML::Node root = Stream::encodeMaterial(bound);
    // a file the database has not scanned yet (copied in from outside) keeps the GUID it came with
    AssetDatabase::AssetInfo known;
    if (!assetDatabase.getInfo(normalizeToProjectRelative(file), known)) {
        std::error_code ec;
        if (fs::exists(file, ec)) {
            try {
                const YAML::Node existing = DocumentFiles::read(file, DocumentKind::Material).root;
                if (existing["guid"]) root["guid"] = existing["guid"];
            } catch (const std::exception&) {
                // unreadable: a new identity, as a new file gets
            }
        }
    }
    prepareDocumentIdentity(root, file);
    return FileUtils::withLineEndings(file, DocumentFiles::materialText(root, file) + "\n");
}

editor::SaveResult editor::Project::writeMaterialFile(const fs::path& relativePath, const Material& material,
                                                    DegradedSavePolicy policy, std::string* error) {
    const fs::path relative = normalizeToProjectRelative(relativePath);
    Material bound = material;
    bindMaterialAssetRefs(bound);
    YAML::Node root = Stream::encodeMaterial(bound);
    const Guid guid = prepareDocumentIdentity(root, relative);
    const SaveResult saved = DocumentFiles::saveMaterial(projectPath / relative, root, policy, error);
    if (saved == SaveResult::Saved) {
        assetDatabase.registerAsset(relative, guid.str());
        assetIndex.updateReferrer(relative.generic_string());
    }
    return saved;
}

void editor::Project::collectMemoryEdges(std::unordered_map<Guid, std::vector<DepEdge>>& edges) {
    const AssetIndex::Roots roots = getAssetRoots();
    auto collectRegistry = [&](EntityRegistry* registry, const Guid& referrer, DepEdgeKind componentKind, std::vector<DepEdge>& out) {
        EntityPath::Cache cache;
        AssetRefSchema::forEachAssetRef(*registry, [&](AssetRef& ref, const AssetRefSite& site) {
            DepEdge edge;
            edge.referrer = referrer;
            edge.kind = site.kind == DepEdgeKind::SceneComponent ? componentKind : site.kind;
            edge.field = site.locator();
            edge.ordinal = site.ordinal;
            edge.entity = site.entity;
            edge.entityPath = EntityPath::of(registry, site.entity, &cache);
            edge.component = site.component;
            edge.expected = site.expected;
            edge.target = ref.guid;
            edge.targetPath = AssetIndex::toProjectRelative(roots, site.base, ref.path);
            if (edge.target.isNull()) {
                const AssetDatabase::Resolution found = assetDatabase.lookupPath(edge.targetPath);
                if (found.found()) edge.target = found.guid;
            }
            if (isValidDepEdge(edge)) out.push_back(std::move(edge));
        });
    };

    // every loaded document answers from memory: its unsaved edits count, and its field
    // locators are the ones the editor edits (AssetRefEditor)
    for (SceneProject* sceneProject : getScenesAndPrefabStages()) {
        if (!sceneProject->scene) {
            continue;
        }
        // a prefab stage edits its prefab asset; a scene its file (a never-saved one has no edges)
        Guid referrer;
        DepEdgeKind kind = DepEdgeKind::SceneComponent;
        if (PrefabStage* stage = getPrefabStage(sceneProject->id)) {
            referrer = assetDatabase.getAssetGuid(stage->assetPath);
            kind = DepEdgeKind::PrefabComponent;
        } else if (!sceneProject->filepath.empty()) {
            referrer = assetDatabase.getAssetGuid(sceneProject->filepath);
        }
        if (referrer.isNull()) {
            continue;
        }
        std::vector<DepEdge>& out = edges[referrer];
        collectRegistry(sceneProject->scene, referrer, kind, out);
        AssetRefSchema::forEachSceneSettingsRef(*sceneProject->scene, [&](AssetRef& ref, const AssetRefSite& site) {
            DepEdge edge;
            edge.referrer = referrer;
            edge.kind = site.kind;
            edge.field = site.locator("settings");
            edge.ordinal = site.ordinal;
            edge.expected = site.expected;
            edge.target = ref.guid;
            edge.targetPath = AssetIndex::toProjectRelative(roots, site.base, ref.path);
            if (isValidDepEdge(edge)) out.push_back(std::move(edge));
        });
        // the build's prefab list and the child scenes
        for (size_t i = 0; i < sceneProject->prefabs.size(); i++) {
            const PrefabSceneInfo& info = sceneProject->prefabs[i];
            DepEdge edge;
            edge.referrer = referrer;
            edge.kind = DepEdgeKind::ScenePrefabList;
            edge.field = "scene/prefabs[" + std::to_string(i) + "]";
            edge.expected = AssetType::Prefab;
            edge.target = Guid::parse(info.guid);
            edge.targetPath = info.prefabPath.generic_string();
            if (isValidDepEdge(edge)) out.push_back(std::move(edge));
        }
        for (size_t i = 0; i < sceneProject->childScenes.size(); i++) {
            const SceneProject* child = getScene(sceneProject->childScenes[i].id);
            if (!child || child->filepath.empty()) continue;
            DepEdge edge;
            edge.referrer = referrer;
            edge.kind = DepEdgeKind::ChildScene;
            edge.field = "scene/childScenes[" + std::to_string(i) + "]";
            edge.expected = AssetType::Scene;
            edge.targetPath = child->filepath.generic_string();
            edge.target = assetDatabase.getAssetGuid(child->filepath);
            if (isValidDepEdge(edge)) out.push_back(std::move(edge));
        }
    }
    for (auto& [prefabPath, prefab] : prefabAssets) {
        if (!prefab.registry) {
            continue;
        }
        const Guid referrer = assetDatabase.getAssetGuid(prefabPath);
        if (referrer.isNull() || edges.count(referrer)) {
            continue;
        }
        collectRegistry(prefab.registry.get(), referrer, DepEdgeKind::PrefabComponent, edges[referrer]);
    }
}

void editor::Project::onAssetFilesChanged(const std::vector<AssetDatabase::Change>& changes) {
    if (changes.empty()) {
        return;
    }
    // prefabs: moved ones get their new path in memory, removed ones leave Missing Prefab
    // instances, added ones relink them (PREFABS.md §3)
    applyAssetChanges(changes);
    assetIndex.update(changes);
    // S25: an asset edited, put back or arriving outside the editor brings its labels
    for (const AssetDatabase::Change& change : changes) {
        if (change.type != AssetDatabase::Change::Type::Removed && Guid::isValidText(change.guid)) {
            assetLabels.refreshAsset(Guid::parse(change.guid));
        }
    }
    if (std::any_of(changes.begin(), changes.end(), [](const AssetDatabase::Change& change) {
            return change.type != AssetDatabase::Change::Type::Modified; })) {
        // references follow their assets' GUIDs: moved ones get the new hints, a file put
        // back resolves again
        refreshAssetHints();
    }
}
