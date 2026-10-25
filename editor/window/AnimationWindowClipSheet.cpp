// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's clip mode (animation clips spec 4.4, 9.R31/R39/R54): toolbar, clip
// dropdown, empty state, read-only and target-less banners, binding rows with the value column,
// keys (select, box, drag, delete, copy/paste/duplicate, context menu), Add Property, Rebind and
// the keyboard shortcuts. Edits go through the AnimationClipController (one project step each).

#include "AnimationWindowClip.h"

#include "imgui_internal.h"
#include "external/IconsFontAwesome6.h"
#include "Engine.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "animation/KeyframeSampling.h"
#include "animation/ModelSubClips.h"
#include "command/type/ClipSceneActions.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/LatticeComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "util/EntityPath.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/Util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    constexpr float kKeyEpsilon = 1e-4f;
    constexpr float kMaxTime = 3600.0f;

    struct SheetMetrics{
        float s;
        float rulerHeight;
        float eventsHeight;
        float rowHeight;
        float labelWidth;
        float valueWidth;
        float indent;
        float keyRadius;
        float keyHit;
        float timePad;      // time 0 sits this far right of the key area edge
        float playheadHalf;
    };

    SheetMetrics sheetMetrics(){
        const float s = Theme::dpiScale();
        SheetMetrics m;
        m.s = s;
        m.rulerHeight = 22.0f * s;
        m.eventsHeight = 18.0f * s;
        m.rowHeight = ImGui::GetFrameHeight() + 2.0f * s;
        m.labelWidth = 320.0f * s;
        m.valueWidth = 104.0f * s;
        m.indent = 14.0f * s;
        m.keyRadius = 6.0f * s;
        m.timePad = 12.0f * s;
        m.keyHit = 7.5f * s;
        m.playheadHalf = 5.0f * s;
        return m;
    }

    ImU32 color(const ImVec4& c, float alpha = -1.0f){
        return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, alpha < 0.0f ? c.w : alpha));
    }

    const char* channelLabel(ClipChannel channel){
        switch (channel){
            case ClipChannel::Position: return "Position";
            case ClipChannel::Rotation: return "Rotation";
            case ClipChannel::Scale: return "Scale";
            case ClipChannel::Morph: return "Blend Shapes";
            case ClipChannel::Lattice: return "Lattice";
            case ClipChannel::Property: return "Property";
        }
        return "";
    }

    std::string rowKey(const ClipTrackData& track){
        return track.path + "|" + anim::channelName(track.channel);
    }

    std::string lastSegment(const std::string& path){
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // Euler degrees (the Inspector's ZYX order, the clip's {"euler"} convention) of an x y z w key
    Vector3 eulerOf(const std::vector<float>& xyzw){
        if (xyzw.size() < 4) return Vector3(0, 0, 0);
        Vector3 e = quaternionFromXYZW(xyzw.data()).normalize().getEulerAngles(RotationOrder::ZYX);
        if (!Engine::isUseDegrees()){
            const float k = 180.0f / 3.14159265358979323846f;
            e = Vector3(e.x * k, e.y * k, e.z * k);
        }
        auto tidy = [](float v){ return std::fabs(v) < 1e-4f ? 0.0f : v; };
        return Vector3(tidy(e.x), tidy(e.y), tidy(e.z));
    }

    std::vector<float> xyzwOf(const Vector3& eulerDegrees){
        std::vector<float> out(4);
        quaternionToXYZW(anim::eulerDegreesToQuaternion(eulerDegrees.x, eulerDegrees.y, eulerDegrees.z), out.data());
        return out;
    }

    std::vector<float> flatten(const anim::TrackSample& sample){
        switch (sample.channel){
            case ClipChannel::Position:
            case ClipChannel::Scale:
                return {sample.vector.x, sample.vector.y, sample.vector.z};
            case ClipChannel::Rotation: {
                std::vector<float> out(4);
                quaternionToXYZW(sample.rotation, out.data());
                return out;
            }
            case ClipChannel::Morph:
                return sample.weights;
            case ClipChannel::Lattice: {
                std::vector<float> out;
                for (const Vector3& v : sample.offsets) out.insert(out.end(), {v.x, v.y, v.z});
                return out;
            }
            case ClipChannel::Property:
                break;
        }
        return {};
    }

    // a key at `time` on `track` (a copy of `key`'s value), replacing one within the epsilon;
    // keys stay sorted. Returns the index.
    size_t upsertKey(ClipTrackData& track, float time, const ClipKey& key){
        for (size_t i = 0; i < track.keys.size(); i++){
            if (std::fabs(track.keys[i].time - time) <= kKeyEpsilon){
                track.keys[i] = key;
                track.keys[i].time = time;
                return i;
            }
        }
        ClipKey inserted = key;
        inserted.time = time;
        auto it = std::upper_bound(track.keys.begin(), track.keys.end(), time,
                                   [](float t, const ClipKey& k){ return t < k.time; });
        const size_t index = (size_t)(it - track.keys.begin());
        track.keys.insert(it, inserted);
        return index;
    }

    std::string formatTime(float time, float fps, bool frames){
        char text[48];
        if (frames){
            std::snprintf(text, sizeof(text), "%d", (int)std::lround(time * fps));
        }else{
            std::snprintf(text, sizeof(text), "%.2f", time);
        }
        return text;
    }

}

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
        selectedEvent = -1;
        drag = Drag::None;
    }
}

bool AnimationWindowClip::open(uint32_t sceneId, Entity root, const std::string& clip, std::string* error){
    AnimationClipController& c = controller();
    if (root == NULL_ENTITY){
        c.bindRoot(0, NULL_ENTITY, nullptr);
    }else if (!c.bindRoot(sceneId, root, error)){
        return false;
    }
    selection.clear();
    selectedEvent = -1;
    drag = Drag::None;
    if (!c.openClip(clip, error)) return false;
    scrollX = 0.0f;
    return true;
}

bool AnimationWindowClip::openState(const std::string& state, std::string* error){
    selection.clear();
    selectedEvent = -1;
    return controller().openState(state, error);
}

bool AnimationWindowClip::hasTargetlessClip() const{
    if (!controllerUsed) return false;
    return document() && controller().getRoot() == NULL_ENTITY;
}

void AnimationWindowClip::update(double dt, bool clipModeShown, bool focused){
    if (!notice.empty() && ImGui::GetTime() - noticeAt > 6.0) notice.clear();
    if (!controllerUsed) return;
    AnimationClipController& c = controller();
    c.update(dt);
    c.setTakesKeys(clipModeShown && focused);
}

void AnimationWindowClip::stopPreview(){
    if (!controllerUsed) return;
    if (drag == Drag::Keys) endDrag();
    drag = Drag::None;
    controller().stopPreview();
}

void AnimationWindowClip::sceneClosed(uint32_t sceneId){
    if (controllerUsed && controller().getSceneId() == sceneId){
        selection.clear();
        selectedEvent = -1;
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

// ---- binding report, rows, values ----

void AnimationWindowClip::refreshReport(){
    std::shared_ptr<AnimationClipDocument> doc = document();
    const uint64_t revision = doc ? doc->revision() : 0;
    const Entity root = valueRoot();
    Scene* scene = valueScene();
    const uint64_t hierarchy = scene ? scene->hierarchyRevision() : 0;
    const std::string clip = doc ? doc->path() : std::string();
    const double now = ImGui::GetTime();
    // components added or removed don't bump the hierarchy revision: a slow refresh covers them
    if (revision == reportRevision && root == reportRoot && hierarchy == reportHierarchy && clip == reportClip &&
        now - reportAt < 0.5){
        return;
    }
    report = doc ? controller().getBindings() : ClipValidationReport();
    reportRevision = revision;
    reportRoot = root;
    reportHierarchy = hierarchy;
    reportClip = clip;
    reportAt = now;
}

void AnimationWindowClip::refreshSelection(){
    selectedRefs.clear();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        selection.clear();
        return;
    }
    std::vector<KeyId> kept;
    for (const KeyId& id : selection){
        AnimationClipController::KeyRef ref;
        if (!AnimationClipController::findKey(doc->data(), id.path, id.channel, id.time, ref)) continue;
        bool duplicate = false;
        for (const AnimationClipController::KeyRef& other : selectedRefs){
            duplicate = duplicate || (other.track == ref.track && other.key == ref.key);
        }
        if (duplicate) continue;
        selectedRefs.push_back(ref);
        kept.push_back(id);
    }
    selection = kept;
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
        selection.push_back({track.path, track.channel, track.keys[ref.key].time});
    }
    refreshSelection();
}

bool AnimationWindowClip::isSelected(size_t track, size_t key) const{
    for (const AnimationClipController::KeyRef& ref : selectedRefs){
        if (ref.track == track && ref.key == key) return true;
    }
    return false;
}

std::vector<std::string> AnimationWindowClip::componentLabels(size_t trackIndex) const{
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || trackIndex >= doc->data().tracks.size()) return {};
    const ClipTrackData& track = doc->data().tracks[trackIndex];
    switch (track.channel){
        case ClipChannel::Position:
        case ClipChannel::Scale:
        case ClipChannel::Rotation:
            return {"x", "y", "z"};
        case ClipChannel::Morph: {
            size_t width = 0;
            for (const ClipKey& key : track.keys) width = std::max(width, key.value.size());
            // morph target names of the bound mesh's model (name -> index)
            std::map<int, std::string> names;
            Scene* scene = valueScene();
            Entity entity = NULL_ENTITY;
            for (const TrackReport& entry : report.tracks){
                if (entry.track == trackIndex) entity = entry.entity;
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
            break;
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
    const std::string rootName = scene && root != NULL_ENTITY ? scene->getEntityName(root) : std::string("(Root)");
    for (size_t t = 0; t < data.tracks.size(); t++){
        const ClipTrackData& track = data.tracks[t];
        Row row;
        row.kind = Row::Kind::Track;
        row.track = (int)t;
        row.label = (track.path.empty() ? rootName : track.path) + " : " + channelLabel(track.channel);
        for (const TrackReport& entry : report.tracks){
            if (entry.track != t) continue;
            const bool bad = entry.status == TrackBindingStatus::MissingPath ||
                             entry.status == TrackBindingStatus::MissingComponent ||
                             entry.status == TrackBindingStatus::LatticeIndexOutOfRange ||
                             entry.status == TrackBindingStatus::MorphIndexOutOfRange ||
                             entry.status == TrackBindingStatus::LegacyModelLayout ||
                             entry.status == TrackBindingStatus::UnaddressableName;
            row.missing = bad;
            row.message = entry.message;
            if (!entry.nearest.empty() && entry.status == TrackBindingStatus::MissingPath){
                row.message += "\nDid you mean: ";
                for (size_t i = 0; i < entry.nearest.size(); i++) row.message += (i ? ", " : "") + entry.nearest[i];
            }
        }
        if (row.missing) row.label += " (Missing!)";
        rows.push_back(row);
        if (track.channel != ClipChannel::Property && expanded.count(rowKey(track))){
            const std::vector<std::string> labels = componentLabels(t);
            for (size_t c = 0; c < labels.size(); c++){
                Row component;
                component.kind = Row::Kind::Component;
                component.track = (int)t;
                component.component = track.channel == ClipChannel::Lattice ? -1 : (int)c;
                component.label = labels[c];
                component.missing = row.missing;
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
    const int index = clip->findTrack(data.path, data.channel);
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

void AnimationWindowClip::togglePreview(){
    AnimationClipController& c = controller();
    if (c.isPreviewing()){
        stopPreview();
        return;
    }
    std::string error;
    if (!c.startPreview(&error)) setNotice(error, true);
}

void AnimationWindowClip::togglePlay(){
    AnimationClipController& c = controller();
    if (c.isPlaying()){
        c.pause();
        return;
    }
    std::string error;
    if (!c.isPreviewing() && !c.startPreview(&error)){
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
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    std::vector<float> times = {0.0f, doc->data().effectiveLength()};
    for (const ClipTrackData& track : doc->data().tracks){
        for (const ClipKey& key : track.keys) times.push_back(key.time);
    }
    std::sort(times.begin(), times.end());
    const float now = controller().getTime();
    if (direction < 0){
        for (auto it = times.rbegin(); it != times.rend(); ++it){
            if (*it < now - kKeyEpsilon){
                seek(*it);
                return;
            }
        }
    }else{
        for (float t : times){
            if (t > now + kKeyEpsilon){
                seek(t);
                return;
            }
        }
    }
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
    c.setLastActionWasClipEdit(false);
    std::string openError;
    if (!c.openClip(result.clip, &openError)) setNotice(openError, true);
    selection.clear();
    selectedEvent = -1;
    setNotice(result.status, false);
    return true;
}

void AnimationWindowClip::askCreateNewClip(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    if (!sceneProject || c.getRoot() == NULL_ENTITY) return;
    const std::string directory = ClipConverters::defaultClipDirectory(project, sceneProject->id, c.getRoot());
    const fs::path start = project->getProjectPath() / fs::u8path(directory);
    const std::string name = sceneProject->scene->getEntityName(c.getRoot());
    std::string chosen = FileDialogs::saveFileDialog(FileUtils::pathToGenericUtf8(start), (name.empty() ? "New Animation" : name) + ".anim",
                                                     FILE_DIALOG_ANIMATION_CLIP);
    if (chosen.empty()) return;
    fs::path file = fs::u8path(chosen);
    if (file.extension() != ".anim") file += ".anim";
    std::string error;
    if (!createNewClip(sceneProject, FileUtils::pathToGenericUtf8(file), &error)) setNotice("Create New Clip: " + error, true);
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
    if (!ClipSceneActions::extractSubClip(project, usePlayer ? sceneProject->id : 0, usePlayer ? c.getRoot() : NULL_ENTITY,
                                          doc->path(), path, usePlayer, result, failure)){
        if (error) *error = failure;
        return false;
    }
    c.setLastActionWasClipEdit(false);
    std::string openError;
    // the player's state now plays the file (it kept its name); else the file opens alone
    const bool opened = usePlayer ? c.openState(state, &openError) : c.openClip(result.clip, &openError);
    if (!opened && !c.openClip(result.clip, &openError)) setNotice(openError, true);
    selection.clear();
    setNotice(result.status, false);
    return true;
}

void AnimationWindowClip::askExtract(SceneProject* sceneProject){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    anim::SubClipReference reference;
    if (!anim::parseSubClipReference(doc->path(), reference)) return;
    const fs::path model = project->getProjectPath() / fs::u8path(reference.model);
    std::string chosen = FileDialogs::saveFileDialog(FileUtils::pathToGenericUtf8(model.parent_path()), reference.clip + ".anim",
                                                     FILE_DIALOG_ANIMATION_CLIP);
    if (chosen.empty()) return;
    fs::path file = fs::u8path(chosen);
    if (file.extension() != ".anim") file += ".anim";
    std::string error;
    if (!extract(sceneProject, FileUtils::pathToGenericUtf8(file), &error)) setNotice("Extract to .anim: " + error, true);
}

bool AnimationWindowClip::selectKeys(const std::vector<KeyId>& keys, bool additive){
    if (!additive) selection.clear();
    for (const KeyId& id : keys) selection.push_back(id);
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
    for (int r = std::max(0, firstRow); r <= lastRow && r < (int)rows.size(); r++){
        const Row& row = rows[(size_t)r];
        for (size_t t = 0; t < data.tracks.size(); t++){
            if (row.kind != Row::Kind::Summary && row.track != (int)t) continue;
            for (const ClipKey& key : data.tracks[t].keys){
                if (key.time >= fromTime - kKeyEpsilon && key.time <= toTime + kKeyEpsilon){
                    found.push_back({data.tracks[t].path, data.tracks[t].channel, key.time});
                }
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
    if (drag == Drag::Keys){
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
    controller().setLastActionWasClipEdit(true);
    selection.clear();
    selectedRefs.clear();
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
        copied.points = track.points;
        copied.key = track.keys[ref.key];
        copied.offset = copied.key.time - earliest;
        clipboard.push_back(copied);
    }
    setNotice("Copied " + std::to_string(clipboard.size()) + (clipboard.size() == 1 ? " key" : " keys"), false);
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
    ClipData data = doc->data();
    std::vector<KeyId> pasted;
    size_t skipped = 0;
    for (const CopiedKey& copied : clipboard){
        const int index = data.findTrack(copied.path, copied.channel);
        if (index < 0 || (copied.channel == ClipChannel::Lattice && data.tracks[(size_t)index].points != copied.points)){
            skipped++;
            continue;
        }
        const float at = std::max(0.0f, time + copied.offset);
        upsertKey(data.tracks[(size_t)index], at, copied.key);
        pasted.push_back({copied.path, copied.channel, at});
    }
    if (pasted.empty()){
        if (error) *error = "no matching bindings in this clip for the copied keys";
        return false;
    }
    if (!controller().commit(data, "Paste Keys", "", error)) return false;
    controller().setLastActionWasClipEdit(true);
    selection = pasted;
    refreshSelection();
    if (skipped) setNotice(std::to_string(skipped) + " copied keys skipped: no matching binding in this clip", true);
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
    for (const auto& copy : copies){
        ClipTrackData& track = data.tracks[copy.first];
        const float at = snapToFrame(copy.second.time + offset);
        upsertKey(track, at, copy.second);
        made.push_back({track.path, track.channel, at});
    }
    if (!controller().commit(data, "Duplicate Keys", "", error)) return false;
    controller().setLastActionWasClipEdit(true);
    selection = made;
    refreshSelection();
    return true;
}

bool AnimationWindowClip::setEase(EaseType ease, std::string* error){
    refreshSelection();
    if (selectedRefs.empty()){
        if (error) *error = "no keys selected";
        return false;
    }
    if (!controller().setEase(selectedRefs, ease, error)) return false;
    controller().setLastActionWasClipEdit(true);
    refreshSelection();
    return true;
}

bool AnimationWindowClip::addKeysAt(int trackIndex, float time, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    ClipData data = doc->data();
    std::vector<KeyId> made;
    for (size_t t = 0; t < data.tracks.size(); t++){
        if (trackIndex >= 0 && (int)t != trackIndex) continue;
        ClipTrackData& track = data.tracks[t];
        if (track.channel == ClipChannel::Property || track.keys.empty()) continue;
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
        made.push_back({track.path, track.channel, time});
    }
    if (made.empty()){
        if (error) *error = "no track to key";
        return false;
    }
    if (!controller().commit(data, "Add Key", "", error)) return false;
    controller().setLastActionWasClipEdit(true);
    selection = made;
    refreshSelection();
    return true;
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
    controller().setLastActionWasClipEdit(true);
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
    controller().setLastActionWasClipEdit(true);
    return true;
}

bool AnimationWindowClip::setPreviewModel(const std::string& model, std::string* error){
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc){
        if (error) *error = "no clip is open";
        return false;
    }
    ClipData data = doc->data();
    std::string value = model;
    if (!value.empty()){
        // asset-relative like ModelComponent::filename
        const fs::path resolved = project->resolveAssetPath(fs::u8path(value));
        const fs::path relative = project->normalizeToAssetsRelative(resolved);
        if (!relative.empty() && relative.generic_string().compare(0, 2, "..") != 0) value = FileUtils::pathToGenericUtf8(relative);
    }
    data.previewModel = value;
    if (!controller().commit(data, "Set Preview Model", "", error)) return false;
    controller().setLastActionWasClipEdit(true);
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
    if (track.channel == ClipChannel::Lattice || track.channel == ClipChannel::Property || component < 0){
        if (error) *error = "this row's values are edited in the Scene view";
        return false;
    }
    if (!c.isPreviewing() && !c.startPreview(error)) return false;
    // what the Scene view shows now, with this component changed
    Scene* scene = valueScene();
    const Entity entity = scene ? anim::resolvePath(scene, valueRoot(), track.path) : NULL_ENTITY;
    std::vector<float> current;
    if (entity == NULL_ENTITY || !anim::readChannelValue(scene, entity, track.channel, track.points, current)){
        if (error) *error = "the track's GameObject is missing";
        return false;
    }
    if (track.channel == ClipChannel::Rotation){
        Vector3 euler = eulerOf(current);
        if (component == 0) euler.x = value;
        if (component == 1) euler.y = value;
        if (component == 2) euler.z = value;
        current = xyzwOf(euler);
    }else{
        if ((size_t)component >= current.size()) current.resize((size_t)component + 1, 0.0f);
        current[(size_t)component] = value;
    }
    // Unity's pink field: shown, not keyed, until K (or the playhead moves)
    if (!c.isRecording() || track.relative) return c.setPendingValue(trackIndex, current, error);
    // Record (T3c): the key at the playhead takes it (one merged step per field drag)
    if (newEdit || valueMergeKey.empty()) valueMergeKey = "clipvalue#" + std::to_string(++valueEditSerial);
    AnimationClipController::KeyRef ref;
    if (AnimationClipController::findKey(doc->data(), track.path, track.channel, c.getTime(), ref)){
        if (!c.setKeyValue(ref, current, valueMergeKey, error)) return false;
    }else{
        ClipData data = doc->data();
        ClipKey key;
        key.value = current;
        upsertKey(data.tracks[trackIndex], c.getTime(), key);
        if (!c.commit(data, "Set Key Value", valueMergeKey, error)) return false;
    }
    c.setLastActionWasClipEdit(true);
    return true;
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
            for (const ClipKey& key : track.keys){
                from = std::min(from, key.time);
                to = std::max(to, key.time);
            }
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

// ---- drawing ----

void AnimationWindowClip::draw(SceneProject* sceneProject, bool focused){
    ImGui::PushID("clip_mode");
    // the window handles its keys while focused (App: AnimationClipAppHooks::takesKeys)
    controller().setTakesKeys(focused);
    refreshReport();
    refreshSelection();
    drawToolbar(sceneProject);
    if (!document()){
        drawEmptyState(sceneProject);
        ImGui::PopID();
        return;
    }
    drawBanners(sceneProject);
    drawSheet(sceneProject, focused);
    drawEventEditor(sceneProject);
    drawAddPropertyPopup();
    drawRebindPopup();
    if (focused) handleShortcuts();
    ImGui::PopID();
}

void AnimationWindowClip::drawToolbar(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    const bool stopped = sceneProject && sceneProject->playState == ScenePlayState::STOPPED;

    // Preview: the clip at the playhead in the Scene view (blue timeline), restored on stop
    {
        const bool on = c.isPreviewing();
        ImGui::BeginDisabled(!doc || (!on && !stopped));
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
        if (ImGui::Button("Preview##clip_preview")) togglePreview();
        if (on) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(on ? "Stop previewing: the scene goes back to its saved values"
                                 : "Preview the clip at the playhead in the Scene view (the scene is not changed)");
    }
    // (T3c adds the Record toggle here)
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    ImGui::BeginDisabled(!doc);
    if (ImGui::Button(ICON_FA_BACKWARD_FAST "##clip_first")) seek(0.0f);
    ImGui::SetItemTooltip("First frame");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_BACKWARD_STEP "##clip_prev_key")) stepKey(-1);
    ImGui::SetItemTooltip("Previous key (Alt+,)");
    ImGui::SameLine();
    if (c.isPlaying()){
        ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
        if (ImGui::Button(ICON_FA_PAUSE "##clip_play")) togglePlay();
        ImGui::PopStyleColor();
    }else{
        ImGui::BeginDisabled(!stopped);
        if (ImGui::Button(ICON_FA_PLAY "##clip_play")) togglePlay();
        ImGui::EndDisabled();
    }
    ImGui::SetItemTooltip("Play / pause the preview (Space)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FORWARD_STEP "##clip_next_key")) stepKey(1);
    ImGui::SetItemTooltip("Next key (Alt+.)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FORWARD_FAST "##clip_last")) seek(doc ? doc->data().effectiveLength() : 0.0f);
    ImGui::SetItemTooltip("Last frame");
    ImGui::SameLine();

    // the playhead in frames or seconds
    const float fps = frameRate();
    if (showFrames){
        int frame = (int)std::lround(c.getTime() * fps);
        ImGui::SetNextItemWidth(Theme::dpi(56.0f));
        if (ImGui::DragInt("##clip_frame", &frame, 0.2f, 0, (int)(kMaxTime * fps))) seek((float)std::max(0, frame) / fps);
    }else{
        float seconds = c.getTime();
        ImGui::SetNextItemWidth(Theme::dpi(64.0f));
        if (ImGui::DragFloat("##clip_seconds", &seconds, 0.01f, 0.0f, kMaxTime, "%.3fs")) seek(seconds);
    }
    ImGui::SetItemTooltip(showFrames ? "Playhead (frame)" : "Playhead (seconds)");
    ImGui::SameLine();
    if (ImGui::Button(showFrames ? "Frames##clip_units" : "Seconds##clip_units")) showFrames = !showFrames;
    ImGui::SetItemTooltip("Show times in frames or in seconds");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    drawClipCombo(sceneProject);
    ImGui::SameLine();

    if (doc){
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        ImGui::BeginDisabled(doc->isReadOnly());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Samples");
        ImGui::SameLine();
        int samples = (int)std::lround(fps);
        ImGui::SetNextItemWidth(Theme::dpi(44.0f));
        ImGui::InputInt("##clip_samples", &samples, 0, 0);
        if (ImGui::IsItemDeactivatedAfterEdit()){
            std::string error;
            if (!setFrameRate((float)std::clamp(samples, 1, 240), &error)) setNotice(error, true);
        }
        ImGui::SetItemTooltip("Samples: the clip's frame rate (the frames grid keys snap to)");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_MAGNET "##clip_snap")){
            std::string error;
            if (!snapKeysToFrames(&error)) setNotice(error, true);
        }
        ImGui::SetItemTooltip("Snap keys to frames (keys closer than half a frame merge, with a warning)");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_DIAMOND "+##clip_key_all")){
            std::string error;
            if (!c.keyAll(c.getTime(), &error)) setNotice(error, true);
            else c.setLastActionWasClipEdit(true);
        }
        ImGui::SetItemTooltip("Add Key: key every property at the playhead with its current value (K)");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FLAG "+##clip_add_event")){
            std::string error;
            if (!addEvent(snapToFrame(c.getTime()), "", {}, &error)) setNotice(error, true);
        }
        ImGui::SetItemTooltip("Add Event at the playhead");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(90.0f));
        ImGui::SliderFloat("##clip_zoom", &pixelsPerSecond, 10.0f, 4000.0f, ICON_FA_MAGNIFYING_GLASS " %.0f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("Zoom (pixels per second). Ctrl+wheel zooms, Shift+wheel pans, F frames the selection, A all");
    }
    if (!notice.empty()){
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, noticeWarning ? Theme::Colors::WarningText : Theme::Colors::SubtleText);
        ImGui::TextUnformatted(notice.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

void AnimationWindowClip::drawClipCombo(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    std::string current = !c.getStateName().empty() ? c.getStateName()
                        : doc ? fs::u8path(doc->path()).filename().u8string() : std::string("(No Clip)");
    if (doc && doc->isReadOnly()) current += " (Read-Only)";
    const float width = std::max(Theme::dpi(150.0f), ImGui::CalcTextSize(current.c_str()).x + ImGui::GetFrameHeight() * 2.0f);
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##clip_combo", current.c_str())){
        const std::vector<std::string> states = c.getStateNames();
        for (size_t i = 0; i < states.size(); i++){
            ImGui::PushID((int)i);
            if (ImGui::Selectable(states[i].c_str(), states[i] == c.getStateName())){
                std::string error;
                if (!openState(states[i], &error)) setNotice(error, true);
            }
            ImGui::PopID();
        }
        // the entity Animations targeting the root stay reachable (R31)
        SceneProject* bound = boundScene();
        const Entity root = c.getRoot();
        if (bound && root != NULL_ENTITY && bound->scene->isEntityCreated(root)){
            Scene* scene = bound->scene;
            auto animations = scene->getComponentArray<AnimationComponent>();
            bool first = true;
            for (size_t i = 0; i < animations->size(); i++){
                const Entity animation = animations->getEntity(i);
                const ActionComponent* action = scene->findComponent<ActionComponent>(animation);
                if (!action || action->target != root) continue;
                if (first) ImGui::Separator();
                first = false;
                std::string label = scene->getEntityName(animation);
                if (label.empty()) label = "Animation";
                label += " (entity Animation)##anim" + std::to_string(animation);
                if (ImGui::Selectable(label.c_str(), false)){
                    entityAnimationRequested = true;
                    requestedScene = bound->id;
                    requestedAnimation = animation;
                }
            }
        }
        ImGui::Separator();
        const bool canCreate = sceneProject && root != NULL_ENTITY && c.getSceneId() == sceneProject->id &&
                               sceneProject->playState == ScenePlayState::STOPPED;
        if (ImGui::Selectable("Create New Clip...", false, canCreate ? 0 : ImGuiSelectableFlags_Disabled)) askCreateNewClip(sceneProject);
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", doc ? doc->path().c_str() : "The clip to edit");
}

bool AnimationWindowClip::drawEntityModeEntries(SceneProject* sceneProject, Entity selected){
    if (!sceneProject || selected == NULL_ENTITY || !sceneProject->scene->isEntityCreated(selected)) return false;
    Scene* scene = sceneProject->scene;
    // an Animation entity selected itself has no clips to offer
    if (scene->findComponent<AnimationComponent>(selected)) return false;
    const Entity playerRoot = AnimationClipController::findPlayerRoot(scene, selected);
    bool picked = false;
    if (playerRoot != NULL_ENTITY){
        const AnimationPlayerComponent& player = scene->getComponent<AnimationPlayerComponent>(playerRoot);
        const std::vector<std::string> states = anim::playerStateNames(player.clips);
        for (size_t i = 0; i < states.size(); i++){
            ImGui::PushID((int)i + 100000);
            if (ImGui::Selectable((states[i] + " (clip)").c_str(), false)){
                bind(sceneProject->id, selected);
                std::string error;
                if (!openState(states[i], &error)) setNotice(error, true);
                picked = true;
            }
            ImGui::PopID();
        }
    }
    const bool canCreate = sceneProject->playState == ScenePlayState::STOPPED;
    if (ImGui::Selectable("Create New Clip...##entity_mode", false, canCreate ? 0 : ImGuiSelectableFlags_Disabled)){
        bind(sceneProject->id, selected);
        askCreateNewClip(sceneProject);
        picked = document() != nullptr;
    }
    ImGui::Separator();
    return picked;
}

void AnimationWindowClip::drawEmptyState(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    SceneProject* bound = boundScene();
    const Entity root = c.getRoot();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, avail.y * 0.25f)));
    auto centered = [&](const std::string& text, const ImVec4* textColor){
        const float w = ImGui::CalcTextSize(text.c_str()).x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (avail.x - w) * 0.5f));
        if (textColor) ImGui::PushStyleColor(ImGuiCol_Text, *textColor);
        ImGui::TextUnformatted(text.c_str());
        if (textColor) ImGui::PopStyleColor();
    };
    if (!bound || root == NULL_ENTITY || !bound->scene->isEntityCreated(root)){
        centered(ICON_FA_FILM, &Theme::Colors::SubtleText);
        centered("Select a GameObject to animate it, or open an Animation Clip from the Project window.", &Theme::Colors::SubtleText);
        return;
    }
    const std::string name = bound->scene->getEntityName(root);
    centered(ICON_FA_FILM, &Theme::Colors::SubtleText);
    if (!c.getStateName().empty()){
        // a state whose clip file can't be opened
        const std::string text = "The clip of state '" + c.getStateName() + "' can't be opened (missing or not a clip).";
        centered(text, &Theme::Colors::WarningText);
    }
    centered("To begin animating " + name + ", create an Animation Clip.", nullptr);
    const char* button = "Create...";
    const float bw = ImGui::CalcTextSize(button).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (avail.x - bw) * 0.5f));
    const bool canCreate = sceneProject && sceneProject->id == bound->id && sceneProject->playState == ScenePlayState::STOPPED;
    ImGui::BeginDisabled(!canCreate);
    if (ImGui::Button(button)) askCreateNewClip(sceneProject);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Adds an Animation Player to %s (when it has none) and a new clip file (one undo step)", name.c_str());
}

void AnimationWindowClip::drawBanners(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    if (doc->isReadOnly()){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
        ImGui::AlignTextToFramePadding();
        ImGui::Text(ICON_FA_LOCK " (Read-Only) %s is an animation of a model: extract it to an .anim file to edit it.", doc->path().c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FILE_EXPORT " Extract to .anim")) askExtract(sceneProject);
        ImGui::SetItemTooltip(c.getStateName().empty() ? "Writes the animation to a new .anim file (one project undo step) and opens it"
                                                       : "Writes the animation to a new .anim file and switches the Animation Player's state to it (one undo step)");
    }
    if (!doc->exists()){
        ImGui::TextColored(Theme::Colors::ErrorText, ICON_FA_TRIANGLE_EXCLAMATION " %s was deleted.", doc->path().c_str());
    }
    if (c.getRoot() == NULL_ENTITY){
        // target-less: the file is edited; the preview runs on an instance of preview.model
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Theme::Colors::SubtleText, "No GameObject bound. Preview on:");
        ImGui::SameLine();
        static char buffer[512];
        static std::string shownFor;
        const std::string model = doc->data().previewModel;
        if (shownFor != doc->path() + "|" + model || !ImGui::IsItemActive()){
            std::snprintf(buffer, sizeof(buffer), "%s", model.c_str());
        }
        ImGui::SetNextItemWidth(Theme::dpi(260.0f));
        ImGui::BeginDisabled(doc->isReadOnly());
        ImGui::InputTextWithHint("##clip_preview_model", "a model or prefab (drop one here)", buffer, sizeof(buffer));
        shownFor = doc->path() + "|" + model;
        if (ImGui::IsItemDeactivatedAfterEdit()){
            std::string error;
            if (!setPreviewModel(buffer, &error)) setNotice(error, true);
        }
        if (ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                const std::vector<std::string> paths = Util::getStringsFromPayload(payload);
                if (!paths.empty()){
                    std::string error;
                    if (!setPreviewModel(paths.front(), &error)) setNotice(error, true);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("The model or prefab the clip previews on when no GameObject is selected (an instance in the open scene)");
    }
    if (report.root != NULL_ENTITY && report.total > report.resolved){
        if (report.legacyModelLayout){
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION
                               " The model is loaded without its node hierarchy, so its bones can't be animated by the clip.");
            ImGui::SameLine();
            const bool canFix = sceneProject && sceneProject->id == c.getSceneId() && sceneProject->playState == ScenePlayState::STOPPED;
            ImGui::BeginDisabled(!canFix);
            if (ImGui::Button("Reload with its hierarchy")){
                ClipActionResult result;
                std::string error;
                c.stopPreview();
                if (ClipSceneActions::reloadModelsForPlayer(project, c.getSceneId(), c.getRoot(), result, error)){
                    c.setLastActionWasClipEdit(false);
                    setNotice(result.status, false);
                }else{
                    setNotice(error, true);
                }
                reportRevision = ~uint64_t(0);
            }
            ImGui::EndDisabled();
        }else{
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION
                               " %zu of %zu bindings are missing: right-click a yellow row to Rebind or Remove it.",
                               report.total - report.resolved, report.total);
        }
    }
}

void AnimationWindowClip::drawRuler(ImDrawList* drawList, ImVec2 origin, float width, float height){
    const SheetMetrics m = sheetMetrics();
    drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), color(Theme::Colors::ToolbarBackground));
    const float fps = frameRate();
    // a tick spacing of at least ~70 px between labels: frames (1, 2, 5, 10...) or seconds
    const float minSpacing = 70.0f * m.s;
    float step = 1.0f / fps;
    const float candidates[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600, 18000};
    for (float frames : candidates){
        step = frames / fps;
        if (step * pixelsPerSecond >= minSpacing) break;
    }
    const float start = std::floor((scrollX / pixelsPerSecond) / step) * step;
    const float end = (scrollX + width) / pixelsPerSecond;
    for (float t = std::max(0.0f, start); t <= end + step; t += step){
        const float x = origin.x + m.timePad + t * pixelsPerSecond - scrollX;
        if (x < origin.x - 1.0f || x > origin.x + width + 1.0f) continue;
        drawList->AddLine(ImVec2(x, origin.y + height * 0.45f), ImVec2(x, origin.y + height), IM_COL32(150, 150, 150, 255));
        const std::string label = formatTime(t, fps, showFrames);
        drawList->AddText(ImVec2(x + 3.0f * m.s, origin.y + 1.0f * m.s), IM_COL32(190, 190, 190, 255), label.c_str());
        // minor ticks
        for (int i = 1; i < 5; i++){
            const float xm = x + step * pixelsPerSecond * (float)i / 5.0f;
            if (xm > origin.x + width) break;
            drawList->AddLine(ImVec2(xm, origin.y + height * 0.75f), ImVec2(xm, origin.y + height), IM_COL32(110, 110, 110, 255));
        }
    }
    drawList->AddLine(ImVec2(origin.x, origin.y + height - 1.0f), ImVec2(origin.x + width, origin.y + height - 1.0f),
                      color(Theme::Colors::ToolbarBorder));
}

void AnimationWindowClip::drawRowLabel(const Row& row, size_t index, ImVec2 pos, float width, float height){
    const SheetMetrics m = sheetMetrics();
    std::shared_ptr<AnimationClipDocument> doc = document();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float textY = pos.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
    if (row.kind == Row::Kind::Summary){
        const std::string clip = doc ? fs::u8path(doc->path()).filename().u8string() : std::string();
        drawList->AddText(ImVec2(pos.x + m.indent * 0.5f, textY), color(Theme::Colors::SubtleText), clip.c_str());
        return;
    }
    const ClipTrackData& track = doc->data().tracks[(size_t)row.track];
    if (row.kind == Row::Kind::Track){
        const bool foldable = track.channel != ClipChannel::Property;
        const bool open = expanded.count(rowKey(track)) > 0;
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 2.0f * m.s, pos.y));
        ImGui::PushID((int)index);
        if (foldable){
            if (ImGui::InvisibleButton("##fold", ImVec2(m.indent, height))){
                if (open) expanded.erase(rowKey(track));
                else expanded.insert(rowKey(track));
            }
            ImGui::RenderArrow(drawList, ImVec2(pos.x + 3.0f * m.s, textY), IM_COL32(200, 200, 200, 255),
                               open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
        }
        // the label: right-click for the row menu (Rebind / Remove)
        ImGui::SetCursorScreenPos(ImVec2(pos.x + m.indent + 2.0f * m.s, pos.y));
        ImGui::InvisibleButton("##label", ImVec2(std::max(1.0f, width - m.indent - m.valueWidth - 4.0f * m.s), height));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)){
            rowMenuTrack = row.track;
            ImGui::OpenPopup("##clip_row_menu");
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)){
            // selects the row's keys (Unity)
            std::vector<KeyId> keys;
            for (const ClipKey& key : track.keys) keys.push_back({track.path, track.channel, key.time});
            const ImGuiIO& io = ImGui::GetIO();
            selectKeys(keys, io.KeyShift || io.KeyCtrl);
        }
        // a long path shows its end ("…/Spine : Rotation"), the whole label in the tooltip
        // (a missing row starts with the warning icon, so it reads as missing even when cut)
        const std::string icon = row.missing ? std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") : std::string();
        const float room = width - m.indent - m.valueWidth - 8.0f * m.s - ImGui::CalcTextSize(icon.c_str()).x;
        std::string shown = row.label;
        if (ImGui::CalcTextSize(shown.c_str()).x > room && track.path.find('/') != std::string::npos){
            std::string tail = row.label.substr(track.path.size());
            std::string path = track.path;
            while (ImGui::CalcTextSize(("..." + path + tail).c_str()).x > room && path.find('/') != std::string::npos){
                path = path.substr(path.find('/') + 1);
            }
            shown = ".../" + path + tail;
        }
        if (!row.message.empty()) ImGui::SetItemTooltip("%s\n%s", row.label.c_str(), row.message.c_str());
        else if (shown != row.label) ImGui::SetItemTooltip("%s", row.label.c_str());
        ImGui::PopID();
        drawList->PushClipRect(ImVec2(pos.x, pos.y), ImVec2(pos.x + width - m.valueWidth - 2.0f * m.s, pos.y + height), true);
        drawList->AddText(ImVec2(pos.x + m.indent + 4.0f * m.s, textY),
                          row.missing ? color(Theme::Colors::WarningText) : ImGui::GetColorU32(ImGuiCol_Text), (icon + shown).c_str());
        drawList->PopClipRect();
        return;
    }
    // a component row (x / y / z, a blend shape, the lattice point count)
    drawList->PushClipRect(ImVec2(pos.x, pos.y), ImVec2(pos.x + width - m.valueWidth - 2.0f * m.s, pos.y + height), true);
    drawList->AddText(ImVec2(pos.x + m.indent * 2.0f + 4.0f * m.s, textY),
                      row.missing ? color(Theme::Colors::WarningText) : color(Theme::Colors::SubtleText), row.label.c_str());
    drawList->PopClipRect();
}

void AnimationWindowClip::drawValueField(const Row& row, ImVec2 pos, float width, float height){
    if (row.kind == Row::Kind::Summary || row.track < 0) return;
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    const SheetMetrics m = sheetMetrics();
    const ClipTrackData& track = doc->data().tracks[(size_t)row.track];
    if (track.channel == ClipChannel::Property) return;
    // what the Scene view shows while previewing (incl. an unkeyed pink value), else the clip's value
    std::vector<float> value;
    bool shown = false;
    if (c.isPreviewing() && !row.missing){
        Scene* scene = valueScene();
        const Entity entity = scene ? anim::resolvePath(scene, valueRoot(), track.path) : NULL_ENTITY;
        shown = entity != NULL_ENTITY && anim::readChannelValue(scene, entity, track.channel, track.points, value);
    }
    if (!shown) value = sampledValue((size_t)row.track, c.getTime());
    const bool pending = c.hasPendingValue((size_t)row.track);
    const float x = pos.x + width - m.valueWidth;
    ImGui::SetCursorScreenPos(ImVec2(x, pos.y + (height - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::PushID(row.track * 64 + row.component + 1);
    if (row.kind == Row::Kind::Track || track.channel == ClipChannel::Lattice){
        char text[96] = "";
        if (track.channel == ClipChannel::Rotation){
            const Vector3 e = eulerOf(value);
            std::snprintf(text, sizeof(text), "%.1f  %.1f  %.1f", e.x, e.y, e.z);
        }else if (track.channel == ClipChannel::Morph){
            std::snprintf(text, sizeof(text), "%zu weights", value.size());
        }else if (track.channel == ClipChannel::Lattice){
            std::snprintf(text, sizeof(text), "%zu points", track.points.size());
        }else if (value.size() >= 3){
            std::snprintf(text, sizeof(text), "%.2f  %.2f  %.2f", value[0], value[1], value[2]);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, pending ? ImVec4(1.0f, 0.55f, 0.65f, 1.0f) : Theme::Colors::SubtleText);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
        ImGui::PopID();
        return;
    }
    // an editable component: unkeyed edits preview in pink until K keys them
    float v = 0.0f;
    if (track.channel == ClipChannel::Rotation){
        const Vector3 e = eulerOf(value);
        v = row.component == 0 ? e.x : row.component == 1 ? e.y : e.z;
    }else if ((size_t)row.component < value.size()){
        v = value[(size_t)row.component];
    }
    if (pending) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.55f, 0.22f, 0.32f, 1.0f));
    ImGui::SetNextItemWidth(m.valueWidth - 4.0f * m.s);
    ImGui::BeginDisabled(doc->isReadOnly() || row.missing);
    const float speed = track.channel == ClipChannel::Rotation ? 0.5f : 0.01f;
    if (ImGui::DragFloat("##value", &v, speed, 0.0f, 0.0f, "%.3f")){
        std::string error;
        if (!setComponentValue((size_t)row.track, row.component, v, ImGui::IsItemActivated(), &error)) setNotice(error, true);
    }
    if (ImGui::IsItemActivated()) valueMergeKey.clear();
    ImGui::EndDisabled();
    if (pending) ImGui::PopStyleColor();
    ImGui::SetItemTooltip(pending ? "Not keyed: K keys it (the value goes when the playhead moves)"
                                  : "The value at the playhead. Editing it previews the change; K keys it");
    ImGui::PopID();
}

void AnimationWindowClip::drawSheet(SceneProject* sceneProject, bool focused){
    (void)sceneProject;
    (void)focused;
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    const SheetMetrics m = sheetMetrics();
    ImGuiIO& io = ImGui::GetIO();
    const std::vector<Row> rows = buildRows();
    const ClipData& data = doc->data();
    const float fps = frameRate();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float editorHeight = eventEditorHeight();
    const float scrollbarHeight = ImGui::GetStyle().ScrollbarSize;
    const float sheetHeight = std::max(80.0f * m.s, avail.y - editorHeight - scrollbarHeight);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float keyAreaX = origin.x + m.labelWidth;
    const float keyAreaWidth = std::max(1.0f, avail.x - m.labelWidth);
    const float headerHeight = m.rulerHeight + m.eventsHeight;
    const float timeX = keyAreaX + m.timePad;
    auto timeAt = [&](float x){ return (x - timeX + scrollX) / pixelsPerSecond; };
    auto xAt = [&](float t){ return timeX + t * pixelsPerSecond - scrollX; };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // header: the column title, the ruler (scrub) and the events lane
    drawList->AddRectFilled(origin, ImVec2(keyAreaX, origin.y + headerHeight), color(Theme::Colors::ToolbarBackground));
    drawRuler(drawList, ImVec2(keyAreaX, origin.y), keyAreaWidth, m.rulerHeight);
    ImGui::SetCursorScreenPos(ImVec2(keyAreaX, origin.y));
    ImGui::InvisibleButton("##clip_ruler", ImVec2(keyAreaWidth, m.rulerHeight));
    if (ImGui::IsItemActive() && (ImGui::IsItemActivated() || io.MouseDelta.x != 0.0f)){
        seek(snapToFrame(std::max(0.0f, timeAt(io.MousePos.x))));
    }
    const bool rulerHovered = ImGui::IsItemHovered();
    drawEventsLane(drawList, ImVec2(keyAreaX, origin.y + m.rulerHeight), keyAreaWidth, m.eventsHeight, timeX);
    const bool eventsHovered = ImGui::IsItemHovered();
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 4.0f * m.s, origin.y + (m.rulerHeight - ImGui::GetFrameHeight()) * 0.5f));
    ImGui::BeginDisabled(doc->isReadOnly());
    if (ImGui::SmallButton("Add Property")) openAddProperty = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(doc->isReadOnly() ? "Read-only clip: extract it to edit it"
                                            : "Animate a property of the bound GameObject or one under it");

    // zoom and pan with the wheel (Ctrl / over the header: zoom at the mouse; Shift: pan)
    const bool overSheet = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                           io.MousePos.x >= keyAreaX && io.MousePos.x <= keyAreaX + keyAreaWidth &&
                           io.MousePos.y >= origin.y && io.MousePos.y <= origin.y + sheetHeight;
    if (overSheet && io.MouseWheel != 0.0f && (io.KeyCtrl || rulerHovered || eventsHovered)){
        const float anchor = timeAt(io.MousePos.x);
        pixelsPerSecond = std::clamp(pixelsPerSecond * (1.0f + io.MouseWheel * 0.1f), 10.0f, 4000.0f);
        scrollX = std::max(0.0f, anchor * pixelsPerSecond - (io.MousePos.x - timeX));
    }else if (overSheet && (io.MouseWheelH != 0.0f || (io.KeyShift && io.MouseWheel != 0.0f))){
        const float wheel = io.MouseWheelH != 0.0f ? io.MouseWheelH : io.MouseWheel;
        scrollX = std::max(0.0f, scrollX - wheel * 60.0f * m.s);
    }
    if (overSheet && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) scrollX = std::max(0.0f, scrollX - io.MouseDelta.x);

    // the rows: labels + value column on the left, keys on the right (scrolls vertically)
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + headerHeight));
    ImGuiWindowFlags childFlags = (io.KeyCtrl || io.KeyShift) ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None;
    ImGui::BeginChild("##clip_rows", ImVec2(avail.x, sheetHeight - headerHeight), ImGuiChildFlags_None, childFlags);
    ImDrawList* rowsList = ImGui::GetWindowDrawList();
    const ImVec2 top = ImGui::GetCursorScreenPos();
    const float contentHeight = std::max((float)rows.size() * m.rowHeight + m.rowHeight, ImGui::GetContentRegionAvail().y);
    const ImVec2 clipMin = ImGui::GetWindowPos();
    const ImVec2 clipMax = ImVec2(clipMin.x + ImGui::GetWindowSize().x, clipMin.y + ImGui::GetWindowSize().y);

    // backgrounds: the timeline is blue while previewing (Unity), red while recording (T3c)
    const ImU32 keyBackground = c.isPreviewing() ? color(Theme::Colors::Selection, 0.28f) : IM_COL32(38, 38, 38, 255);
    rowsList->AddRectFilled(ImVec2(keyAreaX, clipMin.y), ImVec2(keyAreaX + keyAreaWidth, clipMax.y), keyBackground);
    const float clipEnd = xAt(data.effectiveLength());
    if (clipEnd < keyAreaX + keyAreaWidth){
        rowsList->AddRectFilled(ImVec2(std::max(keyAreaX, clipEnd), clipMin.y), ImVec2(keyAreaX + keyAreaWidth, clipMax.y),
                                IM_COL32(0, 0, 0, 60));
    }
    // frame grid lines when frames are wide enough
    if (fps * 1.0f > 0.0f && pixelsPerSecond / fps >= 8.0f * m.s){
        const float first = std::floor(timeAt(keyAreaX) * fps) / fps;
        for (float t = std::max(0.0f, first); xAt(t) <= keyAreaX + keyAreaWidth; t += 1.0f / fps){
            rowsList->AddLine(ImVec2(xAt(t), clipMin.y), ImVec2(xAt(t), clipMax.y), IM_COL32(255, 255, 255, 14));
        }
    }

    // keys of a row
    auto keysOf = [&](const Row& row){
        std::vector<std::pair<size_t, size_t>> out;
        for (size_t t = 0; t < data.tracks.size(); t++){
            if (row.kind != Row::Kind::Summary && (int)t != row.track) continue;
            for (size_t k = 0; k < data.tracks[t].keys.size(); k++) out.push_back({t, k});
        }
        return out;
    };

    int hoveredRow = -1;
    std::vector<std::pair<size_t, size_t>> hoveredKeys;
    float hoveredTime = 0.0f;
    for (size_t r = 0; r < rows.size(); r++){
        const Row& row = rows[r];
        const ImVec2 rowPos(top.x, top.y + (float)r * m.rowHeight);
        if (rowPos.y + m.rowHeight < clipMin.y || rowPos.y > clipMax.y) continue;
        if (r % 2 == 1){
            rowsList->AddRectFilled(ImVec2(origin.x, rowPos.y), ImVec2(keyAreaX, rowPos.y + m.rowHeight), color(Theme::Colors::RowBackgroundAlt));
        }
        if (row.kind == Row::Kind::Summary){
            rowsList->AddRectFilled(ImVec2(keyAreaX, rowPos.y), ImVec2(keyAreaX + keyAreaWidth, rowPos.y + m.rowHeight), IM_COL32(0, 0, 0, 45));
        }
        rowsList->AddLine(ImVec2(origin.x, rowPos.y + m.rowHeight), ImVec2(keyAreaX + keyAreaWidth, rowPos.y + m.rowHeight), IM_COL32(0, 0, 0, 70));
        drawRowLabel(row, r, rowPos, m.labelWidth, m.rowHeight);
        drawValueField(row, rowPos, m.labelWidth, m.rowHeight);

        // the keys: one diamond per time (the summary row merges times of every track)
        rowsList->PushClipRect(ImVec2(keyAreaX, clipMin.y), ImVec2(keyAreaX + keyAreaWidth, clipMax.y), true);
        const float cy = rowPos.y + m.rowHeight * 0.5f;
        std::vector<std::pair<size_t, size_t>> keys = keysOf(row);
        std::sort(keys.begin(), keys.end(), [&](const std::pair<size_t, size_t>& a, const std::pair<size_t, size_t>& b){
            return data.tracks[a.first].keys[a.second].time < data.tracks[b.first].keys[b.second].time;
        });
        for (size_t i = 0; i < keys.size();){
            const float t = data.tracks[keys[i].first].keys[keys[i].second].time;
            size_t j = i;
            bool selected = true;
            std::vector<std::pair<size_t, size_t>> group;
            while (j < keys.size() && std::fabs(data.tracks[keys[j].first].keys[keys[j].second].time - t) <= kKeyEpsilon){
                selected = selected && isSelected(keys[j].first, keys[j].second);
                group.push_back(keys[j]);
                j++;
            }
            const float x = xAt(t);
            const float radius = row.kind == Row::Kind::Component ? m.keyRadius * 0.8f : m.keyRadius;
            ImU32 fill = selected ? color(ImVec4(0.35f, 0.62f, 1.0f, 1.0f)) : IM_COL32(205, 205, 205, 255);
            if (row.missing) fill = selected ? color(ImVec4(0.35f, 0.62f, 1.0f, 1.0f)) : color(Theme::Colors::WarningText, 0.8f);
            if (row.kind == Row::Kind::Summary && !selected) fill = IM_COL32(150, 150, 150, 255);
            const ImVec2 pts[4] = {ImVec2(x, cy - radius), ImVec2(x + radius, cy), ImVec2(x, cy + radius), ImVec2(x - radius, cy)};
            rowsList->AddConvexPolyFilled(pts, 4, fill);
            rowsList->AddPolyline(pts, 4, IM_COL32(20, 20, 20, 255), ImDrawFlags_Closed, 1.0f * m.s);
            if (ImGui::IsWindowHovered() && std::fabs(io.MousePos.x - x) <= m.keyHit && io.MousePos.y >= rowPos.y &&
                io.MousePos.y < rowPos.y + m.rowHeight && io.MousePos.x >= keyAreaX){
                hoveredRow = (int)r;
                hoveredKeys = group;
                hoveredTime = t;
            }
            i = j;
        }
        rowsList->PopClipRect();
    }

    // the key area's input: click / Shift / Ctrl select, box select, drag, right-click menu
    ImGui::SetCursorScreenPos(ImVec2(keyAreaX, top.y));
    ImGui::InvisibleButton("##clip_keys", ImVec2(keyAreaWidth, contentHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const int mouseRow = (int)std::floor((io.MousePos.y - top.y) / m.rowHeight);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
        const bool additive = io.KeyShift || io.KeyCtrl;
        dragStart = io.MousePos;
        if (!hoveredKeys.empty()){
            bool allSelected = true;
            for (const auto& key : hoveredKeys) allSelected = allSelected && isSelected(key.first, key.second);
            std::vector<KeyId> ids;
            for (const auto& key : hoveredKeys){
                const ClipTrackData& track = data.tracks[key.first];
                ids.push_back({track.path, track.channel, track.keys[key.second].time});
            }
            if (io.KeyCtrl && allSelected){
                // Ctrl toggles
                std::vector<KeyId> kept;
                for (const KeyId& id : selection){
                    bool hit = false;
                    for (const KeyId& other : ids){
                        hit = hit || (other.path == id.path && other.channel == id.channel && std::fabs(other.time - id.time) <= kKeyEpsilon);
                    }
                    if (!hit) kept.push_back(id);
                }
                selection = kept;
                refreshSelection();
                drag = Drag::None;
            }else{
                if (!allSelected) selectKeys(ids, additive);
                dragGrabTime = hoveredTime;
                dragAdditive = additive;
                drag = Drag::PendingKeys;
            }
        }else{
            if (!additive){
                selection.clear();
                refreshSelection();
            }
            dragAdditive = additive;
            drag = Drag::Box;
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        if (drag == Drag::PendingKeys && std::fabs(io.MousePos.x - dragStart.x) > 3.0f * m.s){
            std::string error;
            if (!doc->isReadOnly() && beginDrag(&error)){
                drag = Drag::Keys;
            }else{
                if (!error.empty()) setNotice(error, true);
                if (doc->isReadOnly()) setNotice("Read-only clip: extract it to edit it", true);
                drag = Drag::None;
            }
        }
        if (drag == Drag::Keys){
            std::string error;
            if (!updateDrag((io.MousePos.x - dragStart.x) / pixelsPerSecond, &error) && !error.empty()) setNotice(error, true);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (drag == Drag::Box){
            const ImVec2 a(std::min(dragStart.x, io.MousePos.x), std::min(dragStart.y, io.MousePos.y));
            const ImVec2 b(std::max(dragStart.x, io.MousePos.x), std::max(dragStart.y, io.MousePos.y));
            rowsList->AddRectFilled(a, b, color(Theme::Colors::Selection, 0.25f));
            rowsList->AddRect(a, b, color(Theme::Colors::Selection, 0.9f));
        }
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && drag != Drag::None && drag != Drag::PendingEvent && drag != Drag::Event){
        if (drag == Drag::Keys){
            endDrag();
        }else if (drag == Drag::Box){
            if (std::fabs(io.MousePos.x - dragStart.x) > 2.0f * m.s || std::fabs(io.MousePos.y - dragStart.y) > 2.0f * m.s){
                const int rowA = (int)std::floor((dragStart.y - top.y) / m.rowHeight);
                const int rowB = (int)std::floor((io.MousePos.y - top.y) / m.rowHeight);
                boxSelect(rowA, rowB, timeAt(dragStart.x), timeAt(io.MousePos.x), dragAdditive);
            }
        }else if (drag == Drag::PendingKeys && !dragAdditive && !hoveredKeys.empty()){
            // a click (no drag) on one of several selected keys selects only those
            std::vector<KeyId> ids;
            for (const auto& key : hoveredKeys){
                const ClipTrackData& track = data.tracks[key.first];
                ids.push_back({track.path, track.channel, track.keys[key.second].time});
            }
            selectKeys(ids, false);
        }
        drag = Drag::None;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)){
        contextRow = mouseRow;
        contextTime = snapToFrame(std::max(0.0f, timeAt(io.MousePos.x)));
        if (!hoveredKeys.empty()){
            bool anySelected = false;
            for (const auto& key : hoveredKeys) anySelected = anySelected || isSelected(key.first, key.second);
            if (!anySelected){
                std::vector<KeyId> ids;
                for (const auto& key : hoveredKeys){
                    const ClipTrackData& track = data.tracks[key.first];
                    ids.push_back({track.path, track.channel, track.keys[key.second].time});
                }
                selectKeys(ids, false);
            }
            contextTime = hoveredTime;
        }
        ImGui::OpenPopup("##clip_key_menu");
    }
    if (ImGui::IsItemHovered() && hoveredRow >= 0 && drag == Drag::None){
        ImGui::SetTooltip("%s", formatTime(hoveredTime, fps, showFrames).c_str());
    }
    drawKeyContextMenu();
    drawRowMenu();

    // the playhead
    const float playX = xAt(c.getTime());
    if (playX >= keyAreaX && playX <= keyAreaX + keyAreaWidth){
        rowsList->AddLine(ImVec2(playX, clipMin.y), ImVec2(playX, clipMax.y), IM_COL32(255, 70, 70, 255), 1.5f * m.s);
    }
    ImGui::SetCursorScreenPos(top);
    ImGui::Dummy(ImVec2(avail.x, contentHeight));
    ImGui::EndChild();

    // the playhead in the header
    if (playX >= keyAreaX && playX <= keyAreaX + keyAreaWidth){
        drawList->AddLine(ImVec2(playX, origin.y), ImVec2(playX, origin.y + headerHeight), IM_COL32(255, 70, 70, 255), 1.5f * m.s);
        const ImVec2 tri[3] = {ImVec2(playX - m.playheadHalf, origin.y), ImVec2(playX + m.playheadHalf, origin.y),
                               ImVec2(playX, origin.y + m.playheadHalf * 1.6f)};
        drawList->AddConvexPolyFilled(tri, 3, IM_COL32(255, 70, 70, 255));
    }

    // the horizontal scrollbar under the sheet (time)
    const float contentWidth = std::max(keyAreaWidth, (std::max(viewLength(), c.getTime()) + 1.0f) * pixelsPerSecond);
    ImVec2 barPos = ImGui::GetCursorScreenPos();
    ImRect barRect(ImVec2(keyAreaX, barPos.y), ImVec2(keyAreaX + keyAreaWidth, barPos.y + scrollbarHeight));
    ImS64 scrollValue = (ImS64)std::llround(scrollX);
    const ImS64 visibleValue = std::max<ImS64>(1, (ImS64)std::llround(keyAreaWidth));
    const ImS64 contentValue = std::max(visibleValue, (ImS64)std::llround(std::max(contentWidth, scrollX + keyAreaWidth)));
    ImGui::ScrollbarEx(barRect, ImGui::GetID("##clip_hscroll"), ImGuiAxis_X, &scrollValue, visibleValue, contentValue);
    scrollX = std::max(0.0f, (float)scrollValue);
    ImGui::Dummy(ImVec2(avail.x, scrollbarHeight));
}

void AnimationWindowClip::drawKeyContextMenu(){
    if (!ImGui::BeginPopup("##clip_key_menu")) return;
    std::shared_ptr<AnimationClipDocument> doc = document();
    const bool editable = doc && !doc->isReadOnly();
    const std::vector<Row> rows = buildRows();
    const int track = contextRow >= 0 && contextRow < (int)rows.size() ? rows[(size_t)contextRow].track : -1;
    std::string error;
    ImGui::BeginDisabled(!editable);
    if (ImGui::MenuItem(track >= 0 ? "Add Key" : "Add Key (all properties)")){
        if (!addKeysAt(track, contextTime, &error)) setNotice(error, true);
    }
    ImGui::BeginDisabled(selectedRefs.empty());
    if (ImGui::MenuItem("Delete Keys", "Del")){
        if (!deleteSelected(&error)) setNotice(error, true);
    }
    if (ImGui::BeginMenu("Interpolation")){
        struct Choice{ const char* label; EaseType ease; };
        const Choice choices[] = {{"Linear", EaseType::LINEAR}, {"Step (Constant)", EaseType::STEP},
                                  {"Ease In (Cubic)", EaseType::CUBIC_IN}, {"Ease Out (Cubic)", EaseType::CUBIC_OUT},
                                  {"Ease In Out (Cubic)", EaseType::CUBIC_IN_OUT}, {"Ease In Out (Sine)", EaseType::SINE_IN_OUT},
                                  {"Ease In Out (Quad)", EaseType::QUAD_IN_OUT}, {"Back Out", EaseType::BACK_OUT},
                                  {"Elastic Out", EaseType::ELASTIC_OUT}, {"Bounce Out", EaseType::BOUNCE_OUT}};
        for (const Choice& choice : choices){
            if (ImGui::MenuItem(choice.label)){
                if (!setEase(choice.ease, &error)) setNotice(error, true);
            }
        }
        ImGui::EndMenu();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::MenuItem("Copy", "Ctrl+C", false, !selectedRefs.empty())) copySelected();
    if (ImGui::MenuItem("Paste", "Ctrl+V", false, editable && !clipboard.empty())){
        if (!paste(contextTime, &error)) setNotice(error, true);
    }
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, editable && !selectedRefs.empty())){
        if (!duplicateSelected(&error)) setNotice(error, true);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Move Playhead Here")) seek(contextTime);
    ImGui::EndPopup();
}

void AnimationWindowClip::drawRowMenu(){
    if (!ImGui::BeginPopup("##clip_row_menu")) return;
    std::shared_ptr<AnimationClipDocument> doc = document();
    const bool editable = doc && !doc->isReadOnly();
    const bool valid = doc && rowMenuTrack >= 0 && rowMenuTrack < (int)doc->data().tracks.size();
    bool missing = false;
    for (const Row& row : buildRows()){
        if (row.kind == Row::Kind::Track && row.track == rowMenuTrack) missing = row.missing;
    }
    std::string error;
    ImGui::BeginDisabled(!editable || !valid);
    if (ImGui::MenuItem(missing ? "Rebind..." : "Retarget...")){
        rebindTrack = rowMenuTrack;
        rebindSearch[0] = '\0';
        openRebind = true;
    }
    ImGui::SetItemTooltip("Rewrites the property's path in the clip: it changes for every user of the clip");
    if (ImGui::MenuItem("Add Key at Playhead")){
        if (!addKeysAt(rowMenuTrack, controller().getTime(), &error)) setNotice(error, true);
    }
    if (ImGui::MenuItem("Remove Property")){
        if (!controller().removeTrack((size_t)rowMenuTrack, &error)) setNotice(error, true);
        else controller().setLastActionWasClipEdit(true);
        selection.clear();
    }
    ImGui::EndDisabled();
    ImGui::EndPopup();
}

void AnimationWindowClip::drawAddPropertyPopup(){
    if (openAddProperty){
        openAddProperty = false;
        addPropertySearch[0] = '\0';
        ImGui::OpenPopup("Add Property##clip");
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(Theme::dpi(300.0f), Theme::dpi(120.0f)), ImVec2(Theme::dpi(520.0f), Theme::dpi(520.0f)));
    if (!ImGui::BeginPopup("Add Property##clip")) return;
    Scene* scene = valueScene();
    const Entity root = valueRoot();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!scene || root == NULL_ENTITY || !doc){
        ImGui::TextColored(Theme::Colors::WarningText, "No GameObject to animate: select one, or preview on the clip's model.");
        ImGui::EndPopup();
        return;
    }
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS " Search", addPropertySearch, sizeof(addPropertySearch));
    const std::string filter = addPropertySearch;
    auto lower = [](std::string s){
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch){ return (char)std::tolower(ch); });
        return s;
    };
    const std::string needle = lower(filter);
    const ClipData& data = doc->data();
    std::string error;
    bool done = false;

    // the channels an entity offers
    auto channelsOf = [&](Entity entity){
        std::vector<ClipChannel> channels = {ClipChannel::Position, ClipChannel::Rotation, ClipChannel::Scale};
        if (const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity)){
            bool morphs = false;
            for (unsigned int s = 0; s < mesh->numSubmeshes; s++) morphs = morphs || mesh->submeshes[s].hasMorphTarget;
            if (morphs) channels.push_back(ClipChannel::Morph);
        }
        if (scene->findComponent<LatticeComponent>(entity)) channels.push_back(ClipChannel::Lattice);
        return channels;
    };
    auto addChannels = [&](Entity entity, const std::string& path){
        for (ClipChannel channel : channelsOf(entity)){
            const bool exists = data.findTrack(path, channel) >= 0;
            std::string label = std::string(channelLabel(channel)) + (exists ? "  (animated)" : "");
            ImGui::PushID((int)channel);
            if (ImGui::Selectable(label.c_str(), false, exists ? ImGuiSelectableFlags_Disabled : 0)){
                size_t track = 0;
                if (!controller().addProperty(path, channel, &track, &error)){
                    setNotice(error, true);
                }else{
                    controller().setLastActionWasClipEdit(true);
                    std::shared_ptr<AnimationClipDocument> now = document();
                    if (now && track < now->data().tracks.size()) expanded.insert(rowKey(now->data().tracks[track]));
                }
                done = true;
            }
            ImGui::PopID();
        }
    };
    ImGui::BeginChild("##tree", ImVec2(Theme::dpi(380.0f), Theme::dpi(320.0f)), ImGuiChildFlags_None);
    // the bound root's hierarchy, depth first in child order; unaddressable names warn (R39)
    std::function<void(Entity, const std::string&, bool)> visit = [&](Entity entity, const std::string& path, bool addressable){
        if (done) return;
        const std::string name = scene->getEntityName(entity);
        const std::string reason = entity == root ? std::string() : EntityPath::unaddressableReason(name);
        const bool ok = addressable && reason.empty();
        const std::vector<Entity> children = anim::getChildren(scene, entity);
        const bool matches = needle.empty() || lower(name).find(needle) != std::string::npos;
        ImGui::PushID((int)entity);
        if (!needle.empty()){
            // a flat list of the matches with their paths
            if (matches){
                if (ok){
                    if (ImGui::TreeNodeEx("##match", ImGuiTreeNodeFlags_DefaultOpen, "%s", path.empty() ? name.c_str() : path.c_str())){
                        addChannels(entity, path);
                        ImGui::TreePop();
                    }
                }else{
                    ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s", name.c_str());
                    ImGui::SetItemTooltip("%s", reason.empty() ? "a GameObject above it can't be addressed" : reason.c_str());
                }
            }
            for (Entity child : children){
                std::string childPath;
                const bool childOk = ok && EntityPath::relative(scene, root, child, childPath);
                visit(child, childPath, childOk || !ok ? childOk : false);
            }
        }else if (!ok){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s (can't be animated)", name.c_str());
            ImGui::SetItemTooltip("%s", reason.empty() ? "a GameObject above it can't be addressed by an animation path" : reason.c_str());
        }else{
            const std::string label = entity == root ? name + " (root)" : name;
            if (ImGui::TreeNodeEx("##node", entity == root ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s", label.c_str())){
                addChannels(entity, path);
                for (Entity child : children){
                    std::string childPath;
                    const bool childOk = EntityPath::relative(scene, root, child, childPath);
                    visit(child, childPath, childOk);
                }
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    };
    visit(root, "", true);
    ImGui::EndChild();
    if (done) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void AnimationWindowClip::drawRebindPopup(){
    if (openRebind){
        openRebind = false;
        ImGui::OpenPopup("Rebind##clip");
    }
    if (!ImGui::BeginPopup("Rebind##clip")) return;
    std::shared_ptr<AnimationClipDocument> doc = document();
    Scene* scene = valueScene();
    const Entity root = valueRoot();
    if (!doc || rebindTrack < 0 || rebindTrack >= (int)doc->data().tracks.size()){
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const ClipTrackData& track = doc->data().tracks[(size_t)rebindTrack];
    ImGui::Text("Rebind %s", ClipValidation::trackLabel(track).c_str());
    const size_t users = findClipUsers(project, doc->path()).size();
    ImGui::TextColored(Theme::Colors::WarningText, "This rewrites the path in the clip: it changes the clip for %zu %s in open scenes.",
                       users, users == 1 ? "user" : "users");
    if (!scene || root == NULL_ENTITY){
        ImGui::TextColored(Theme::Colors::SubtleText, "Bind a GameObject (or preview on the clip's model) to pick the new path.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(Theme::dpi(380.0f));
    ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS " Search", rebindSearch, sizeof(rebindSearch));
    const std::string needle = rebindSearch;
    std::vector<std::string> candidates = ClipValidation::nearestPaths(scene, root, track.path, 5);
    const size_t nearestCount = candidates.size();
    for (const auto& entry : anim::descendantPaths(scene, root)){
        if (std::find(candidates.begin(), candidates.end(), entry.first) == candidates.end()) candidates.push_back(entry.first);
    }
    ImGui::BeginChild("##paths", ImVec2(Theme::dpi(380.0f), Theme::dpi(260.0f)), ImGuiChildFlags_Borders);
    std::string error;
    for (size_t i = 0; i < candidates.size(); i++){
        const std::string& path = candidates[i];
        if (!needle.empty() && path.find(needle) == std::string::npos) continue;
        if (i == nearestCount && nearestCount > 0 && needle.empty()) ImGui::Separator();
        const std::string label = (path.empty() ? std::string("(root)") : path) + (i < nearestCount ? "  (nearest)" : "");
        ImGui::PushID((int)i);
        if (ImGui::Selectable(label.c_str(), false)){
            if (!controller().retargetTrack((size_t)rebindTrack, path, &error)){
                setNotice(error, true);
            }else{
                controller().setLastActionWasClipEdit(true);
                setNotice("Rebound to '" + path + "' (changes the clip for " + std::to_string(users) +
                          (users == 1 ? " user" : " users") + ")", false);
            }
            reportRevision = ~uint64_t(0);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void AnimationWindowClip::handleShortcuts(){
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    std::string error;
    auto report = [&](bool ok){
        if (!ok && !error.empty()) setNotice(error, true);
        error.clear();
    };
    const bool plain = !io.KeyCtrl && !io.KeyAlt && !io.KeySuper;
    if (plain && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay();
    if (plain && ImGui::IsKeyPressed(ImGuiKey_K, false)){
        const bool ok = io.KeyShift ? c.keyModified(c.getTime(), &error) : c.keyAll(c.getTime(), &error);
        if (ok) c.setLastActionWasClipEdit(true);
        report(ok);
    }
    if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Comma, true)){
        if (io.KeyAlt) stepKey(-1);
        else stepFrame(-1);
    }
    if (!io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Period, true)){
        if (io.KeyAlt) stepKey(1);
        else stepFrame(1);
    }
    if (plain && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))){
        if (!selectedRefs.empty()) report(deleteSelected(&error));
        else if (selectedEvent >= 0) report(deleteEvent(selectedEvent, &error));
    }
    if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt){
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelected();
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) report(paste(c.getTime(), &error));
        if (ImGui::IsKeyPressed(ImGuiKey_D, false)) report(duplicateSelected(&error));
    }
    const float width = std::max(1.0f, ImGui::GetWindowWidth() - sheetMetrics().labelWidth);
    if (plain && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false)) frameKeys(true, width);
    if (plain && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false)) frameKeys(false, width);
}
