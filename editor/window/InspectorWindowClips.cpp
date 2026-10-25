// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's animation clip sections (animation clips spec 4.2, 9.R7/R8/R14/R30/R45):
// - the Animation Player section (clip list, default clip, settings, Bindings foldout, the live
//   states while playing);
// - the asset section of the inspected .anim (or of a model's read-only sub-clips): settings,
//   tracks, events, preview model, users, warnings; edits go through AnimationClipDocument (project
//   history);
// - the Model section's clip rows and the AnimationComponent section's clip buttons.
// Window actions (open, convert, extract, use player, create) are AnimationClipInspectorHooks:
// a button shows once its hook is set. Owner: T2c.

#include "InspectorWindow.h"
#include "InspectorWindowClips.h"

#include "animation/AnimationClipDocument.h"
#include "animation/AnimationClipInspectorHooks.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipValidation.h"
#include "animation/ModelSubClips.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/AnimationComponent.h"
#include "component/ModelComponent.h"
#include "external/IconsFontAwesome6.h"
#include "object/AnimationPlayer.h"
#include "util/FileDialogs.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "Out.h"
#include "Theme.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <unordered_set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

// ---- pure helpers (InspectorWindowClips.h) ----

bool inspectorclips::isSubClip(const std::string& clip){
    return anim::isSubClipReference(clip);
}

std::string inspectorclips::clipFieldText(const std::string& clip){
    if (clip.empty()) return "";
    anim::SubClipReference reference;
    if (anim::parseSubClipReference(clip, reference)){
        return reference.clip;
    }
    return fs::path(clip).filename().generic_string();
}

std::string inspectorclips::missingClipText(const std::string& clip){
    std::string name = clipFieldText(clip);
    anim::SubClipReference reference;
    if (anim::parseSubClipReference(clip, reference)){
        name += " in " + fs::path(reference.model).filename().generic_string();
    }
    return "Missing (" + name + ")";
}

const char* inspectorclips::wrapLabel(ClipWrapMode wrap){
    switch (wrap){
        case ClipWrapMode::Default: return "Default";
        case ClipWrapMode::Once: return "Once";
        case ClipWrapMode::Loop: return "Loop";
        case ClipWrapMode::PingPong: return "Ping Pong";
        case ClipWrapMode::ClampForever: return "Clamp Forever";
    }
    return "Default";
}

std::string inspectorclips::formatSeconds(float seconds){
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f s", std::isfinite(seconds) ? seconds : 0.0f);
    return buffer;
}

std::string inspectorclips::usersText(size_t count){
    return std::to_string(count) + (count == 1 ? " user" : " users") + " in open scenes";
}

bool inspectorclips::reportChecked(const ClipValidationReport& report){
    for (const TrackReport& track : report.tracks){
        if (track.status != TrackBindingStatus::Unchecked) return true;
    }
    for (const EventReport& event : report.events){
        if (event.status != EventHandlerStatus::Unchecked) return true;
    }
    return false;
}

std::string inspectorclips::bindingsSummary(const ClipValidationReport& report){
    if (report.legacyModelLayout){
        return "legacy model layout";
    }
    if (!reportChecked(report)){
        return "Unchecked";
    }
    return std::to_string(report.resolved) + "/" + std::to_string(report.total) + " bound";
}

bool inspectorclips::trackHasProblem(const TrackReport& track){
    return track.status != TrackBindingStatus::Ok && track.status != TrackBindingStatus::Unchecked &&
           track.status != TrackBindingStatus::NotSampled;
}

std::string inspectorclips::trackProblemText(const TrackReport& track){
    std::string text = (track.path.empty() ? std::string("(root)") : track.path) + " : " + anim::channelName(track.channel) + ": ";
    std::string why = track.message;
    if (why.empty()){
        switch (track.status){
            case TrackBindingStatus::MissingPath: why = "no GameObject at this path"; break;
            case TrackBindingStatus::DuplicateSiblingName: why = "several siblings have this name"; break;
            case TrackBindingStatus::MissingComponent: why = "the GameObject has nothing this channel animates"; break;
            case TrackBindingStatus::LatticeIndexOutOfRange: why = "lattice points outside the cage"; break;
            case TrackBindingStatus::MorphIndexOutOfRange: why = "more weights than the mesh has morph targets"; break;
            case TrackBindingStatus::LegacyModelLayout: why = "the model is in the legacy layout"; break;
            case TrackBindingStatus::UnaddressableName: why = "a name on the path can't be addressed"; break;
            default: why = "not bound"; break;
        }
    }
    text += why;
    if (!track.nearest.empty() && why.find("did you mean") == std::string::npos){
        text += " (did you mean ";
        for (size_t i = 0; i < track.nearest.size(); i++){
            if (i > 0) text += i + 1 == track.nearest.size() ? " or " : ", ";
            text += track.nearest[i].empty() ? std::string("(root)") : track.nearest[i];
        }
        text += "?)";
    }
    return text;
}

std::string inspectorclips::eventProblemText(const EventReport& event, float time){
    if (!event.message.empty()){
        return "Event " + event.function + " at " + formatSeconds(time) + ": " + event.message;
    }
    return "Event " + event.function + " at " + formatSeconds(time) + ": no script on the root defines " + event.function + "()";
}

std::string inspectorclips::eventArgsText(const std::vector<AnimationEventArg>& args){
    std::string text;
    for (const AnimationEventArg& arg : args){
        if (!text.empty()) text += ", ";
        if (const double* number = std::get_if<double>(&arg)){
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", *number);
            text += buffer;
        }else if (const std::string* string = std::get_if<std::string>(&arg)){
            text += "\"" + *string + "\"";
        }else if (const bool* boolean = std::get_if<bool>(&arg)){
            text += *boolean ? "true" : "false";
        }
    }
    return text;
}

std::string inspectorclips::modelAnimationRowLabel(const std::string& entityName, const std::vector<std::string>& subClipNames,
                                                   size_t index, const std::string& fallback){
    if (index < subClipNames.size() && !entityName.empty()){
        const std::string& name = subClipNames[index];
        // the loader names the entity after the glTF animation; its sub-clip is that name or,
        // for a later duplicate, "<name> <k>"
        if (name == entityName ||
                (name.size() > entityName.size() + 1 && name.compare(0, entityName.size(), entityName) == 0 &&
                 name[entityName.size()] == ' ' &&
                 std::all_of(name.begin() + (long)entityName.size() + 1, name.end(), [](unsigned char c){ return std::isdigit(c) != 0; }))){
            return name;
        }
    }
    return entityName.empty() ? fallback : entityName;
}

std::vector<Entity> inspectorclips::topLevelEntities(EntityRegistry* registry, const std::vector<Entity>& entities){
    std::vector<Entity> roots;
    if (!registry) return roots;
    std::unordered_set<Entity> listed;
    for (Entity entity : entities){
        if (entity != NULL_ENTITY && registry->isEntityCreated(entity)) listed.insert(entity);
    }
    std::unordered_set<Entity> added;
    for (Entity entity : entities){
        if (!listed.count(entity) || added.count(entity)) continue;
        bool underListed = false;
        Transform* transform = registry->findComponent<Transform>(entity);
        Entity parent = transform ? transform->parent : NULL_ENTITY;
        // bounded by the entity count: a broken parent cycle can't loop forever
        for (size_t guard = 0; parent != NULL_ENTITY && guard < 100000; guard++){
            if (listed.count(parent)){
                underListed = true;
                break;
            }
            Transform* parentTransform = registry->isEntityCreated(parent) ? registry->findComponent<Transform>(parent) : nullptr;
            parent = parentTransform ? parentTransform->parent : NULL_ENTITY;
        }
        if (!underListed){
            roots.push_back(entity);
            added.insert(entity);
        }
    }
    return roots;
}

// ---- file helpers ----

namespace{

    constexpr double CLIP_RETRY_SECONDS = 1.0;
    constexpr size_t MAX_PROBLEM_ROWS = 24;

    // the asset-relative form (what components store) of a path inside the assets folder; empty
    // when it is outside
    std::string assetRelativePath(Project* project, const fs::path& path){
        const fs::path assetsPath = fs::absolute(project->getAssetsPath()).lexically_normal();
        const fs::path filePath = fs::absolute(project->resolveAssetPath(path)).lexically_normal();
        std::error_code errorCode;
        const fs::path relative = fs::relative(filePath, assetsPath, errorCode);
        if (errorCode || relative.empty() || *relative.begin() == ".."){
            return "";
        }
        return relative.generic_string();
    }

    // an .anim's GUID (clips[i].guid follows clips[i].clip): the database's, else the file's
    std::string clipGuidOf(Project* project, const std::string& stored){
        if (stored.empty() || anim::isSubClipReference(stored)) return "";
        const fs::path full = project->resolveAssetPath(stored);
        std::string guid = project->getAssetDatabase().getGuid(full);
        if (guid.empty()){
            std::ifstream file(full, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            guid = anim::readClipGuid(text);
        }
        return guid;
    }

    // the clip's file (the model of a sub-clip) is on disk
    bool clipFileExists(Project* project, const std::string& clip){
        if (clip.empty()) return false;
        anim::SubClipReference reference;
        const std::string file = anim::parseSubClipReference(clip, reference) ? reference.model : clip;
        std::error_code ec;
        return fs::is_regular_file(project->resolveAssetPath(file), ec);
    }

    // a file field like the Model and Pose Animator ones: the name right-aligned on the label colour
    void drawFileLabel(const char* id, const std::string& text, const std::string& tooltip, const ImVec4* color, float width, const char* suffix = nullptr){
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);
        ImGui::BeginChild(id, ImVec2(std::max(width, 1.0f), ImGui::GetFrameHeight()), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        // a narrow field shows the tag as a lock (the tooltip names the reference)
        if (suffix && ImGui::CalcTextSize(text.c_str()).x + ImGui::CalcTextSize(suffix).x + ImGui::GetStyle().ItemSpacing.x + 4 >
                ImGui::GetContentRegionAvail().x){
            suffix = ICON_FA_LOCK;
        }
        const float suffixWidth = suffix ? ImGui::CalcTextSize(suffix).x + ImGui::GetStyle().ItemSpacing.x : 0.0f;
        const float textWidth = ImGui::CalcTextSize(text.c_str()).x + suffixWidth;
        ImGui::SetCursorPosX(std::max(0.0f, ImGui::GetContentRegionAvail().x - textWidth - 2));
        ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
        if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
        ImGui::TextUnformatted(text.c_str());
        if (color) ImGui::PopStyleColor();
        if (suffix){
            ImGui::SameLine();
            ImGui::TextDisabled("%s", suffix);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (!tooltip.empty()){
            ImGui::SetItemTooltip("%s", tooltip.c_str());
        }
    }

    float iconButtonWidth(const char* icon){
        return ImGui::CalcTextSize(icon).x + ImGui::GetStyle().FramePadding.x * 2;
    }

    // the .anim and model files a "resource_files" payload carries (other files are ignored)
    std::vector<std::string> clipFilesOf(const ImGuiPayload* payload){
        std::vector<std::string> files;
        for (const std::string& file : Util::getStringsFromPayload(payload)){
            if (Util::isAnimationClipFile(file) || Util::isModelFile(file)) files.push_back(file);
        }
        return files;
    }

}

// ---- clip documents and model clips, cached per frame ----

std::shared_ptr<AnimationClipDocument> editor::InspectorWindow::findClipDocument(const std::string& clip, std::string* error){
    if (clip.empty()){
        if (error) *error = "no clip";
        return nullptr;
    }
    if (clipCacheProject != project->getProjectPath()){
        // another project: its documents are gone (AnimationClipDocument::closeAll)
        clipDocuments.clear();
        modelClipsCache.clear();
        clipUsersCache.clear();
        bindingsCache.clear();
        clipCacheProject = project->getProjectPath();
    }
    const double now = ImGui::GetTime();
    ClipDocumentEntry& entry = clipDocuments[clip];
    if (std::shared_ptr<AnimationClipDocument> document = entry.document.lock()){
        if (document->exists()){
            return document;
        }
    }else if (!entry.error.empty() && now < entry.retryAt){
        if (error) *error = entry.error;
        return nullptr;
    }
    std::string why;
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, clip, &why);
    entry.document = document;
    entry.error = document ? std::string() : (why.empty() ? std::string("can't open the clip") : why);
    entry.retryAt = now + CLIP_RETRY_SECONDS;
    if (!document && error) *error = entry.error;
    if (clipDocuments.size() > 256){
        clipDocuments.clear();
    }
    return document;
}

const std::vector<anim::ModelClipInfo>& editor::InspectorWindow::modelClipsOf(const std::string& model){
    static const std::vector<anim::ModelClipInfo> none;
    if (model.empty()) return none;
    const double now = ImGui::GetTime();
    std::error_code ec;
    const fs::path full = project->resolveAssetPath(model);
    const int64_t stamp = fs::is_regular_file(full, ec) ? (int64_t)fs::last_write_time(full, ec).time_since_epoch().count() : 0;
    auto it = modelClipsCache.find(model);
    if (it != modelClipsCache.end() && it->second.stamp == stamp && (now < it->second.retryAt || !it->second.failed)){
        return it->second.clips;
    }
    if (modelClipsCache.size() > 64){
        modelClipsCache.clear();
    }
    ModelClipsEntry& entry = modelClipsCache[model];
    entry.stamp = stamp;
    entry.retryAt = now + CLIP_RETRY_SECONDS;
    entry.clips.clear();
    entry.failed = !stamp || !anim::listModelFileClips(model, entry.clips, nullptr);
    return entry.clips;
}

const std::vector<ClipUser>& editor::InspectorWindow::clipUsersOf(const std::string& clip){
    const double now = ImGui::GetTime();
    auto it = clipUsersCache.find(clip);
    if (it == clipUsersCache.end() || now >= it->second.retryAt){
        if (clipUsersCache.size() > 64) clipUsersCache.clear();
        ClipUsersEntry& entry = clipUsersCache[clip];
        entry.users = findClipUsers(project, clip);
        entry.retryAt = now + CLIP_RETRY_SECONDS;
        return entry.users;
    }
    return it->second.users;
}

const ClipValidationReport& editor::InspectorWindow::clipBindingsOf(SceneProject* sceneProject, Entity root, const std::string& clip,
                                                                   AnimationClipDocument& document){
    const std::string key = std::to_string(sceneProject->id) + ":" + std::to_string(root) + ":" + clip;
    Scene* scene = sceneProject->scene;
    const uint64_t hierarchy = scene->hierarchyRevision();
    const uint64_t names = scene->nameRevision();
    const double now = ImGui::GetTime();
    auto it = bindingsCache.find(key);
    if (it != bindingsCache.end() && it->second.revision == document.revision() && it->second.hierarchy == hierarchy &&
            it->second.names == names && now < it->second.retryAt){
        return it->second.report;
    }
    if (bindingsCache.size() > 64){
        bindingsCache.clear();
    }
    BindingsEntry& entry = bindingsCache[key];
    entry.revision = document.revision();
    entry.hierarchy = hierarchy;
    entry.names = names;
    // the scripts on the root may change without a hierarchy change (handlers)
    entry.retryAt = now + CLIP_RETRY_SECONDS;
    entry.report = ClipValidation::validateInScene(project, sceneProject, document.data(), root);
    return entry.report;
}

// ---- Animation Player section ----

void editor::InspectorWindow::setPlayerClipEntry(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index, const std::string& stored){
    AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return;
    if (index >= player->clips.size()){
        // a new entry
        std::vector<AnimationPlayerClip> clips = player->clips;
        AnimationPlayerClip added;
        added.clip = stored;
        added.guid = clipGuidOf(project, stored);
        clips.push_back(added);
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
            new PropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneProject->id, entity, cpType, "clips", clips));
        return;
    }
    // the GUID follows the path (the old one would win on the next load, R8)
    const std::string prefix = "clips[" + std::to_string(index) + "]";
    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
    multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, prefix + ".clip", stored);
    multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, prefix + ".guid", clipGuidOf(project, stored));
    multiCmd->setNoMerge();
    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
}

// A picked or dropped file for entry `index` (SIZE_MAX: a new entry): an .anim is set at once, a
// model asks which of its animations (the "Model Clip" popup). False when it can't be used.
bool editor::InspectorWindow::usePlayerClipFile(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index, const std::string& file){
    const std::string relative = assetRelativePath(project, fs::path(file));
    if (relative.empty()){
        playerClipError = "The clip must be an .anim file or a model (.gltf, .glb, .fbx) inside the assets folder.";
        return false;
    }
    if (Util::isAnimationClipFile(relative)){
        setPlayerClipEntry(cpType, sceneProject, entity, index, relative);
        return true;
    }
    if (Util::isModelFile(relative)){
        if (modelClipsOf(relative).empty()){
            playerClipError = "'" + relative + "' has no animations.";
            return false;
        }
        playerSubClipModel = relative;
        playerSubClipEntry = index;
        playerSubClipEntity = entity;
        playerSubClipScene = sceneProject->id;
        playerSubClipPopupRequested = true;
        return true;
    }
    playerClipError = "The clip must be an .anim file or a model (.gltf, .glb, .fbx) inside the assets folder.";
    return false;
}

void editor::InspectorWindow::drawAnimationPlayerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings defaultClipSettings;
    defaultClipSettings.help = "The state that plays automatically; empty = the first clip.";
    RowSettings autoSettings;
    autoSettings.help = "The default clip plays when the game starts (or the GameObject is instantiated).";
    RowSettings speedSettings;
    speedSettings.secondColSize = 6 * ImGui::GetFontSize();
    speedSettings.help = "Multiplies the speed of every state; negative plays backwards.";
    RowSettings fadeSettings;
    fadeSettings.secondColSize = 6 * ImGui::GetFontSize();
    fadeSettings.help = "Seconds of the crossfade when play(name, fade), crossFade(name) or Model:playAnimation give none.";

    if (entities.size() == 1){
        drawAnimationPlayerClips(cpType, sceneProject, entities[0]);
    }else{
        beginTable(cpType, getLabelSize("Play Automatically"), "player_clips_multi");
        propertyHeader("Clips");
        ImGui::TextDisabled("Select a single GameObject to edit the clips");
        endTable();
    }

    beginTable(cpType, getLabelSize("Play Automatically"), "player_settings");
    if (entities.size() == 1){
        drawPlayerDefaultClipRow(cpType, sceneProject, entities[0]);
    }else{
        propertyRow(RowPropertyType::String, cpType, "defaultClip", "Default Clip", sceneProject, entities, defaultClipSettings);
    }
    propertyRow(RowPropertyType::Bool, cpType, "playAutomatically", "Play Automatically", sceneProject, entities, autoSettings);
    propertyRow(RowPropertyType::Float, cpType, "speed", "Speed", sceneProject, entities, speedSettings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "defaultFadeTime", "Default Fade", sceneProject, entities, fadeSettings);
    endTable();

    if (entities.size() != 1){
        return;
    }
    drawAnimationPlayerBindings(sceneProject, entities[0]);
    if (sceneProject->playState != ScenePlayState::STOPPED){
        drawAnimationPlayerStates(cpType, sceneProject, entities[0]);
    }
}

void editor::InspectorWindow::drawAnimationPlayerClips(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    Scene* scene = sceneProject->scene;
    AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return;
    // a copy: the commands below change the component while the rows are drawn
    const std::vector<AnimationPlayerClip> clips = player->clips;
    const std::vector<std::string> states = anim::playerStateNames(clips);
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    const AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();

    ImGui::SeparatorText("Clips");

    // count, + Add Clip (a drop target too: dropped clips are appended), Create New Clip...
    const float labelWidth = getLabelSize("Play Automatically");
    beginTable(cpType, labelWidth, "player_clips_header");
    prefabRowProperty = "clips";
    propertyHeader("Clips", -1, false, false);
    prefabRowProperty.clear();
    ImGui::Text("%zu", clips.size());
    const char* createLabel = "Create New Clip...##player_clip_create";
    const float createWidth = hooks.createNewClip ? ImGui::CalcTextSize("Create New Clip...").x + ImGui::GetStyle().FramePadding.x * 2 +
                                                    ImGui::GetStyle().ItemSpacing.x : 0.0f;
    if (drawSummaryAddButton(ICON_FA_PLUS " Add Clip##player_clip_add", createWidth)){
        std::vector<AnimationPlayerClip> newClips = clips;
        newClips.push_back(AnimationPlayerClip());
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
            new PropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneProject->id, entity, cpType, "clips", newClips));
    }
    UIProbe::item("player.addClip");
    ImGui::SetItemTooltip("Add an entry to the clip list (or drop .anim files here)");
    if (stopped && ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            const std::vector<std::string> files = clipFilesOf(payload);
            // several .anim files: one undo step
            std::vector<AnimationPlayerClip> newClips = clips;
            bool added = false;
            for (const std::string& file : files){
                const std::string relative = assetRelativePath(project, fs::path(file));
                if (!relative.empty() && Util::isAnimationClipFile(relative)){
                    AnimationPlayerClip entry;
                    entry.clip = relative;
                    entry.guid = clipGuidOf(project, relative);
                    newClips.push_back(entry);
                    added = true;
                }
            }
            if (added){
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new PropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneProject->id, entity, cpType, "clips", newClips));
            }else if (!files.empty()){
                usePlayerClipFile(cpType, sceneProject, entity, SIZE_MAX, files[0]);
            }
        }
        ImGui::EndDragDropTarget();
    }
    if (hooks.createNewClip){
        ImGui::SameLine();
        ImGui::BeginDisabled(!stopped);
        if (ImGui::Button(createLabel)){
            hooks.createNewClip(project, sceneProject->id, entity);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Create an .anim file and add it to this Animation Player (one undo step)");
    }
    endTable();

    if (!clips.empty()){
        const ImVec4 warning = Theme::Colors::WarningText;
        beginTable(cpType, labelWidth, "player_clips");
        for (size_t i = 0; i < clips.size(); i++){
            const AnimationPlayerClip& entry = clips[i];
            const std::string prefix = "clips[" + std::to_string(i) + "]";
            const bool subClip = inspectorclips::isSubClip(entry.clip);
            const bool exists = clipFileExists(project, entry.clip);
            ImGui::PushID((int)i);

            // state name | file field, picker, clear, open, remove
            prefabRowProperty = prefix + ".clip";
            propertyHeader(states[i].empty() ? "Clip " + std::to_string(i) : states[i], -1, false, false);
            prefabRowProperty.clear();

            const char* openIcon = ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE;
            float buttons = iconButtonWidth(ICON_FA_FOLDER_OPEN) + iconButtonWidth(ICON_FA_XMARK) + iconButtonWidth(ICON_FA_TRASH_CAN) +
                            ImGui::GetStyle().ItemSpacing.x * 3;
            if (hooks.openClip){
                buttons += iconButtonWidth(openIcon) + ImGui::GetStyle().ItemSpacing.x;
            }
            const float fieldWidth = ImGui::GetContentRegionAvail().x - buttons;

            ImGui::BeginGroup();
            if (entry.clip.empty()){
                const ImVec4 disabled = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                drawFileLabel("clipfile", "< None >", "", &disabled, fieldWidth);
            }else if (!exists){
                // R8: a deleted clip keeps its reference (undoing the delete relinks it)
                drawFileLabel("clipfile", inspectorclips::missingClipText(entry.clip), entry.clip, &warning, fieldWidth);
            }else{
                drawFileLabel("clipfile", inspectorclips::clipFieldText(entry.clip),
                              subClip ? entry.clip + " (Read-Only: Extract to .anim to edit it)" : entry.clip, nullptr, fieldWidth,
                              subClip ? "(Read-Only)" : nullptr);
            }
            ImGui::EndGroup();
            if (stopped && ImGui::BeginDragDropTarget()){
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                    const std::vector<std::string> files = clipFilesOf(payload);
                    if (!files.empty()){
                        usePlayerClipFile(cpType, sceneProject, entity, i, files[0]);
                    }
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "##clip_pick")){
                const std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(),
                                                                             FILE_DIALOG_ANIMATION_CLIP | FILE_DIALOG_MODEL);
                if (!path.empty()){
                    usePlayerClipFile(cpType, sceneProject, entity, i, path);
                }
            }
            ImGui::SetItemTooltip("Choose an .anim file or a model's animation");
            ImGui::SameLine();
            ImGui::BeginDisabled(entry.clip.empty() && entry.guid.empty());
            if (ImGui::Button(ICON_FA_XMARK "##clip_clear")){
                setPlayerClipEntry(cpType, sceneProject, entity, i, "");
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("None: keep the entry without a clip");
            if (hooks.openClip){
                ImGui::SameLine();
                ImGui::BeginDisabled(!exists || !stopped);
                if (ImGui::Button((std::string(openIcon) + "##clip_open").c_str())){
                    hooks.openClip(project, sceneProject->id, entity, entry.clip);
                }
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Open in the Animation window");
            }
            ImGui::SameLine();
            bool removed = false;
            if (ImGui::Button(ICON_FA_TRASH_CAN "##clip_remove")){
                std::vector<AnimationPlayerClip> newClips = clips;
                newClips.erase(newClips.begin() + (long)i);
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new PropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneProject->id, entity, cpType, "clips", newClips));
                removed = true;
            }
            ImGui::SetItemTooltip("Remove the clip from the list");
            if (removed){
                ImGui::PopID();
                break;
            }

            // state name (empty = the file / sub-clip name)
            RowSettings nameSettings;
            nameSettings.indent = true;
            nameSettings.help = "The state name scripts play; empty = \"" + anim::defaultStateName(entry) + "\".";
            propertyRow(RowPropertyType::String, cpType, prefix + ".name", "Name", sceneProject, {entity}, nameSettings);

            // wrap: a one-byte enum (PropertyCmd<ClipWrapMode>, never the int Enum row)
            ClipWrapMode clipWrap = ClipWrapMode::Once;
            bool clipWrapKnown = false;
            if (exists){
                if (std::shared_ptr<AnimationClipDocument> document = findClipDocument(entry.clip, nullptr)){
                    clipWrap = document->data().wrap;
                    clipWrapKnown = true;
                }
            }
            const std::string defaultLabel = clipWrapKnown ? std::string("Default (") + inspectorclips::wrapLabel(clipWrap) + ")" : std::string("Default");
            prefabRowProperty = prefix + ".wrap";
            if (propertyHeader("Wrap", -1, entry.wrap != ClipWrapMode::Default, false, true)){
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new PropertyCmd<ClipWrapMode>(project, sceneProject->id, entity, cpType, prefix + ".wrap", ClipWrapMode::Default));
            }
            prefabRowProperty.clear();
            const std::string current = entry.wrap == ClipWrapMode::Default ? defaultLabel : inspectorclips::wrapLabel(entry.wrap);
            if (ImGui::BeginCombo("##clip_wrap", current.c_str())){
                for (ClipWrapMode wrap : {ClipWrapMode::Default, ClipWrapMode::Once, ClipWrapMode::Loop, ClipWrapMode::PingPong, ClipWrapMode::ClampForever}){
                    const std::string label = wrap == ClipWrapMode::Default ? defaultLabel : inspectorclips::wrapLabel(wrap);
                    if (ImGui::Selectable(label.c_str(), wrap == entry.wrap) && wrap != entry.wrap){
                        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                            new PropertyCmd<ClipWrapMode>(project, sceneProject->id, entity, cpType, prefix + ".wrap", wrap));
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("How this state wraps: Default uses the clip's own wrap mode");

            ImGui::PopID();
        }
        endTable();
    }

    // a model was picked or dropped: which of its animations
    if (playerSubClipPopupRequested){
        playerSubClipPopupRequested = false;
        ImGui::OpenPopup("Model Clip##player_subclip");
    }
    if (ImGui::BeginPopup("Model Clip##player_subclip")){
        ImGui::TextDisabled("%s", fs::path(playerSubClipModel).filename().string().c_str());
        ImGui::Separator();
        for (const anim::ModelClipInfo& info : modelClipsOf(playerSubClipModel)){
            const std::string label = info.name + "##subclip" + std::to_string(info.index);
            if (ImGui::Selectable(label.c_str())){
                if (playerSubClipScene == sceneProject->id && playerSubClipEntity == entity){
                    setPlayerClipEntry(cpType, sceneProject, entity, playerSubClipEntry,
                                       anim::formatSubClipReference(playerSubClipModel, info.name));
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", inspectorclips::formatSeconds(info.duration).c_str());
        }
        ImGui::EndPopup();
    }

    if (!playerClipError.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", playerClipError.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK "##player_clip_error")){
            playerClipError.clear();
        }
    }
}

void editor::InspectorWindow::drawPlayerDefaultClipRow(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return;
    const std::string defaultClip = player->defaultClip;
    const std::vector<std::string> states = anim::playerStateNames(player->clips);

    prefabRowProperty = "defaultClip";
    if (propertyHeader("Default Clip", -1, !defaultClip.empty())){
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
            new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "defaultClip", ""));
    }
    prefabRowProperty.clear();

    const bool known = defaultClip.empty() || std::find(states.begin(), states.end(), defaultClip) != states.end();
    const std::string firstLabel = states.empty() ? std::string("None") : "First Clip (" + states[0] + ")";
    const std::string preview = defaultClip.empty() ? firstLabel : (known ? defaultClip : defaultClip + " (not in the list)");
    if (!known) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
    const bool open = ImGui::BeginCombo("##player_default_clip", preview.c_str());
    if (!known) ImGui::PopStyleColor();
    if (open){
        if (ImGui::Selectable(firstLabel.c_str(), defaultClip.empty()) && !defaultClip.empty()){
            CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "defaultClip", ""));
        }
        for (size_t i = 0; i < states.size(); i++){
            const std::string label = states[i] + "##default" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), states[i] == defaultClip) && states[i] != defaultClip){
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new PropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "defaultClip", states[i]));
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The state that plays automatically; empty = the first clip");
}

void editor::InspectorWindow::drawAnimationPlayerBindings(SceneProject* sceneProject, Entity entity){
    AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player || player->clips.empty()) return;
    const std::vector<AnimationPlayerClip> clips = player->clips;
    const std::vector<std::string> states = anim::playerStateNames(clips);
    const ImVec4 warning = Theme::Colors::WarningText;

    const bool bindingsOpen = ImGui::TreeNodeEx("Bindings##player_bindings", ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    UIProbe::item("player.bindings");
    if (!bindingsOpen){
        return;
    }
    for (size_t i = 0; i < clips.size(); i++){
        const AnimationPlayerClip& entry = clips[i];
        ImGui::PushID((int)i);
        if (entry.clip.empty()){
            ImGui::TextDisabled("%s: no clip", states[i].c_str());
            ImGui::PopID();
            continue;
        }
        if (!clipFileExists(project, entry.clip)){
            ImGui::TextColored(warning, "%s: %s", states[i].c_str(), inspectorclips::missingClipText(entry.clip).c_str());
            ImGui::PopID();
            continue;
        }
        std::string error;
        std::shared_ptr<AnimationClipDocument> document = findClipDocument(entry.clip, &error);
        if (!document){
            ImGui::TextColored(Theme::Colors::ErrorText, "%s: %s", states[i].c_str(), error.c_str());
            ImGui::PopID();
            continue;
        }
        const ClipValidationReport& report = clipBindingsOf(sceneProject, entity, entry.clip, *document);
        const bool checked = inspectorclips::reportChecked(report);
        const std::string summary = inspectorclips::bindingsSummary(report);
        const bool problems = !report.ok() || report.legacyModelLayout;
        if (problems){
            ImGui::TextColored(warning, ICON_FA_TRIANGLE_EXCLAMATION " %s: %s", states[i].c_str(), summary.c_str());
        }else if (!checked){
            ImGui::TextDisabled("%s: %s", states[i].c_str(), summary.c_str());
            ImGui::SetItemTooltip("The bindings are not checked yet");
        }else{
            ImGui::Text("%s: %s", states[i].c_str(), summary.c_str());
        }

        ImGui::Indent();
        size_t shown = 0;
        size_t hidden = 0;
        if (report.legacyModelLayout){
            ImGui::PushStyleColor(ImGuiCol_Text, warning);
            ImGui::TextWrapped("A model under this GameObject is loaded without its node hierarchy: reload it to bind the clip's paths.");
            ImGui::PopStyleColor();
        }
        for (const TrackReport& track : report.tracks){
            if (!inspectorclips::trackHasProblem(track) || (report.legacyModelLayout && track.status == TrackBindingStatus::LegacyModelLayout)){
                continue;
            }
            if (shown >= MAX_PROBLEM_ROWS){
                hidden++;
                continue;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, warning);
            ImGui::TextWrapped("%s", inspectorclips::trackProblemText(track).c_str());
            ImGui::PopStyleColor();
            shown++;
        }
        const std::vector<ClipEventData>& events = document->data().events;
        for (const EventReport& event : report.events){
            if (event.status != EventHandlerStatus::NotFound) continue;
            if (shown >= MAX_PROBLEM_ROWS){
                hidden++;
                continue;
            }
            const float time = event.event < events.size() ? events[event.event].time : 0.0f;
            ImGui::PushStyleColor(ImGuiCol_Text, warning);
            ImGui::TextWrapped("%s", inspectorclips::eventProblemText(event, time).c_str());
            ImGui::PopStyleColor();
            shown++;
        }
        for (const ClipWarning& clipWarning : report.warnings){
            if (shown >= MAX_PROBLEM_ROWS){
                hidden++;
                continue;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, warning);
            ImGui::TextWrapped("%s%s%s", clipWarning.path.c_str(), clipWarning.path.empty() ? "" : ": ", clipWarning.message.c_str());
            ImGui::PopStyleColor();
            shown++;
        }
        if (hidden > 0){
            ImGui::TextDisabled("... and %zu more", hidden);
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void editor::InspectorWindow::drawAnimationPlayerStates(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    Scene* scene = sceneProject->scene;
    if (!scene->findComponent<AnimationPlayerComponent>(entity)) return;
    const std::vector<std::string> states = anim::playerStateNames(scene->getComponent<AnimationPlayerComponent>(entity).clips);
    if (states.empty()) return;

    ImGui::SeparatorText("States");
    UIProbe::item("player.states");
    // the component exists: the object adds none
    AnimationPlayer playerObject(scene, entity);
    beginTable(cpType, getLabelSize("Play Automatically"), "player_states");
    for (size_t i = 0; i < states.size(); i++){
        const AnimationStateInfo info = playerObject.getStateInfo(states[i]);
        ImGui::PushID((int)i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (info.playing){
            ImGui::TextUnformatted(ICON_FA_PLAY);
        }else{
            ImGui::TextDisabled(ICON_FA_PAUSE);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(states[i].c_str());
        ImGui::TableNextColumn();
        if (!info.error.empty()){
            ImGui::TextColored(Theme::Colors::ErrorText, "%s", info.error.c_str());
        }else if (!info.loaded){
            ImGui::TextDisabled("Loading...");
        }else{
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.2f / %.2f s", info.time, info.length);
            const float weightWidth = ImGui::CalcTextSize("w 0.00").x + ImGui::GetStyle().ItemSpacing.x;
            const float fraction = info.length > 0.0f ? std::clamp(info.time / info.length, 0.0f, 1.0f) : 0.0f;
            ImGui::ProgressBar(fraction, ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - weightWidth), 0), overlay);
            ImGui::SameLine();
            if (info.weight > 0.0f){
                ImGui::Text("w %.2f", info.weight);
            }else{
                ImGui::TextDisabled("w %.2f", info.weight);
            }
            ImGui::SetItemTooltip("Weight %.2f (target %.2f), speed %.2f, %s%s, %zu/%zu tracks bound", info.weight, info.targetWeight,
                                  info.speed, inspectorclips::wrapLabel(info.wrap), info.queued ? ", queued" : "",
                                  info.boundTracks, info.trackCount);
        }
        ImGui::PopID();
    }
    endTable();
}

// ---- the inspected asset (.anim, or a model's clips) ----

bool editor::InspectorWindow::drawInspectedAsset(){
    const fs::path asset = project->getInspectedAsset();
    if (asset.empty()) return false;
    std::error_code ec;
    if (!fs::is_regular_file(asset, ec)) return false;
    const std::string file = asset.string();
    if (Util::isAnimationClipFile(file)){
        drawClipAsset(asset);
        return true;
    }
    if (Util::isModelFile(file)){
        const std::string relative = assetRelativePath(project, asset);
        if (!relative.empty() && !modelClipsOf(relative).empty()){
            drawModelClipsAsset(asset, relative);
            return true;
        }
    }
    return false;
}

void editor::InspectorWindow::drawAssetHeader(const char* icon, const fs::path& asset, const char* kind){
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(icon);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    const std::string name = asset.filename().string();
    ImGui::TextUnformatted(name.c_str());
    UIProbe::item("asset.header");
    ImGui::SetItemTooltip("%s", project->normalizeToProjectRelative(asset).generic_string().c_str());
    const float kindWidth = ImGui::CalcTextSize(kind).x;
    ImGui::SameLine();
    const float space = ImGui::GetContentRegionAvail().x - kindWidth;
    if (space > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + space);
    ImGui::TextDisabled("%s", kind);
    ImGui::Separator();
}

void editor::InspectorWindow::drawClipAsset(const fs::path& asset){
    drawAssetHeader(ICON_FA_FILM, asset, "Animation Clip");
    std::string error;
    std::shared_ptr<AnimationClipDocument> document = findClipDocument(asset.generic_string(), &error);
    if (!document){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    const std::string reference = assetRelativePath(project, asset);
    drawClipDocument(*document, reference.empty() ? document->path() : reference);
}

void editor::InspectorWindow::drawModelClipsAsset(const fs::path& asset, const std::string& model){
    drawAssetHeader(ICON_FA_CUBE, asset, "Model");
    const std::vector<anim::ModelClipInfo> clips = modelClipsOf(model);

    ImGui::SeparatorText("Animation Clips");
    ImGui::TextDisabled("Read-only clips of the model: Extract to .anim to edit one.");
    if (std::none_of(clips.begin(), clips.end(), [&](const anim::ModelClipInfo& info){ return info.name == modelAssetClip; })){
        modelAssetClip = clips.empty() ? std::string() : clips[0].name;
    }
    if (ImGui::BeginTable("model_asset_clips", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)){
        ImGui::TableSetupColumn("Clip");
        ImGui::TableSetupColumn("Length", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("000.00 s").x + ImGui::GetStyle().CellPadding.x * 2);
        for (const anim::ModelClipInfo& info : clips){
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = info.name + "##model_asset_clip" + std::to_string(info.index);
            if (ImGui::Selectable(label.c_str(), info.name == modelAssetClip, ImGuiSelectableFlags_SpanAllColumns)){
                modelAssetClip = info.name;
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", inspectorclips::formatSeconds(info.duration).c_str());
        }
        ImGui::EndTable();
    }
    if (modelAssetClip.empty()) return;

    const std::string reference = anim::formatSubClipReference(model, modelAssetClip);
    ImGui::SeparatorText(modelAssetClip.c_str());
    std::string error;
    std::shared_ptr<AnimationClipDocument> document = findClipDocument(reference, &error);
    if (!document){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    drawClipDocument(*document, reference);
}

// The settings, tracks, events, users and warnings of one clip document. Edits commit through
// the document (one project undo step each; a drag merges into one).
void editor::InspectorWindow::drawClipDocument(AnimationClipDocument& document, const std::string& reference){
    const AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    const bool readOnly = document.isReadOnly();
    const bool playing = project->isAnyScenePlaying();
    // the document's data (a commit replaces it: edits copy it first)
    const ClipData& data = document.data();
    const ImVec4 warning = Theme::Colors::WarningText;

    auto commit = [&](const ClipData& newData, const std::string& undoName, const std::string& mergeKey){
        std::string error;
        if (!document.commit(newData, undoName, mergeKey, &error)){
            clipAssetError = error.empty() ? std::string("The clip could not be written") : error;
            Out::error("%s: %s", document.path().c_str(), clipAssetError.c_str());
        }else{
            clipAssetError.clear();
        }
    };

    // Open in Animation window, Extract to .anim (sub-clips)
    if (hooks.openClip || (readOnly && hooks.extractSubClip)){
        ImGui::BeginDisabled(playing);
        if (hooks.openClip){
            if (ImGui::Button(ICON_FA_FILM " Open in Animation Window##clip_asset_open")){
                hooks.openClip(project, 0, NULL_ENTITY, reference);
            }
            ImGui::SetItemTooltip("Edit the clip in the Animation window");
        }
        if (readOnly && hooks.extractSubClip){
            if (hooks.openClip) ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FILE_EXPORT " Extract to .anim##clip_asset_extract")){
                hooks.extractSubClip(project, 0, NULL_ENTITY, reference);
            }
            ImGui::SetItemTooltip("Write the clip to an editable .anim file (keeps its source)");
        }
        ImGui::EndDisabled();
    }
    if (readOnly){
        ImGui::TextDisabled(ICON_FA_LOCK " Read-only: a clip of %s", data.source.path.empty() ? "a model" : data.source.path.c_str());
    }

    ImGui::SeparatorText("Settings");
    const float labelWidth = getLabelSize("Preview Model");
    beginTable(ComponentType::AnimationPlayerComponent, labelWidth, "clip_asset_settings");
    ImGui::BeginDisabled(readOnly);

    // Frame Rate (Unity's Sample Rate): the window's frame grid; key times stay in seconds
    propertyHeader("Frame Rate");
    float frameRate = data.frameRate;
    if (ImGui::DragFloat("##clip_frame_rate", &frameRate, 0.5f, 1.0f, 240.0f, "%.0f fps", ImGuiSliderFlags_AlwaysClamp)){
        if (frameRate != data.frameRate && frameRate > 0.0f){
            ClipData newData = data;
            newData.frameRate = frameRate;
            commit(newData, "Clip Frame Rate", "inspector.frameRate#" + std::to_string(clipDragSerial));
        }
    }
    if (ImGui::IsItemDeactivated()) clipDragSerial++;
    ImGui::SetItemTooltip("Samples per second of the Animation window's frame grid (key times are kept in seconds)");

    propertyHeader("Wrap Mode");
    if (ImGui::BeginCombo("##clip_wrap_mode", inspectorclips::wrapLabel(data.wrap))){
        for (ClipWrapMode wrap : {ClipWrapMode::Once, ClipWrapMode::Loop, ClipWrapMode::PingPong, ClipWrapMode::ClampForever}){
            if (ImGui::Selectable(inspectorclips::wrapLabel(wrap), wrap == data.wrap) && wrap != data.wrap){
                ClipData newData = data;
                newData.wrap = wrap;
                commit(newData, "Clip Wrap Mode", "");
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("How the clip wraps when a player entry's wrap is Default");

    // Length: automatic (the last key or event) or explicit
    propertyHeader("Length");
    bool automatic = !data.hasExplicitLength();
    if (ImGui::Checkbox("Auto##clip_length_auto", &automatic)){
        ClipData newData = data;
        newData.length = automatic ? -1.0f : data.effectiveLength();
        commit(newData, automatic ? "Clip Length Auto" : "Clip Length", "");
    }
    UIProbe::item("clipAsset.lengthAuto");
    ImGui::SetItemTooltip("Auto: the clip ends at its last key or event");
    ImGui::SameLine();
    if (automatic){
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", inspectorclips::formatSeconds(data.effectiveLength()).c_str());
    }else{
        float length = data.length;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##clip_length", &length, 0.01f, 0.0f, 3600.0f, "%.3f s", ImGuiSliderFlags_AlwaysClamp)){
            if (length != data.length && length >= 0.0f){
                ClipData newData = data;
                newData.length = length;
                commit(newData, "Clip Length", "inspector.length#" + std::to_string(clipDragSerial));
            }
        }
        if (ImGui::IsItemDeactivated()) clipDragSerial++;
    }

    // Preview Model: what a target-less Animation window previews the clip on (a model or prefab)
    propertyHeader("Preview Model");
    {
        const float clearWidth = iconButtonWidth(ICON_FA_XMARK);
        const float pickWidth = iconButtonWidth(ICON_FA_FOLDER_OPEN);
        const float fieldWidth = ImGui::GetContentRegionAvail().x - clearWidth - pickWidth - ImGui::GetStyle().ItemSpacing.x * 2;
        const ImVec4 disabled = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
        auto setPreviewModel = [&](const std::string& file){
            const std::string relative = file.empty() ? std::string() : assetRelativePath(project, fs::path(file));
            if (!file.empty() && (relative.empty() || !(Util::isModelFile(relative) || Util::isPrefabFile(relative)))){
                clipAssetError = "The preview model must be a model or a prefab inside the assets folder.";
                return;
            }
            if (relative == data.previewModel) return;
            ClipData newData = data;
            newData.previewModel = relative;
            commit(newData, "Clip Preview Model", "");
        };
        ImGui::BeginGroup();
        if (data.previewModel.empty()){
            drawFileLabel("clip_preview_model", "< None >", "", &disabled, fieldWidth);
        }else{
            drawFileLabel("clip_preview_model", fs::path(data.previewModel).filename().string(), data.previewModel, nullptr, fieldWidth);
        }
        ImGui::EndGroup();
        if (!readOnly && ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                const std::vector<std::string> files = Util::getStringsFromPayload(payload);
                if (!files.empty()) setPreviewModel(files[0]);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##clip_preview_pick")){
            const std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_MODEL | FILE_DIALOG_PREFAB);
            if (!path.empty()) setPreviewModel(path);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(data.previewModel.empty());
        if (ImGui::Button(ICON_FA_XMARK "##clip_preview_clear")){
            setPreviewModel("");
        }
        ImGui::EndDisabled();
    }

    ImGui::EndDisabled();
    endTable();

    if (!clipAssetError.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", clipAssetError.c_str());
        ImGui::PopStyleColor();
    }

    // Tracks: path : channel, key count
    ImGui::SeparatorText(("Tracks (" + std::to_string(data.tracks.size()) + ")").c_str());
    if (data.tracks.empty()){
        ImGui::TextDisabled("No tracks: add properties in the Animation window.");
    }else if (ImGui::BeginTable("clip_asset_tracks", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                                ImVec2(0, std::min((float)data.tracks.size() + 1.0f, 12.0f) * ImGui::GetTextLineHeightWithSpacing() + 4))){
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Path : Channel");
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Keys 0000").x);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin((int)data.tracks.size());
        while (clipper.Step()){
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++){
                const ClipTrackData& track = data.tracks[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const std::string label = track.path.empty() ? std::string("(root) : ") + anim::channelName(track.channel) : ClipValidation::trackLabel(track);
                ImGui::TextUnformatted(label.c_str());
                if (track.channel == ClipChannel::Property){
                    ImGui::SameLine();
                    ImGui::TextDisabled("(not played)");
                }
                ImGui::TableNextColumn();
                ImGui::Text("%zu", track.keys.size());
            }
        }
        ImGui::EndTable();
    }

    // Events: time, function(args)
    ImGui::SeparatorText(("Events (" + std::to_string(data.events.size()) + ")").c_str());
    if (data.events.empty()){
        ImGui::TextDisabled("No events: add them on the Animation window's events lane.");
    }else if (ImGui::BeginTable("clip_asset_events", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)){
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0000.00 s").x);
        ImGui::TableSetupColumn("Function");
        for (const ClipEventData& event : data.events){
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(inspectorclips::formatSeconds(event.time).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s(%s)", event.function.c_str(), inspectorclips::eventArgsText(event.args).c_str());
        }
        ImGui::EndTable();
    }

    // Users: the Animation Player entries in open scenes and loaded prefabs (R45)
    const std::vector<ClipUser>& users = clipUsersOf(reference);
    ImGui::SeparatorText("Users");
    ImGui::TextUnformatted(inspectorclips::usersText(users.size()).c_str());
    ImGui::Indent();
    size_t listed = 0;
    for (const ClipUser& user : users){
        if (listed++ >= MAX_PROBLEM_ROWS){
            ImGui::TextDisabled("... and %zu more", users.size() - MAX_PROBLEM_ROWS);
            break;
        }
        SceneProject* userScene = user.sceneId ? project->getScene(user.sceneId) : nullptr;
        std::string where = user.prefabAsset ? fs::path(user.prefabPath).filename().string() : (userScene ? userScene->name : std::string("?"));
        std::string entityName;
        if (userScene && userScene->scene && userScene->scene->isEntityCreated(user.entity)){
            entityName = userScene->scene->getEntityName(user.entity);
        }
        ImGui::BulletText("%s: %s (clip %zu)", where.c_str(), entityName.empty() ? "GameObject" : entityName.c_str(), user.entry);
    }
    ImGui::Unindent();

    // Warnings of the last load and of the clip build
    std::vector<ClipWarning> warnings = document.loadWarnings();
    if (std::shared_ptr<const AnimationClip> clip = document.clip()){
        for (const ClipWarning& clipWarning : clip->getWarnings()){
            const bool known = std::any_of(warnings.begin(), warnings.end(), [&](const ClipWarning& other){
                return other.path == clipWarning.path && other.message == clipWarning.message;
            });
            if (!known) warnings.push_back(clipWarning);
        }
    }
    if (!warnings.empty()){
        ImGui::SeparatorText(("Warnings (" + std::to_string(warnings.size()) + ")").c_str());
        size_t shown = 0;
        for (const ClipWarning& clipWarning : warnings){
            if (shown++ >= MAX_PROBLEM_ROWS){
                ImGui::TextDisabled("... and %zu more", warnings.size() - MAX_PROBLEM_ROWS);
                break;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, warning);
            ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION " %s%s%s", clipWarning.path.c_str(), clipWarning.path.empty() ? "" : ": ",
                               clipWarning.message.c_str());
            ImGui::PopStyleColor();
        }
    }
}

// ---- Model section: the model file's clips (spec 4.2, 9.R15) ----

void editor::InspectorWindow::drawModelClipRows(ComponentType cpType, SceneProject* sceneProject, Entity entity){
    ModelComponent* model = sceneProject->scene->findComponent<ModelComponent>(entity);
    if (!model || model->filename.empty()) return;
    const std::string filename = model->filename;
    const bool animationEntities = model->animationEntities;
    const std::vector<anim::ModelClipInfo> clips = modelClipsOf(filename);
    if (clips.empty()) return;
    const AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    const bool hasPlayer = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity) != nullptr;

    ImGui::SeparatorText("Model Clips");
    UIProbe::item("model.clips");
    if (hooks.useAnimationPlayer && animationEntities){
        ImGui::BeginDisabled(!stopped);
        if (ImGui::Button(ICON_FA_FILM " Use Animation Player##model_use_player", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
            hooks.useAnimationPlayer(project, sceneProject->id, entity);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Play the model's clips with an Animation Player: removes the entity animations (one undo step)");
    }else if (!animationEntities){
        ImGui::TextDisabled("Played by an Animation Player (no entity animations).");
    }

    const float labelWidth = getLabelSize("Animation 00");
    beginTable(cpType, labelWidth, "model_clips");
    for (const anim::ModelClipInfo& info : clips){
        ImGui::PushID(info.index);
        const std::string reference = anim::formatSubClipReference(filename, info.name);
        propertyHeader(info.name, -1, false, false);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", inspectorclips::formatSeconds(info.duration).c_str());
        ImGui::SetItemTooltip("%s: %zu channels", reference.c_str(), info.channels);

        float buttons = 0.0f;
        if (hooks.addToAnimationPlayer) buttons += iconButtonWidth(ICON_FA_PLUS) + ImGui::GetStyle().ItemSpacing.x;
        if (hooks.extractSubClip) buttons += iconButtonWidth(ICON_FA_FILE_EXPORT) + ImGui::GetStyle().ItemSpacing.x;
        if (hooks.openClip) buttons += iconButtonWidth(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE) + ImGui::GetStyle().ItemSpacing.x;
        if (buttons > 0.0f){
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttons + ImGui::GetStyle().ItemSpacing.x));
        }
        ImGui::BeginDisabled(!stopped);
        bool first = true;
        if (hooks.addToAnimationPlayer){
            if (ImGui::Button(ICON_FA_PLUS "##model_clip_add")){
                hooks.addToAnimationPlayer(project, sceneProject->id, entity, reference);
            }
            ImGui::SetItemTooltip(hasPlayer ? "Add to Animation Player" : "Add to Animation Player (adds one)");
            first = false;
        }
        if (hooks.extractSubClip){
            if (!first) ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FILE_EXPORT "##model_clip_extract")){
                hooks.extractSubClip(project, sceneProject->id, entity, reference);
            }
            ImGui::SetItemTooltip("Extract to .anim (an editable copy)");
            first = false;
        }
        if (hooks.openClip){
            if (!first) ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE "##model_clip_open")){
                hooks.openClip(project, sceneProject->id, entity, reference);
            }
            ImGui::SetItemTooltip("Open in the Animation window (read-only)");
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    endTable();
}

// the sub-clip names of the Model's file, for the entity animations table's labels
std::vector<std::string> editor::InspectorWindow::modelSubClipNames(const std::string& model){
    std::vector<std::string> names;
    for (const anim::ModelClipInfo& info : modelClipsOf(model)){
        if (info.index >= 0 && (size_t)info.index >= names.size()) names.resize((size_t)info.index + 1);
        if (info.index >= 0) names[(size_t)info.index] = info.name;
    }
    return names;
}

// ---- AnimationComponent section: Open in Animation window, Convert to Clip... ----

void editor::InspectorWindow::drawAnimationClipButtons(SceneProject* sceneProject, Entity entity){
    const AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    if (!hooks.openEntityAnimation && !hooks.convertToClip) return;
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    ImGui::BeginDisabled(!stopped);
    if (hooks.openEntityAnimation){
        if (ImGui::Button(ICON_FA_FILM " Open in Animation window##animation_open")){
            hooks.openEntityAnimation(project, sceneProject->id, entity);
        }
        ImGui::SetItemTooltip("Edit this animation in the Animation window");
        if (hooks.convertToClip) ImGui::SameLine();
    }
    if (hooks.convertToClip){
        if (ImGui::Button(ICON_FA_FILE_EXPORT " Convert to Clip...##animation_convert")){
            hooks.convertToClip(project, sceneProject->id, entity);
        }
        ImGui::SetItemTooltip("Write its keyframe tracks to a shared .anim clip played by an Animation Player");
    }
    ImGui::EndDisabled();
}
