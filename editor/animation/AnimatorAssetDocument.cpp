// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorAssetDocument.h"

#include "Project.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "component/AnimationPlayerComponent.h"
#include "command/CommandHistory.h"
#include "command/CommandNames.h"
#include "command/type/WriteFilesCmd.h"
#include "pool/AnimatorAssetPool.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <system_error>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace{

    struct Registry{
        Project* project = nullptr;
        fs::path projectPath;
        std::map<std::string, std::weak_ptr<AnimatorAssetDocument>> documents;
    };

    Registry& registry(){
        static Registry* r = new Registry();
        return *r;
    }

    void useProject(Project* project){
        Registry& reg = registry();
        if (reg.project != project || reg.projectPath != project->getProjectPath()){
            reg.documents.clear();
            reg.project = project;
            reg.projectPath = project->getProjectPath();
        }
    }

    bool fail(std::string* error, const std::string& message){
        if (error) *error = message;
        return false;
    }

    bool isInside(const fs::path& root, const fs::path& path, fs::path* relative){
        const fs::path rel = path.lexically_normal().lexically_relative(root.lexically_normal());
        if (rel.empty() || *rel.begin() == "..") return false;
        if (relative) *relative = rel;
        return true;
    }

    std::string key(const fs::path& absolute){
        return absolute.lexically_normal().generic_string();
    }

}

bool AnimatorAssetDocument::kindOf(const std::string& path, Kind& out){
    if (Util::isAnimatorControllerFile(path)){ out = Kind::Controller; return true; }
    if (Util::isAvatarMaskFile(path)){ out = Kind::Mask; return true; }
    if (Util::isMontageFile(path)){ out = Kind::Montage; return true; }
    return false;
}

const char* AnimatorAssetDocument::kindName(Kind kind){
    switch (kind){
        case Kind::Controller: return "Animator Controller";
        case Kind::Mask: return "Avatar Mask";
        case Kind::Montage: return "Animation Montage";
    }
    return "";
}

std::shared_ptr<AnimatorAssetDocument> AnimatorAssetDocument::open(Project* project, const std::string& path, std::string* error){
    if (!project || project->getProjectPath().empty()){
        fail(error, "no project is open");
        return nullptr;
    }
    Kind kind;
    if (!kindOf(path, kind)){
        fail(error, "'" + path + "' is not a .controller, .mask or .montage file");
        return nullptr;
    }
    useProject(project);
    const fs::path root = project->getProjectPath().lexically_normal();
    const fs::path given = FileUtils::pathFromUtf8(path);
    fs::path absolute = given.is_absolute() ? given.lexically_normal() : (root / given).lexically_normal();
    std::error_code ec;
    if (!given.is_absolute() && !fs::exists(absolute, ec)){
        const fs::path asset = project->resolveAssetPath(given);
        if (!asset.empty() && fs::exists(asset, ec)) absolute = asset.lexically_normal();
    }
    fs::path relative;
    if (!isInside(root, absolute, &relative)){
        fail(error, "'" + path + "' is outside the project");
        return nullptr;
    }
    Registry& reg = registry();
    auto it = reg.documents.find(key(absolute));
    if (it != reg.documents.end()){
        if (std::shared_ptr<AnimatorAssetDocument> existing = it->second.lock()) return existing;
    }
    std::string text;
    if (!ProjectChecks::readFile(absolute, text)){
        fail(error, "cannot read '" + relative.generic_string() + "'");
        return nullptr;
    }
    auto document = std::make_shared<AnimatorAssetDocument>();
    document->project = project;
    document->assetKind = kind;
    document->filePath = absolute;
    document->relativePath = relative.generic_string();
    document->assetRelative = project->normalizeToAssetsRelative(absolute).generic_string();
    if (!document->load(text, error)) return nullptr;
    reg.documents[key(absolute)] = document;
    return document;
}

std::shared_ptr<AnimatorAssetDocument> AnimatorAssetDocument::create(Project* project, const std::string& path, Kind kind,
                                                                     const std::string& undoName, std::string* error){
    if (!project || project->getProjectPath().empty()){
        fail(error, "no project is open");
        return nullptr;
    }
    Kind fromPath;
    if (!kindOf(path, fromPath) || fromPath != kind){
        fail(error, std::string("'") + path + "' does not have the extension of an " + kindName(kind));
        return nullptr;
    }
    const fs::path root = project->getProjectPath().lexically_normal();
    const fs::path given = FileUtils::pathFromUtf8(path);
    const fs::path absolute = given.is_absolute() ? given.lexically_normal() : (root / given).lexically_normal();
    if (!isInside(root, absolute, nullptr)){
        fail(error, "'" + path + "' is outside the project");
        return nullptr;
    }
    std::error_code ec;
    if (fs::exists(absolute, ec)){
        fail(error, "'" + path + "' exists already");
        return nullptr;
    }
    std::string text;
    const std::string guid = anim::generateClipGuid();
    if (kind == Kind::Controller){
        anim::ControllerDef def;
        def.guid = guid;
        anim::LayerDef layer;
        layer.name = "Base Layer";
        def.layers.push_back(layer);
        anim::normalizeController(def);
        text = anim::writeController(def);
    }else if (kind == Kind::Mask){
        anim::AvatarMaskDef def;
        def.guid = guid;
        text = anim::writeMask(def);
    }else{
        anim::MontageDef def;
        def.guid = guid;
        anim::normalizeMontage(def);
        text = anim::writeMontage(def);
    }
    auto* command = new WriteFilesCmd({{absolute, text}}, [](const std::vector<fs::path>& changed){
        AnimatorAssetDocument::filesChanged(changed);
    });
    command->setName(undoName.empty() ? std::string("Create ") + kindName(kind) + " " + CommandNames::file(absolute) : undoName);
    command->setNoMerge();
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)){
        fail(error, "writing '" + path + "' failed" + (failure->empty() ? std::string() : " (" + *failure + ")"));
        return nullptr;
    }
    return open(project, absolute.string(), error);
}

void AnimatorAssetDocument::filesChanged(const std::vector<fs::path>& paths){
    Registry& reg = registry();
    for (const fs::path& path : paths){
        AnimatorAssetPool::invalidate(path.string());
        auto it = reg.documents.find(key(path));
        if (it == reg.documents.end()) continue;
        if (std::shared_ptr<AnimatorAssetDocument> document = it->second.lock()){
            document->reload();
        }else{
            reg.documents.erase(it);
        }
    }
}

void AnimatorAssetDocument::closeAll(){
    registry().documents.clear();
}

const std::string& AnimatorAssetDocument::guid() const{
    switch (assetKind){
        case Kind::Controller: return controllerData.guid;
        case Kind::Mask: return maskData.guid;
        case Kind::Montage: return montageData.guid;
    }
    return controllerData.guid;
}

bool AnimatorAssetDocument::load(const std::string& newText, std::string* loadError){
    std::vector<ClipWarning> warnings;
    anim::AnimatorParseError parseError;
    bool ok = false;
    if (assetKind == Kind::Controller){
        anim::ControllerParseResult parsed = anim::parseController(newText);
        ok = parsed.ok;
        parseError = parsed.error;
        if (ok){
            controllerData = std::move(parsed.data);
            warnings = std::move(parsed.warnings);
        }
    }else if (assetKind == Kind::Mask){
        anim::MaskParseResult parsed = anim::parseMask(newText);
        ok = parsed.ok;
        parseError = parsed.error;
        if (ok){
            maskData = std::move(parsed.data);
            warnings = std::move(parsed.warnings);
        }
    }else{
        anim::MontageParseResult parsed = anim::parseMontage(newText);
        ok = parsed.ok;
        parseError = parsed.error;
        if (ok){
            montageData = std::move(parsed.data);
            warnings = std::move(parsed.warnings);
        }
    }
    if (!ok){
        error = relativePath + ": " + parseError.toString();
        return fail(loadError, error);
    }
    error.clear();
    text = newText;
    loadWarnings = std::move(warnings);
    documentRevision++;
    return true;
}

void AnimatorAssetDocument::reload(){
    std::string newText;
    if (!ProjectChecks::readFile(filePath, newText)){
        error = "'" + relativePath + "' does not exist any more";
        notify();
        return;
    }
    if (newText == text && error.empty()) return;
    // this document's own commit keeps the merge epoch (a drag's commits merge); anything else
    // (an undo, a redo, an outside edit) starts a new one
    const bool own = !writing.empty() && newText == writing;
    writing.clear();
    std::string loadError;
    load(newText, &loadError);
    if (!own) mergeEpoch++;
    notify();
}

bool AnimatorAssetDocument::write(const std::string& newText, const std::string& undoName, const std::string& mergeKey, std::string* writeError){
    if (newText == text) return true;
    std::string onDisk;
    if (ProjectChecks::readFile(filePath, onDisk) && onDisk != text){
        reload();
        return fail(writeError, "'" + relativePath + "' changed on disk since it was read: it was reloaded, make the edit again");
    }
    auto* command = new WriteFilesCmd({{filePath, newText}}, [](const std::vector<fs::path>& changed){
        AnimatorAssetDocument::filesChanged(changed);
    });
    writing = newText;
    command->setName(undoName.empty() ? std::string("Edit ") + kindName(assetKind) + " " + CommandNames::file(filePath) : undoName);
    if (mergeKey.empty()) command->setNoMerge();
    else command->setMergeKey(relativePath + "\n" + std::to_string(mergeEpoch) + "\n" + mergeKey);
    auto failure = std::make_shared<std::string>();
    command->reportFailureTo(failure);
    if (!project->getProjectCommandHistory()->addCommand(command)){
        writing.clear();
        return fail(writeError, "writing '" + relativePath + "' failed" + (failure->empty() ? std::string() : " (" + *failure + ")"));
    }
    writing.clear();
    return true;
}

bool AnimatorAssetDocument::commit(const anim::ControllerDef& data, const std::string& undoName, const std::string& mergeKey, std::string* commitError){
    if (assetKind != Kind::Controller) return fail(commitError, "'" + relativePath + "' is not an Animator Controller");
    anim::ControllerDef copy = data;
    copy.guid = controllerData.guid.empty() ? anim::generateClipGuid() : controllerData.guid;
    anim::normalizeController(copy);
    return write(anim::writeController(copy), undoName, mergeKey, commitError);
}

bool AnimatorAssetDocument::commit(const anim::AvatarMaskDef& data, const std::string& undoName, const std::string& mergeKey, std::string* commitError){
    if (assetKind != Kind::Mask) return fail(commitError, "'" + relativePath + "' is not an Avatar Mask");
    anim::AvatarMaskDef copy = data;
    copy.guid = maskData.guid.empty() ? anim::generateClipGuid() : maskData.guid;
    anim::normalizeMask(copy);
    return write(anim::writeMask(copy), undoName, mergeKey, commitError);
}

bool AnimatorAssetDocument::commit(const anim::MontageDef& data, const std::string& undoName, const std::string& mergeKey, std::string* commitError){
    if (assetKind != Kind::Montage) return fail(commitError, "'" + relativePath + "' is not an Animation Montage");
    anim::MontageDef copy = data;
    copy.guid = montageData.guid.empty() ? anim::generateClipGuid() : montageData.guid;
    anim::normalizeMontage(copy);
    return write(anim::writeMontage(copy), undoName, mergeKey, commitError);
}

int AnimatorAssetDocument::addListener(std::function<void()> listener){
    const int id = nextListener++;
    listeners[id] = std::move(listener);
    return id;
}

void AnimatorAssetDocument::removeListener(int id){
    listeners.erase(id);
}

void AnimatorAssetDocument::notify(){
    // a listener may remove itself
    std::vector<std::function<void()>> calls;
    for (const auto& entry : listeners) calls.push_back(entry.second);
    for (const auto& call : calls) call();
}

size_t jasperengine::editor::appendPlayerStates(anim::ControllerDef& def, const AnimationPlayerComponent& player){
    if (def.layers.empty()){
        anim::LayerDef layer;
        layer.name = "Base Layer";
        def.layers.push_back(layer);
    }
    anim::StateMachineDef& machine = def.layers[0].machine;
    const std::vector<std::string> names = anim::playerStateNames(player.clips);
    const size_t first = machine.states.size();
    for (size_t i = 0; i < player.clips.size(); i++){
        anim::StateDef s;
        s.name = names[i];
        s.motion.clip.path = player.clips[i].clip;
        s.motion.clip.guid = player.clips[i].guid;
        if (player.clips[i].wrap == ClipWrapMode::Loop || player.clips[i].wrap == ClipWrapMode::PingPong) s.loop = AnimatorStateLoop::Loop;
        else if (player.clips[i].wrap == ClipWrapMode::Once || player.clips[i].wrap == ClipWrapMode::ClampForever) s.loop = AnimatorStateLoop::Once;
        const size_t cell = first + i;
        s.position = Vector2(240.0f + 220.0f * (float)(cell % 4), 120.0f + 90.0f * (float)(cell / 4));
        machine.states.push_back(s);
    }
    if (!player.defaultClip.empty()) machine.defaultState = player.defaultClip;
    return player.clips.size();
}
