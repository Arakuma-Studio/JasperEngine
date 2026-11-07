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
#include "animation/ModelClipSettings.h"
#include "pose/HumanoidMapIO.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    using Result = AssetOps::Result;

    // The files an operation moves: the path, and its meta beside it when it is a file
    // (a directory carries the metas inside it). A model's .humanoid bone mapping (pose
    // animation) and .clips settings (S74 D4) go with it the same way: they describe that model file
    std::vector<std::pair<fs::path, fs::path>> withMeta(const fs::path& from, const fs::path& to) {
        std::vector<std::pair<fs::path, fs::path>> pairs{{from, to}};
        std::error_code ec;
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
                const fs::path clips = FileUtils::pathFromUtf8(anim::modelClipSettingsPath(FileUtils::pathToUtf8(from)));
                if (fs::is_regular_file(clips, ec)) {
                    pairs.push_back({clips, FileUtils::pathFromUtf8(anim::modelClipSettingsPath(FileUtils::pathToUtf8(to)))});
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

Result AssetOps::copy(Project& project, const fs::path& from, const fs::path& to, std::map<std::string, std::string>* copyGuids) {
    Result result;
    std::error_code ec;
    if (!fs::exists(from, ec)) {
        result.error = from.string() + " does not exist";
        return result;
    }
    if (fs::exists(to, ec)) {
        result.error = to.string() + " already exists";
        return result;
    }
    // the metas go along (their import settings and labels); the copies get new GUIDs below
    for (const auto& [source, target] : withMeta(from, to)) {
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
