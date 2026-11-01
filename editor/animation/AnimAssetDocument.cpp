// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimAssetDocument.h"

#include "Out.h"
#include "Project.h"
#include "animation/ModelSubClips.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "pool/AnimatorControllerPool.h"
#include "pool/AvatarMaskPool.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    std::string keyOf(const fs::path& absolute){
        return absolute.lexically_normal().generic_string();
    }

    bool isSameOrUnder(const fs::path& base, const fs::path& path){
        const std::string b = base.lexically_normal().generic_string();
        const std::string p = path.lexically_normal().generic_string();
        if (b.empty() || p.empty()) return false;
        if (p == b) return true;
        const std::string prefix = (b.back() == '/') ? b : b + "/";
        return p.compare(0, prefix.size(), prefix) == 0;
    }

    bool isInside(const fs::path& root, const fs::path& absolute, fs::path* relative = nullptr){
        const fs::path rel = absolute.lexically_normal().lexically_relative(root.lexically_normal());
        if (rel.empty() || *rel.begin() == "..") return false;
        if (relative) *relative = rel;
        return true;
    }

    // a path in any accepted form (absolute, project-relative, asset-relative) to an absolute one
    // inside the project; empty when outside
    fs::path absoluteOf(Project* project, const std::string& path){
        if (!project || project->getProjectPath().empty() || path.empty()) return fs::path();
        const fs::path root = project->getProjectPath().lexically_normal();
        const fs::path given = FileUtils::pathFromUtf8(path);
        fs::path absolute;
        if (given.is_absolute()){
            absolute = given.lexically_normal();
        }else{
            absolute = (root / given).lexically_normal();
            std::error_code ec;
            if (!fs::exists(absolute, ec)){
                const fs::path assetForm = project->resolveAssetPath(given);
                if (!assetForm.empty() && fs::exists(assetForm, ec)) absolute = assetForm.lexically_normal();
            }
        }
        return isInside(root, absolute) ? absolute : fs::path();
    }

    // ---- the Animation Players of the open scenes, prefab stages and loaded prefab assets ----

    void forEachPlayer(Project* project, const std::function<void(AnimAssetUser, AnimationPlayerComponent&)>& visit){
        if (!project) return;
        auto visitRegistry = [&](EntityRegistry* registry, AnimAssetUser site){
            if (!registry) return;
            auto players = registry->getComponentArray<AnimationPlayerComponent>();
            for (size_t i = 0; i < players->size(); i++){
                site.entity = players->getEntity(i);
                visit(site, players->getComponentFromIndex(i));
            }
        };
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()){
            if (!sceneProject->scene) continue;
            AnimAssetUser site;
            site.sceneId = sceneProject->id;
            visitRegistry(sceneProject->scene, site);
        }
        project->forEachPrefabAsset([&](const fs::path& path, Prefab& prefab){
            AnimAssetUser site;
            site.prefabAsset = true;
            site.prefabPath = path.generic_string();
            visitRegistry(prefab.registry.get(), site);
        });
    }

    fs::path storedFile(Project* project, const std::string& stored){
        return stored.empty() ? fs::path() : project->resolveAssetPath(FileUtils::pathFromUtf8(stored)).lexically_normal();
    }

    // ---- per type ----

    template<class Data> struct Traits;

    template<> struct Traits<anim::AvatarMaskData>{
        static const char* what(){ return "an Avatar Mask"; }
        static bool isFile(const std::string& path){ return Util::isAvatarMaskFile(path); }
        static bool parse(const std::string& text, anim::AvatarMaskData& out, std::vector<ClipWarning>& warnings, anim::ClipParseError& error){
            anim::AvatarMaskParseResult parsed = anim::parseAvatarMask(text);
            if (!parsed.ok()){
                error = parsed.error;
                return false;
            }
            out = std::move(*parsed.data);
            warnings = std::move(parsed.warnings);
            return true;
        }
        static std::string write(const anim::AvatarMaskData& data){ return anim::writeAvatarMask(data); }
        static void normalize(anim::AvatarMaskData& data, std::vector<ClipWarning>* warnings){ anim::normalizeAvatarMask(data, warnings); }
        static std::string& guid(anim::AvatarMaskData& data){ return data.guid; }
        static const std::string& guid(const anim::AvatarMaskData& data){ return data.guid; }
        static void invalidate(const fs::path& absolute){ AvatarMaskPool::invalidate(absolute.generic_string()); }
        // the players whose layers' masks are (under) `changed` load their layers again (the
        // pool generation alone reaches the others: runtime masks, controller layers)
        static void markPlayers(Project* project, const fs::path& changed){
            forEachPlayer(project, [&](AnimAssetUser, AnimationPlayerComponent& player){
                for (const AnimationPlayerLayer& layer : player.layers){
                    if (!layer.mask.empty() && isSameOrUnder(changed, storedFile(project, layer.mask))) player.needsLoad = true;
                }
            });
        }
    };

    template<> struct Traits<anim::ControllerData>{
        static const char* what(){ return "an Animator Controller"; }
        static bool isFile(const std::string& path){ return Util::isAnimatorControllerFile(path); }
        static bool parse(const std::string& text, anim::ControllerData& out, std::vector<ClipWarning>& warnings, anim::ClipParseError& error){
            anim::ControllerParseResult parsed = anim::parseController(text);
            if (!parsed.ok()){
                error = parsed.error;
                return false;
            }
            out = std::move(*parsed.data);
            warnings = std::move(parsed.warnings);
            return true;
        }
        static std::string write(const anim::ControllerData& data){ return anim::writeController(data); }
        static void normalize(anim::ControllerData& data, std::vector<ClipWarning>* warnings){ anim::normalizeController(data, warnings); }
        static std::string& guid(anim::ControllerData& data){ return data.guid; }
        static const std::string& guid(const anim::ControllerData& data){ return data.guid; }
        static void invalidate(const fs::path& absolute){ AnimatorControllerPool::invalidate(absolute.generic_string()); }
        static void markPlayers(Project* project, const fs::path& changed){
            forEachPlayer(project, [&](AnimAssetUser, AnimationPlayerComponent& player){
                if (!player.controller.empty() && isSameOrUnder(changed, storedFile(project, player.controller))) player.needsLoad = true;
            });
        }
    };

    template<class Data>
    struct Registry{
        Project* project = nullptr;
        fs::path projectPath;
        std::map<std::string, std::shared_ptr<AnimAssetDocument<Data>>> documents;
        std::function<void(const std::vector<fs::path>&)> filesWrittenHook;
    };

    template<class Data>
    Registry<Data>& registry(){
        static Registry<Data> instance;
        return instance;
    }

    template<class Data>
    void useProject(Project* project){
        Registry<Data>& reg = registry<Data>();
        if (reg.project != project || reg.projectPath != project->getProjectPath()){
            reg.documents.clear();
            reg.project = project;
            reg.projectPath = project->getProjectPath();
        }
    }

    // the absolute file of a document path, and its project-relative form
    template<class Data>
    bool resolveTarget(Project* project, const std::string& path, fs::path& absolute, std::string& relative, std::string* error){
        if (!project || project->getProjectPath().empty()) return fail(error, "no project is open");
        if (path.empty()) return fail(error, "no path given");
        const fs::path root = project->getProjectPath().lexically_normal();
        const fs::path given = FileUtils::pathFromUtf8(path);
        if (given.is_absolute()){
            absolute = given.lexically_normal();
        }else{
            absolute = (root / given).lexically_normal();
            std::error_code ec;
            if (!fs::exists(absolute, ec)){
                const fs::path assetForm = project->resolveAssetPath(given);
                if (!assetForm.empty() && fs::exists(assetForm, ec)) absolute = assetForm.lexically_normal();
            }
        }
        fs::path rel;
        if (!isInside(root, absolute, &rel)) return fail(error, "'" + path + "' is outside the project");
        if (!Traits<Data>::isFile(absolute.string())){
            return fail(error, "'" + path + "' is not " + std::string(Traits<Data>::what()) + " file");
        }
        relative = rel.generic_string();
        return true;
    }

}

// ---- users ----

std::vector<AnimAssetUser> jasperengine::editor::findMaskUsers(Project* project, const std::string& mask){
    std::vector<AnimAssetUser> users;
    if (!project || project->getProjectPath().empty() || mask.empty()) return users;
    std::string guid;
    fs::path file;
    if (anim::isClipGuid(mask)){
        guid = mask;
        const fs::path relative = project->getAssetDatabase().getPath(guid);
        if (!relative.empty()) file = (project->getProjectPath() / relative).lexically_normal();
    }else{
        file = absoluteOf(project, mask);
        if (file.empty()) return users;
        guid = project->getAssetDatabase().getGuid(file);
    }
    forEachPlayer(project, [&](AnimAssetUser site, AnimationPlayerComponent& player){
        for (size_t i = 0; i < player.layers.size(); i++){
            const AnimationPlayerLayer& layer = player.layers[i];
            const bool match = (!guid.empty() && layer.maskGuid == guid) ||
                               (!layer.mask.empty() && !file.empty() && storedFile(project, layer.mask) == file);
            if (!match) continue;
            site.layer = (int)i;
            users.push_back(site);
        }
    });
    // the layers of the project's controllers
    const fs::path root = project->getProjectPath().lexically_normal();
    for (const fs::path& controllerFile : ProjectChecks::findProjectFiles(root, {".controller"})){
        std::string text;
        if (!ProjectChecks::readFile(controllerFile, text)) continue;
        anim::ControllerParseResult parsed = anim::parseController(text);
        if (!parsed.ok()) continue;
        fs::path relative;
        isInside(root, controllerFile, &relative);
        for (size_t l = 0; l < parsed.data->layers.size(); l++){
            const anim::ControllerLayer& layer = parsed.data->layers[l];
            const bool match = (!guid.empty() && layer.maskGuid == guid) ||
                               (!layer.mask.empty() && !file.empty() && storedFile(project, layer.mask) == file);
            if (!match) continue;
            AnimAssetUser user;
            user.layer = (int)l;
            user.controllerFile = relative.generic_string();
            users.push_back(user);
        }
    }
    return users;
}

std::vector<AnimAssetUser> jasperengine::editor::findControllerUsers(Project* project, const std::string& controller){
    std::vector<AnimAssetUser> users;
    if (!project || project->getProjectPath().empty() || controller.empty()) return users;
    std::string guid;
    fs::path file;
    if (anim::isClipGuid(controller)){
        guid = controller;
        const fs::path relative = project->getAssetDatabase().getPath(guid);
        if (!relative.empty()) file = (project->getProjectPath() / relative).lexically_normal();
    }else{
        file = absoluteOf(project, controller);
        if (file.empty()) return users;
        guid = project->getAssetDatabase().getGuid(file);
    }
    forEachPlayer(project, [&](AnimAssetUser site, AnimationPlayerComponent& player){
        const bool match = (!guid.empty() && player.controllerGuid == guid) ||
                           (!player.controller.empty() && !file.empty() && storedFile(project, player.controller) == file);
        if (match) users.push_back(site);
    });
    return users;
}

// ---- reference remaps inside controllers ----

std::vector<AnimAssetHintEdit> jasperengine::editor::remapControllerReferences(Project* project, const fs::path& oldPath, const fs::path& newPath){
    std::vector<AnimAssetHintEdit> edits;
    if (!project || project->getProjectPath().empty()) return edits;
    const fs::path root = project->getProjectPath().lexically_normal();
    const fs::path oldAbsolute = (oldPath.is_absolute() ? oldPath : root / oldPath).lexically_normal();
    const fs::path newAbsolute = (newPath.is_absolute() ? newPath : root / newPath).lexically_normal();
    std::error_code ec;
    // what a controller names: clips (.anim, a model's sub-clips), masks, or a directory of some
    const std::string oldText = oldAbsolute.string();
    if (!fs::is_directory(newAbsolute, ec) && !Util::isAnimationClipFile(oldText) && !Util::isModelFile(oldText) &&
        !Util::isAvatarMaskFile(oldText)){
        return edits;
    }
    auto remapStored = [&](std::string& stored){
        if (stored.empty()) return false;
        std::string clipPart;
        std::string filePart = stored;
        anim::SubClipReference reference;
        if (anim::parseSubClipReference(stored, reference)){
            filePart = reference.model;
            clipPart = reference.clip;
        }
        const fs::path file = storedFile(project, filePart);
        if (!isSameOrUnder(oldAbsolute, file)) return false;
        const fs::path rest = file.lexically_relative(oldAbsolute);
        const fs::path moved = (rest == "." ? newAbsolute : newAbsolute / rest).lexically_normal();
        const std::string form = project->normalizeToAssetsRelative(moved).generic_string();
        const std::string updated = clipPart.empty() && !anim::isSubClipReference(stored) ? form : anim::formatSubClipReference(form, clipPart);
        if (updated == stored) return false;
        stored = updated;
        return true;
    };
    const std::string oldName = oldAbsolute.filename().string();
    std::vector<fs::path> written;
    for (const fs::path& file : ProjectChecks::findProjectFiles(root, {".controller"})){
        std::string text;
        if (!ProjectChecks::readFile(file, text) || oldName.empty() || text.find(oldName) == std::string::npos) continue;
        anim::ControllerParseResult parsed = anim::parseController(text);
        if (!parsed.ok()) continue;
        anim::ControllerData data = *parsed.data;
        bool changed = false;
        for (anim::ControllerLayer& layer : data.layers){
            changed |= remapStored(layer.mask);
            for (anim::ControllerState& state : layer.states){
                changed |= remapStored(state.clip);
                for (anim::BlendTreeChild& child : state.blendTree.children) changed |= remapStored(child.clip);
            }
        }
        if (!changed) continue;
        const std::string newText = anim::writeController(data);
        if (newText == text) continue;
        if (!FileUtils::writeFileReplacing(file, newText)){
            Out::warning("Could not update the clip and mask paths of %s", file.string().c_str());
            continue;
        }
        edits.push_back({file, text, newText});
        written.push_back(file);
    }
    if (!written.empty()){
        AnimatorControllerDocument::filesChanged(written, project);
        // the MCP file sync: rewritten by the editor, not changed outside it
        if (registry<anim::ControllerData>().filesWrittenHook) registry<anim::ControllerData>().filesWrittenHook(written);
    }
    return edits;
}

std::vector<fs::path> jasperengine::editor::relinkControllerGuids(Project* project, const std::map<std::string, fs::path>& moved){
    std::vector<fs::path> written;
    if (!project || project->getProjectPath().empty() || moved.empty()) return written;
    auto relink = [&](std::string& stored, const std::string& guid){
        if (guid.empty() || anim::isSubClipReference(stored)) return false;
        const auto found = moved.find(guid);
        if (found == moved.end()) return false;
        const std::string form = project->normalizeToAssetsRelative(found->second).generic_string();
        if (fs::path(stored).lexically_normal().generic_string() == form) return false;
        stored = form;
        return true;
    };
    for (const fs::path& file : ProjectChecks::findProjectFiles(project->getProjectPath().lexically_normal(), {".controller"})){
        std::string text;
        if (!ProjectChecks::readFile(file, text)) continue;
        bool named = false;
        for (const auto& entry : moved) named |= text.find(entry.first) != std::string::npos;
        if (!named) continue;
        anim::ControllerParseResult parsed = anim::parseController(text);
        if (!parsed.ok()) continue;
        anim::ControllerData data = *parsed.data;
        bool changed = false;
        for (anim::ControllerLayer& layer : data.layers){
            changed |= relink(layer.mask, layer.maskGuid);
            for (anim::ControllerState& state : layer.states){
                changed |= relink(state.clip, state.clipGuid);
                for (anim::BlendTreeChild& child : state.blendTree.children) changed |= relink(child.clip, child.clipGuid);
            }
        }
        if (!changed) continue;
        const std::string newText = anim::writeController(data);
        if (newText == text) continue;
        if (!FileUtils::writeFileReplacing(file, newText)){
            Out::warning("Could not update the clip and mask paths of %s", file.string().c_str());
            continue;
        }
        written.push_back(file);
    }
    if (!written.empty()){
        AnimatorControllerDocument::filesChanged(written, project);
        // the MCP file sync: rewritten by the editor, not changed outside it
        if (registry<anim::ControllerData>().filesWrittenHook) registry<anim::ControllerData>().filesWrittenHook(written);
    }
    return written;
}

// ---- documents ----

template<class Data>
void AnimAssetDocument<Data>::documentFilesWritten(Project* project, const std::vector<fs::path>& changed){
    for (const fs::path& path : changed){
        std::error_code ec;
        if (fs::is_regular_file(path, ec)) project->importPrefabFiles(path);
        else project->cleanupClipFilePath(path);
    }
    filesChanged(changed, project);
    if (registry<Data>().filesWrittenHook) registry<Data>().filesWrittenHook(changed);
}

template<class Data>
void AnimAssetDocument<Data>::setFilesWrittenHook(std::function<void(const std::vector<fs::path>&)> hook){
    registry<Data>().filesWrittenHook = std::move(hook);
}

template<class Data>
std::string AnimAssetDocument<Data>::fileText(const Data& data){
    return Traits<Data>::write(data);
}

template<class Data>
std::shared_ptr<AnimAssetDocument<Data>> AnimAssetDocument<Data>::open(Project* project, const std::string& path, std::string* error){
    fs::path absolute;
    std::string relative;
    if (!resolveTarget<Data>(project, path, absolute, relative, error)) return nullptr;
    useProject<Data>(project);
    Registry<Data>& reg = registry<Data>();
    const std::string key = keyOf(absolute);
    auto it = reg.documents.find(key);
    if (it != reg.documents.end()){
        std::shared_ptr<AnimAssetDocument> document = it->second;
        if (document->stampChanged() || !document->onDisk) document->reloadFromDisk();
        if (!document->onDisk){
            fail(error, "'" + document->relativePath + "' does not exist");
            return nullptr;
        }
        return document;
    }
    std::shared_ptr<AnimAssetDocument> document(new AnimAssetDocument());
    document->project = project;
    document->relativePath = relative;
    document->filePath = absolute;
    if (!document->load(error)) return nullptr;
    reg.documents[key] = document;
    return document;
}

template<class Data>
std::shared_ptr<AnimAssetDocument<Data>> AnimAssetDocument<Data>::create(Project* project, const std::string& path, const Data& initial, std::string* error){
    fs::path absolute;
    std::string relative;
    if (!resolveTarget<Data>(project, path, absolute, relative, error)) return nullptr;
    std::error_code ec;
    if (fs::exists(absolute, ec)){
        fail(error, "'" + relative + "' already exists");
        return nullptr;
    }
    useProject<Data>(project);
    Registry<Data>& reg = registry<Data>();
    const std::string key = keyOf(absolute);
    std::shared_ptr<AnimAssetDocument> document;
    auto it = reg.documents.find(key);
    const bool existing = it != reg.documents.end();
    if (existing){
        document = it->second;   // a document whose file was deleted (an undone creation)
    }else{
        document.reset(new AnimAssetDocument());
        document->project = project;
        document->relativePath = relative;
        document->filePath = absolute;
        reg.documents[key] = document;
    }
    Data copy = initial;
    const AssetDatabase& database = project->getAssetDatabase();
    std::string& guid = Traits<Data>::guid(copy);
    if (!anim::isClipGuid(guid) || database.contains(guid)){
        do {
            guid = AssetDatabase::generateGuid();
        } while (database.contains(guid));
    }
    if (!document->writeUndoable(Traits<Data>::write(copy), "", error)){
        if (!existing && !document->onDisk) reg.documents.erase(key);
        return nullptr;
    }
    if (!document->onDisk) document->reloadFromDisk();
    return document;
}

template<class Data>
std::vector<std::shared_ptr<AnimAssetDocument<Data>>> AnimAssetDocument<Data>::openDocuments(){
    std::vector<std::shared_ptr<AnimAssetDocument>> documents;
    for (const auto& entry : registry<Data>().documents) documents.push_back(entry.second);
    return documents;
}

template<class Data>
bool AnimAssetDocument<Data>::load(std::string* error){
    std::error_code ec;
    if (!fs::is_regular_file(filePath, ec)) return fail(error, "'" + relativePath + "' does not exist");
    std::string content;
    if (!ProjectChecks::readFile(filePath, content)) return fail(error, "cannot read '" + relativePath + "'");
    Data data;
    std::vector<ClipWarning> parsedWarnings;
    anim::ClipParseError parseError;
    if (!Traits<Data>::parse(content, data, parsedWarnings, parseError)){
        return fail(error, "'" + relativePath + "' is not " + std::string(Traits<Data>::what()) + ": " + parseError.toString());
    }
    current = std::move(data);
    warnings = std::move(parsedWarnings);
    text = std::move(content);
    onDisk = true;
    rev++;
    takeStamp();
    return true;
}

template<class Data>
void AnimAssetDocument<Data>::takeStamp(){
    std::error_code ec;
    const auto time = fs::last_write_time(filePath, ec);
    stampTime = ec ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
    const auto size = fs::file_size(filePath, ec);
    stampSize = ec ? 0 : static_cast<uint64_t>(size);
}

template<class Data>
bool AnimAssetDocument<Data>::stampChanged() const{
    std::error_code ec;
    if (!fs::is_regular_file(filePath, ec)) return onDisk;
    const auto time = fs::last_write_time(filePath, ec);
    if (ec) return false;
    const auto size = fs::file_size(filePath, ec);
    if (ec) return false;
    return !onDisk || static_cast<int64_t>(time.time_since_epoch().count()) != stampTime || static_cast<uint64_t>(size) != stampSize;
}

template<class Data>
void AnimAssetDocument<Data>::revisionChanged(){
    Traits<Data>::invalidate(filePath);
    Traits<Data>::markPlayers(project, filePath);
}

template<class Data>
void AnimAssetDocument<Data>::reloadFromDisk(){
    reload();
}

template<class Data>
typename AnimAssetDocument<Data>::ReloadResult AnimAssetDocument<Data>::reload(){
    const ReloadResult result = reloadContent();
    // an outside change ends the merge of the edit before it
    if (!committing && result != ReloadResult::Unchanged && result != ReloadResult::Busy) mergeEpoch++;
    return result;
}

template<class Data>
typename AnimAssetDocument<Data>::ReloadResult AnimAssetDocument<Data>::reloadContent(){
    std::error_code ec;
    if (!fs::is_regular_file(filePath, ec)){
        if (onDisk){
            onDisk = false;
            text.clear();
            rev++;
            revisionChanged();
            notify(Change::Removed);
        }
        return ReloadResult::Removed;
    }
    std::string newText;
    if (!ProjectChecks::readFile(filePath, newText)) return ReloadResult::Busy;
    if (onDisk && newText == text){
        takeStamp();
        return ReloadResult::Unchanged;
    }
    Data data;
    std::vector<ClipWarning> newWarnings;
    anim::ClipParseError parseError;
    if (!Traits<Data>::parse(newText, data, newWarnings, parseError)){
        // the last good data stays (players keep it too); a commit replaces the broken file
        warnings = {{"", "the file on disk is not " + std::string(Traits<Data>::what()) + " (" + parseError.toString() +
                         "): showing its last valid content"}};
        text = std::move(newText);
        onDisk = true;
        takeStamp();
        notify(Change::Reloaded);
        return ReloadResult::Kept;
    }
    current = std::move(data);
    warnings = std::move(newWarnings);
    text = std::move(newText);
    onDisk = true;
    rev++;
    takeStamp();
    revisionChanged();
    notify(committing ? Change::Edited : Change::Reloaded);
    return ReloadResult::Changed;
}

template<class Data>
std::string AnimAssetDocument<Data>::keptGuid() const{
    const std::string& own = Traits<Data>::guid(current);
    if (anim::isClipGuid(own)) return own;
    if (project){
        const std::string known = project->getAssetDatabase().getGuid(filePath);
        if (anim::isClipGuid(known)) return known;
        fs::path relative;
        if (isInside(project->getProjectPath(), filePath, &relative)) return AssetDatabase::legacyGuid(relative);
    }
    return AssetDatabase::generateGuid();
}

template<class Data>
bool AnimAssetDocument<Data>::writeUndoable(const std::string& newText, const std::string& mergeKey, std::string* error){
    if (onDisk && newText == text) return true;
    Project* owner = project;
    auto* command = new WriteFilesCmd({{filePath, newText}}, [owner](const std::vector<fs::path>& changed){
        documentFilesWritten(owner, changed);
    });
    if (mergeKey.empty()) command->setNoMerge();
    else command->setMergeKey(relativePath + "\n" + std::to_string(mergeEpoch) + "\n" + mergeKey);
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    const bool wasCommitting = committing;
    committing = true;
    const bool written = project->getProjectCommandHistory()->addCommand(command);
    committing = wasCommitting;
    if (!written){
        return fail(error, "writing '" + relativePath + "' failed (" +
                    (failure->empty() ? std::string("permissions, a read-only disk, or a parent path that is a file") : *failure) + ")");
    }
    return true;
}

template<class Data>
bool AnimAssetDocument<Data>::commit(const Data& newData, const std::string& undoName, const std::string& mergeKey, std::string* error,
                                     std::vector<ClipWarning>* commitWarnings){
    (void)undoName;
    if (!onDisk) return fail(error, "'" + relativePath + "' does not exist any more");
    if (stampChanged()){
        reload();
        return fail(error, "'" + relativePath + "' changed on disk since it was read: it was reloaded, make the edit again");
    }
    Data copy = newData;
    Traits<Data>::guid(copy) = keptGuid();
    Traits<Data>::normalize(copy, commitWarnings);
    return writeUndoable(Traits<Data>::write(copy), mergeKey, error);
}

template<class Data>
void AnimAssetDocument<Data>::filesChanged(const std::vector<fs::path>& paths, Project* project){
    Registry<Data>& reg = registry<Data>();
    if (!project) project = reg.project;
    std::vector<std::shared_ptr<AnimAssetDocument>> changed;
    for (const auto& entry : reg.documents){
        for (const fs::path& path : paths){
            if (isSameOrUnder(path, entry.second->filePath)){
                changed.push_back(entry.second);
                break;
            }
        }
    }
    std::set<std::string> covered;
    for (const std::shared_ptr<AnimAssetDocument>& document : changed){
        if (document->reload() != ReloadResult::Busy && document->project == project) covered.insert(keyOf(document->filePath));
    }
    // the files no document has open: the pool and the players hear of them all the same
    if (!project) return;
    for (const fs::path& path : paths){
        const fs::path normal = path.lexically_normal();
        std::error_code ec;
        const bool ownFile = Traits<Data>::isFile(normal.string());
        const bool directory = !ownFile && (fs::is_directory(normal, ec) || !fs::exists(normal, ec));
        if ((!ownFile && !directory) || covered.count(keyOf(normal))) continue;
        if (ownFile){
            // a file that does not parse any more: the players keep the last valid one
            std::string content;
            Data data;
            std::vector<ClipWarning> ignored;
            anim::ClipParseError parseError;
            if (fs::is_regular_file(normal, ec) && ProjectChecks::readFile(normal, content) && !Traits<Data>::parse(content, data, ignored, parseError)){
                continue;
            }
            Traits<Data>::invalidate(normal);
        }
        Traits<Data>::markPlayers(project, normal);
    }
}

template<class Data>
void AnimAssetDocument<Data>::filesMoved(const fs::path& oldPath, const fs::path& newPath){
    Registry<Data>& reg = registry<Data>();
    std::vector<std::pair<std::string, std::shared_ptr<AnimAssetDocument>>> moved;
    for (const auto& entry : reg.documents){
        if (isSameOrUnder(oldPath, entry.second->filePath)) moved.emplace_back(entry.first, entry.second);
    }
    for (auto& [oldKey, document] : moved){
        const fs::path rest = document->filePath.lexically_relative(oldPath.lexically_normal());
        const fs::path filePath = (rest == "." ? newPath : newPath / rest).lexically_normal();
        document->filePath = filePath;
        fs::path relative;
        document->relativePath = document->project && isInside(document->project->getProjectPath(), filePath, &relative)
            ? relative.generic_string() : filePath.generic_string();
        reg.documents.erase(oldKey);
        reg.documents[keyOf(filePath)] = document;
        document->mergeEpoch++;
        document->rev++;
        document->takeStamp();
        document->notify(Change::Reloaded);
    }
}

template<class Data>
void AnimAssetDocument<Data>::checkDiskChanges(){
    std::vector<std::shared_ptr<AnimAssetDocument>> documents;
    for (const auto& entry : registry<Data>().documents){
        if (entry.second->stampChanged()) documents.push_back(entry.second);
    }
    for (const std::shared_ptr<AnimAssetDocument>& document : documents) document->reload();
}

template<class Data>
void AnimAssetDocument<Data>::closeAll(){
    Registry<Data>& reg = registry<Data>();
    reg.documents.clear();
    reg.project = nullptr;
    reg.projectPath.clear();
}

template<class Data>
int AnimAssetDocument<Data>::addListener(Listener listener){
    const int id = nextListener++;
    listeners[id] = std::move(listener);
    return id;
}

template<class Data>
void AnimAssetDocument<Data>::removeListener(int id){
    listeners.erase(id);
}

template<class Data>
void AnimAssetDocument<Data>::notify(Change change){
    std::map<int, Listener> copy = listeners;
    for (auto& entry : copy){
        if (listeners.count(entry.first) && entry.second) entry.second(*this, change);
    }
}

namespace jasperengine::editor{
    template class AnimAssetDocument<anim::AvatarMaskData>;
    template class AnimAssetDocument<anim::ControllerData>;
}
