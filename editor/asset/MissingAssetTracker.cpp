// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "MissingAssetTracker.h"

#include <algorithm>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

const MissingAssetTracker::Group* MissingAssetTracker::findGroup(const Guid& guid, const std::string& lastPath) const {
    for (const Group& group : groups) {
        if (!guid.isNull() ? group.guid == guid : group.guid.isNull() && group.lastPath == lastPath) {
            return &group;
        }
    }
    return nullptr;
}

void MissingAssetTracker::update(const AssetDatabase& database, const AssetIndex& index, bool force) {
    // the overlay (unsaved edits) is part of the index's answers, so a forced update follows them
    if (!force && database.getVersion() == databaseVersion && index.getVersion() == indexVersion) {
        return;
    }
    databaseVersion = database.getVersion();
    indexVersion = index.getVersion();

    std::map<std::pair<Guid, std::string>, Group> missing;
    std::map<std::pair<Guid, std::string>, ResolvedByPath> healed;
    index.forEachEdge([&](const DepEdge& edge) {
        if (isTextEdge(edge.kind)) {
            return; // path references in source text: listed when a rename breaks them (R14)
        }
        Referrer referrer;
        referrer.owner = edge.referrer;
        referrer.ownerPath = index.referrerPath(edge.referrer);
        AssetDatabase::AssetInfo owner;
        if (database.getInfo(edge.referrer, owner)) referrer.ownerType = owner.type;
        referrer.entity = edge.entity;
        referrer.entityPath = edge.entityPath;
        referrer.component = edge.component;
        const size_t slash = edge.field.find('/');
        referrer.field = slash == std::string::npos ? edge.field : edge.field.substr(slash + 1);
        if (!edge.component.empty() && referrer.field.compare(0, edge.component.size() + 1, edge.component + ".") == 0) {
            referrer.field = referrer.field.substr(edge.component.size() + 1);
        }
        referrer.locator = edge.field;
        referrer.kind = edge.kind;

        // prefabs never bind by path (editor/project/PREFABS.md §3)
        const bool allowPath = edge.expected != AssetType::Prefab;
        const AssetDatabase::Resolution resolution = !edge.target.isNull()
            ? database.resolve(edge.target, edge.targetPath, edge.expected, allowPath)
            : database.resolve(Guid(), edge.targetPath, edge.expected, false);
        if (resolution.by == AssetDatabase::ResolvedBy::PathFallback) {
            ResolvedByPath& entry = healed[{edge.target, edge.targetPath}];
            entry.referencedGuid = edge.target;
            entry.currentGuid = resolution.guid;
            entry.path = resolution.path;
            entry.referrers.push_back(referrer);
            return;
        }
        if (resolution.found()) {
            return;
        }
        // a shader fork's assets-relative spelling ("shaders/x" for "assets/shaders/x") is not missing:
        // the editor, exports and games all find it (the rule of Project::findShaderFile). What it
        // resolves to stays out of resolve(), or every load would rewrite the reference.
        if (edge.expected == AssetType::Shader && database.resolveInAssetsDir(edge.targetPath).found()) {
            return;
        }
        // a reference written before GUIDs names its asset by path: the GUID that asset got when
        // the project was upgraded is its legacy GUID (R10 step 4), so it joins the references
        // that hold that GUID (and a redirect of it resolves it too)
        const Guid key = !edge.target.isNull() || edge.targetPath.empty()
            ? edge.target : Guid::parse(AssetDatabase::legacyGuid(edge.targetPath));
        Group& group = missing[{key, key.isNull() ? edge.targetPath : std::string()}];
        group.guid = key;
        if (group.lastPath.empty()) group.lastPath = edge.targetPath;
        if (group.expected == AssetType::Unknown) group.expected = edge.expected;
        group.referrers.push_back(referrer);
    });

    auto byOwner = [](const Referrer& a, const Referrer& b) {
        return a.ownerPath != b.ownerPath ? a.ownerPath < b.ownerPath : a.locator < b.locator;
    };
    std::vector<Group> nextGroups;
    for (auto& [key, group] : missing) {
        std::sort(group.referrers.begin(), group.referrers.end(), byOwner);
        nextGroups.push_back(std::move(group));
    }
    std::sort(nextGroups.begin(), nextGroups.end(), [](const Group& a, const Group& b) {
        return a.lastPath != b.lastPath ? a.lastPath < b.lastPath : a.guid < b.guid;
    });
    std::vector<ResolvedByPath> nextHealed;
    for (auto& [key, entry] : healed) {
        std::sort(entry.referrers.begin(), entry.referrers.end(), byOwner);
        nextHealed.push_back(std::move(entry));
    }
    groups = std::move(nextGroups);
    resolvedByPath = std::move(nextHealed);
    orphanMetas = database.getOrphanMetas();
    readOnlyLinked = database.getReadOnlyLinkedAssets();
    version++;
}
