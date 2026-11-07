// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AssetOps.h"

#include "Out.h"
#include "Project.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetTypes.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"
#include "util/Util.h"
#include "pose/HumanoidMapIO.h"
#include "project/SceneFileIdentity.h"
#include "titleart/TitleArtBake.h"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    using Result = AssetOps::Result;

    // The files an operation moves: the path, and its meta beside it when it is a file
    // (a directory carries the metas inside it). A model's .humanoid bone mapping (pose
    // animation) goes with it the same way: it describes that model file
    // A .titleart's derived files (its baked PNGs, their metas, its bake sidecar) go with it on a
    // move, a delete and its restore, renamed after the document (VatAssetFiles' pairing); a copy
    // leaves them (`derived` false): the copy has a GUID of its own and bakes its own outputs
    std::vector<std::pair<fs::path, fs::path>> withMeta(const fs::path& from, const fs::path& to, bool derived = true) {
        std::vector<std::pair<fs::path, fs::path>> pairs{{from, to}};
        std::error_code ec;
        if (derived && fs::is_regular_file(from, ec) && Util::isTitleArtFile(FileUtils::pathToUtf8(from))) {
            const std::string oldName = from.filename().string();
            for (const fs::path& file : titleArtDerivedFiles(from)) {
                const std::string name = file.filename().string();
                const std::string newName = name.compare(0, oldName.size(), oldName) == 0 ? to.filename().string() + name.substr(oldName.size()) : name;
                const fs::path rest = file.parent_path().lexically_relative(from.parent_path());
                pairs.push_back({file, (to.parent_path() / rest / newName).lexically_normal()});
            }
        }
        if (fs::is_regular_file(from, ec)) {
            const fs::path meta = AssetTypes::metaPathFor(from);
            if (fs::is_regular_file(meta, ec)) {
                pairs.push_back({meta, AssetTypes::metaPathFor(to)});
            }
            if (Util::isModelFile(FileUtils::pathToUtf8(from))) {
                const fs::path humanoid = FileUtils::pathFromUtf8(pose::humanoidSidecarPath(FileUtils::pathToUtf8(from)));
                if (fs::is_regular_file(humanoid, ec)) {
                    pairs.push_back({humanoid, FileUtils::pathFromUtf8(pose::humanoidSidecarPath(FileUtils::pathToUtf8(to)))});
                }
            }
        }
        return pairs;
    }

    std::string relativeTo(const Project& project, const fs::path& path) {
        return project.normalizeToProjectRelative(path).generic_string();
    }

    void addWritten(Result& result, const Project& project, const fs::path& path) {
        const std::string relative = relativeTo(project, path);
        if (std::find(result.filesWritten.begin(), result.filesWritten.end(), relative) == result.filesWritten.end()) {
            result.filesWritten.push_back(relative);
        }
    }

    // The files a directory move or copy carries (for files_written)
    void addWrittenTree(Result& result, const Project& project, const fs::path& root) {
        std::error_code ec;
        if (fs::is_regular_file(root, ec)) {
            addWritten(result, project, root);
            return;
        }
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->is_regular_file(ec)) addWritten(result, project, it->path());
        }
    }

    bool isShaderOrDirectory(const fs::path& path) {
        std::error_code ec;
        if (fs::is_directory(path, ec)) {
            return true;
        }
        const AssetTypeInfo* info = AssetTypes::find(path.filename(), fs::path());
        return info && info->type == AssetType::Shader;
    }

    // The database's changes reach the rest of the editor; what the identity operations
    // wrote (metas of files that became assets) is part of files_written
    void finish(Project& project, Result& result) {
        for (const AssetDatabase::Change& change : result.changes) {
            if (change.type == AssetDatabase::Change::Type::Added) {
                addWritten(result, project, project.getProjectPath() / change.path);
                const AssetTypeInfo* info = AssetTypes::find(change.path, fs::path());
                if (info && info->identity == AssetIdentity::Sidecar) {
                    addWritten(result, project, project.getProjectPath() / AssetTypes::metaPathFor(change.path));
                }
            }
        }
        project.onAssetFilesChanged(result.changes);
        result.ok = true;
    }

}

namespace {

    bool intoItself(const fs::path& from, const fs::path& to) {
        std::error_code ec;
        if (!fs::is_directory(from, ec)) {
            return false;
        }
        // strictly below: the folder's own place is a duplicate beside it (a free name), not into it
        auto below = [](const fs::path& path, const fs::path& dir) {
            return safewrite::isAtOrBelow(path, dir) && !safewrite::isAtOrBelow(dir, path);
        };
        if (below(to.lexically_normal(), from.lexically_normal())) {
            return true;
        }
        const fs::path fromReal = safewrite::resolvedPath(from);
        const fs::path toReal = safewrite::resolvedPath(to);
        return !fromReal.empty() && !toReal.empty() && below(toReal, fromReal);
    }

    std::string movedName(const fs::path& path) {
        return "'" + FileUtils::pathToUtf8(path.filename()) + "'";
    }

}

std::string AssetOps::moveRefusal(const fs::path& from, const fs::path& to) {
    if (intoItself(from, to)) {
        return "Can't move " + movedName(from) + " into itself: pick a folder outside it.";
    }
    return std::string();
}

std::string AssetOps::copyRefusal(const fs::path& from, const fs::path& to) {
    if (intoItself(from, to)) {
        return "Can't copy " + movedName(from) + " into itself: pick a folder outside it.";
    }
    std::error_code ec;
    if (fs::is_directory(from, ec)) {
        size_t entries = 0;
        for (fs::recursive_directory_iterator it(from, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end && entries < 200000; it.increment(ec), entries++) {
            if (safewrite::isProjectFileName(it->path())) {
                return "Can't copy " + movedName(from) + ": it holds " + FileUtils::pathToGenericUtf8(it->path().lexically_relative(from.parent_path())) +
                       ", and a second project.yaml would be another project inside this one.";
            }
        }
    }
    return std::string();
}

std::vector<std::pair<fs::path, fs::path>> AssetOps::withCompanions(const fs::path& from, const fs::path& to) {
    return withMeta(from, to);
}

fs::path AssetOps::uniqueTrashPath(const fs::path& trashDir, const fs::path& file) {
    std::error_code ec;
    fs::path candidate = trashDir / file.filename();
    if (!fs::exists(candidate, ec)) {
        return candidate;
    }
    const std::string stem = file.stem().string();
    const std::string extension = file.extension().string();
    for (int counter = 1;; counter++) {
        candidate = trashDir / (stem + " (" + std::to_string(counter) + ")" + extension);
        if (!fs::exists(candidate, ec) && !fs::exists(AssetTypes::metaPathFor(candidate), ec)) {
            return candidate;
        }
    }
}

std::vector<AssetOps::TextReference> AssetOps::textReferencesInto(Project& project, const fs::path& path) {
    std::vector<TextReference> broken;
    const std::string relative = relativeTo(project, path);
    if (relative.empty()) {
        return broken;
    }
    const Guid guid = project.getAssetDatabase().getAssetGuid(relative);
    for (const DepEdge& edge : project.getAssetIndex().referencesTo(guid, relative, false, 100000)) {
        if (!isTextEdge(edge.kind)) {
            continue;
        }
        // a referrer that moves along (inside a moved directory) keeps its relative links
        const std::string referrer = project.getAssetIndex().referrerPath(edge.referrer);
        if (referrer == relative || referrer.compare(0, relative.size() + 1, relative + "/") == 0) {
            continue;
        }
        broken.push_back({referrer, edge.line, edge.targetPath, edge.kind});
    }
    std::sort(broken.begin(), broken.end(), [](const TextReference& a, const TextReference& b) {
        return a.referrer != b.referrer ? a.referrer < b.referrer : a.line < b.line;
    });
    return broken;
}

Result AssetOps::move(Project& project, const fs::path& from, const fs::path& to) {
    Result result;
    std::error_code ec;
    if (!fs::exists(from, ec)) {
        result.error = from.string() + " does not exist";
        return result;
    }
    if (std::string refusal = moveRefusal(from, to); !refusal.empty()) {
        result.error = std::move(refusal);
        return result;
    }
    if (fs::exists(to, ec)) {
        result.error = to.string() + " already exists";
        return result;
    }
    // a file no asset refresh has seen yet (written a moment ago: the refresh is a Background
    // walk) gets its identity first, so the references bound to it below follow the move
    AssetDatabase& database = project.getAssetDatabase();
    if (database.isOpen() && (fs::is_directory(from, ec) || database.getAssetGuid(from).isNull())) {
        project.onAssetFilesChanged(database.importAssets(from));
    }
    // a path assigned in an open document since its last save follows the file too
    project.bindLoadedAssetRefs();
    result.textReferencesBroken = textReferencesInto(project, from);
    const bool shaders = isShaderOrDirectory(from) || isShaderOrDirectory(to);

    // the file, then its meta; a failure puts back what moved
    const auto pairs = withMeta(from, to);
    for (size_t i = 0; i < pairs.size(); i++) {
        if (!assetio::renameFile(pairs[i].first, pairs[i].second, &result.error)) {
            for (size_t back = i; back-- > 0;) {
                std::string ignored;
                assetio::renameFile(pairs[back].second, pairs[back].first, &ignored);
            }
            return result;
        }
    }
    for (const auto& [source, target] : pairs) {
        addWrittenTree(result, project, target);
    }

    result.changes = project.getAssetDatabase().moveAssets(from, to);
    // a document's derived files keep their GUIDs too (scenes place the baked PNGs by GUID)
    for (size_t i = 1; i < pairs.size(); i++) {
        if (AssetTypes::isMetaFile(pairs[i].first) || !Util::isImageFile(FileUtils::pathToUtf8(pairs[i].first))) continue;
        if (fs::is_directory(from, ec)) break;
        const std::vector<AssetDatabase::Change> derived = project.getAssetDatabase().moveAssets(pairs[i].first, pairs[i].second);
        result.changes.insert(result.changes.end(), derived.begin(), derived.end());
    }
    // the scene list and the open tabs name scene files by path
    if (project.moveSceneFileHints(from, to)) {
        addWritten(result, project, project.getProjectPath() / "project.yaml");
    }
    finish(project, result);
    // the open clip documents (a clip's, a model's sub-clips) and the asset the Inspector shows
    // go with their files
    project.remapClipFilePath(from, to);
    project.remapInspectedAsset(from, to);
    if (shaders) {
        project.invalidateCustomShaders();
    }
    return result;
}

namespace {

    // Two files with one scene id make the loader renumber one of them; two scenes with one name
    // clash in the generated code: each copied .scene gets its own of both
    bool giveCopiedScenesTheirIdentity(Project& project, const fs::path& from, const fs::path& to,
                                       std::map<std::string, AssetOps::SceneCopy>* copyScenes, AssetOps::Result& result) {
        std::vector<std::pair<fs::path, fs::path>> scenes;   // (source, copy)
        std::error_code ec;
        if (fs::is_directory(to, ec)) {
            for (fs::recursive_directory_iterator it(to, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && Util::isSceneFile(FileUtils::pathToUtf8(it->path()))) {
                    scenes.emplace_back(from / it->path().lexically_relative(to), it->path());
                }
            }
        } else if (Util::isSceneFile(FileUtils::pathToUtf8(to))) {
            scenes.emplace_back(from, to);
        }
        if (scenes.empty()) {
            return true;
        }
        std::sort(scenes.begin(), scenes.end());

        const fs::path fromRelative = project.normalizeToProjectRelative(from);
        const bool imported = fromRelative.empty() || fromRelative.is_absolute() || *fromRelative.begin() == "..";
        std::set<uint32_t> usedIds;
        std::set<std::string> usedNames;
        for (const SceneProject& scene : project.getScenes()) {
            usedIds.insert(scene.id);
            usedNames.insert(scene.name);
        }
        auto uniqueName = [&](const std::string& base) {
            std::string name = base;
            for (int n = 1; usedNames.count(name); n++) {
                name = base + " " + std::to_string(n);
            }
            return name;
        };

        for (const auto& [source, copy] : scenes) {
            std::string text;
            uint32_t fileId = 0;
            std::string fileName;
            if (!assetio::readFile(copy, text) || !sceneid::readSceneHeader(text, fileId, fileName)) {
                continue;  // not a scene this editor reads: no id to clash
            }
            const std::string relative = project.normalizeToProjectRelative(copy).generic_string();
            AssetOps::SceneCopy identity;
            const AssetOps::SceneCopy* recorded = nullptr;
            if (copyScenes) {
                const auto it = copyScenes->find(relative);
                if (it != copyScenes->end()) recorded = &it->second;
            }
            if (recorded) {
                // a redo: the identity the first copy got, when nothing has taken it since
                identity = *recorded;
                if (usedIds.count(identity.id)) {
                    const uint32_t taken = identity.id;
                    identity.id = project.allocateSceneId();
                    Out::warning("Scene id %u of the copy %s is taken now: it gets id %u", taken, relative.c_str(), identity.id);
                }
                if (usedNames.count(identity.name)) {
                    identity.name = uniqueName(identity.name);
                }
            } else {
                const bool keepId = imported && fileId != 0 && !usedIds.count(fileId);
                identity.id = keepId ? fileId : project.allocateSceneId();
                while (usedIds.count(identity.id)) {
                    identity.id = project.allocateSceneId();
                }
                // a name that followed the file follows the copy's file
                const std::string sourceStem = FileUtils::pathToUtf8(source.stem());
                const std::string copyStem = FileUtils::pathToUtf8(copy.stem());
                identity.name = uniqueName(fileName.empty() || fileName == sourceStem ? copyStem : fileName);
            }
            usedIds.insert(identity.id);
            usedNames.insert(identity.name);

            if (identity.id != fileId || identity.name != fileName) {
                std::string error;
                if (!sceneid::rewriteSceneHeader(copy, identity.id, identity.name, &error)) {
                    result.error = "cannot give the copied scene " + relative + " its own id: " + error;
                    return false;
                }
                FileUtils::reportWritten(copy);
            }
            if (copyScenes) (*copyScenes)[relative] = identity;
            result.copiedScenes.emplace_back(relative, identity);
        }
        return true;
    }

}

Result AssetOps::copy(Project& project, const fs::path& from, const fs::path& to, std::map<std::string, std::string>* copyGuids,
                      std::map<std::string, SceneCopy>* copyScenes) {
    Result result;
    std::error_code ec;
    if (!fs::exists(from, ec)) {
        result.error = from.string() + " does not exist";
        return result;
    }
    if (std::string refusal = copyRefusal(from, to); !refusal.empty()) {
        result.error = std::move(refusal);
        return result;
    }
    if (fs::exists(to, ec)) {
        result.error = to.string() + " already exists";
        return result;
    }
    // the metas go along (their import settings and labels); the copies get new GUIDs below
    for (const auto& [source, target] : withMeta(from, to, false)) {
        // in a write scope (util/SafeWrite.h) beneath its root, never through a planted link
        if (target.has_parent_path()) FileUtils::createDirectories(target.parent_path(), ec);
        if (!FileUtils::copyPath(source, target, false, ec)) {
            const std::string why = safewrite::lastScopedError().empty() ? ec.message() : safewrite::lastScopedError();
            result.error = "cannot copy " + source.string() + ": " + why;
            std::error_code ignored;
            FileUtils::removeAll(to, ignored);
            FileUtils::removePath(AssetTypes::metaPathFor(to), ignored);
            return result;
        }
        // the editor's own copy: the file watcher does not report it back (S05 R10)
        FileUtils::reportWritten(target);
        addWrittenTree(result, project, target);
    }
    result.changes = project.getAssetDatabase().importAssets(to, true, copyGuids);
    if (!giveCopiedScenesTheirIdentity(project, from, to, copyScenes, result)) {
        std::error_code ignored;
        project.getAssetDatabase().removeAssets(to);
        FileUtils::removeAll(to, ignored);
        FileUtils::removePath(AssetTypes::metaPathFor(to), ignored);
        return result;
    }
    finish(project, result);
    if (isShaderOrDirectory(to)) {
        project.invalidateCustomShaders();
    }
    return result;
}

Result AssetOps::removeCopy(Project& project, const fs::path& path) {
    Result result;
    std::error_code ec;
    const bool shaders = isShaderOrDirectory(path);
    for (const auto& [source, target] : withMeta(path, path)) {
        FileUtils::removeAll(source, ec);
        FileUtils::reportRemoved(source);
    }
    result.changes = project.getAssetDatabase().removeAssets(path);
    finish(project, result);
    project.cleanupInspectedAsset(path);
    if (shaders) {
        project.invalidateCustomShaders();
    }
    return result;
}

Result AssetOps::remove(Project& project, const fs::path& path, fs::path& trashPath) {
    Result result;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        result.error = path.string() + " does not exist";
        return result;
    }
    // a path assigned in an open document since its last save is listed by the GUID it named
    project.bindLoadedAssetRefs();
    result.textReferencesBroken = textReferencesInto(project, path);
    const bool shaders = isShaderOrDirectory(path);
    const fs::path trashDir = project.getProjectPath() / ".trash";
    // the editor's own .trash: named for the deny-by-default guard of an MCP call's scope
    safewrite::EditorInternal ownTrash(trashDir);
    FileUtils::createDirectories(trashDir, ec);
    trashPath = uniqueTrashPath(trashDir, path);
    const auto pairs = withMeta(path, trashPath);
    for (size_t i = 0; i < pairs.size(); i++) {
        if (!assetio::renameFile(pairs[i].first, pairs[i].second, &result.error)) {
            for (size_t back = i; back-- > 0;) {
                std::string ignored;
                assetio::renameFile(pairs[back].second, pairs[back].first, &ignored);
            }
            trashPath.clear();
            return result;
        }
    }
    // the references stay: they show as Missing until the file comes back (R15)
    result.changes = project.getAssetDatabase().removeAssets(path);
    finish(project, result);
    project.cleanupInspectedAsset(path);
    if (shaders) {
        project.invalidateCustomShaders();
    }
    return result;
}

Result AssetOps::restore(Project& project, const fs::path& trashPath, const fs::path& original) {
    Result result;
    std::error_code ec;
    if (!fs::exists(trashPath, ec)) {
        result.error = trashPath.string() + " is no longer in the trash";
        return result;
    }
    if (fs::exists(original, ec)) {
        result.error = original.string() + " already exists";
        return result;
    }
    // out of the editor's own .trash (the deny-by-default guard of an MCP call's scope)
    safewrite::EditorInternal ownTrash(trashPath.parent_path());
    const auto pairs = withMeta(trashPath, original);
    for (size_t i = 0; i < pairs.size(); i++) {
        if (!assetio::renameFile(pairs[i].first, pairs[i].second, &result.error)) {
            for (size_t back = i; back-- > 0;) {
                std::string ignored;
                assetio::renameFile(pairs[back].second, pairs[back].first, &ignored);
            }
            return result;
        }
    }
    result.changes = project.getAssetDatabase().importAssets(original);
    finish(project, result);
    if (isShaderOrDirectory(original)) {
        project.invalidateCustomShaders();
    }
    return result;
}
