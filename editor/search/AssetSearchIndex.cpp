// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetSearchIndex.h"
#include "io/watch/LinkedFolders.h"

#include "Project.h"
#include "asset/AssetTypes.h"

#include <algorithm>
#include <set>
#include <cstdint>
#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

AssetRecordsView::AssetRecordsView(std::shared_ptr<const std::vector<AssetRecord>> list, std::unordered_set<std::string> missingSet):
    records(std::move(list)), missing(std::move(missingSet)) {
}

std::unordered_set<std::string> AssetRecordsView::referrersOf(const std::string&) const {
    // a frozen view answers no dependency question: the editor runs ref: on its live index
    return {};
}

AssetSearchIndex::~AssetSearchIndex() {
    detach();
}

void AssetSearchIndex::attach(Project* owner) {
    detach();
    project = owner;
    databaseVersion = 0;
    labelsVersion = 0;
    walkWanted = true;
}

void AssetSearchIndex::detach() {
    if (walking) {
        folderWalk.cancel();
        folderWalk.wait();
        walking = false;
    }
    project = nullptr;
    records = std::make_shared<std::vector<AssetRecord>>();
    byPath.clear();
    byGuid.clear();
    walkedFolders.clear();
    revision++;
}

void AssetSearchIndex::startFolderWalk() {
    if (!project || project->getProjectPath().empty()) return;
    const fs::path root = project->getProjectPath();
    walking = true;
    walkWanted = false;
    folderWalk = jobs::submit([root]() {
        auto folders = std::make_shared<std::vector<std::string>>();
        std::error_code ec;
        for (watch::LinkedDirectoryIterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (jobs::cancelRequested()) break;
            std::error_code entryEc;
            if (!it->is_directory(entryEc)) continue;
            const std::string name = it->path().filename().string();
            if (AssetTypes::isSkippedDirectory(name)) {
                it.disable_recursion_pending();
                continue;
            }
            folders->push_back(it->path().lexically_relative(root).generic_string());
        }
        return folders;
    }, {jobs::Lane::Background, jobs::GateClass::Default, "asset-search-folders"});
}

void AssetSearchIndex::refresh() {
    if (!project || !project->getAssetDatabase().isOpen()) return;
    if (walking && folderWalk.isReady()) {
        walking = false;
        try {
            walkedFolders = *folderWalk.get();
        } catch (const std::exception&) {
            walkedFolders.clear();
        }
        databaseVersion = 0;   // the folders join the records
    }
    const uint64_t db = project->getAssetDatabase().getVersion();
    const uint64_t labels = project->getAssetLabels().version();
    if (db == databaseVersion && labels == labelsVersion) return;
    if (db != databaseVersion && !walking && databaseVersion != 0) walkWanted = true;
    databaseVersion = db;
    labelsVersion = labels;
    rebuild();
    if (walkWanted && !walking) startFolderWalk();
}

void AssetSearchIndex::rebuild() {
    auto list = std::make_shared<std::vector<AssetRecord>>();
    std::vector<AssetRecord>& records = *list;
    byPath.clear();
    byGuid.clear();
    const AssetLabels& labelStore = project->getAssetLabels();
    std::unordered_map<Guid, std::vector<std::string>> labels;
    for (auto& [guid, list] : labelStore.all()) labels.emplace(guid, std::move(list));

    std::set<std::string> folders(walkedFolders.begin(), walkedFolders.end());
    records.reserve(project->getAssetDatabase().assetCount() + folders.size());
    project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
        if (info.type == AssetType::SubAsset || info.type == AssetType::Unknown) return;
        AssetRecord record;
        record.guid = info.guid.str();
        record.path = info.path;
        const fs::path path(info.path);
        record.name = path.filename().string();
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        record.ext = ext;
        record.type = assetTypeName(info.type);
        if (auto it = labels.find(info.guid); it != labels.end()) record.labels = it->second;
        prepareAssetRecord(record);
        records.push_back(std::move(record));
        // its folders, even when the walk has not seen them yet
        for (fs::path parent = path.parent_path(); !parent.empty() && parent != "."; parent = parent.parent_path()) {
            if (!folders.insert(parent.generic_string()).second) break;
        }
    });
    for (const std::string& folder : folders) {
        AssetRecord record;
        record.path = folder;
        record.name = fs::path(folder).filename().string();
        record.type = "folder";
        record.folder = true;
        prepareAssetRecord(record);
        records.push_back(std::move(record));
    }
    for (size_t i = 0; i < records.size(); i++) {
        byPath[records[i].path] = i;
        if (!records[i].guid.empty()) byGuid[records[i].guid] = i;
    }
    this->records = std::move(list);
    revision++;
}

const AssetRecord* AssetSearchIndex::findByPath(const std::string& projectRelative) const {
    auto it = byPath.find(projectRelative);
    return it == byPath.end() ? nullptr : &(*records)[it->second];
}

const AssetRecord* AssetSearchIndex::findByGuid(const std::string& guid) const {
    auto it = byGuid.find(guid);
    return it == byGuid.end() ? nullptr : &(*records)[it->second];
}

std::unordered_set<std::string> AssetSearchIndex::referrersOf(const std::string& pathOrGuid) const {
    std::unordered_set<std::string> out;
    if (!project) return out;
    Guid target;
    std::string targetPath;
    if (Guid::isValidText(pathOrGuid)) {
        target = Guid::parse(pathOrGuid);
        targetPath = project->getAssetDatabase().getPath(target).generic_string();
    } else {
        targetPath = fs::path(pathOrGuid).lexically_normal().generic_string();
        target = project->getAssetDatabase().getAssetGuid(targetPath);
    }
    const AssetIndex& index = project->getAssetIndex();
    for (const DepEdge& edge : index.referencesTo(target, targetPath, false, SIZE_MAX)) {
        if (!edge.referrer.isNull()) out.insert(edge.referrer.str());
        const std::string referrerPath = index.referrerPath(edge.referrer);
        if (!referrerPath.empty()) out.insert(referrerPath);
    }
    return out;
}

std::unordered_set<std::string> AssetSearchIndex::withMissingReferences() const {
    std::unordered_set<std::string> out;
    if (!project) return out;
    MissingAssetTracker& missing = project->getMissingAssets();
    for (const MissingAssetTracker::Group& group : missing.getGroups()) {
        for (const MissingAssetTracker::Referrer& referrer : group.referrers) {
            if (!referrer.owner.isNull()) out.insert(referrer.owner.str());
        }
    }
    return out;
}
