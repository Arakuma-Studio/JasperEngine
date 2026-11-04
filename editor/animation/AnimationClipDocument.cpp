// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClipDocument.h"
#include "component/AnimationPlayerComponent.h"
#include "AnimatorAssetDocument.h"

#include "animation/AnimatorAssetIO.h"
#include "pool/AnimatorAssetPool.h"
#include "command/CommandNames.h"

#include "Out.h"
#include "Project.h"
#include "animation/ModelSubClips.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "pool/AnimationClipPool.h"
#include "subsystem/MeshSystem.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <cctype>
#include <cstdlib>
#include <set>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // The open documents, by key (the absolute normalised .anim path, or "<model>#<clip>"); they
    // belong to `project`
    struct Registry {
        Project* project = nullptr;
        fs::path projectPath;
        std::map<std::string, std::shared_ptr<AnimationClipDocument>> documents;
        std::function<void(const std::vector<fs::path>&)> filesWrittenHook;
    };

    Registry& registry(){
        static Registry instance;
        return instance;
    }

    std::string keyOf(const fs::path& absolute){
        return absolute.lexically_normal().generic_string();
    }

    void clearWatchedModels();

    // Another project (or the same object reopened elsewhere) drops the documents of the old one
    void useProject(Project* project){
        Registry& reg = registry();
        if (reg.project != project || reg.projectPath != project->getProjectPath()){
            reg.documents.clear();
            clearWatchedModels();
            reg.project = project;
            reg.projectPath = project->getProjectPath();
        }
    }

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    // `path` is `base` or lies under it (lexically, generic separators)
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
        if (rel.empty() || *rel.begin() == ".."){
            return false;
        }
        if (relative) *relative = rel;
        return true;
    }

    // What a document path names: an .anim file, or the model of a sub-clip and its clip part
    struct Target {
        fs::path absolute;          // the .anim, or the model file
        std::string relative;       // project-relative generic (+ "#<clip>")
        std::string fileRelative;   // the file part of relative
        bool subClip = false;
        std::string clip;           // sub-clip part
    };

    // Absolute, project-relative or asset-relative (the component form: tried when no file is
    // at the project-relative place); false outside the project or of another type
    bool resolveTarget(Project* project, const std::string& path, Target& out, std::string* error){
        if (!project || project->getProjectPath().empty()){
            return fail(error, "no project is open");
        }
        if (path.empty()){
            return fail(error, "no clip path given");
        }
        const fs::path root = project->getProjectPath().lexically_normal();
        anim::SubClipReference reference;
        out.subClip = anim::parseSubClipReference(path, reference);
        out.clip = out.subClip ? reference.clip : std::string();
        const fs::path given = FileUtils::pathFromUtf8(out.subClip ? reference.model : path);

        if (given.is_absolute()){
            out.absolute = given.lexically_normal();
        }else{
            out.absolute = (root / given).lexically_normal();
            std::error_code ec;
            if (!fs::exists(out.absolute, ec)){
                const fs::path assetForm = project->resolveAssetPath(given);
                if (!assetForm.empty() && fs::exists(assetForm, ec)){
                    out.absolute = assetForm.lexically_normal();
                }
            }
        }
        fs::path rel;
        if (!isInside(root, out.absolute, &rel)){
            return fail(error, "'" + path + "' is outside the project");
        }
        if (out.subClip ? !Util::isModelFile(out.absolute.string()) : !Util::isAnimationClipFile(out.absolute.string())){
            return fail(error, "'" + path + "' is not an .anim file or a model sub-clip (\"<model>#<clip>\")");
        }
        out.fileRelative = rel.generic_string();
        out.relative = out.fileRelative + (out.subClip ? "#" + out.clip : std::string());
        return true;
    }

    std::string assetForm(Project* project, const fs::path& absolute){
        return project->normalizeToAssetsRelative(absolute).generic_string();
    }

    // The animation index a sub-clip part names (a name, else a number when no animation has
    // that name), -1 when unknown or the model's clips can't be listed
    int subClipIndex(const std::string& assetModel, const std::string& clip, std::string* canonicalName = nullptr){
        std::vector<anim::ModelClipInfo> infos;
        std::string error;
        if (!anim::listModelFileClips(assetModel, infos, &error)){
            return -1;
        }
        for (const anim::ModelClipInfo& info : infos){
            if (info.name == clip){
                if (canonicalName) *canonicalName = info.name;
                return info.index;
            }
        }
        if (!clip.empty() && clip.find_first_not_of("0123456789") == std::string::npos){
            const int index = std::atoi(clip.c_str());
            for (const anim::ModelClipInfo& info : infos){
                if (info.index == index){
                    if (canonicalName) *canonicalName = info.name;
                    return info.index;
                }
            }
        }
        return -1;
    }

    // ---- the Animation Players of the open scenes, prefab stages and loaded prefab assets ----

    struct PlayerSite {
        uint32_t sceneId = 0;
        Entity entity = NULL_ENTITY;
        bool prefabAsset = false;
        std::string prefabPath;
    };

    void forEachPlayer(Project* project, const std::function<void(const PlayerSite&, AnimationPlayerComponent&)>& visit){
        if (!project) return;
        auto visitRegistry = [&](EntityRegistry* registry, PlayerSite site){
            if (!registry) return;
            auto players = registry->getComponentArray<AnimationPlayerComponent>();
            for (size_t i = 0; i < players->size(); i++){
                site.entity = players->getEntity(i);
                visit(site, players->getComponentFromIndex(i));
            }
        };
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()){
            if (!sceneProject->scene) continue;
            PlayerSite site;
            site.sceneId = sceneProject->id;
            visitRegistry(sceneProject->scene, site);
        }
        project->forEachPrefabAsset([&](const fs::path& path, Prefab& prefab){
            PlayerSite site;
            site.prefabAsset = true;
            site.prefabPath = path.generic_string();
            visitRegistry(prefab.registry.get(), site);
        });
    }

    // The file an entry references (the model of a sub-clip), absolute
    fs::path entryFile(Project* project, const std::string& clip, std::string* subClipPart = nullptr){
        anim::SubClipReference reference;
        if (anim::parseSubClipReference(clip, reference)){
            if (subClipPart) *subClipPart = reference.clip;
            return project->resolveAssetPath(FileUtils::pathFromUtf8(reference.model)).lexically_normal();
        }
        if (subClipPart) subClipPart->clear();
        return project->resolveAssetPath(FileUtils::pathFromUtf8(clip)).lexically_normal();
    }

    // The players using a changed file (an .anim, a model's sub-clips, or anything under a
    // directory) reload; the pool drops what they had loaded from there
    void markPlayers(Project* project, const fs::path& changed){
        forEachPlayer(project, [&](const PlayerSite&, AnimationPlayerComponent& player){
            for (const AnimationPlayerClip& entry : player.clips){
                if (entry.clip.empty()) continue;
                std::string part;
                const fs::path file = entryFile(project, entry.clip, &part);
                if (!isSameOrUnder(changed, file)) continue;
                // what the player had loaded from there (a directory: each file under it)
                if (!part.empty()){
                    AnimationClipPool::invalidateModel(file.generic_string());
                }else{
                    AnimationClipPool::invalidate(file.generic_string());
                }
                player.needsLoad = true;
            }
        });
    }

    // The .gltf files next to a changed (or gone) .bin that name it as a buffer: their sub-clips
    // changed with it
    std::vector<fs::path> gltfFilesUsing(const fs::path& buffer){
        std::vector<fs::path> models;
        std::error_code ec;
        const std::string name = buffer.filename().string();
        for (fs::directory_iterator it(buffer.parent_path(), ec), end; !ec && it != end; it.increment(ec)){
            const fs::path candidate = it->path();
            std::string ext = candidate.extension().string();
            for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (ext != ".gltf") continue;
            std::string text;
            if (ProjectChecks::readFile(candidate, text) && text.find(name) != std::string::npos){
                models.push_back(candidate.lexically_normal());
            }
        }
        return models;
    }

    bool isBufferFile(const fs::path& path){
        std::string ext = path.extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return ext == ".bin";
    }

    // The editor's poll of the models whose sub-clips players use (no other part of the editor
    // watches .gltf/.glb files): each model's time and size, with the .bin buffers next to a
    // .gltf it names
    struct ModelStamp {
        int64_t time = 0;
        uint64_t size = 0;
        std::vector<fs::path> buffers;
        std::vector<std::pair<int64_t, uint64_t>> bufferStamps;
    };
    std::map<std::string, ModelStamp>& watchedModels(){
        static std::map<std::string, ModelStamp> models;
        return models;
    }

    void clearWatchedModels(){
        watchedModels().clear();
    }

    std::pair<int64_t, uint64_t> statFile(const fs::path& path){
        std::error_code ec;
        const auto time = fs::last_write_time(path, ec);
        const int64_t t = ec ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
        const auto size = fs::file_size(path, ec);
        return {t, ec ? 0 : static_cast<uint64_t>(size)};
    }

    ModelStamp stampModel(const fs::path& model, const ModelStamp* previous){
        ModelStamp stamp;
        std::tie(stamp.time, stamp.size) = statFile(model);
        std::string ext = model.extension().string();
        for (char& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (ext == ".gltf"){
            if (previous && previous->time == stamp.time && previous->size == stamp.size){
                stamp.buffers = previous->buffers;   // the .gltf text did not change: nor its buffers
            }else{
                std::string text;
                std::error_code ec;
                if (ProjectChecks::readFile(model, text)){
                    for (fs::directory_iterator it(model.parent_path(), ec), end; !ec && it != end; it.increment(ec)){
                        if (isBufferFile(it->path()) && text.find(it->path().filename().string()) != std::string::npos){
                            stamp.buffers.push_back(it->path());
                        }
                    }
                }
            }
            for (const fs::path& buffer : stamp.buffers){
                stamp.bufferStamps.push_back(statFile(buffer));
            }
        }
        return stamp;
    }

    // After a document's WriteFilesCmd ran, was undone or redone: the asset database learns the
    // files (a created clip registers its GUID; an undone creation is gone), then the documents,
    // the pool and the players, then the MCP sync
    void documentFilesWritten(Project* project, const std::vector<fs::path>& changed){
        for (const fs::path& path : changed){
            std::error_code ec;
            if (fs::is_regular_file(path, ec)){
                project->importPrefabFiles(path);
            }else{
                // gone (an undone creation): the database forgets it, its references stay Missing
                project->onAssetFilesChanged(project->getAssetDatabase().removeAssets(path));
            }
        }
        AnimationClipDocument::filesChanged(changed, project);
        if (registry().filesWrittenHook){
            registry().filesWrittenHook(changed);
        }
    }

    // A file the editor wrote outside a document commit (the R26 hint rewrites): the documents
    // reload, and the MCP sync stamps it (not reported as an outside change)
    void clipFilesRewritten(Project* project, const std::vector<fs::path>& written){
        if (written.empty()) return;
        AnimationClipDocument::filesChanged(written, project);
        if (registry().filesWrittenHook){
            registry().filesWrittenHook(written);
        }
    }

}

// ---- clip users ----

std::vector<ClipUser> jasperengine::editor::findClipUsers(Project* project, const std::string& clip){
    std::vector<ClipUser> users;
    if (!project || project->getProjectPath().empty() || clip.empty()){
        return users;
    }

    std::string guid;
    fs::path file;
    std::string part;
    bool subClip = false;
    if (anim::isClipGuid(clip)){
        guid = clip;
        const fs::path relative = project->getAssetDatabase().getPath(guid);
        if (!relative.empty()) file = (project->getProjectPath() / relative).lexically_normal();
    }else{
        Target target;
        if (!resolveTarget(project, clip, target, nullptr)){
            return users;
        }
        file = target.absolute;
        subClip = target.subClip;
        part = target.clip;
        if (!subClip) guid = project->getAssetDatabase().getGuid(file);
    }
    // a sub-clip named by index or by name: the same animation (when the model's clips are known)
    const std::string assetModel = subClip ? assetForm(project, file) : std::string();
    const int partIndex = subClip ? subClipIndex(assetModel, part) : -1;

    forEachPlayer(project, [&](const PlayerSite& site, AnimationPlayerComponent& player){
        for (size_t i = 0; i < player.clips.size(); i++){
            const AnimationPlayerClip& entry = player.clips[i];
            if (entry.clip.empty() && entry.guid.empty()) continue;
            bool match = false;
            if (!subClip && !guid.empty() && entry.guid == guid){
                match = true;
            }else if (!entry.clip.empty() && !file.empty()){
                std::string entryPart;
                const fs::path entryPath = entryFile(project, entry.clip, &entryPart);
                if (entryPath == file){
                    if (!subClip){
                        match = entryPart.empty();
                    }else if (!entryPart.empty()){
                        match = entryPart == part || (partIndex >= 0 && subClipIndex(assetModel, entryPart) == partIndex);
                    }
                }
            }
            if (!match) continue;
            ClipUser user;
            user.sceneId = site.sceneId;
            user.entity = site.entity;
            user.entry = i;
            user.prefabAsset = site.prefabAsset;
            user.prefabPath = site.prefabPath;
            users.push_back(user);
        }
    });
    return users;
}

// ---- R26: hints inside .anim files follow renamed files ----

std::vector<ClipHintEdit> jasperengine::editor::remapClipHints(Project* project, const fs::path& oldPath, const fs::path& newPath){
    std::vector<ClipHintEdit> edits;
    if (!project || project->getProjectPath().empty()){
        return edits;
    }
    const fs::path root = project->getProjectPath().lexically_normal();
    const fs::path oldAbsolute = (oldPath.is_absolute() ? oldPath : root / oldPath).lexically_normal();
    const fs::path newAbsolute = (newPath.is_absolute() ? newPath : root / newPath).lexically_normal();

    // only what a hint can name: models and prefabs (preview), scenes and pose animations
    // (source), clips and masks (an Animator controller's or montage's), or a directory holding some
    std::error_code ec;
    auto hintable = [](const fs::path& path){
        const std::string p = path.string();
        return Util::isModelFile(p) || Util::isPrefabFile(p) || Util::isSceneFile(p) || Util::isPoseAnimationFile(p) ||
               Util::isAnimationClipFile(p) || Util::isAvatarMaskFile(p);
    };
    // (the Animator's controllers and montages naming a moved clip, model or mask are rewritten
    // below: their path hints follow, their GUIDs stay)
    if (!fs::is_directory(newAbsolute, ec) && !hintable(oldAbsolute) && !hintable(newAbsolute)){
        return edits;
    }

    // the forms a hint may be written in: asset-relative (the writer's), project-relative
    std::vector<std::pair<std::string, std::string>> forms;
    fs::path oldRel, newRel;
    if (isInside(root, oldAbsolute, &oldRel) && isInside(root, newAbsolute, &newRel)){
        forms.push_back({oldRel.generic_string(), newRel.generic_string()});
    }
    const std::string oldAsset = assetForm(project, oldAbsolute);
    const std::string newAsset = assetForm(project, newAbsolute);
    if (!fs::path(oldAsset).is_absolute() && !fs::path(newAsset).is_absolute() &&
        (forms.empty() || forms[0].first != oldAsset)){
        forms.push_back({oldAsset, newAsset});
    }
    if (forms.empty()){
        return edits;
    }
    auto remap = [&](std::string& value){
        if (value.empty()) return false;
        const std::string current = fs::path(value).lexically_normal().generic_string();
        for (const auto& [from, to] : forms){
            if (current == from){
                value = to;
                return true;
            }
            if (current.compare(0, from.size() + 1, from + "/") == 0){
                value = to + current.substr(from.size());
                return true;
            }
        }
        return false;
    };

    // a hint naming the old path holds its last component: other clips are not parsed
    const std::string oldName = oldAbsolute.filename().string();
    std::vector<fs::path> written;
    for (const fs::path& file : ProjectChecks::findProjectFiles(root, {".anim"})){
        std::string text;
        if (!ProjectChecks::readFile(file, text) || oldName.empty() || text.find(oldName) == std::string::npos) continue;
        anim::ClipParseResult parsed = anim::parseClip(text);
        if (!parsed.ok()) continue;
        ClipData data = *parsed.data;
        bool changed = remap(data.previewModel);
        changed |= remap(data.source.path);
        if (!changed) continue;
        const std::string newText = anim::writeClip(data);
        if (newText == text) continue;
        if (!FileUtils::writeFileReplacing(file, newText)){
            Out::warning("Could not update the preview/source path of %s", file.string().c_str());
            continue;
        }
        edits.push_back({file, text, newText});
        written.push_back(file);
    }

    // the Animator's controllers and montages name clips (and masks) by path: they follow too
    // (a "<model>#<clip>" its model); the GUIDs they hold stay
    auto remapRef = [&](anim::AnimatorAssetRef& ref){
        anim::SubClipReference subClip;
        if (anim::parseSubClipReference(ref.path, subClip)){
            std::string model = subClip.model;
            if (!remap(model)) return false;
            ref.path = anim::formatSubClipReference(model, subClip.clip);
            return true;
        }
        return remap(ref.path);
    };
    for (const fs::path& file : ProjectChecks::findProjectFiles(root, {".controller", ".montage"})){
        std::string text;
        if (!ProjectChecks::readFile(file, text) || oldName.empty() || text.find(oldName) == std::string::npos) continue;
        std::string newText;
        if (Util::isAnimatorControllerFile(file.string())){
            anim::ControllerParseResult parsed = anim::parseController(text);
            if (!parsed.ok) continue;
            bool changed = false;
            anim::visitControllerRefs(parsed.data, [&](anim::AnimatorAssetRef& ref, const std::string&){ changed |= remapRef(ref); });
            if (!changed) continue;
            newText = anim::writeController(parsed.data);
        }else{
            anim::MontageParseResult parsed = anim::parseMontage(text);
            if (!parsed.ok || !remapRef(parsed.data.clip)) continue;
            newText = anim::writeMontage(parsed.data);
        }
        if (newText == text) continue;
        if (!FileUtils::writeFileReplacing(file, newText)){
            Out::warning("Could not update the clip paths of %s", file.string().c_str());
            continue;
        }
        edits.push_back({file, text, newText});
        AnimatorAssetPool::invalidate(file.string());
    }
    clipFilesRewritten(project, written);
    return edits;
}

void jasperengine::editor::restoreClipHints(Project* project, const std::vector<ClipHintEdit>& edits){
    std::vector<fs::path> written;
    for (const ClipHintEdit& edit : edits){
        std::string text;
        if (!ProjectChecks::readFile(edit.file, text) || text != edit.after){
            // edited since: what it holds now stays
            Out::warning("%s changed since the move: its preview/source path was left as it is now", edit.file.string().c_str());
            continue;
        }
        if (FileUtils::writeFileReplacing(edit.file, edit.before)){
            if (Util::isAnimatorAssetFile(edit.file.string())) AnimatorAssetPool::invalidate(edit.file.string());
            else written.push_back(edit.file);
        }
    }
    if (project) clipFilesRewritten(project, written);
}

// ---- documents ----

void AnimationClipDocument::setFilesWrittenHook(std::function<void(const std::vector<fs::path>&)> hook){
    // the Animator's documents' commits (controllers, masks, montages) reach the same file sync
    AnimatorAssetDocument::setFilesWrittenHook(hook);
    registry().filesWrittenHook = std::move(hook);
}

void AnimationClipDocument::filesWritten(Project* project, const std::vector<fs::path>& paths){
    if (!project || paths.empty()) return;
    documentFilesWritten(project, paths);
}

std::string AnimationClipDocument::fileText(const ClipData& data){
    return anim::writeClip(data);
}

std::shared_ptr<AnimationClipDocument> AnimationClipDocument::open(Project* project, const std::string& path, std::string* error){
    Target target;
    if (!resolveTarget(project, path, target, error)){
        return nullptr;
    }
    useProject(project);
    Registry& reg = registry();

    // a sub-clip named by index is the document of its name
    std::string subClipReference;
    if (target.subClip){
        const std::string assetModel = assetForm(project, target.absolute);
        std::string name;
        if (subClipIndex(assetModel, target.clip, &name) >= 0 && name != target.clip){
            target.clip = name;
            target.relative = target.fileRelative + "#" + name;
        }
        subClipReference = anim::formatSubClipReference(assetModel, target.clip);
    }
    const std::string key = target.subClip ? keyOf(target.absolute) + "#" + target.clip : keyOf(target.absolute);

    auto it = reg.documents.find(key);
    if (it != reg.documents.end()){
        std::shared_ptr<AnimationClipDocument> document = it->second;
        // changed by something that did not tell (another program, between two polls)
        if (document->stampChanged() || !document->onDisk){
            document->reloadFromDisk();
        }
        if (!document->onDisk){
            fail(error, "'" + document->relativePath + "' does not exist");
            return nullptr;
        }
        return document;
    }

    std::shared_ptr<AnimationClipDocument> document(new AnimationClipDocument());
    document->project = project;
    document->relativePath = target.relative;
    document->statPath = target.absolute;
    document->filePath = target.subClip ? fs::path(target.absolute.generic_string() + "#" + target.clip) : target.absolute;
    document->subClipReference = subClipReference;
    document->readOnly = target.subClip;
    if (!document->load(error)){
        return nullptr;
    }
    reg.documents[key] = document;
    return document;
}

std::shared_ptr<AnimationClipDocument> AnimationClipDocument::create(Project* project, const std::string& path, const ClipData& initial,
                                                                     const std::string& undoName, std::string* error){
    (void)undoName;   // the command history has no step names
    Target target;
    if (!resolveTarget(project, path, target, error)){
        return nullptr;
    }
    if (target.subClip){
        fail(error, "'" + path + "' is a model sub-clip: they are read-only (Extract to .anim makes a file)");
        return nullptr;
    }
    std::error_code ec;
    if (fs::exists(target.absolute, ec)){
        fail(error, "'" + target.relative + "' already exists");
        return nullptr;
    }
    useProject(project);
    Registry& reg = registry();
    const std::string key = keyOf(target.absolute);

    std::shared_ptr<AnimationClipDocument> document;
    auto it = reg.documents.find(key);
    const bool existing = it != reg.documents.end();
    if (existing){
        document = it->second;   // a document whose file was deleted (an undone creation)
    }else{
        document.reset(new AnimationClipDocument());
        document->project = project;
        document->relativePath = target.relative;
        document->filePath = target.absolute;
        document->statPath = target.absolute;
        reg.documents[key] = document;
    }

    // a new asset: its own GUID (one that another file already has would make it a copy)
    ClipData copy = initial;
    const AssetDatabase& database = project->getAssetDatabase();
    if (!anim::isClipGuid(copy.guid) || database.contains(copy.guid)){
        do {
            copy.guid = AssetDatabase::generateGuid();
        } while (database.contains(copy.guid));
    }
    if (!document->writeUndoable(anim::writeClip(copy), "", error)){
        // a document of an undone creation stays for its holders (a redo brings the file back)
        if (!existing && !document->onDisk){
            reg.documents.erase(key);
        }
        return nullptr;
    }
    if (!document->onDisk){
        // the write's notification did not reach it (no file after a successful write?)
        document->reloadFromDisk();
    }
    return document;
}

std::vector<std::shared_ptr<AnimationClipDocument>> AnimationClipDocument::openDocuments(){
    std::vector<std::shared_ptr<AnimationClipDocument>> documents;
    for (const auto& entry : registry().documents){
        documents.push_back(entry.second);
    }
    return documents;
}

bool AnimationClipDocument::load(std::string* error){
    std::error_code ec;
    if (!fs::is_regular_file(statPath, ec)){
        return fail(error, "'" + relativePath + "' does not exist");
    }
    if (readOnly){
        ClipData data;
        std::vector<ClipWarning> buildWarnings;
        std::string buildError;
        if (!anim::buildModelSubClip(subClipReference, data, buildWarnings, &buildError)){
            return fail(error, "'" + relativePath + "': " + (buildError.empty() ? std::string("the model has no such animation") : buildError));
        }
        current = std::move(data);
        warnings = std::move(buildWarnings);
        text = anim::writeClip(current);
    }else{
        std::string fileContent;
        if (!ProjectChecks::readFile(filePath, fileContent)){
            return fail(error, "cannot read '" + relativePath + "'");
        }
        anim::ClipParseResult parsed = anim::parseClip(fileContent);
        if (!parsed.ok()){
            return fail(error, "'" + relativePath + "' is not an animation clip: " + parsed.error.toString());
        }
        current = std::move(*parsed.data);
        warnings = std::move(parsed.warnings);
        text = std::move(fileContent);
    }
    onDisk = true;
    rev++;
    takeStamp();
    return true;
}

void AnimationClipDocument::takeStamp(){
    std::error_code ec;
    const auto time = fs::last_write_time(statPath, ec);
    stampTime = ec ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
    const auto size = fs::file_size(statPath, ec);
    stampSize = ec ? 0 : static_cast<uint64_t>(size);
}

bool AnimationClipDocument::stampChanged() const{
    std::error_code ec;
    if (!fs::is_regular_file(statPath, ec)){
        return onDisk;
    }
    const auto time = fs::last_write_time(statPath, ec);
    if (ec) return false;
    const auto size = fs::file_size(statPath, ec);
    if (ec) return false;
    return !onDisk || static_cast<int64_t>(time.time_since_epoch().count()) != stampTime || static_cast<uint64_t>(size) != stampSize;
}

void AnimationClipDocument::revisionChanged(){
    if (readOnly){
        AnimationClipPool::invalidateModel(statPath.generic_string());
    }else{
        AnimationClipPool::invalidate(filePath.generic_string());
    }
    markPlayers(project, statPath);
}

void AnimationClipDocument::reloadFromDisk(){
    reload();
}

AnimationClipDocument::ReloadResult AnimationClipDocument::reload(){
    const ReloadResult result = reloadContent();
    // an outside change (not this document's own commit) ends the merge of the edit before it:
    // undoing the next drag must not restore what the outside change replaced
    if (!committing && result != ReloadResult::Unchanged && result != ReloadResult::Busy){
        mergeEpoch++;
    }
    return result;
}

AnimationClipDocument::ReloadResult AnimationClipDocument::reloadContent(){
    std::error_code ec;
    if (!fs::is_regular_file(statPath, ec)){
        if (onDisk){
            onDisk = false;
            text.clear();
            rev++;
            revisionChanged();
            notify(Change::Removed);
        }
        return ReloadResult::Removed;
    }

    ClipData data;
    std::vector<ClipWarning> newWarnings;
    std::string newText;
    if (readOnly){
        // the model changed: the sub-clip is built again, from a new parse of the file (the
        // pooled tinygltf parse and the pool's sub-clips of it are dropped first; the images of
        // the entities showing the model stay, nothing reloads them here)
        if (stampChanged()){
            MeshSystem::invalidateModelParse(statPath.generic_string());
        }
        std::string buildError;
        if (!anim::buildModelSubClip(subClipReference, data, newWarnings, &buildError)){
            warnings = {{"", "the model's animation can't be read (" + buildError + "): showing its last valid content"}};
            onDisk = true;
            takeStamp();
            notify(Change::Reloaded);
            return ReloadResult::Kept;
        }
        newText = anim::writeClip(data);
        if (onDisk && newText == text){
            takeStamp();
            return ReloadResult::Unchanged;
        }
    }else{
        if (!ProjectChecks::readFile(filePath, newText)){
            return ReloadResult::Busy;   // being written: the next poll or notification reads it
        }
        if (onDisk && newText == text){
            takeStamp();
            return ReloadResult::Unchanged;
        }
        anim::ClipParseResult parsed = anim::parseClip(newText);
        if (!parsed.ok()){
            // keep the last good data (the players keep playing it too); an edit through
            // commit() replaces the broken file
            warnings = {{"", "the file on disk is not a valid animation clip (" + parsed.error.toString() + "): showing its last valid content"}};
            text = std::move(newText);
            onDisk = true;
            takeStamp();
            notify(Change::Reloaded);
            return ReloadResult::Kept;
        }
        data = std::move(*parsed.data);
        newWarnings = std::move(parsed.warnings);
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

std::string AnimationClipDocument::keptGuid() const{
    if (anim::isClipGuid(current.guid)){
        return current.guid;
    }
    if (project){
        const std::string known = project->getAssetDatabase().getGuid(statPath);
        if (anim::isClipGuid(known)){
            return known;
        }
        fs::path relative;
        if (isInside(project->getProjectPath(), statPath, &relative)){
            return AssetDatabase::legacyGuid(relative);
        }
    }
    return AssetDatabase::generateGuid();
}

bool AnimationClipDocument::writeUndoable(const std::string& newText, const std::string& mergeKey, std::string* error){
    if (onDisk && newText == text){
        return true;
    }
    Project* owner = project;
    auto* command = new WriteFilesCmd({{filePath, newText}}, [owner](const std::vector<fs::path>& changed){
        documentFilesWritten(owner, changed);
    });
    // the step's name in the undo history (S24)
    command->setName("Edit Animation Clip " + CommandNames::file(filePath));
    if (mergeKey.empty()){
        command->setNoMerge();
    }else{
        command->setMergeKey(relativePath + "\n" + std::to_string(mergeEpoch) + "\n" + mergeKey);
    }
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    const bool wasCommitting = committing;
    committing = true;
    // addCommand runs execute() (which reloads this document) and deletes the command when it fails
    const bool written = project->getProjectCommandHistory()->addCommand(command);
    committing = wasCommitting;
    if (!written){
        return fail(error, "writing '" + relativePath + "' failed (" +
                    (failure->empty() ? std::string("permissions, a read-only disk, or a parent path that is a file") : *failure) + ")");
    }
    return true;
}

bool AnimationClipDocument::commit(const ClipData& newData, const std::string& undoName, const std::string& mergeKey,
                                   std::string* error, std::vector<ClipWarning>* warnings){
    (void)undoName;   // the command history has no step names
    if (readOnly){
        return fail(error, "'" + relativePath + "' is read-only (a model's animation): Extract to .anim to edit a copy");
    }
    if (!onDisk){
        return fail(error, "'" + relativePath + "' does not exist any more");
    }
    // written by something else since it was read (the poll or auto-sync had not seen it yet):
    // the edit was made on old content, so it is not written over the new one
    if (stampChanged()){
        reload();
        return fail(error, "'" + relativePath + "' changed on disk since it was read: it was reloaded, make the edit again");
    }
    ClipData copy = newData;
    // the file keeps its identity whatever the edit holds
    copy.guid = keptGuid();
    anim::normalize(copy, warnings);
    return writeUndoable(anim::writeClip(copy), mergeKey, error);
}

void AnimationClipDocument::filesChanged(const std::vector<fs::path>& givenPaths, Project* project){
    Registry& reg = registry();
    if (!project) project = reg.project;
    // the Animator's documents (controllers, masks, montages) hear of the same changes (one
    // broadcast for the animation assets)
    {
        std::vector<fs::path> animatorAssets;
        for (const fs::path& path : givenPaths){
            if (Util::isAnimatorAssetFile(path.string())) animatorAssets.push_back(path);
        }
        if (!animatorAssets.empty()) AnimatorAssetDocument::filesChanged(animatorAssets);
    }

    // a .gltf's buffer changed: the .gltf did
    std::vector<fs::path> paths = givenPaths;
    for (const fs::path& path : givenPaths){
        if (isBufferFile(path)){
            const std::vector<fs::path> models = gltfFilesUsing(path.lexically_normal());
            paths.insert(paths.end(), models.begin(), models.end());
        }
    }
    // a changed model: its pooled parse goes first (MeshSystem::invalidateModelParse drops it
    // and its "<model>#*" sub-clips), so the documents below and the players rebuild from the file.
    // Only the parse: the entities showing the model are not reloaded here, so their images stay
    // in the pools (a file the editor wrote and loaded itself, a bake, is picked up here too)
    std::set<std::string> modelsDropped;
    for (const fs::path& path : paths){
        const fs::path normal = path.lexically_normal();
        if (Util::isModelFile(normal.string()) && modelsDropped.insert(keyOf(normal)).second){
            MeshSystem::invalidateModelParse(normal.generic_string());
        }
    }
    // a changed .humanoid sidecar (the model's Avatar, anim-retarget 7.4): the model's clips and its
    // humanoid rig are built again (a humanoid model's clips, clips retargeted from it)
    for (const fs::path& path : paths){
        const fs::path normal = path.lexically_normal();
        if (normal.extension() != ".humanoid") continue;
        fs::path model = normal;
        model.replace_extension();
        if (Util::isModelFile(model.string())) AnimationClipPool::invalidateModel(model.generic_string());
    }

    std::vector<std::shared_ptr<AnimationClipDocument>> changed;
    for (const auto& entry : reg.documents){
        for (const fs::path& path : paths){
            if (isSameOrUnder(path, entry.second->statPath)){
                changed.push_back(entry.second);
                break;
            }
        }
    }
    // the clips an open document read again took care of themselves: a new revision told the
    // pool and the players (revisionChanged), the same text needs nothing, a broken file keeps
    // the last valid clip
    std::set<std::string> covered;
    for (const std::shared_ptr<AnimationClipDocument>& document : changed){
        const ReloadResult result = document->reload();
        if (!document->readOnly && result != ReloadResult::Busy && document->project == project){
            covered.insert(keyOf(document->statPath));
        }
    }

    // the files no document has open: the pool and the players hear of them all the same
    if (!project) return;
    for (const fs::path& path : paths){
        const fs::path normal = path.lexically_normal();
        const std::string p = normal.string();
        std::error_code ec;
        const bool clipFile = Util::isAnimationClipFile(p);
        const bool modelFile = Util::isModelFile(p);
        // a directory moved or deleted: what was under it (a gone path that is no clip or model,
        // "clips.v2" too)
        const bool directory = !clipFile && !modelFile && (fs::is_directory(normal, ec) || !fs::exists(normal, ec));
        if ((!clipFile && !modelFile && !directory) || covered.count(keyOf(normal))){
            continue;
        }
        if (clipFile){
            // a file that does not parse any more: the players keep the last valid clip, as an
            // open document does (the problem is reported by whoever saw the change)
            std::string content;
            if (fs::is_regular_file(normal, ec) && ProjectChecks::readFile(normal, content) && !anim::parseClip(content).ok()){
                continue;
            }
            AnimationClipPool::invalidate(normal.generic_string());
        }
        markPlayers(project, normal);
    }
}

void AnimationClipDocument::filesMoved(const fs::path& oldPath, const fs::path& newPath){
    // the Animator's documents follow the move too
    AnimatorAssetDocument::filesMoved(oldPath, newPath);
    Registry& reg = registry();
    std::vector<std::pair<std::string, std::shared_ptr<AnimationClipDocument>>> moved;
    for (const auto& entry : reg.documents){
        if (isSameOrUnder(oldPath, entry.second->statPath)){
            moved.emplace_back(entry.first, entry.second);
        }
    }
    for (auto& [oldKey, document] : moved){
        const fs::path rest = document->statPath.lexically_relative(oldPath.lexically_normal());
        const fs::path statPath = (rest == "." ? newPath : newPath / rest).lexically_normal();
        std::string clipPart;
        if (document->readOnly){
            clipPart = document->filePath.generic_string().substr(document->statPath.generic_string().size() + 1);
        }
        document->statPath = statPath;
        std::string key = keyOf(statPath);
        fs::path relative;
        const std::string fileRelative = document->project && isInside(document->project->getProjectPath(), statPath, &relative)
            ? relative.generic_string() : statPath.generic_string();
        if (document->readOnly){
            document->filePath = fs::path(statPath.generic_string() + "#" + clipPart);
            document->subClipReference = anim::formatSubClipReference(assetForm(document->project, statPath), clipPart);
            document->relativePath = fileRelative + "#" + clipPart;
            key += "#" + clipPart;
        }else{
            document->filePath = statPath;
            document->relativePath = fileRelative;
        }
        reg.documents.erase(oldKey);
        reg.documents[key] = document;
        document->mergeEpoch++;
        document->rev++;
        document->takeStamp();
        document->notify(Change::Reloaded);
    }
}

void AnimationClipDocument::checkDiskChanges(Project* project){
    // the Animator's documents check theirs too
    AnimatorAssetDocument::checkDiskChanges();
    std::vector<std::shared_ptr<AnimationClipDocument>> documents;
    for (const auto& entry : registry().documents){
        if (entry.second->stampChanged()){
            documents.push_back(entry.second);
        }
    }
    std::set<std::string> rebuilt;
    for (const std::shared_ptr<AnimationClipDocument>& document : documents){
        document->reload();
        if (document->readOnly){
            rebuilt.insert(keyOf(document->statPath));
        }
    }

    // the models the players' sub-clips come from (edited or reimported outside the editor)
    if (!project || project->getProjectPath().empty()) return;
    std::set<std::string> used;
    forEachPlayer(project, [&](const PlayerSite&, AnimationPlayerComponent& player){
        for (const AnimationPlayerClip& entry : player.clips){
            std::string part;
            if (anim::isSubClipReference(entry.clip)){
                used.insert(keyOf(entryFile(project, entry.clip, &part)));
            }
        }
    });
    // and of the open sub-clip documents (their .bin buffers: the document watches the .gltf only)
    for (const auto& entry : registry().documents){
        if (entry.second->readOnly) used.insert(keyOf(entry.second->statPath));
    }
    std::map<std::string, ModelStamp>& watched = watchedModels();
    std::vector<fs::path> changed;
    for (auto it = watched.begin(); it != watched.end();){
        if (!used.count(it->first)){
            it = watched.erase(it);
            continue;
        }
        const ModelStamp now = stampModel(FileUtils::pathFromUtf8(it->first), &it->second);
        if (now.time != it->second.time || now.size != it->second.size || now.buffers != it->second.buffers ||
            now.bufferStamps != it->second.bufferStamps){
            // a model a document just rebuilt from: its players were told (revisionChanged)
            if (!rebuilt.count(it->first)){
                changed.push_back(FileUtils::pathFromUtf8(it->first));
            }
            it->second = now;
        }
        ++it;
    }
    for (const std::string& model : used){
        if (!watched.count(model)){
            watched[model] = stampModel(FileUtils::pathFromUtf8(model), nullptr);
        }
    }
    if (!changed.empty()){
        filesChanged(changed, project);
    }
}

void AnimationClipDocument::closeAll(){
    // the Animator's documents close with them (another project)
    AnimatorAssetDocument::closeAll();
    Registry& reg = registry();
    reg.documents.clear();
    reg.project = nullptr;
    reg.projectPath.clear();
    watchedModels().clear();
}

std::shared_ptr<const AnimationClip> AnimationClipDocument::clip(){
    if (!built || builtRevision != rev){
        built = AnimationClip::build(current);
        builtRevision = rev;
    }
    return built;
}

int AnimationClipDocument::addListener(Listener listener){
    int id = nextListener++;
    listeners[id] = std::move(listener);
    return id;
}

void AnimationClipDocument::removeListener(int id){
    listeners.erase(id);
}

void AnimationClipDocument::notify(Change change){
    // a listener may add or remove listeners
    std::map<int, Listener> copy = listeners;
    for (auto& entry : copy){
        if (listeners.count(entry.first) && entry.second){
            entry.second(*this, change);
        }
    }
}
