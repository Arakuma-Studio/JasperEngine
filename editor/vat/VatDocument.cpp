// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vat/VatDocument.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/WriteFilesCmd.h"
#include "component/VertexAnimatorComponent.h"
#include "pool/VertexAnimationPool.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"
#include "vat/VatAssetFiles.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;
namespace jv = jasperengine::vat;
namespace ev = jasperengine::editor::vat;

namespace {

    struct Registry {
        Project* project = nullptr;
        fs::path projectPath;
        std::map<std::string, std::shared_ptr<VatDocument>> documents;
        std::function<void(const std::vector<fs::path>&)> filesWrittenHook;
        // the poll of the .vat files the open scenes' animators use: (time, size) of the .vat
        // and of its mesh, by key
        std::map<std::string, std::tuple<int64_t, uint64_t, int64_t, uint64_t>> watched;
    };

    Registry& registry(){
        static Registry instance;
        return instance;
    }

    std::string keyOf(const fs::path& absolute){
        return absolute.lexically_normal().generic_string();
    }

    void useProject(Project* project){
        Registry& reg = registry();
        if (reg.project != project || reg.projectPath != project->getProjectPath()){
            reg.documents.clear();
            reg.watched.clear();
            reg.project = project;
            reg.projectPath = project->getProjectPath();
        }
    }

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
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
        if (rel.empty() || *rel.begin() == ".."){
            return false;
        }
        if (relative) *relative = rel;
        return true;
    }

    // absolute, project-relative or asset-relative (tried when nothing is at the project-relative place)
    bool resolveTarget(Project* project, const std::string& path, fs::path& absolute, std::string& relative, std::string* error){
        if (!project || project->getProjectPath().empty()){
            return fail(error, "no project is open");
        }
        if (path.empty()){
            return fail(error, "no .vat path given");
        }
        const fs::path root = project->getProjectPath().lexically_normal();
        const fs::path given = FileUtils::pathFromUtf8(path);
        if (given.is_absolute()){
            absolute = given.lexically_normal();
        }else{
            absolute = (root / given).lexically_normal();
            std::error_code ec;
            if (!fs::exists(absolute, ec)){
                const fs::path assetForm = project->resolveAssetPath(given);
                if (!assetForm.empty() && fs::exists(assetForm, ec)){
                    absolute = assetForm.lexically_normal();
                }
            }
        }
        // the generated mesh names its .vat
        if (ev::isVatMeshFile(absolute.generic_string())){
            absolute = ev::pairPathOf(absolute);
        }
        fs::path rel;
        if (!isInside(root, absolute, &rel)){
            return fail(error, "'" + path + "' is outside the project");
        }
        if (!Util::isVertexAnimationFile(absolute.string())){
            return fail(error, "'" + path + "' is not a .vat file (a Vertex Animation)");
        }
        relative = rel.generic_string();
        return true;
    }

    std::pair<int64_t, uint64_t> statFile(const fs::path& path){
        std::error_code ec;
        const auto time = fs::last_write_time(path, ec);
        const int64_t t = ec ? 0 : static_cast<int64_t>(time.time_since_epoch().count());
        const auto size = fs::file_size(path, ec);
        return {t, ec ? 0 : static_cast<uint64_t>(size)};
    }

    // every Vertex Animator of the open scenes, prefab stages and loaded prefab assets
    void forEachAnimator(Project* project, const std::function<void(VertexAnimatorComponent&)>& visit){
        if (!project) return;
        auto visitRegistry = [&](EntityRegistry* registry){
            if (!registry) return;
            auto animators = registry->getComponentArray<VertexAnimatorComponent>();
            for (size_t i = 0; i < animators->size(); i++){
                visit(animators->getComponentFromIndex(i));
            }
        };
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()){
            if (sceneProject->scene) visitRegistry(sceneProject->scene);
        }
        project->forEachPrefabAsset([&](const fs::path&, Prefab& prefab){
            visitRegistry(prefab.registry.get());
        });
    }

    fs::path animatorFile(Project* project, const std::string& animation){
        return project->resolveAssetPath(FileUtils::pathFromUtf8(animation)).lexically_normal();
    }

    // The animators using a changed .vat (or anything under a directory) load it again
    void markAnimators(Project* project, const fs::path& changed){
        forEachAnimator(project, [&](VertexAnimatorComponent& animator){
            if (animator.animation.empty()) return;
            if (isSameOrUnder(changed, animatorFile(project, animator.animation))){
                animator.needsLoad = true;
            }
        });
    }

    // After a document's WriteFilesCmd ran, was undone or redone: the asset database learns the
    // file (same GUID, new time and size), then the documents, the pool and the animators, then
    // the MCP sync
    void documentFilesWritten(Project* project, const std::vector<fs::path>& changed){
        for (const fs::path& path : changed){
            std::error_code ec;
            if (fs::is_regular_file(path, ec)){
                project->importPrefabFiles(path);
            }
        }
        VatDocument::filesChanged(changed, project);
        if (registry().filesWrittenHook){
            registry().filesWrittenHook(changed);
        }
    }

    bool sameBounds(const jv::VatBounds& a, const jv::VatBounds& b){
        return a.valid == b.valid && a.min == b.min && a.max == b.max;
    }

    // a part of the file as its canonical text (the recipe has no ==)
    std::string recipeText(const jv::VatRecipe& recipe){
        jv::VatData probe;
        probe.recipe = recipe;
        return jv::writeVat(probe);
    }

    std::string statsText(const jv::VatStats& stats){
        jv::VatData probe;
        probe.stats = stats;
        return jv::writeVat(probe);
    }

    bool sameLayout(const jv::VatLayout& a, const jv::VatLayout& b){
        return a.precision == b.precision && a.width == b.width && a.height == b.height && a.samples == b.samples &&
               a.vertices == b.vertices && a.bones == b.bones && a.normals == b.normals && a.colors == b.colors &&
               a.layoutHash == b.layoutHash;
    }

}

// ---- static helpers ----

const char* const VatDocument::LOOP_WRAP_REFUSAL =
    "loop clips are sampled without a duplicate end frame: change the wrap in the baker and rebake";

void VatDocument::setFilesWrittenHook(std::function<void(const std::vector<fs::path>&)> hook){
    registry().filesWrittenHook = std::move(hook);
}

std::string VatDocument::fileText(const jv::VatData& data){
    jv::VatData copy = data;
    normalize(copy, nullptr);
    return jv::writeVat(copy);
}

std::string VatDocument::generatedChange(const jv::VatData& current, const jv::VatData& edited){
    if (current.mode != edited.mode) return "mode";
    if (current.mesh != edited.mesh) return "mesh";
    if (!sameLayout(current.layout, edited.layout)) return "layout";
    if (!sameBounds(current.bounds, edited.bounds)) return "bounds";
    if (statsText(current.stats) != statsText(edited.stats)) return "stats";
    if (recipeText(current.recipe) != recipeText(edited.recipe)) return "recipe";
    // a clip is known by its first frame: clips may go (their rows stay until a rebake), never
    // come, and keep their generated fields
    std::set<int> seen;
    for (size_t i = 0; i < edited.clips.size(); i++){
        const jv::VatClip& clip = edited.clips[i];
        const std::string path = "clips[" + std::to_string(i) + "]";
        auto it = std::find_if(current.clips.begin(), current.clips.end(), [&](const jv::VatClip& c){ return c.start == clip.start; });
        if (it == current.clips.end() || !seen.insert(clip.start).second){
            return path + " (a clip the bake did not make)";
        }
        if (it->frames != clip.frames) return path + ".frames";
        if (it->fps != clip.fps) return path + ".fps";
        // a loop clip has no duplicate end frame, the others do: their rows can't swap
        auto loops = [](ClipWrapMode wrap){ return wrap == ClipWrapMode::Loop || wrap == ClipWrapMode::Default; };
        if (loops(it->wrap) != loops(clip.wrap)) return path + ".wrap (" + LOOP_WRAP_REFUSAL + ")";
        if (it->restNone != clip.restNone) return path + ".rest";
        if (!sameBounds(it->bounds, clip.bounds)) return path + ".bounds";
    }
    // sockets find their track column by path (recipe.sockets): names and order may change,
    // their paths may not (each baked socket once, none added)
    if (current.sockets.size() != edited.sockets.size()) return "sockets (count)";
    std::set<std::string> paths;
    for (size_t i = 0; i < edited.sockets.size(); i++){
        const std::string& path = edited.sockets[i].path;
        const bool baked = std::any_of(current.sockets.begin(), current.sockets.end(), [&](const jv::VatSocket& s){ return s.path == path; });
        if (!baked || !paths.insert(path).second) return "sockets[" + std::to_string(i) + "].path";
    }
    return std::string();
}

bool VatDocument::normalize(jv::VatData& data, std::string* error){
    std::set<std::string> names;
    for (size_t i = 0; i < data.clips.size(); i++){
        jv::VatClip& clip = data.clips[i];
        const std::string path = "clips[" + std::to_string(i) + "]";
        if (clip.name.empty()) return fail(error, path + ".name: a clip needs a name");
        if (!names.insert(clip.name).second) return fail(error, path + ".name: two clips are named '" + clip.name + "'");
        if (!std::isfinite(clip.speed)) return fail(error, path + ".speed: expected a number");
        if (clip.wrap == ClipWrapMode::Default) clip.wrap = ClipWrapMode::Loop;
        for (size_t e = 0; e < clip.events.size(); e++){
            const ClipEventData& event = clip.events[e];
            if (!std::isfinite(event.time)) return fail(error, path + ".events[" + std::to_string(e) + "].time: expected a number");
            if (event.function.empty()){
                return fail(error, path + ".events[" + std::to_string(e) + "].function: an event calls a function (a Lua name)");
            }
        }
        std::stable_sort(clip.events.begin(), clip.events.end(), [](const ClipEventData& a, const ClipEventData& b){
            return std::tie(a.time, a.function) < std::tie(b.time, b.function);
        });
    }
    std::set<std::string> sockets;
    for (size_t i = 0; i < data.sockets.size(); i++){
        const std::string& name = data.sockets[i].name;
        if (name.empty()) return fail(error, "sockets[" + std::to_string(i) + "].name: a socket needs a name");
        if (!sockets.insert(name).second) return fail(error, "sockets[" + std::to_string(i) + "].name: two sockets are named '" + name + "'");
    }
    return true;
}

int VatDocument::unusedSamples(const jv::VatData& data){
    std::vector<bool> used((size_t)std::max(data.layout.samples, 0), false);
    for (const jv::VatClip& clip : data.clips){
        for (int f = std::max(clip.start, 0); f < clip.start + clip.frames && f < (int)used.size(); f++){
            used[(size_t)f] = true;
        }
    }
    return (int)std::count(used.begin(), used.end(), false);
}

// ---- documents ----

std::shared_ptr<VatDocument> VatDocument::open(Project* project, const std::string& path, std::string* error){
    fs::path absolute;
    std::string relative;
    if (!resolveTarget(project, path, absolute, relative, error)){
        return nullptr;
    }
    useProject(project);
    Registry& reg = registry();
    const std::string key = keyOf(absolute);

    auto it = reg.documents.find(key);
    if (it != reg.documents.end()){
        std::shared_ptr<VatDocument> document = it->second;
        if (document->stampChanged() || !document->onDisk){
            document->reloadFromDisk();
        }
        if (!document->onDisk){
            fail(error, "'" + document->relativePath + "' does not exist");
            return nullptr;
        }
        return document;
    }

    std::shared_ptr<VatDocument> document(new VatDocument());
    document->project = project;
    document->relativePath = relative;
    document->filePath = absolute;
    if (!document->load(error)){
        return nullptr;
    }
    reg.documents[key] = document;
    return document;
}

std::vector<std::shared_ptr<VatDocument>> VatDocument::openDocuments(){
    std::vector<std::shared_ptr<VatDocument>> documents;
    for (const auto& entry : registry().documents){
        documents.push_back(entry.second);
    }
    return documents;
}

std::string VatDocument::assetPath() const{
    return project ? project->normalizeToAssetsRelative(filePath).generic_string() : relativePath;
}

bool VatDocument::load(std::string* error){
    std::error_code ec;
    if (!fs::is_regular_file(filePath, ec)){
        return fail(error, "'" + relativePath + "' does not exist");
    }
    std::string fileContent;
    if (!ProjectChecks::readFile(filePath, fileContent)){
        return fail(error, "cannot read '" + relativePath + "'");
    }
    jv::VatParseResult parsed = jv::parseVat(fileContent);
    if (!parsed.ok()){
        return fail(error, "'" + relativePath + "' is not a Vertex Animation: " + parsed.error);
    }
    current = std::move(*parsed.data);
    warnings = std::move(parsed.warnings);
    text = std::move(fileContent);
    onDisk = true;
    rev++;
    takeStamp();
    return true;
}

void VatDocument::takeStamp(){
    std::tie(stampTime, stampSize) = statFile(filePath);
}

bool VatDocument::stampChanged() const{
    std::error_code ec;
    if (!fs::is_regular_file(filePath, ec)){
        return onDisk;
    }
    const auto stamp = statFile(filePath);
    return !onDisk || stamp.first != stampTime || stamp.second != stampSize;
}

void VatDocument::revisionChanged(){
    VertexAnimationPool::invalidate(filePath.generic_string());
    markAnimators(project, filePath);
}

void VatDocument::reloadFromDisk(){
    reload();
}

VatDocument::ReloadResult VatDocument::reload(){
    const ReloadResult result = reloadContent();
    // an outside change ends the merge of the edit before it
    if (!committing && result != ReloadResult::Unchanged && result != ReloadResult::Busy){
        mergeEpoch++;
    }
    return result;
}

VatDocument::ReloadResult VatDocument::reloadContent(){
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
    if (!ProjectChecks::readFile(filePath, newText)){
        return ReloadResult::Busy;
    }
    if (onDisk && newText == text){
        takeStamp();
        return ReloadResult::Unchanged;
    }
    jv::VatParseResult parsed = jv::parseVat(newText);
    if (!parsed.ok()){
        // the last good data stays (the animators keep playing it too); a commit replaces the file
        warnings = {{"", "the file on disk is not a valid Vertex Animation (" + parsed.error + "): showing its last valid content"}};
        text = std::move(newText);
        onDisk = true;
        takeStamp();
        notify(Change::Reloaded);
        return ReloadResult::Kept;
    }
    current = std::move(*parsed.data);
    warnings = std::move(parsed.warnings);
    text = std::move(newText);
    onDisk = true;
    rev++;
    takeStamp();
    revisionChanged();
    notify(committing ? Change::Edited : Change::Reloaded);
    return ReloadResult::Changed;
}

std::string VatDocument::keptGuid() const{
    if (AssetDatabase::isGuid(current.guid)){
        return current.guid;
    }
    if (project){
        const std::string known = project->getAssetDatabase().getGuid(filePath);
        if (AssetDatabase::isGuid(known)){
            return known;
        }
        fs::path relative;
        if (isInside(project->getProjectPath(), filePath, &relative)){
            return AssetDatabase::legacyGuid(relative);
        }
    }
    return AssetDatabase::generateGuid();
}

bool VatDocument::writeUndoable(const std::string& newText, const std::string& mergeKey, std::string* error){
    if (onDisk && newText == text){
        return true;
    }
    Project* owner = project;
    auto* command = new WriteFilesCmd({{filePath, newText}}, [owner](const std::vector<fs::path>& changed){
        documentFilesWritten(owner, changed);
    });
    if (mergeKey.empty()){
        command->setNoMerge();
    }else{
        command->setMergeKey(relativePath + "\n" + std::to_string(mergeEpoch) + "\n" + mergeKey);
    }
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

bool VatDocument::commit(const jv::VatData& newData, const std::string& undoName, const std::string& mergeKey, std::string* error){
    (void)undoName;   // the command history has no step names
    if (!onDisk){
        return fail(error, "'" + relativePath + "' does not exist any more");
    }
    if (stampChanged()){
        reload();
        return fail(error, "'" + relativePath + "' changed on disk since it was read: it was reloaded, make the edit again");
    }
    const std::string generated = generatedChange(current, newData);
    if (!generated.empty()){
        const size_t wrap = generated.find(".wrap (");
        if (wrap != std::string::npos){
            return fail(error, generated.substr(0, wrap + 5) + ": " + LOOP_WRAP_REFUSAL);
        }
        return fail(error, "generated field " + generated + " can't be edited: rebake (Inspector > Rebake, or "
                           "bake_vertex_animation {rebake})");
    }
    jv::VatData copy = newData;
    copy.guid = keptGuid();
    copy.unknownKeys = current.unknownKeys;
    if (!normalize(copy, error)){
        return false;
    }
    return writeUndoable(jv::writeVat(copy), mergeKey, error);
}

void VatDocument::filesChanged(const std::vector<fs::path>& givenPaths, Project* project){
    Registry& reg = registry();
    if (!project) project = reg.project;

    // a mesh stands for its .vat
    std::vector<fs::path> paths;
    for (const fs::path& path : givenPaths){
        const fs::path normal = path.lexically_normal();
        paths.push_back(ev::isVatMeshFile(normal.generic_string()) ? ev::pairPathOf(normal) : normal);
    }

    std::vector<std::shared_ptr<VatDocument>> changed;
    for (const auto& entry : reg.documents){
        for (const fs::path& path : paths){
            if (isSameOrUnder(path, entry.second->filePath)){
                changed.push_back(entry.second);
                break;
            }
        }
    }
    std::set<std::string> covered;
    for (const std::shared_ptr<VatDocument>& document : changed){
        const ReloadResult result = document->reload();
        // a new revision told the pool and the animators; the same text needs nothing; a broken
        // file keeps the last valid asset
        if (result != ReloadResult::Busy && document->project == project){
            covered.insert(keyOf(document->filePath));
        }
    }

    if (!project) return;
    for (size_t i = 0; i < paths.size(); i++){
        const fs::path& normal = paths[i];
        const bool meshChanged = ev::isVatMeshFile(givenPaths[i].generic_string());
        std::error_code ec;
        const bool vatFile = Util::isVertexAnimationFile(normal.string());
        const bool directory = !vatFile && (fs::is_directory(normal, ec) || !fs::exists(normal, ec));
        if (!vatFile && !directory) continue;
        // a document read it already (a new mesh with the same JSON still reloads the asset)
        if (vatFile && covered.count(keyOf(normal)) && !meshChanged) continue;
        if (vatFile){
            std::string content;
            if (fs::is_regular_file(normal, ec) && ProjectChecks::readFile(normal, content) && !jv::parseVat(content).ok()){
                continue;
            }
            VertexAnimationPool::invalidate(normal.generic_string());
        }
        markAnimators(project, normal);
    }
}

void VatDocument::filesMoved(const fs::path& oldPath, const fs::path& newPath){
    Registry& reg = registry();
    std::vector<std::pair<std::string, std::shared_ptr<VatDocument>>> moved;
    for (const auto& entry : reg.documents){
        if (isSameOrUnder(oldPath, entry.second->filePath)){
            moved.emplace_back(entry.first, entry.second);
        }
    }
    for (auto& [oldKey, document] : moved){
        const fs::path rest = document->filePath.lexically_relative(oldPath.lexically_normal());
        const fs::path filePath = (rest == "." ? newPath : newPath / rest).lexically_normal();
        if (!Util::isVertexAnimationFile(filePath.string())){
            continue;   // renamed to another type: the file is no .vat any more (it reads as removed)
        }
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

void VatDocument::checkDiskChanges(Project* project){
    std::vector<fs::path> changed;
    for (const auto& entry : registry().documents){
        if (entry.second->stampChanged()){
            changed.push_back(entry.second->filePath);
        }
    }

    if (project && !project->getProjectPath().empty()){
        useProject(project);
        // the .vat files the animators use, and their meshes (a rebake writes both)
        std::set<std::string> used;
        forEachAnimator(project, [&](VertexAnimatorComponent& animator){
            if (!animator.animation.empty() && Util::isVertexAnimationFile(animator.animation)){
                used.insert(keyOf(animatorFile(project, animator.animation)));
            }
        });
        auto& watched = registry().watched;
        for (auto it = watched.begin(); it != watched.end();){
            if (!used.count(it->first)){
                it = watched.erase(it);
                continue;
            }
            const fs::path file = FileUtils::pathFromUtf8(it->first);
            const auto vatStamp = statFile(file);
            const auto meshStamp = statFile(ev::pairPathOf(file));
            const auto now = std::make_tuple(vatStamp.first, vatStamp.second, meshStamp.first, meshStamp.second);
            if (now != it->second){
                if (std::find(changed.begin(), changed.end(), file) == changed.end()){
                    changed.push_back(file);
                }
                // the mesh alone changed: filesChanged is told about the mesh (the asset reloads
                // even when the JSON is the same)
                if (std::get<2>(now) != std::get<2>(it->second) || std::get<3>(now) != std::get<3>(it->second)){
                    changed.push_back(ev::pairPathOf(file));
                }
                it->second = now;
            }
            ++it;
        }
        for (const std::string& file : used){
            if (!watched.count(file)){
                const fs::path path = FileUtils::pathFromUtf8(file);
                const auto vatStamp = statFile(path);
                const auto meshStamp = statFile(ev::pairPathOf(path));
                watched[file] = std::make_tuple(vatStamp.first, vatStamp.second, meshStamp.first, meshStamp.second);
            }
        }
    }
    if (!changed.empty()){
        filesChanged(changed, project);
    }
}

void VatDocument::closeAll(){
    Registry& reg = registry();
    reg.documents.clear();
    reg.watched.clear();
    reg.project = nullptr;
    reg.projectPath.clear();
}

int VatDocument::addListener(Listener listener){
    int id = nextListener++;
    listeners[id] = std::move(listener);
    return id;
}

void VatDocument::removeListener(int id){
    listeners.erase(id);
}

void VatDocument::notify(Change change){
    std::map<int, Listener> copy = listeners;
    for (auto& entry : copy){
        if (listeners.count(entry.first) && entry.second){
            entry.second(*this, change);
        }
    }
}
