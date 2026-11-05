// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's clip mode, its state and actions (animation clips spec 4.4,
// 9.R31/R39/R54): binding, the binding report, the key selection, the preview helpers and every
// edit (select, box, drag, delete, copy/paste/duplicate, ease, keys, snap, Samples, values). Edits
// go through the AnimationClipController (one project step each). The drawing is in
// AnimationWindowClipDraw.cpp.

#include "AnimationWindowClip.h"
#include "component/Transform.h"
#include "animation/ModelSubClips.h"
#include "sprite/SpriteSheetPool.h"
#include "animation/ClipPropertyNames.h"
#include "animation/PropertyBindings.h"
#include "AnimationWindowClipUtil.h"

#include "Out.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "animation/ModelSubClips.h"
#include "command/type/ClipSceneActions.h"
#include "component/AnimationPlayerComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/ScriptComponent.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include <chrono>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::clipui;

namespace fs = std::filesystem;

// ---- basics ----

AnimationWindowClip::AnimationWindowClip(Project* project): project(project){
}

AnimationClipController& AnimationWindowClip::controller() const{
    controllerUsed = true;
    return AnimationClipController::shared(project);
}

std::shared_ptr<AnimationClipDocument> AnimationWindowClip::document() const{
    return controller().getDocument();
}

SceneProject* AnimationWindowClip::boundScene() const{
    const uint32_t id = controller().getSceneId();
    return id ? project->getScene(id) : nullptr;
}

Scene* AnimationWindowClip::valueScene() const{
    AnimationClipController& c = controller();
    const uint32_t id = c.isPreviewing() ? c.getPreviewSceneId() : c.getSceneId();
    SceneProject* sceneProject = id ? project->getScene(id) : nullptr;
    return sceneProject ? sceneProject->scene : nullptr;
}

Entity AnimationWindowClip::valueRoot() const{
    AnimationClipController& c = controller();
    Entity root = c.isPreviewing() ? c.getPreviewRoot() : c.getRoot();
    Scene* scene = valueScene();
    return scene && root != NULL_ENTITY && scene->isEntityCreated(root) ? root : NULL_ENTITY;
}

void AnimationWindowClip::setNotice(const std::string& text, bool warning){
    notice = text;
    noticeWarning = warning;
    noticeAt = ImGui::GetTime();
    if (warning && !text.empty()) Out::warning("Animation: " + text);
}

float AnimationWindowClip::scaledLabelWidth() const{
    return labelWidth * Theme::dpiScale();
}

float AnimationWindowClip::frameRate() const{
    std::shared_ptr<AnimationClipDocument> doc = document();
    const float fps = doc ? doc->data().frameRate : 30.0f;
    return std::isfinite(fps) && fps >= 1.0f ? fps : 30.0f;
}

float AnimationWindowClip::snapToFrame(float time) const{
    const float fps = frameRate();
    return std::round(time * fps) / fps;
}

float AnimationWindowClip::viewLength() const{
    std::shared_ptr<AnimationClipDocument> doc = document();
    return std::max(1.0f, doc ? doc->data().effectiveLength() : 0.0f);
}

// ---- mode ----

void AnimationWindowClip::bind(uint32_t sceneId, Entity selected){
    AnimationClipController& c = controller();
    const Entity oldRoot = c.getRoot();
    const uint32_t oldScene = c.getSceneId();
    const std::string oldClip = c.getClipPath();
    std::string error;
    if (selected == NULL_ENTITY){
        c.bindRoot(0, NULL_ENTITY, &error);
    }else if (!c.bindRoot(sceneId, selected, &error)){
        setNotice(error, true);
    }else if (!c.hasPlayer() && c.getDocument() && c.getRoot() != oldRoot){
        // a GameObject without a player shows its empty state, not the clip opened before
        c.closeClip();
    }
    if (c.getRoot() != oldRoot || c.getSceneId() != oldScene || c.getClipPath() != oldClip){
        selection.clear();
        selectEvent(-1);
        drag = Drag::None;
        notice.clear();
        previewOverride.clear();
    }
}

void AnimationWindowClip::refreshBinding(){
    // the bound GameObject got an Animation Player (or its first clip) after it was selected: its
    // default state opens, as if it had been selected now
    AnimationClipController& c = controller();
    SceneProject* bound = boundScene();
    const Entity root = c.getRoot();
    // an undone Create New Clip: the file and the player are gone, the empty state shows again
    if (bound && root != NULL_ENTITY && bound->scene->isEntityCreated(root) && c.getDocument() && !c.getDocument()->exists() &&
        !c.getDocument()->isReadOnly() && !c.hasPlayer()){
        c.closeClip();
        selection.clear();
        selectEvent(-1);
        return;
    }
    if (!bound || root == NULL_ENTITY || !bound->scene->isEntityCreated(root) || c.getDocument() || !c.getStateName().empty()) return;
    if (!c.hasPlayer() || c.getStateNames().empty()) return;
    const uint32_t sceneId = bound->id;
    c.bindRoot(0, NULL_ENTITY, nullptr);
    std::string error;
    if (!c.bindRoot(sceneId, root, &error)) setNotice(error, true);
}

bool AnimationWindowClip::open(uint32_t sceneId, Entity root, const std::string& clip, std::string* error){
    AnimationClipController& c = controller();
    if (root == NULL_ENTITY){
        c.bindRoot(0, NULL_ENTITY, nullptr);
    }else if (!c.bindRoot(sceneId, root, error)){
        return false;
    }
    selection.clear();
    selectEvent(-1);
    drag = Drag::None;
    notice.clear();
    previewOverride.clear();
    if (!c.openClip(clip, error)) return false;
    scrollX = 0.0f;
    return true;
}

bool AnimationWindowClip::openState(const std::string& state, std::string* error){
    selection.clear();
    selectEvent(-1);
    notice.clear();
    return controller().openState(state, error);
}

bool AnimationWindowClip::hasTargetlessClip() const{
    if (!controllerUsed) return false;
    return document() && controller().getRoot() == NULL_ENTITY;
}

void AnimationWindowClip::update(double dt, bool clipModeShown, bool focused){
    if (!notice.empty() && ImGui::GetTime() - noticeAt > 8.0) notice.clear();
    if (!controllerUsed) return;
    AnimationClipController& c = controller();
    // (the App's per-frame hook, AnimationClipAppHooks::frame, runs the controller's update: playback,
    // re-applies, deferred stops, also while this window is closed)
    (void)dt;
    c.setTakesKeys(clipModeShown && focused);
}

void AnimationWindowClip::stopPreview(){
    if (!controllerUsed) return;
    if (drag == Drag::Keys || drag == Drag::CurveKeys) endDrag();
    drag = Drag::None;
    controller().stopPreview();
}

void AnimationWindowClip::sceneClosed(uint32_t sceneId){
    if (controllerUsed && controller().getSceneId() == sceneId){
        selection.clear();
        selectEvent(-1);
        drag = Drag::None;
    }
    reportRevision = ~uint64_t(0);
}

bool AnimationWindowClip::takeEntityAnimationRequest(uint32_t& sceneId, Entity& animation){
    if (!entityAnimationRequested) return false;
    entityAnimationRequested = false;
    sceneId = requestedScene;
    animation = requestedAnimation;
    return true;
}

// ---- preview helpers (I5) ----

std::string AnimationWindowClip::previewModelOf() const{
    if (!previewOverride.empty()) return previewOverride;
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return std::string();
    if (!doc->data().previewModel.empty()) return doc->data().previewModel;
    // a model's animation previews on that model
    anim::SubClipReference reference;
    if (doc->isReadOnly() && anim::parseSubClipReference(doc->path(), reference)) return reference.model;
    return std::string();
}

Entity AnimationWindowClip::findModelInstance(SceneProject* sceneProject, const std::string& model) const{
    if (!sceneProject || !sceneProject->scene || model.empty()) return NULL_ENTITY;
    Scene* scene = sceneProject->scene;
    // asset-relative (ModelComponent::filename) or project-relative ("assets/...")
    fs::path target = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(model)).lexically_normal();
    std::error_code ec;
    const fs::path projectRelative = (project->getProjectPath() / editor::FileUtils::pathFromUtf8(model)).lexically_normal();
    if (!fs::exists(target, ec) && fs::exists(projectRelative, ec)) target = projectRelative;
    auto matches = [&](Entity entity){
        if (!scene->isEntityCreated(entity)) return false;
        if (const ModelComponent* component = scene->findComponent<ModelComponent>(entity)){
            if (!component->filename.empty() &&
                project->resolveAssetPath(FileUtils::pathFromUtf8(component->filename.path)).lexically_normal() == target) return true;
        }
        PrefabObjectRef ref;
        if (project->isPrefabInstanceRoot(sceneProject->id, entity) && project->findPrefabObject(sceneProject->id, entity, ref) &&
            !ref.path.empty()){
            const fs::path prefab = ref.path.is_absolute() ? ref.path : project->getProjectPath() / ref.path;
            if (prefab.lexically_normal() == target) return true;
        }
        return false;
    };
    for (Entity entity : sceneProject->selectedEntities){
        if (matches(entity)) return entity;
    }
    auto transforms = scene->getComponentArray<Transform>();
    for (size_t i = 0; i < transforms->size(); i++){
        if (matches(transforms->getEntity(i))) return transforms->getEntity(i);
    }
    return NULL_ENTITY;
}

std::string AnimationWindowClip::previewBlocker(SceneProject* sceneProject) const{
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return "No clip is open";
    if (c.isPreviewing()) return std::string();
    if (!sceneProject || sceneProject->playState != ScenePlayState::STOPPED) return "Previews run in edit mode (stop Play first)";
    SceneProject* bound = boundScene();
    const Entity root = c.getRoot();
    if (bound && root != NULL_ENTITY && bound->scene->isEntityCreated(root)){
        if (bound->id != sceneProject->id) return "The bound GameObject is in another scene: show its scene tab";
        return std::string();
    }
    const std::string model = previewModelOf();
    if (model.empty()) return "Nothing to preview on: select a GameObject, or set the Preview Model";
    if (findModelInstance(sceneProject, model) == NULL_ENTITY){
        return "No instance of " + FileUtils::pathToUtf8(FileUtils::pathFromUtf8(model).filename()) +
               " in this scene: select a GameObject, or drop a model or prefab on Preview Model";
    }
    return std::string();
}

bool AnimationWindowClip::startPreview(SceneProject* sceneProject, std::string* error){
    AnimationClipController& c = controller();
    if (c.isPreviewing()) return true;
    const std::string blocker = previewBlocker(sceneProject);
    if (!blocker.empty()){
        if (error) *error = blocker;
        return false;
    }
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (c.getRoot() == NULL_ENTITY && (!previewOverride.empty() || doc->data().previewModel.empty())){
        // the controller previews a target-less clip on preview.model only: an override or a
        // sub-clip's own model binds the instance (the clip stays open)
        const Entity instance = findModelInstance(sceneProject, previewModelOf());
        const std::string path = doc->path();
        if (!c.bindRoot(sceneProject->id, instance, error)) return false;
        if (c.getClipPath() != path && !c.openClip(path, error)) return false;
    }
    return c.startPreview(error);
}

// ---- binding report, rows, values ----

std::vector<std::string> AnimationWindowClip::scriptFunctions() const{
    // the methods of the value root's Lua scripts, read again only when the root, its script
    // list or a script file's time stamp changed (stats at most once a second, no file read per
    // frame)
    AnimationClipController& c = controller();
    const uint32_t id = c.isPreviewing() ? c.getPreviewSceneId() : c.getSceneId();
    SceneProject* sceneProject = id ? project->getScene(id) : nullptr;
    const Entity root = valueRoot();
    if (!sceneProject || root == NULL_ENTITY){
        if (scriptCache.root != NULL_ENTITY) scriptCache.version++;
        scriptCache = ScriptCache{{}, 0, NULL_ENTITY, std::string(), -10.0, false, scriptCache.version};
        return {};
    }
    const double now = ImGui::GetTime();
    if (scriptCache.sceneId == id && scriptCache.root == root && now - scriptCache.checkedAt < 1.0) return scriptCache.functions;
    // the stamp: each script entry's type, path, class, enabled flag and file time
    std::string stamp;
    bool cpp = false;
    if (const ScriptComponent* scripts = sceneProject->scene->findComponent<ScriptComponent>(root)){
        for (const ScriptEntry& entry : scripts->scripts){
            if (entry.type == ScriptType::CPP) cpp = true;
            stamp += std::to_string((int)entry.type) + (entry.enabled ? "1" : "0") + entry.path + "|" + entry.className + "|";
            if (entry.type == ScriptType::LUA && !entry.path.empty()){
                std::error_code ec;
                const auto time = fs::last_write_time(project->resolveLuaPath(FileUtils::pathFromUtf8(entry.path.path)), ec);
                if (!ec) stamp += std::to_string((long long)time.time_since_epoch().count());
            }
            stamp += ";";
        }
    }
    scriptCache.checkedAt = now;
    if (scriptCache.sceneId == id && scriptCache.root == root && scriptCache.stamp == stamp) return scriptCache.functions;
    scriptCache.sceneId = id;
    scriptCache.root = root;
    scriptCache.stamp = stamp;
    scriptCache.hasCpp = cpp;
    scriptCache.functions = ClipValidation::scriptFunctions(project, sceneProject, root);
    scriptCache.version++;
    return scriptCache.functions;
}

void AnimationWindowClip::refreshReport(){
    std::shared_ptr<AnimationClipDocument> doc = document();
    const uint64_t revision = doc ? doc->revision() : 0;
    const Entity root = valueRoot();
    Scene* scene = valueScene();
    const uint64_t hierarchy = scene ? scene->hierarchyRevision() : 0;
    const std::string clip = doc ? doc->path() : std::string();
    const double now = ImGui::GetTime();
    // the script methods (cached, stamp-checked) are part of the key
    scriptFunctions();
    const uint64_t scripts = scriptCache.version;
    // components added or removed under the root don't bump the hierarchy revision: a slow
    // check (2 s, only while the window draws) covers them
    if (revision == reportRevision && root == reportRoot && hierarchy == reportHierarchy && clip == reportClip &&
        scripts == reportScripts && now - reportAt < 2.0){
        return;
    }
    report = ClipValidationReport();
    if (doc){
        if (scene && root != NULL_ENTITY){
            ClipValidationOptions options;
            options.scriptFunctions = scriptCache.functions;
            options.hasCppScript = scriptCache.hasCpp;
            report = ClipValidation::validate(doc->data(), scene, root, options);
        }else{
            report = ClipValidation::validate(doc->data(), nullptr, NULL_ENTITY);
        }
    }
    reportRevision = revision;
    reportRoot = root;
    reportHierarchy = hierarchy;
    reportClip = clip;
    reportScripts = scripts;
    reportAt = now;
}

bool AnimationWindowClip::KeyId::operator==(const KeyId& other) const{
    return channel == other.channel && time == other.time && path == other.path && property == other.property;
}

AnimationWindowClip::KeyId AnimationWindowClip::keyIdOf(const ClipTrackData& track, float time){
    KeyId id;
    id.path = track.path;
    id.channel = track.channel;
    id.time = time;
    id.property = propertyId(track);
    return id;
}

void AnimationWindowClip::refreshSelection(){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        selection.clear();
        selectedRefs.clear();
        selectedMask.clear();
        resolvedSelection.clear();
        resolvedDocument.reset();
        return;
    }
    // nothing changed since the last resolve: an O(S) compare, no lookups
    if (doc == resolvedDocument.lock() && doc->revision() == selectionRevision && selection == resolvedSelection) return;
    const ClipData& data = doc->data();
    // (path, channel) -> track, once per revision
    if (doc != indexDocument.lock() || doc->revision() != indexRevision){
        trackIndex.clear();
        for (size_t t = 0; t < data.tracks.size(); t++){
            trackIndex[rowKey(data.tracks[t])] = (int)t;
        }
        indexDocument = doc;
        indexRevision = doc->revision();
    }
    selectedMask.assign(data.tracks.size(), std::vector<char>());
    for (size_t t = 0; t < data.tracks.size(); t++) selectedMask[t].assign(data.tracks[t].keys.size(), 0);
    selectedRefs.clear();
    std::vector<KeyId> kept;
    kept.reserve(selection.size());
    for (const KeyId& id : selection){
        auto it = trackIndex.find(id.path + "|" + anim::channelName(id.channel) + (id.property.empty() ? std::string() : "|" + id.property));
        if (it == trackIndex.end()) continue;
        const std::vector<ClipKey>& keys = data.tracks[(size_t)it->second].keys;
        // keys are sorted: the nearest within the epsilon
        auto at = std::lower_bound(keys.begin(), keys.end(), id.time - kKeyEpsilon,
                                   [](const ClipKey& k, float t){ return k.time < t; });
        if (at == keys.end() || at->time > id.time + kKeyEpsilon) continue;
        if (at + 1 != keys.end() && std::fabs((at + 1)->time - id.time) < std::fabs(at->time - id.time)) ++at;
        const size_t key = (size_t)(at - keys.begin());
        char& mark = selectedMask[(size_t)it->second][key];
        if (mark) continue;
        mark = 1;
        selectedRefs.push_back({(size_t)it->second, key});
        kept.push_back({id.path, id.channel, at->time, id.property});
    }
    selection = kept;
    resolvedSelection = kept;
    resolvedDocument = doc;
    selectionRevision = doc->revision();
}

void AnimationWindowClip::setSelectionFromRefs(const std::vector<AnimationClipController::KeyRef>& refs){
    selection.clear();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    const ClipData& data = doc->data();
    for (const AnimationClipController::KeyRef& ref : refs){
        if (ref.track >= data.tracks.size() || ref.key >= data.tracks[ref.track].keys.size()) continue;
        const ClipTrackData& track = data.tracks[ref.track];
        selection.push_back(keyIdOf(track, track.keys[ref.key].time));
    }
    refreshSelection();
}

bool AnimationWindowClip::isSelected(size_t track, size_t key) const{
    return track < selectedMask.size() && key < selectedMask[track].size() && selectedMask[track][key];
}

std::vector<std::string> AnimationWindowClip::componentLabels(size_t trackIndex) const{
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || trackIndex >= doc->data().tracks.size()) return {};
    const ClipTrackData& track = doc->data().tracks[trackIndex];
    switch (track.channel){
        case ClipChannel::Position:
        case ClipChannel::Scale:
        case ClipChannel::Rotation:
        case ClipChannel::Humanoid:
        case ClipChannel::HumanoidPosition:
            return angleComponentNames(track);
        case ClipChannel::Morph: {
            size_t width = 0;
            for (const ClipKey& key : track.keys) width = std::max(width, key.value.size());
            // morph target names of the bound mesh (its own), else of its model (name -> index)
            std::map<int, std::string> names;
            Scene* scene = valueScene();
            Entity entity = NULL_ENTITY;
            for (const TrackReport& entry : report.tracks){
                if (entry.track == trackIndex) entity = entry.entity;
            }
            if (scene && entity != NULL_ENTITY && scene->isEntityCreated(entity)){
                if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
                    for (size_t i = 0; i < mesh->morphTargetNames.size(); i++) names[(int)i] = mesh->morphTargetNames[i];
                    if (!mesh->morphTargetNames.empty()) entity = NULL_ENTITY;
                }
            }
            for (int depth = 0; scene && entity != NULL_ENTITY && depth < 256; depth++){
                if (!scene->isEntityCreated(entity)) break;
                if (const ModelComponent* model = scene->findComponent<ModelComponent>(entity)){
                    for (const auto& entry : model->morphNameMapping) names[entry.second] = entry.first;
                    break;
                }
                const Transform* transform = scene->findComponent<Transform>(entity);
                entity = transform ? transform->parent : NULL_ENTITY;
            }
            std::vector<std::string> labels;
            for (size_t i = 0; i < width; i++){
                auto it = names.find((int)i);
                labels.push_back(it != names.end() && !it->second.empty() ? it->second : "Weight " + std::to_string(i));
            }
            return labels;
        }
        case ClipChannel::Lattice:
            return {std::to_string(track.points.size()) + " lattice points"};
        case ClipChannel::Property:
            // a colour's r g b a, a vector's x y z w; one value has no component rows
            if (!track.known || track.swizzle >= 0 || track.width < 2) return {};
            if (track.kind == PropertyKind::Color3 || track.kind == PropertyKind::Color4){
                std::vector<std::string> labels = {"r", "g", "b", "a"};
                labels.resize(track.width);
                return labels;
            }else{
                std::vector<std::string> labels = {"x", "y", "z", "w"};
                labels.resize(track.width);
                return labels;
            }
    }
    return {};
}

std::vector<AnimationWindowClip::Row> AnimationWindowClip::buildRows() const{
    std::vector<Row> rows;
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return rows;
    const ClipData& data = doc->data();
    Row summary;
    summary.kind = Row::Kind::Summary;
    rows.push_back(summary);
    Scene* scene = valueScene();
    const Entity root = valueRoot();
    const std::string rootName = scene && root != NULL_ENTITY ? scene->getEntityName(root) : std::string(rootLabel());
    // the report by track (one pass)
    std::vector<const TrackReport*> reports(data.tracks.size(), nullptr);
    for (const TrackReport& entry : report.tracks){
        if (entry.track < reports.size()) reports[entry.track] = &entry;
    }
    // tracks the state's layer mask hides (dimmed)
    const std::vector<std::string> masked = controller().maskedTracks();
    for (size_t t = 0; t < data.tracks.size(); t++){
        const ClipTrackData& track = data.tracks[t];
        Row row;
        if (t < masked.size()) row.masked = masked[t];
        row.kind = Row::Kind::Track;
        row.track = (int)t;
        // a property row: "Name : Image.Color" (the group as the bound GameObject shows it)
        if (track.channel == ClipChannel::Property){
            const Entity entity = reports[t] ? reports[t]->entity : NULL_ENTITY;
            row.channelText = ClipPropertyNames::trackLabel(scene, entity, track);
        }else{
            row.channelText = channelLabel(track.channel);
        }
        row.label = (track.path.empty() ? rootName : track.path) + " : " + row.channelText;
        // Unity's form: the leaf GameObject's name (the full path in the tooltip)
        row.leaf = (track.path.empty() ? rootName : lastSegment(track.path)) + " : " + row.channelText;
        if (isHumanoidChannel(track.channel)){
            // "Humanoid : Left Upper Arm" (anim-retarget 9)
            row.label = humanoidTrackLabel(track.channel, track.humanBone);
            row.leaf = row.label;
        }
        if (const TrackReport* entry = reports[t]){
            const bool bad = entry->status == TrackBindingStatus::MissingPath ||
                             entry->status == TrackBindingStatus::MissingComponent ||
                             entry->status == TrackBindingStatus::LatticeIndexOutOfRange ||
                             entry->status == TrackBindingStatus::MorphIndexOutOfRange ||
                             entry->status == TrackBindingStatus::LegacyModelLayout ||
                             entry->status == TrackBindingStatus::UnaddressableName ||
                             entry->status == TrackBindingStatus::UnknownProperty ||
                             entry->status == TrackBindingStatus::HumanoidNoRig;
            row.missing = bad;
            row.message = entry->message;
            if (!entry->nearest.empty() && entry->status == TrackBindingStatus::MissingPath){
                row.message += "\nDid you mean: ";
                for (size_t i = 0; i < entry->nearest.size(); i++) row.message += (i ? ", " : "") + entry->nearest[i];
            }
        }
        if (row.missing) row.label += " (Missing!)";
        rows.push_back(row);
        if (expanded.count(rowKey(track))){
            const std::vector<std::string> labels = componentLabels(t);
            for (size_t c = 0; c < labels.size(); c++){
                Row component;
                component.kind = Row::Kind::Component;
                component.track = (int)t;
                component.component = track.channel == ClipChannel::Lattice ? -1 : (int)c;
                component.label = labels[c];
                component.leaf = labels[c];
                component.missing = row.missing;
                component.masked = row.masked;
                rows.push_back(component);
            }
        }
    }
    return rows;
}

std::vector<float> AnimationWindowClip::sampledValue(size_t trackIndex, float time) const{
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || trackIndex >= doc->data().tracks.size()) return {};
    const ClipTrackData& data = doc->data().tracks[trackIndex];
    std::shared_ptr<const AnimationClip> clip = doc->clip();
    if (!clip) return {};
    const int index = builtTrack(*clip, data);
    if (index < 0) return {};
    anim::TrackSample sample;
    if (!anim::sampleTrack(clip->getTracks()[(size_t)index], time, sample) || !sample.valid) return {};
    return flatten(sample);
}

// ---- actions ----

void AnimationWindowClip::seek(float time){
    AnimationClipController& c = controller();
    if (!std::isfinite(time)) return;
    time = std::clamp(time, 0.0f, kMaxTime);
    if (c.isPlaying()) c.pause();
    c.seek(time);
}

void AnimationWindowClip::togglePreview(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    if (c.isPreviewing()){
        stopPreview();
        return;
    }
    std::string error;
    if (!startPreview(sceneProject, &error)) setNotice(error, true);
}

void AnimationWindowClip::togglePlay(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    if (c.isPlaying()){
        // a paused playhead sits on a frame (Unity): keys made there land on the grid (I3)
        c.pause();
        c.seek(snapToFrame(c.getTime()));
        return;
    }
    std::string error;
    if (!c.isPreviewing() && !startPreview(sceneProject, &error)){
        setNotice(error, true);
        return;
    }
    c.play();
}

void AnimationWindowClip::stepFrame(int direction){
    const float fps = frameRate();
    const long frame = std::lround(controller().getTime() * fps) + direction;
    seek(std::max(0.0f, (float)frame / fps));
}

void AnimationWindowClip::stepKey(int direction){
    // keys only (the first / last buttons go to the clip's start and end)
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    const float now = controller().getTime();
    float best = direction < 0 ? -1.0f : kMaxTime + 1.0f;
    for (const ClipTrackData& track : doc->data().tracks){
        if (direction < 0){
            auto it = std::lower_bound(track.keys.begin(), track.keys.end(), now - kKeyEpsilon,
                                       [](const ClipKey& k, float t){ return k.time < t; });
            if (it != track.keys.begin()) best = std::max(best, std::prev(it)->time);
        }else{
            auto it = std::upper_bound(track.keys.begin(), track.keys.end(), now + kKeyEpsilon,
                                       [](float t, const ClipKey& k){ return t < k.time; });
            if (it != track.keys.end()) best = std::min(best, it->time);
        }
    }
    if (best >= 0.0f && best <= kMaxTime) seek(best);
}

bool AnimationWindowClip::createNewClip(SceneProject* sceneProject, const std::string& path, std::string* error){
    AnimationClipController& c = controller();
    const Entity root = c.getRoot();
    if (!sceneProject || root == NULL_ENTITY || c.getSceneId() != sceneProject->id){
        if (error) *error = "select the GameObject to animate first";
        return false;
    }
    ClipActionResult result;
    std::string failure;
    // the composite (file + Animation Player) is one scene step; the preview goes back first
    c.stopPreview();
    if (!ClipSceneActions::createNewClip(project, sceneProject->id, root, path, result, failure)){
        if (error) *error = failure;
        return false;
    }
    std::string openError;
    if (!c.openClip(result.clip, &openError)) setNotice(openError, true);
    selection.clear();
    selectEvent(-1);
    setNotice("Created " + FileUtils::pathToUtf8(FileUtils::pathFromUtf8(result.clip).filename()) + " (one scene undo step)", false);
    return true;
}

bool AnimationWindowClip::dropClips(SceneProject* sceneProject, Entity fallback, const std::vector<std::string>& paths, std::string* error){
    std::vector<std::string> clips;
    for (const std::string& path : paths){
        if (Util::isAnimationClipFile(path)) clips.push_back(path);
        // an .aseprite file (S44 R33): its tag clips
        if (SpriteSheetPool::isAsepritePath(path) && project->isInsideAssetsPath(path)){
            const std::string asset = project->normalizeToAssetsRelative(path).generic_string();
            std::vector<anim::ModelClipInfo> infos;
            if (anim::listModelFileClips(asset, infos, nullptr)){
                for (const anim::ModelClipInfo& info : infos) clips.push_back(asset + "#" + info.name);
            }
        }
    }
    if (clips.empty()){
        if (error) *error = "only animation clips (.anim) and imported .aseprite files can be dropped on the Animation window";
        return false;
    }
    AnimationClipController& c = controller();
    Entity root = NULL_ENTITY;
    if (sceneProject && sceneProject->scene){
        if (c.getSceneId() == sceneProject->id && c.getRoot() != NULL_ENTITY && sceneProject->scene->isEntityCreated(c.getRoot())){
            root = c.getRoot();
        }else if (fallback != NULL_ENTITY && sceneProject->scene->isEntityCreated(fallback)){
            const Entity playerRoot = AnimationClipController::findPlayerRoot(sceneProject->scene, fallback);
            root = playerRoot != NULL_ENTITY ? playerRoot : fallback;
        }
    }
    if (root == NULL_ENTITY){
        // nothing to animate: the clip opens on its own (edits the file), as from the Project window
        return open(0, NULL_ENTITY, clips.front(), error);
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        if (error) *error = "stop the game to add clips to the Animation Player of " + sceneProject->scene->getEntityName(root);
        return false;
    }
    // the clips join the player in one scene step; the preview goes back first
    c.stopPreview();
    ClipActionResult result;
    std::string failure;
    if (!ClipSceneActions::addClipsToPlayer(project, sceneProject->id, root, clips, result, failure)){
        if (error) *error = failure;
        return false;
    }
    if (!open(sceneProject->id, result.player, clips.front(), error)) return false;
    std::string status = result.status;
    for (const ClipWarning& warning : result.warnings) status += "; " + warning.message;
    setNotice(status, !result.warnings.empty());
    return true;
}

void AnimationWindowClip::askCreateNewClip(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    if (!sceneProject || c.getRoot() == NULL_ENTITY) return;
    const std::string file = ClipSceneActions::askNewClipPath(project, ClipConverters::defaultClipDirectory(project, sceneProject->id, c.getRoot()),
                                                              sceneProject->scene->getEntityName(c.getRoot()));
    if (file.empty()) return;
    std::string error;
    if (!createNewClip(sceneProject, file, &error)) setNotice("Create New Clip: " + error, true);
}

bool AnimationWindowClip::extract(SceneProject* sceneProject, const std::string& path, std::string* error){
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || !doc->isReadOnly()){
        if (error) *error = "the open clip is not a model sub-clip";
        return false;
    }
    const std::string state = c.getStateName();
    const bool usePlayer = !state.empty() && c.getRoot() != NULL_ENTITY && sceneProject && c.getSceneId() == sceneProject->id;
    if (usePlayer) c.stopPreview();
    ClipActionResult result;
    std::string failure;
    const uint64_t clock = AnimationClipController::clipCommitClock(project);
    if (!ClipSceneActions::extractSubClip(project, usePlayer ? sceneProject->id : 0, usePlayer ? c.getRoot() : NULL_ENTITY,
                                          doc->path(), path, usePlayer, result, failure)){
        if (error) *error = failure;
        return false;
    }
    // the file alone is a clip commit of the window: Ctrl+Z undoes it (9.R5c)
    if (result.projectStep) AnimationClipController::markClipCommit(clock);
    std::string openError;
    // the player's state now plays the file (it kept its name); else the file opens alone
    const bool opened = usePlayer ? c.openState(state, &openError) : c.openClip(result.clip, &openError);
    if (!opened && !c.openClip(result.clip, &openError)) setNotice(openError, true);
    selection.clear();
    selectEvent(-1);
    previewOverride.clear();
    setNotice("Extracted to " + FileUtils::pathToUtf8(FileUtils::pathFromUtf8(result.clip).filename()) +
              (result.projectStep ? " (one project undo step)" : " (one scene undo step)"), false);
    return true;
}

void AnimationWindowClip::askExtract(SceneProject* sceneProject){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    anim::SubClipReference reference;
    if (!anim::parseSubClipReference(doc->path(), reference)) return;
    const std::string file = ClipSceneActions::askNewClipPath(project, ClipSceneActions::subClipDirectory(project, doc->path()), reference.clip);
    if (file.empty()) return;
    std::string error;
    if (!extract(sceneProject, file, &error)) setNotice("Extract to .anim: " + error, true);
}

bool AnimationWindowClip::selectKeys(const std::vector<KeyId>& keys, bool additive){
    if (!additive) selection.clear();
    // whole keys: no per-curve point selection any more (the curves view's own clicks use
    // selectCurvePoints)
    if (!curveSelection.empty()){
        curveSelection.clear();
        curveSelectionVersion++;
    }
    selection.insert(selection.end(), keys.begin(), keys.end());
    // one selection (Unity): keys and the event don't stay selected together
    if (!keys.empty() || !additive) selectEvent(-1);
    refreshSelection();
    return true;
}

bool AnimationWindowClip::boxSelect(int firstRow, int lastRow, float fromTime, float toTime, bool additive){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return false;
    const std::vector<Row> rows = buildRows();
    if (firstRow > lastRow) std::swap(firstRow, lastRow);
    if (fromTime > toTime) std::swap(fromTime, toTime);
    std::vector<KeyId> found;
    const ClipData& data = doc->data();
    std::vector<char> done(data.tracks.size(), 0);
    for (int r = std::max(0, firstRow); r <= lastRow && r < (int)rows.size(); r++){
        const Row& row = rows[(size_t)r];
        for (size_t t = 0; t < data.tracks.size(); t++){
            if (row.kind != Row::Kind::Summary && row.track != (int)t) continue;
            if (done[t]) continue;
            done[t] = 1;
            const std::vector<ClipKey>& keys = data.tracks[t].keys;
            auto it = std::lower_bound(keys.begin(), keys.end(), fromTime - kKeyEpsilon,
                                       [](const ClipKey& k, float time){ return k.time < time; });
            for (; it != keys.end() && it->time <= toTime + kKeyEpsilon; ++it){
                found.push_back(keyIdOf(data.tracks[t], it->time));
            }
        }
    }
    return selectKeys(found, additive);
}

bool AnimationWindowClip::beginDrag(std::string* error){
    refreshSelection();
    if (selectedRefs.empty()){
        if (error) *error = "no keys selected";
        return false;
    }
    if (!controller().beginKeyDrag(selectedRefs, error)) return false;
    dragAppliedDelta = 0.0f;
    // the earliest selected key may not go below 0
    float earliest = kMaxTime;
    for (const KeyId& id : selection) earliest = std::min(earliest, id.time);
    dragMinTime = earliest;
    drag = Drag::Keys;
    return true;
}

bool AnimationWindowClip::updateDrag(float rawDelta, std::string* error){
    if (drag != Drag::Keys) return false;
    // frame snapped: the grabbed key lands on a frame
    float delta = snapToFrame(dragGrabTime + rawDelta) - dragGrabTime;
    delta = std::max(delta, -dragMinTime);
    if (std::fabs(delta - dragAppliedDelta) < 1e-6f) return true;
    std::vector<AnimationClipController::KeyRef> moved;
    if (!controller().updateKeyDrag(delta, nullptr, &moved, error)){
        controller().endKeyDrag();
        drag = Drag::None;
        return false;
    }
    dragAppliedDelta = delta;
    setSelectionFromRefs(moved);
    return true;
}

void AnimationWindowClip::endDrag(){
    if (drag == Drag::Keys || drag == Drag::CurveKeys){
        controller().endKeyDrag();
        // keys the drag left on one time collapsed into one
        const std::vector<ClipWarning>& warnings = controller().getLastWarnings();
        if (!warnings.empty()) setNotice(warnings.front().message, true);
    }
    drag = Drag::None;
    refreshSelection();
}

bool AnimationWindowClip::deleteSelected(std::string* error){
    refreshSelection();
    if (selectedRefs.empty()){
        if (error) *error = "no keys selected";
        return false;
    }
    if (!controller().deleteKeys(selectedRefs, error)) return false;
    selection.clear();
    refreshSelection();
    return true;
}

void AnimationWindowClip::copySelected(){
    refreshSelection();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || selectedRefs.empty()) return;
    const ClipData& data = doc->data();
    float earliest = kMaxTime;
    for (const AnimationClipController::KeyRef& ref : selectedRefs) earliest = std::min(earliest, data.tracks[ref.track].keys[ref.key].time);
    clipboard.clear();
    for (const AnimationClipController::KeyRef& ref : selectedRefs){
        const ClipTrackData& track = data.tracks[ref.track];
        CopiedKey copied;
        copied.path = track.path;
        copied.channel = track.channel;
        copied.component = track.component;
        copied.property = track.property;
        copied.kind = track.kind;
        copied.points = track.points;
        copied.key = track.keys[ref.key];
        copied.offset = copied.key.time - earliest;
        clipboard.push_back(copied);
    }
    setNotice("Copied " + std::to_string(clipboard.size()) + (clipboard.size() == 1 ? " key" : " keys"), false);
}

namespace {

    // the width a track's keys have (morph: its widest key)
    size_t trackWidth(const ClipTrackData& track){
        size_t width = 0;
        for (const ClipKey& key : track.keys) width = std::max(width, key.value.size());
        return width;
    }

    // `key` made to fit `track`: tangents added (zero) or dropped to match the track's keys;
    // false when the value width differs (another morph target count, another rotation layout)
    bool fitKey(const ClipTrackData& track, ClipKey& key){
        const size_t width = trackWidth(track);
        if (width && key.value.size() != width) return false;
        const bool tangents = !track.keys.empty() && track.keys.front().hasTangents();
        if (tangents && !key.hasTangents()){
            key.inTangent.assign(key.value.size(), 0.0f);
            key.outTangent.assign(key.value.size(), 0.0f);
        }else if (!tangents){
            key.inTangent.clear();
            key.outTangent.clear();
        }
        return true;
    }

}

bool AnimationWindowClip::paste(float time, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    if (clipboard.empty()){
        if (error) *error = "nothing copied";
        return false;
    }
    refreshSelection();
    ClipData data = doc->data();
    // keys copied from one property paste onto the one property whose keys are selected (Unity),
    // else each onto its own binding
    int target = -1;
    bool oneSource = true;
    for (const CopiedKey& copied : clipboard){
        oneSource = oneSource && copied.path == clipboard.front().path && copied.channel == clipboard.front().channel &&
                    copied.component == clipboard.front().component && copied.property == clipboard.front().property;
    }
    if (oneSource && !selectedRefs.empty()){
        bool oneTrack = true;
        for (const AnimationClipController::KeyRef& ref : selectedRefs) oneTrack = oneTrack && ref.track == selectedRefs.front().track;
        const ClipTrackData& selected = data.tracks[selectedRefs.front().track];
        // (a property's keys onto another GameObject's same property, or onto a property of their kind)
        const bool sameProperty = selected.channel != ClipChannel::Property ||
            (selected.kind == clipboard.front().kind && selected.width == clipboard.front().key.value.size());
        const bool otherTrack = selected.path != clipboard.front().path || selected.component != clipboard.front().component ||
                                selected.property != clipboard.front().property;
        if (oneTrack && selected.channel == clipboard.front().channel && otherTrack && sameProperty &&
            (selected.channel != ClipChannel::Lattice || selected.points == clipboard.front().points)){
            target = (int)selectedRefs.front().track;
        }
    }
    std::vector<KeyId> pasted;
    size_t skipped = 0, replaced = 0;
    for (const CopiedKey& copied : clipboard){
        const int index = target >= 0 ? target : copied.channel == ClipChannel::Property
            ? data.findPropertyTrack(copied.path, copied.component, copied.property) : data.findTrack(copied.path, copied.channel);
        if (index < 0 || (copied.channel == ClipChannel::Lattice && data.tracks[(size_t)index].points != copied.points)){
            skipped++;
            continue;
        }
        ClipTrackData& track = data.tracks[(size_t)index];
        ClipKey key = copied.key;
        if (!fitKey(track, key)){
            skipped++;
            continue;
        }
        const float at = std::clamp(snapToFrame(time + copied.offset), 0.0f, kMaxTime);
        bool wasThere = false;
        upsertKey(track, at, key, &wasThere);
        if (wasThere) replaced++;
        pasted.push_back(keyIdOf(track, at));
    }
    if (pasted.empty()){
        if (error) *error = "no matching property in this clip for the copied keys";
        return false;
    }
    if (!controller().commit(data, "Paste Keys", "", error)) return false;
    selection = pasted;
    selectEvent(-1);
    refreshSelection();
    std::string text = "Pasted " + std::to_string(pasted.size()) + (pasted.size() == 1 ? " key" : " keys");
    if (replaced) text += ", replacing " + std::to_string(replaced);
    if (skipped) text += "; " + std::to_string(skipped) + " skipped (no matching property, or another value size)";
    setNotice(text, skipped > 0);
    return true;
}

bool AnimationWindowClip::duplicateSelected(std::string* error){
    refreshSelection();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || selectedRefs.empty()){
        if (error) *error = "no keys selected";
        return false;
    }
    ClipData data = doc->data();
    float earliest = kMaxTime, latest = 0.0f;
    for (const AnimationClipController::KeyRef& ref : selectedRefs){
        const float t = data.tracks[ref.track].keys[ref.key].time;
        earliest = std::min(earliest, t);
        latest = std::max(latest, t);
    }
    const float offset = (latest - earliest) + 1.0f / frameRate();
    std::vector<std::pair<size_t, ClipKey>> copies;
    for (const AnimationClipController::KeyRef& ref : selectedRefs){
        copies.push_back({ref.track, data.tracks[ref.track].keys[ref.key]});
    }
    std::vector<KeyId> made;
    size_t replaced = 0;
    for (const auto& copy : copies){
        ClipTrackData& track = data.tracks[copy.first];
        const float at = std::min(snapToFrame(copy.second.time + offset), kMaxTime);
        bool wasThere = false;
        upsertKey(track, at, copy.second, &wasThere);
        if (wasThere) replaced++;
        made.push_back(keyIdOf(track, at));
    }
    if (!controller().commit(data, "Duplicate Keys", "", error)) return false;
    selection = made;
    refreshSelection();
    if (replaced) setNotice("Duplicated " + std::to_string(made.size()) + " keys, replacing " + std::to_string(replaced), true);
    return true;
}

bool AnimationWindowClip::setEase(EaseType ease, std::string* error){
    refreshSelection();
    if (selectedRefs.empty()){
        if (error) *error = "no keys selected";
        return false;
    }
    if (!controller().setEase(selectedRefs, ease, error)) return false;
    refreshSelection();
    return true;
}

bool AnimationWindowClip::setTangentMode(ClipTangentMode mode, std::string* error){
    refreshSelection();
    if (selectedRefs.empty()){
        if (error) *error = "no keys selected";
        return false;
    }
    if (!controller().setTangentMode(selectedRefs, mode, error)) return false;
    refreshSelection();
    return true;
}

bool AnimationWindowClip::addKeysAt(int trackIndex, float time, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    // keys land on the Samples grid (I3)
    time = std::clamp(snapToFrame(time), 0.0f, kMaxTime);
    ClipData data = doc->data();
    std::vector<KeyId> made;
    for (size_t t = 0; t < data.tracks.size(); t++){
        if (trackIndex >= 0 && (int)t != trackIndex) continue;
        ClipTrackData& track = data.tracks[t];
        if ((track.channel == ClipChannel::Property && !track.known) || track.keys.empty()) continue;
        // the curve's own value there: adding a key never changes the shape
        std::vector<float> value = sampledValue(t, time);
        if (value.empty()) continue;
        ClipKey key;
        // the segment the key splits keeps its ease on both halves
        for (const ClipKey& existing : track.keys){
            if (existing.time <= time) key.ease = existing.ease;
        }
        key.value = value;
        if (track.keys.front().hasTangents()){
            key.inTangent.assign(value.size(), 0.0f);
            key.outTangent.assign(value.size(), 0.0f);
        }
        upsertKey(track, time, key);
        made.push_back(keyIdOf(track, time));
    }
    if (made.empty()){
        if (error) *error = "no track to key";
        return false;
    }
    if (!controller().commit(data, "Add Key", "", error)) return false;
    selection = made;
    selectEvent(-1);
    refreshSelection();
    return true;
}

bool AnimationWindowClip::keyAt(bool modifiedOnly, std::string* error){
    AnimationClipController& c = controller();
    // K / Shift+K / Add Key key at the playhead, on the Samples grid (I3)
    const float time = snapToFrame(c.getTime());
    if (std::fabs(time - c.getTime()) > 1e-6f) c.setTime(time);
    const bool ok = modifiedOnly ? c.keyModified(time, error) : c.keyAll(time, error);
    return ok;
}

bool AnimationWindowClip::snapKeysToFrames(std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    const float fps = frameRate();
    ClipData data = doc->data();
    size_t moved = 0, merged = 0;
    for (ClipTrackData& track : data.tracks){
        std::vector<ClipKey> snapped;
        float keptDistance = 0.0f;
        for (const ClipKey& key : track.keys){
            ClipKey k = key;
            k.time = std::round(key.time * fps) / fps;
            const float distance = std::fabs(k.time - key.time);
            if (distance > 1e-6f) moved++;
            // two keys on one frame (closer than half a frame before): the one nearer the frame
            // stays (the later one on a tie)
            if (!snapped.empty() && std::fabs(snapped.back().time - k.time) <= kKeyEpsilon){
                merged++;
                if (distance <= keptDistance){
                    snapped.back() = k;
                    keptDistance = distance;
                }
                continue;
            }
            snapped.push_back(k);
            keptDistance = distance;
        }
        track.keys = snapped;
    }
    if (moved == 0){
        setNotice("Every key is on a frame already", false);
        return true;
    }
    if (!controller().commit(data, "Snap Keys to Frames", "", error)) return false;
    refreshSelection();
    std::string text = "Snapped " + std::to_string(moved) + (moved == 1 ? " key" : " keys") + " to frames";
    if (merged){
        text += "; " + std::to_string(merged) + (merged == 1 ? " key was" : " keys were") +
                " merged (closer than half a frame to another key)";
    }
    setNotice(text, merged > 0);
    return true;
}

bool AnimationWindowClip::setFrameRate(float rate, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    if (!std::isfinite(rate) || rate < 1.0f || rate > 240.0f){
        if (error) *error = "Samples (frame rate) is 1 to 240";
        return false;
    }
    ClipData data = doc->data();
    data.frameRate = rate;
    if (!controller().commit(data, "Set Samples", "", error)) return false;
    // keys between the new frames: say how to put them on the grid (R54)
    size_t off = 0;
    for (const ClipTrackData& track : data.tracks){
        for (const ClipKey& key : track.keys){
            if (std::fabs(key.time * rate - std::round(key.time * rate)) > 1e-3f) off++;
        }
    }
    if (off) setNotice(std::to_string(off) + (off == 1 ? " key is" : " keys are") + " between frames: Snap keys to frames puts them on the grid", true);
    return true;
}

bool AnimationWindowClip::setPreviewModel(const std::string& model, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    std::string value = model;
    if (!value.empty()){
        if (!anim::isModelFilePath(value) && editor::FileUtils::pathFromUtf8(value).extension() != ".prefab"){
            if (error) *error = "Preview Model takes a model (.gltf, .glb, .fbx, .obj) or a prefab";
            return false;
        }
        // asset-relative like ModelComponent::filename
        const fs::path resolved = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(value));
        const fs::path relative = project->normalizeToAssetsRelative(resolved);
        if (!relative.empty() && relative.generic_string().compare(0, 2, "..") != 0) value = FileUtils::pathToGenericUtf8(relative);
    }
    if (doc->isReadOnly()){
        // a model's animation can't be written: the override lasts for this session
        previewOverride = value;
        return true;
    }
    previewOverride.clear();
    ClipData data = doc->data();
    data.previewModel = value;
    if (!controller().commit(data, "Set Preview Model", "", error)) return false;
    return true;
}

bool AnimationWindowClip::setComponentValue(size_t trackIndex, int component, float value, bool newEdit, std::string* error){
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || trackIndex >= doc->data().tracks.size()){
        if (error) *error = "no such track";
        return false;
    }
    const ClipTrackData& track = doc->data().tracks[trackIndex];
    if (track.channel == ClipChannel::Property && component >= 0){
        // a component of a property (a colour's r, a vector's y): the whole value with it changed
        std::vector<float> current;
        Scene* scene = valueScene();
        const Entity entity = scene ? anim::resolvePath(scene, valueRoot(), track.path) : NULL_ENTITY;
        anim::PropertyRef ref;
        anim::parsePropertyRef(track.component, track.property, ref);
        const anim::PropertyBinding binding = entity != NULL_ENTITY ? anim::bindProperty(scene, entity, ref, track.width) : anim::PropertyBinding();
        float whole[4] = {0, 0, 0, 0};
        if (binding.status == anim::PropertyBindStatus::Bound && anim::readPropertyKeySpace(scene, entity, binding, whole)){
            current.assign(whole, whole + binding.width);
        }else{
            current = sampledValue(trackIndex, c.getTime());
        }
        if ((size_t)component >= current.size()){
            if (error) *error = "no such component";
            return false;
        }
        current[(size_t)component] = value;
        return setPropertyValue(trackIndex, current, newEdit, error);
    }
    if (track.channel == ClipChannel::Lattice || track.channel == ClipChannel::Property || component < 0){
        if (error) *error = "this row's values are edited in the Scene view";
        return false;
    }
    if (isHumanoidChannel(track.channel)){
        // a humanoid track has no GameObject of its own: the field keys its value at the playhead
        // (slots in joint angles, converted to E; anim-retarget 9)
        std::vector<float> current = sampledValue(trackIndex, c.getTime());
        if (current.size() < (isRotationChannel(track.channel) ? 4u : 3u)){
            if (error) *error = "the track has no value at the playhead";
            return false;
        }
        if (isRotationChannel(track.channel)){
            Vector3 angles = (eulerEditTrack == (int)trackIndex) ? eulerEdit : anglesOf(track, current);
            if (component == 0) angles.x = value;
            if (component == 1) angles.y = value;
            if (component == 2) angles.z = value;
            eulerEdit = angles;
            current = xyzwOfAngles(track, angles);
        }else if (component < 3){
            current[(size_t)component] = value;
        }
        if (newEdit || valueMergeKey.empty()) valueMergeKey = "clipvalue#" + std::to_string(++valueEditSerial);
        ClipData data = doc->data();
        ClipKey key;
        key.value = current;
        upsertKey(data.tracks[trackIndex], snapToFrame(c.getTime()), key);
        return c.commit(data, "Set Key Value", valueMergeKey, error);
    }
    if (!c.isPreviewing() && !startPreview(project->getSelectedScene(), error)) return false;
    // what the Scene view shows now, with this component changed
    Scene* scene = valueScene();
    const Entity entity = scene ? anim::resolvePath(scene, valueRoot(), track.path) : NULL_ENTITY;
    std::vector<float> current;
    if (entity == NULL_ENTITY || !anim::readChannelValue(scene, entity, track.channel, track.points, current)){
        if (error) *error = "the track's GameObject is missing";
        return false;
    }
    if (track.channel == ClipChannel::Rotation){
        // the triple being dragged stays as typed (no re-decomposition flips through +-90, M13)
        Vector3 euler = (eulerEditTrack == (int)trackIndex) ? eulerEdit : eulerOf(current);
        if (component == 0) euler.x = value;
        if (component == 1) euler.y = value;
        if (component == 2) euler.z = value;
        eulerEdit = euler;
        current = xyzwOf(euler);
    }else{
        if ((size_t)component >= current.size()) current.resize((size_t)component + 1, 0.0f);
        current[(size_t)component] = value;
    }
    // Unity's pink field: shown, not keyed, until K (or the playhead moves)
    if (!c.isRecording() || track.relative) return c.setPendingValue(trackIndex, current, error);
    // Record (T3c): the key at the playhead takes it (one merged step per field drag)
    if (newEdit || valueMergeKey.empty()) valueMergeKey = "clipvalue#" + std::to_string(++valueEditSerial);
    const float time = snapToFrame(c.getTime());
    AnimationClipController::KeyRef ref;
    if (AnimationClipController::findKey(doc->data(), track, time, ref)){
        if (!c.setKeyValue(ref, current, valueMergeKey, error)) return false;
    }else{
        ClipData data = doc->data();
        ClipKey key;
        key.value = current;
        upsertKey(data.tracks[trackIndex], time, key);
        if (!c.commit(data, "Set Key Value", valueMergeKey, error)) return false;
    }
    return true;
}

bool AnimationWindowClip::setPropertyValue(size_t trackIndex, const std::vector<float>& value, bool newEdit, std::string* error){
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || trackIndex >= doc->data().tracks.size() || doc->data().tracks[trackIndex].channel != ClipChannel::Property){
        if (error) *error = "no such property track";
        return false;
    }
    const ClipTrackData& track = doc->data().tracks[trackIndex];
    if (!c.isPreviewing() && !startPreview(project->getSelectedScene(), error)) return false;
    // the track's own layout: a swizzle track holds its component of the whole value
    std::vector<float> keyValue = value;
    if (track.swizzle >= 0 && value.size() > 1){
        if ((size_t)track.swizzle >= value.size()){
            if (error) *error = "no such component";
            return false;
        }
        keyValue = {value[(size_t)track.swizzle]};
    }
    if (track.discrete){
        for (float& v : keyValue) v = std::round(v);
    }
    // Unity's pink field: shown, not keyed, until K (or the playhead moves)
    if (!c.isRecording()) return c.setPendingValue(trackIndex, keyValue, error);
    if (newEdit || valueMergeKey.empty()) valueMergeKey = "clipvalue#" + std::to_string(++valueEditSerial);
    const float time = snapToFrame(c.getTime());
    AnimationClipController::KeyRef ref;
    if (AnimationClipController::findKey(doc->data(), track, time, ref)){
        return c.setKeyValue(ref, keyValue, valueMergeKey, error);
    }
    ClipData data = doc->data();
    ClipKey key;
    key.value = keyValue;
    upsertKey(data.tracks[trackIndex], time, key);
    return c.commit(data, "Set Key Value", valueMergeKey, error);
}

void AnimationWindowClip::frameKeys(bool selectedOnly, float width){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || width <= 1.0f) return;
    refreshSelection();
    float from = kMaxTime, to = -1.0f;
    const ClipData& data = doc->data();
    if (selectedOnly && !selectedRefs.empty()){
        for (const AnimationClipController::KeyRef& ref : selectedRefs){
            const float t = data.tracks[ref.track].keys[ref.key].time;
            from = std::min(from, t);
            to = std::max(to, t);
        }
    }else{
        for (const ClipTrackData& track : data.tracks){
            if (track.keys.empty()) continue;
            from = std::min(from, track.keys.front().time);
            to = std::max(to, track.keys.back().time);
        }
        for (const ClipEventData& event : data.events){
            from = std::min(from, event.time);
            to = std::max(to, event.time);
        }
        from = std::min(from, 0.0f);
        to = std::max(to, data.effectiveLength());
    }
    if (to < from) return;
    if (to - from < 2.0f / frameRate()){
        from -= 0.5f;
        to += 0.5f;
    }
    pixelsPerSecond = std::clamp(width * 0.9f / (to - from), 10.0f, 4000.0f);
    scrollX = std::max(0.0f, from * pixelsPerSecond - width * 0.05f);
}
