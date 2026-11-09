// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's animation clip sections (animation clips spec 4.2, 9.R7/R8/R14/R30/R45):
// - the Animation Player section (default clip, clip list, settings, Bindings foldout, the live
//   states while playing);
// - the asset section of the inspected .anim (or of a model's read-only sub-clips): settings,
//   tracks, events, preview model, users, warnings; edits go through AnimationClipDocument (project
//   history);
// - the Model section's clip buttons and rows and the AnimationComponent section's clip buttons.
// Window actions (open, convert, extract, use player, create) are AnimationClipInspectorHooks:
// a button shows once its hook is set. Owner: T2c.

#include "animation/AnimatorAssetIO.h"
#include "animation/ClipReport.h"
#include "window/BitmapFontInspector.h"
#include "media/VideoFormats.h"
#include "component/Transform.h"
#include "util/NativePlugins.h"
#include "InspectorWindow.h"
#include "window/HumanoidConvertDialog.h"
#include "window/ModelClipSettingsView.h"
#include "window/ReduceKeysDialog.h"
#include "pose/PoseRigProvider.h"
#include "App.h"
#include "Backend.h"
#include "animation/AvatarMaskWeights.h"
#include "animation/HumanoidClip.h"
#include "pool/AnimationClipPool.h"
#include "util/PhysicsMaterialAssets.h"
#include "InspectorWindowClips.h"
#include "InspectorVatBake.h"
#include "util/FileUtils.h"

#include "animation/AnimationClipController.h"
#include "animation/AnimationClipDocument.h"
#include "animation/ClipEditorUtil.h"
#include "animation/AnimationClipInspectorHooks.h"
#include "animation/AnimationClipIO.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipValidation.h"
#include "animation/ModelSubClips.h"
#include "animation/PreviewOwner.h"
#include "audio/AudioMixerDocument.h"
#include "command/CommandHandle.h"
#include "command/type/AudioSettingsCmd.h"
#include "command/type/ClipSceneActions.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/ModelComponent.h"
#include "external/IconsFontAwesome6.h"
#include "object/AnimationPlayer.h"
#include "util/FileDialogs.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "Out.h"
#include "Scene.h"
#include "Theme.h"
#include "EditorHost.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
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

std::string inspectorclips::missingClipTooltip(const std::string& clip){
    anim::SubClipReference reference;
    if (anim::parseSubClipReference(clip, reference)){
        return "Missing: " + reference.model + " has no animation \"" + reference.clip + "\" (renamed or removed in the model)";
    }
    return "Missing: " + clip + " (deleted or moved outside the project; undo the delete to relink)";
}

bool inspectorclips::subClipNamed(const std::vector<std::string>& subClipNames, const std::string& clip){
    if (std::find(subClipNames.begin(), subClipNames.end(), clip) != subClipNames.end()) return true;
    // a number is an index when no animation has that name (ModelSubClips' reading rule)
    if (clip.empty() || clip.size() > 9 || !std::all_of(clip.begin(), clip.end(), [](unsigned char c){ return std::isdigit(c) != 0; })){
        return false;
    }
    return std::stoul(clip) < subClipNames.size();
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

std::string inspectorclips::formatRatio(float ratio){
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", std::isfinite(ratio) ? ratio : 0.0f);
    return text;
}

const char* inspectorclips::channelLabel(ClipChannel channel){
    switch (channel){
        case ClipChannel::Position: return "Position";
        case ClipChannel::Rotation: return "Rotation";
        case ClipChannel::Scale: return "Scale";
        case ClipChannel::Morph: return clipui::channelLabel(channel);
        case ClipChannel::Lattice: return "Lattice";
        case ClipChannel::Property: return "Property";
        case ClipChannel::Humanoid:
        case ClipChannel::HumanoidPosition: return clipui::channelLabel(channel);
    }
    return "";
}

std::string inspectorclips::formatSeconds(float seconds){
    return clipui::formatSeconds(seconds);
}

std::string inspectorclips::usersText(size_t sceneUsers, size_t prefabUsers){
    std::string text = std::to_string(sceneUsers) + (sceneUsers == 1 ? " user" : " users") + " in open scenes";
    if (prefabUsers > 0){
        text += ", " + std::to_string(prefabUsers) + " in loaded prefabs";
    }
    return text;
}

namespace{

    // a byte position that does not split a UTF-8 sequence (moved back to its lead byte)
    size_t utf8Boundary(const std::string& text, size_t position){
        while (position > 0 && position < text.size() && (static_cast<unsigned char>(text[position]) & 0xC0) == 0x80){
            position--;
        }
        return position;
    }

}

std::string inspectorclips::elideMiddle(const std::string& text, float maxWidth, const TextWidth& width){
    if (text.empty() || width(text) <= maxWidth) return text;
    // keep a third at the start and the rest at the end, shrinking until it fits
    for (size_t kept = text.size() - 1; kept > 0; kept--){
        const size_t head = utf8Boundary(text, kept / 3);
        const size_t tailStart = utf8Boundary(text, text.size() - (kept - kept / 3));
        if (tailStart < head) continue;
        const std::string candidate = text.substr(0, head) + "..." + text.substr(tailStart);
        if (width(candidate) <= maxWidth) return candidate;
    }
    return "...";
}

std::string inspectorclips::elideStart(const std::string& text, float maxWidth, const TextWidth& width){
    if (text.empty() || width(text) <= maxWidth) return text;
    for (size_t kept = text.size() - 1; kept > 0; kept--){
        const std::string candidate = "..." + text.substr(utf8Boundary(text, text.size() - kept));
        if (width(candidate) <= maxWidth) return candidate;
    }
    return "...";
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

size_t inspectorclips::problemCount(const ClipValidationReport& report){
    size_t count = report.legacyModelLayout ? 1 : 0;
    for (const TrackReport& track : report.tracks){
        // the legacy layout counts once, not per track it hides
        if (trackHasProblem(track) && track.status != TrackBindingStatus::LegacyModelLayout) count++;
    }
    for (const EventReport& event : report.events){
        if (event.status == EventHandlerStatus::NotFound) count++;
    }
    return count;
}

namespace{

    std::string leafName(const std::string& path){
        if (path.empty()) return clipui::rootLabel();
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

}

std::string inspectorclips::trackProblemText(const TrackReport& track){
    std::string text = leafName(track.path) + " : " + (track.channel == ClipChannel::Property ? track.property : channelLabel(track.channel)) + ": ";
    if (track.channel == ClipChannel::Property && !track.reason.empty() &&
        (track.status == TrackBindingStatus::MissingComponent || track.status == TrackBindingStatus::UnknownProperty)){
        // the binding's own reason ("no 'light' component", "did you mean 'intensity'?")
        return text + track.reason;
    }
    switch (track.status){
        case TrackBindingStatus::MissingPath: text += "missing"; break;
        case TrackBindingStatus::DuplicateSiblingName: text += "several siblings share a name"; break;
        case TrackBindingStatus::MissingComponent: text += "nothing to animate on the GameObject"; break;
        case TrackBindingStatus::LatticeIndexOutOfRange: text += "lattice points outside the cage"; break;
        case TrackBindingStatus::MorphIndexOutOfRange: text += "more weights than morph targets"; break;
        case TrackBindingStatus::LegacyModelLayout: text += "under a legacy-layout model"; break;
        case TrackBindingStatus::UnaddressableName: text += "a name on the path can't be addressed"; break;
        case TrackBindingStatus::UnknownProperty: text += "not an animatable property"; break;
        default: text += "not bound"; break;
    }
    if (!track.nearest.empty()){
        text += " (did you mean ";
        for (size_t i = 0; i < track.nearest.size(); i++){
            if (i > 0) text += i + 1 == track.nearest.size() ? " or " : ", ";
            text += leafName(track.nearest[i]);
        }
        text += "?)";
    }
    return text;
}

std::string inspectorclips::eventProblemText(const EventReport& event, float time){
    if (!event.message.empty()){
        // the validation's own line ("event f at 0.5 s: ..."), capitalized for a row
        std::string text = event.message;
        text[0] = (char)std::toupper((unsigned char)text[0]);
        return text;
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

std::vector<Entity> inspectorclips::actionPreviewEntities(Scene* scene, Entity action){
    std::vector<Entity> touched;
    if (!scene) return touched;
    std::unordered_set<Entity> seen;
    std::function<void(Entity)> walk = [&](Entity e){
        if (e == NULL_ENTITY || !scene->isEntityCreated(e) || !seen.insert(e).second) return;
        touched.push_back(e);
        if (ActionComponent* component = scene->findComponent<ActionComponent>(e)){
            walk(component->target);
        }
        if (AnimationComponent* animation = scene->findComponent<AnimationComponent>(e)){
            for (const ActionFrame& frame : animation->actions){
                walk(frame.action);
            }
        }
    };
    walk(action);
    return touched;
}

std::shared_ptr<AnimationClipDocument> inspectorclips::ClipDocumentCache::find(Project* project, const std::string& clip, double now,
                                                                              double retrySeconds, std::string* error){
    Entry& entry = entries[clip];
    if (std::shared_ptr<AnimationClipDocument> document = entry.document.lock()){
        // still the file the reference named (a renamed one took its document along)
        if (document->exists() && document->path() == entry.path){
            return document;
        }
    }else if (!entry.error.empty() && now < entry.retryAt){
        if (error) *error = entry.error;
        return nullptr;
    }
    std::string why;
    std::shared_ptr<AnimationClipDocument> document = AnimationClipDocument::open(project, clip, &why);
    entry.document = document;
    entry.path = document ? document->path() : std::string();
    entry.error = document ? std::string() : (why.empty() ? std::string("can't open the clip") : why);
    entry.retryAt = now + retrySeconds;
    if (!document && error) *error = entry.error;
    return document;
}

std::vector<int> inspectorclips::acquirePreviewOwners(uint32_t sceneId, Scene* scene, Entity action, std::function<void()> stop){
    std::vector<int> tokens;
    const std::vector<Entity> written = actionPreviewEntities(scene, action);
    auto writes = std::make_shared<std::unordered_set<Entity>>(written.begin(), written.end());
    for (Entity root : topLevelEntities(scene, written)){
        // the roots don't overlap one another: acquiring one never stops this preview's other owners
        const PreviewOwner::Token token = PreviewOwner::acquire(sceneId, root, scene, "Inspector action preview", stop);
        if (token == 0) continue;
        tokens.push_back(token);
        PreviewOwner::setWrites(token, [writes](Entity entity){ return writes->count(entity) > 0; });
    }
    return tokens;
}

inspectorclips::InspectedObjectTracker::Result inspectorclips::InspectedObjectTracker::update(const fs::path& asset, uint32_t newSceneId,
                                                                                            const std::vector<Entity>& newSelection){
    Result result;
    const bool selectionChanged = known && (newSceneId != sceneId || newSelection != selection);
    // an asset set before this frame: one set in this frame is the latest click and wins
    result.clearAsset = selectionChanged && !asset.empty() && asset == assetSeen;
    result.changed = selectionChanged || asset != assetSeen;
    known = true;
    sceneId = newSceneId;
    selection = newSelection;
    assetSeen = result.clearAsset ? fs::path() : asset;
    return result;
}

// ---- the sections' state ----

namespace jasperengine::editor{

    struct InspectorClipState{
        // the info line of the clip shown: worked out once per document revision
        std::string infoReference;
        uint64_t infoRevision = 0;
        std::string infoLine;
        struct ModelClipsEntry{
            int64_t stamp = 0;
            int checkedFrame = -1;
            double retryAt = 0.0;
            bool failed = false;
            std::vector<anim::ModelClipInfo> clips;
        };
        struct UsersEntry{
            double retryAt = 0.0;
            std::vector<ClipUser> users;
        };
        struct BindingsEntry{
            uint64_t revision = 0;
            uint64_t hierarchy = 0;
            uint64_t names = 0;
            double checkedAt = 0.0;
            ClipValidationReport report;
        };
        struct ExistsEntry{
            bool exists = false;
            double retryAt = 0.0;
        };

        fs::path project;
        inspectorclips::ClipDocumentCache documents;
        std::map<std::string, ModelClipsEntry> modelClips;
        std::map<std::string, UsersEntry> users;
        std::map<std::string, BindingsEntry> bindings;
        std::map<std::string, ExistsEntry> exists;
        // the inspected asset's asset-relative form (fs::relative stats; once per asset)
        fs::path relativeOf;
        std::string relative;

        inspectorclips::InspectedObjectTracker tracker;

        // a model picked or dropped on an entry: the "Model Clip" popup chooses its animation
        std::string subClipModel;
        size_t subClipEntry = 0;
        std::string subClipEntryClip;       // the entry's clip when the popup opened (still that entry?)
        size_t subClipEntryCount = 0;
        Entity subClipEntity = NULL_ENTITY;
        uint32_t subClipScene = 0;
        bool subClipPopupRequested = false;

        std::string playerError;
        std::string assetError;
        // the model asset section: its clips' settings, the selection whose clip shows below
        std::unique_ptr<ModelClipSettingsView> modelClipSettings;
        // the Bindings report's humanoid rig of the inspected player (anim-retarget)
        std::shared_ptr<anim::HumanoidSlots> humanoidRig;
        Entity humanoidRoot = NULL_ENTITY;
        uint64_t humanoidRevision = 0;
        uint64_t humanoidPool = 0;
        // a Retarget entry's source model rig (the one retarget path reads its clip through it):
        // looked up once per pool change; a failure is kept until the pool changes or a second passed
        struct ConversionEntry{
            std::shared_ptr<const anim::RetargetSource> source;
            std::string error;
            uint64_t pool = 0;
            double retryAt = 0.0;
        };
        std::map<std::string, ConversionEntry> conversions;

        // a clip setting being dragged: committed once when the drag ends
        bool editingFrameRate = false;
        int pendingFrameRate = 30;
        bool editingLength = false;
        float pendingLength = 0.0f;

        void forgetProject(const fs::path& path){
            if (project == path) return;
            // another project: its documents are gone (AnimationClipDocument::closeAll)
            documents.clear();
            modelClips.clear();
            users.clear();
            bindings.clear();
            exists.clear();
            relativeOf.clear();
            relative.clear();
            modelClipSettings.reset();
            project = path;
        }
    };

    void InspectorClipStateDeleter::operator()(InspectorClipState* state) const{
        delete state;
    }

}

InspectorClipState& editor::InspectorWindow::clipState(){
    if (!clipStatePtr){
        clipStatePtr.reset(new InspectorClipState());
    }
    clipStatePtr->forgetProject(project->getProjectPath());
    return *clipStatePtr;
}

void editor::InspectorWindow::trackInspectedObject(){
    InspectorClipState& state = clipState();
    uint32_t sceneId = project->getSelectedSceneForProperties();
    if (!project->getScene(sceneId)){
        sceneId = project->getSelectedSceneId();
    }
    const inspectorclips::InspectedObjectTracker::Result result =
        state.tracker.update(project->getInspectedAsset(), sceneId, project->getSelectedEntities(sceneId));
    if (result.clearAsset){
        project->clearInspectedAsset();
    }
    if (result.changed){
        // error lines belong to what was shown
        state.playerError.clear();
        state.assetError.clear();
    }
}

// ---- file helpers ----

namespace{

    constexpr double CLIP_RETRY_SECONDS = 1.0;
    // validation re-runs at most this often, whatever changes (a spawn per frame in play mode)
    constexpr double BINDINGS_MIN_SECONDS = 0.25;
    constexpr size_t MAX_PROBLEM_ROWS = 24;

    const ImVec4 OK_COLOR(0.45f, 0.8f, 0.45f, 1.0f);

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

    float textWidth(const std::string& text){
        return ImGui::CalcTextSize(text.c_str()).x;
    }

    float iconButtonWidth(const char* icon){
        return ImGui::CalcTextSize(icon).x + ImGui::GetStyle().FramePadding.x * 2;
    }

    // a file field like the Model and Pose Animator ones, left-aligned: the text elided in the
    // middle (the file name keeps its end), an optional icon before it and a tag after it (the
    // tag becomes a lock when the field is narrow)
    void drawFileLabel(const char* id, const std::string& text, const std::string& tooltip, const ImVec4* color, float width,
                       const char* icon = nullptr, const char* tag = nullptr){
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Colors::filenameLabel);
        ImGui::BeginChild(id, ImVec2(std::max(width, 1.0f), ImGui::GetFrameHeight()), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        float available = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x * 2;
        if (tag && textWidth(text) + textWidth(tag) + spacing > available){
            tag = ICON_FA_LOCK;
        }
        if (icon) available -= textWidth(icon) + spacing;
        if (tag) available -= textWidth(tag) + spacing;
        const std::string shown = inspectorclips::elideMiddle(text, std::max(available, 0.0f), textWidth);
        ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y));
        if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
        if (icon){
            ImGui::TextUnformatted(icon);
            ImGui::SameLine();
        }
        ImGui::TextUnformatted(shown.c_str());
        if (color) ImGui::PopStyleColor();
        if (tag){
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tag);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        if (!tooltip.empty()){
            ImGui::SetItemTooltip("%s", tooltip.c_str());
        }
    }

    // the files of a "resource_files" payload that `accept` takes
    std::vector<std::string> payloadFiles(const ImGuiPayload* payload, const std::function<bool(const std::string&)>& accept){
        std::vector<std::string> files;
        if (!payload || !payload->IsDataType("resource_files")) return files;
        for (const std::string& file : Util::getStringsFromPayload(payload)){
            if (accept(file)) files.push_back(file);
        }
        return files;
    }

    bool isClipOrModelFile(const std::string& file){
        return Util::isAnimationClipFile(file) || Util::isModelFile(file);
    }

    bool isModelOrPrefabFile(const std::string& file){
        return Util::isModelFile(file) || Util::isPrefabFile(file);
    }

    // a drag in flight carries a file `accept` takes: only then the item becomes a drop target
    // (no highlight for a texture over a clip field)
    bool draggingFiles(const std::function<bool(const std::string&)>& accept){
        return !payloadFiles(ImGui::GetDragDropPayload(), accept).empty();
    }

    // ---- caches (per frame work stays a map lookup; see InspectorClipState) ----

    std::shared_ptr<AnimationClipDocument> findClipDocument(Project* project, InspectorClipState& state, const std::string& clip, std::string* error){
        if (clip.empty()){
            if (error) *error = "no clip";
            return nullptr;
        }
        std::shared_ptr<AnimationClipDocument> document = state.documents.find(project, clip, ImGui::GetTime(), CLIP_RETRY_SECONDS, error);
        if (state.documents.size() > 256){
            state.documents.clear();
        }
        return document;
    }

    const std::vector<anim::ModelClipInfo>& modelClipsOf(Project* project, InspectorClipState& state, const std::string& model){
        static const std::vector<anim::ModelClipInfo> none;
        if (model.empty()) return none;
        const int frame = ImGui::GetFrameCount();
        auto it = state.modelClips.find(model);
        if (it != state.modelClips.end() && it->second.checkedFrame == frame){
            return it->second.clips;
        }
        const double now = ImGui::GetTime();
        std::error_code ec;
        const fs::path full = project->resolveAssetPath(model);
        const int64_t stamp = fs::is_regular_file(full, ec) ? (int64_t)fs::last_write_time(full, ec).time_since_epoch().count() : 0;
        if (it != state.modelClips.end() && it->second.stamp == stamp && (now < it->second.retryAt || !it->second.failed)){
            it->second.checkedFrame = frame;
            return it->second.clips;
        }
        if (state.modelClips.size() > 64){
            state.modelClips.clear();
        }
        InspectorClipState::ModelClipsEntry& entry = state.modelClips[model];
        entry.stamp = stamp;
        entry.checkedFrame = frame;
        entry.retryAt = now + CLIP_RETRY_SECONDS;
        entry.clips.clear();
        entry.failed = !stamp || !anim::listModelFileClips(model, entry.clips, nullptr);
        return entry.clips;
    }

    std::vector<std::string> namesOf(const std::vector<anim::ModelClipInfo>& clips){
        std::vector<std::string> names;
        for (const anim::ModelClipInfo& info : clips) names.push_back(info.name);
        return names;
    }

    // the clip is there: an .anim file on disk, or a model on disk naming that animation (I3);
    // checked at most once a second per reference
    bool clipExists(Project* project, InspectorClipState& state, const std::string& clip){
        if (clip.empty()) return false;
        const double now = ImGui::GetTime();
        auto it = state.exists.find(clip);
        if (it != state.exists.end() && now < it->second.retryAt){
            return it->second.exists;
        }
        anim::SubClipReference reference;
        const bool subClip = anim::parseSubClipReference(clip, reference);
        std::error_code ec;
        bool exists = fs::is_regular_file(project->resolveAssetPath(subClip ? reference.model : clip), ec);
        if (exists && subClip){
            const std::vector<anim::ModelClipInfo>& clips = modelClipsOf(project, state, reference.model);
            // a model that can't be listed is not called missing (the Bindings line says why)
            const auto listed = state.modelClips.find(reference.model);
            if (listed != state.modelClips.end() && !listed->second.failed){
                exists = inspectorclips::subClipNamed(namesOf(clips), reference.clip);
            }
        }
        if (state.exists.size() > 256) state.exists.clear();
        state.exists[clip] = {exists, now + CLIP_RETRY_SECONDS};
        return exists;
    }

    const std::vector<ClipUser>& clipUsersOf(Project* project, InspectorClipState& state, const std::string& clip){
        const double now = ImGui::GetTime();
        auto it = state.users.find(clip);
        if (it == state.users.end() || now >= it->second.retryAt){
            if (state.users.size() > 64) state.users.clear();
            InspectorClipState::UsersEntry& entry = state.users[clip];
            entry.users = findClipUsers(project, clip);
            entry.retryAt = now + CLIP_RETRY_SECONDS;
            return entry.users;
        }
        return it->second.users;
    }

    const ClipValidationReport& clipBindingsOf(Project* project, InspectorClipState& state, SceneProject* sceneProject, Entity root,
                                               const std::string& clip, AnimationClipDocument& document){
        const std::string key = std::to_string(sceneProject->id) + ":" + std::to_string(root) + ":" + clip;
        Scene* scene = sceneProject->scene;
        const uint64_t hierarchy = scene->hierarchyRevision();
        const uint64_t names = scene->nameRevision();
        const double now = ImGui::GetTime();
        auto it = state.bindings.find(key);
        if (it != state.bindings.end()){
            const bool same = it->second.revision == document.revision() && it->second.hierarchy == hierarchy && it->second.names == names;
            // unchanged: once a second (the root's scripts may change the handlers); changed: at
            // most every BINDINGS_MIN_SECONDS
            const double age = now - it->second.checkedAt;
            if ((same && age < CLIP_RETRY_SECONDS) || (!same && age < BINDINGS_MIN_SECONDS)){
                return it->second.report;
            }
        }
        if (state.bindings.size() > 64){
            state.bindings.clear();
        }
        InspectorClipState::BindingsEntry& entry = state.bindings[key];
        entry.revision = document.revision();
        entry.hierarchy = hierarchy;
        entry.names = names;
        entry.checkedAt = now;
        entry.report = ClipValidation::validateInScene(project, sceneProject, document.data(), root);
        return entry.report;
    }

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

// Removes an entry; the default clip naming its state goes back to "the first clip" in the same step
void editor::InspectorWindow::removePlayerClipEntry(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index){
    AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player || index >= player->clips.size()) return;
    const std::vector<std::string> states = anim::playerStateNames(player->clips);
    std::vector<AnimationPlayerClip> clips = player->clips;
    clips.erase(clips.begin() + (long)index);
    MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
    multiCmd->addPropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneProject->id, entity, cpType, "clips", clips);
    if (!player->defaultClip.empty() && player->defaultClip == states[index]){
        multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "defaultClip", std::string());
    }
    multiCmd->setNoMerge();
    CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
}

// Picked or dropped files for entry `index` (SIZE_MAX: new entries): .anim files are set (one
// step), the first model asks which of its animations (the "Model Clip" popup). False when
// nothing could be used.
bool editor::InspectorWindow::usePlayerClipFiles(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index,
                                                 const std::vector<std::string>& files){
    InspectorClipState& state = clipState();
    AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return false;
    std::vector<std::string> clips;
    std::string model;
    std::vector<std::string> rejected;
    for (const std::string& file : files){
        const std::string relative = assetRelativePath(project, fs::path(file));
        if (!relative.empty() && Util::isAnimationClipFile(relative)){
            clips.push_back(relative);
        }else if (!relative.empty() && Util::isModelFile(relative) && model.empty()){
            model = relative;
        }else{
            rejected.push_back(fs::path(file).filename().string());
        }
    }
    if (index != SIZE_MAX && !clips.empty()){
        // one file for one entry
        setPlayerClipEntry(cpType, sceneProject, entity, index, clips[0]);
        model.clear();
    }else if (!clips.empty()){
        std::vector<AnimationPlayerClip> entries = player->clips;
        for (const std::string& clip : clips){
            AnimationPlayerClip entry;
            entry.clip = clip;
            entry.guid = clipGuidOf(project, clip);
            entries.push_back(entry);
        }
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
            new PropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneProject->id, entity, cpType, "clips", entries));
    }
    if (!model.empty()){
        if (modelClipsOf(project, state, model).empty()){
            rejected.push_back(fs::path(model).filename().string() + " (no animations)");
        }else{
            player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
            state.subClipModel = model;
            state.subClipEntry = index;
            state.subClipEntryClip = player && index < player->clips.size() ? player->clips[index].clip : std::string();
            state.subClipEntryCount = player ? player->clips.size() : 0;
            state.subClipEntity = entity;
            state.subClipScene = sceneProject->id;
            state.subClipPopupRequested = true;
        }
    }
    if (!rejected.empty()){
        std::string names;
        for (const std::string& name : rejected) names += (names.empty() ? "" : ", ") + name;
        state.playerError = "Not a clip: " + names + ". A clip is an .anim file or a model (.gltf, .glb, .fbx) inside the assets folder.";
    }
    return !clips.empty() || state.subClipPopupRequested;
}

void editor::InspectorWindow::drawAnimationPlayerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities){
    RowSettings autoSettings;
    autoSettings.help = "The default clip plays when the game starts (or the GameObject is instantiated).";
    RowSettings speedSettings;
    speedSettings.secondColSize = 6 * ImGui::GetFontSize();
    speedSettings.help = "Multiplies the speed of every state; negative plays backwards.";
    RowSettings fadeSettings;
    fadeSettings.secondColSize = 6 * ImGui::GetFontSize();
    fadeSettings.help = "Seconds of the crossfade when play(name, fade), crossFade(name) or Model:playAnimation give none.";

    // the Animator Controller row: create one from the clips (an Animator plays it), or what a
    // player that hands over to its Animator plays (upstream's controller players)
    if (entities.size() == 1){
        drawAnimationPlayerController(cpType, sceneProject, entities[0]);
    }

    // Unity's order: the default clip ("Animation"), the list, then the settings
    beginTable(cpType, getLabelSize("Play Automatically"), "player_default");
    drawPlayerDefaultClipRow(cpType, sceneProject, entities);
    endTable();

    if (entities.size() == 1){
        drawAnimationPlayerLayers(cpType, sceneProject, entities[0]);
        drawAnimationPlayerClips(cpType, sceneProject, entities[0]);
    }else{
        beginTable(cpType, getLabelSize("Play Automatically"), "player_clips_multi");
        propertyHeader("Clips");
        ImGui::TextDisabled("Select a single GameObject to edit the clips");
        endTable();
    }

    beginTable(cpType, getLabelSize("Play Automatically"), "player_settings");
    propertyRow(RowPropertyType::Bool, cpType, "playAutomatically", "Play Automatically", sceneProject, entities, autoSettings);
    propertyRow(RowPropertyType::Float, cpType, "speed", "Speed", sceneProject, entities, speedSettings);
    propertyRow(RowPropertyType::FloatPositive, cpType, "defaultFadeTime", "Default Fade", sceneProject, entities, fadeSettings);
    drawPoseFrameRateRow(cpType, sceneProject, entities, sceneProject->scene->getComponent<AnimationPlayerComponent>(entities[0]).poseFrameRate);
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
    InspectorClipState& state = clipState();
    Scene* scene = sceneProject->scene;
    AnimationPlayerComponent* player = scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player) return;
    // a copy: the commands below change the component while the rows are drawn
    const std::vector<AnimationPlayerClip> clips = player->clips;
    const std::vector<std::string> states = anim::playerStateNames(clips);
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    const AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    const float labelWidth = getLabelSize("Play Automatically");

    // count, + Add Clip (a drop target too: dropped clips are appended), Create New Clip...
    beginTable(cpType, labelWidth, "player_clips_header");
    prefabRowProperty = "clips";
    propertyHeader("Clips", -1, false, false);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%zu", clips.size());
    if (drawSummaryAddButton(ICON_FA_PLUS " Add Clip##player_clip_add")){
        std::vector<AnimationPlayerClip> newClips = clips;
        newClips.push_back(AnimationPlayerClip());
        CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
            new PropertyCmd<std::vector<AnimationPlayerClip>>(project, sceneProject->id, entity, cpType, "clips", newClips));
    }
    UIProbe::item("player.addClip");
    ImGui::SetItemTooltip("Add an entry to the clip list (or drop .anim files and models here)");
    if (stopped && draggingFiles(isClipOrModelFile) && ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            usePlayerClipFiles(cpType, sceneProject, entity, SIZE_MAX, payloadFiles(payload, isClipOrModelFile));
        }
        ImGui::EndDragDropTarget();
    }
    if (hooks.createNewClip){
        // its own row: the Inspector is often too narrow for both buttons on one line
        propertyHeader("");
        ImGui::BeginDisabled(!stopped);
        if (ImGui::Button("Create New Clip...##player_clip_create", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
            hooks.createNewClip(project, sceneProject->id, entity);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Create an .anim file and add it to this Animation Player (one undo step)");
    }
    // Bake ▾ › Vertex Animation… (the baker window prefilled with this GameObject)
    if (vatbakeentry::available()){
        propertyHeader("");
        VatBakeEntry entry;
        entry.kind = VatBakeEntry::Kind::AnimationPlayer;
        entry.sceneId = sceneProject->id;
        entry.entity = entity;
        vatbakeentry::bakeButton(project, entry, stopped, "player_bake", "player.bake");
    }
    endTable();

    if (!clips.empty()){
        const ImVec4 warning = Theme::Colors::WarningText;
        beginTable(cpType, labelWidth, "player_clips");
        for (size_t i = 0; i < clips.size(); i++){
            const AnimationPlayerClip& entry = clips[i];
            const bool subClip = inspectorclips::isSubClip(entry.clip);
            const bool exists = clipExists(project, state, entry.clip);
            ImGui::PushID((int)i);

            // state name | file field, picker, clear (open, extract, show, remove: the row menu).
            // The row's Apply/Revert is the whole entry: clip and guid go together (R8)
            prefabRowProperty = "clips[" + std::to_string(i) + "]";
            propertyHeader(states[i].empty() ? "Clip " + std::to_string(i) : states[i], -1, false, false);

            const float buttons = iconButtonWidth(ICON_FA_FOLDER_OPEN) + iconButtonWidth(ICON_FA_XMARK) + ImGui::GetStyle().ItemSpacing.x * 2;
            const float fieldWidth = ImGui::GetContentRegionAvail().x - buttons;
            const std::string menuHint = "\nRight-click for more";

            ImGui::BeginGroup();
            if (entry.clip.empty()){
                const ImVec4 disabled = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
                drawFileLabel("clipfile", "< None >", "Right-click for more", &disabled, fieldWidth);
            }else if (!exists){
                // R8: a deleted clip keeps its reference (undoing the delete relinks it)
                drawFileLabel("clipfile", inspectorclips::clipFieldText(entry.clip), inspectorclips::missingClipTooltip(entry.clip) + menuHint,
                              &warning, fieldWidth, ICON_FA_TRIANGLE_EXCLAMATION);
            }else{
                std::string tooltip = entry.clip;
                if (subClip) tooltip += "\n(Read-Only: Extract to .anim in the row menu to edit it)";
                drawFileLabel("clipfile", inspectorclips::clipFieldText(entry.clip), tooltip + menuHint, nullptr, fieldWidth,
                              nullptr, subClip ? "Read-Only" : nullptr);
            }
            ImGui::EndGroup();
            UIProbe::item("player.clip." + std::to_string(i));
            // the field is a child window: hover by its rectangle
            if (ImGui::IsMouseHoveringRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                    ImGui::IsMouseReleased(ImGuiMouseButton_Right)){
                ImGui::OpenPopup("clip_row_menu");
            }
            if (stopped && draggingFiles(isClipOrModelFile) && ImGui::BeginDragDropTarget()){
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                    usePlayerClipFiles(cpType, sceneProject, entity, i, payloadFiles(payload, isClipOrModelFile));
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "##clip_pick")){
                const std::string path = editor::FileDialogs::openFileDialog(project->getAssetsPath().string(),
                                                                             FILE_DIALOG_ANIMATION_CLIP | FILE_DIALOG_MODEL);
                if (!path.empty()){
                    usePlayerClipFiles(cpType, sceneProject, entity, i, {path});
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

            bool removed = false;
            if (ImGui::BeginPopup("clip_row_menu")){
                ImGui::TextDisabled("%s", states[i].empty() ? "Clip" : states[i].c_str());
                ImGui::Separator();
                if (hooks.openClip && ImGui::MenuItem("Open in Animation Window", nullptr, false, exists && stopped)){
                    hooks.openClip(project, sceneProject->id, entity, entry.clip);
                }
                if (subClip && hooks.extractSubClip && ImGui::MenuItem("Extract to .anim...", nullptr, false, exists && stopped)){
                    hooks.extractSubClip(project, sceneProject->id, entity, entry.clip);
                }
                if (ImGui::MenuItem("Convert to Humanoid Clip...", nullptr, false, exists && stopped)){
                    HumanoidConvertDialog::open(project, entry.clip, sceneProject->id, entity, i);
                }
                if (ImGui::MenuItem("Show in Project", nullptr, false, exists)){
                    anim::SubClipReference reference;
                    const std::string file = anim::parseSubClipReference(entry.clip, reference) ? reference.model : entry.clip;
                    revealAssetInProject(project->resolveAssetPath(file));
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear", nullptr, false, !entry.clip.empty() || !entry.guid.empty())){
                    setPlayerClipEntry(cpType, sceneProject, entity, i, "");
                }
                if (ImGui::MenuItem("Remove")){
                    removePlayerClipEntry(cpType, sceneProject, entity, i);
                    removed = true;
                }
                UIProbe::item("player.clipMenu.remove");
                ImGui::EndPopup();
            }
            if (removed){
                ImGui::PopID();
                break;
            }

            drawPlayerClipNameRow(cpType, sceneProject, entity, i);
            drawPlayerClipLayerRow(cpType, sceneProject, entity, i);

            // wrap: a one-byte enum (PropertyCmd<ClipWrapMode>, never the int Enum row)
            ClipWrapMode clipWrap = ClipWrapMode::Once;
            bool clipWrapKnown = false;
            if (exists){
                if (std::shared_ptr<AnimationClipDocument> document = findClipDocument(project, state, entry.clip, nullptr)){
                    clipWrap = document->data().wrap;
                    clipWrapKnown = true;
                }
            }
            const std::string prefix = "clips[" + std::to_string(i) + "]";
            const std::string defaultLabel = clipWrapKnown ? std::string("Default (") + inspectorclips::wrapLabel(clipWrap) + ")" : std::string("Default");
            prefabRowProperty = prefix + ".wrap";
            if (propertyHeader("Wrap", -1, entry.wrap != ClipWrapMode::Default, false, true)){
                CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                    new PropertyCmd<ClipWrapMode>(project, sceneProject->id, entity, cpType, prefix + ".wrap", ClipWrapMode::Default));
            }
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

            // humanoid playback (anim-retarget 9): Retarget for a generic clip; Mirror and Foot IK
            // for a humanoid (or retargeted) one, with its badge
            bool humanoidClip = false;
            if (exists){
                if (std::shared_ptr<AnimationClipDocument> document = findClipDocument(project, state, entry.clip, nullptr)){
                    humanoidClip = document->data().isHumanoid();
                }
                anim::SubClipReference reference;
                if (!humanoidClip && anim::parseSubClipReference(entry.clip, reference)){
                    humanoidClip = PoseRigProvider::forModelFile(project, project->resolveAssetPath(fs::u8path(reference.model)).lexically_normal())->overrides.isHumanoid();
                }
            }
            auto flagRow = [&](const char* label, const char* field, bool value, const char* tooltip){
                prefabRowProperty = prefix + field;
                if (propertyHeader(label, -1, value, false, true)){
                    CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                        new PropertyCmd<bool>(project, sceneProject->id, entity, cpType, prefix + field, false));
                }
                bool checked = value;
                ImGui::BeginDisabled(!stopped);
                if (ImGui::Checkbox((std::string("##") + field).c_str(), &checked)){
                    CommandHandle::get(project->getSelectedSceneId())->addCommandNoMerge(
                        new PropertyCmd<bool>(project, sceneProject->id, entity, cpType, prefix + field, checked));
                }
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("%s", tooltip);
            };
            if (!humanoidClip){
                flagRow("Retarget", ".retarget", entry.retarget, "Play as a humanoid clip, retargeted from its source model");
            }
            if (humanoidClip || entry.retarget){
                ImGui::SameLine();
                ImGui::TextColored(Theme::Colors::PrefabText, ICON_FA_PERSON " Humanoid");
                ImGui::SetItemTooltip(humanoidClip ? "A humanoid clip: it plays on this character's humanoid rig"
                                                   : "Retargeted: converted from its source model's rig at load");
                flagRow("Mirror", ".mirror", entry.mirror, "Left and right swapped (humanoid clips)");
                flagRow("Foot IK", ".footIK", entry.footIK, "The feet reach the clip's foot goals: no sliding on other leg proportions");
            }

            ImGui::PopID();
        }
        endTable();
    }

    // a model was picked or dropped: which of its animations
    if (state.subClipPopupRequested){
        state.subClipPopupRequested = false;
        ImGui::OpenPopup("Model Clip##player_subclip");
    }
    if (ImGui::BeginPopup("Model Clip##player_subclip")){
        ImGui::TextDisabled("%s", fs::path(state.subClipModel).filename().string().c_str());
        ImGui::Separator();
        // a copy: setPlayerClipEntry may refresh the caches
        const std::vector<anim::ModelClipInfo> modelClips = modelClipsOf(project, state, state.subClipModel);
        for (const anim::ModelClipInfo& info : modelClips){
            const std::string label = info.name + "##subclip" + std::to_string(info.index);
            if (ImGui::Selectable(label.c_str()) && state.subClipScene == sceneProject->id && state.subClipEntity == entity){
                // still the entry the popup was opened for (an undo or remove meanwhile moves it)
                AnimationPlayerComponent* current = scene->findComponent<AnimationPlayerComponent>(entity);
                const bool append = state.subClipEntry == SIZE_MAX;
                const bool same = current && current->clips.size() == state.subClipEntryCount &&
                                  (append || (state.subClipEntry < current->clips.size() && current->clips[state.subClipEntry].clip == state.subClipEntryClip));
                if (same){
                    setPlayerClipEntry(cpType, sceneProject, entity, state.subClipEntry, anim::formatSubClipReference(state.subClipModel, info.name));
                }else{
                    state.playerError = "The clip list changed while choosing: pick the model again.";
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", inspectorclips::formatSeconds(info.duration).c_str());
        }
        ImGui::EndPopup();
    }

    if (!state.playerError.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", state.playerError.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(ICON_FA_XMARK " Dismiss##player_clip_error")){
            state.playerError.clear();
        }
    }
}

// The entry's state name: empty = the file / sub-clip name, shown as the hint (with the alias a
// taken name gets). A rename of the default clip's state renames defaultClip in the same step.
void editor::InspectorWindow::drawPlayerClipNameRow(ComponentType cpType, SceneProject* sceneProject, Entity entity, size_t index){
    AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player || index >= player->clips.size()) return;
    const AnimationPlayerClip entry = player->clips[index];
    const std::vector<std::string> states = anim::playerStateNames(player->clips);
    const std::string defaultClip = player->defaultClip;
    const std::string prefix = "clips[" + std::to_string(index) + "]";

    // the commands of a new name (+ the default clip following it)
    auto nameCommand = [&](const std::string& name){
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, prefix + ".name", name);
        if (!defaultClip.empty() && defaultClip == states[index]){
            std::vector<AnimationPlayerClip> renamed = player->clips;
            renamed[index].name = name;
            multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "defaultClip", anim::playerStateNames(renamed)[index]);
        }
        return multiCmd;
    };

    prefabRowProperty = prefix + ".name";
    if (propertyHeader("Name", -1, !entry.name.empty(), false, true)){
        MultiPropertyCmd* multiCmd = nameCommand("");
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
        return;
    }

    // the hint: the state name scripts play now (a taken name gets " 2": say so)
    const std::string base = entry.name.empty() ? anim::defaultStateName(entry) : entry.name;
    std::string hint = states[index];
    if (states[index] != base){
        hint += " (\"" + base + "\" is taken)";
    }
    const std::string help = "The state name scripts play (play, crossFade, getState). Empty = \"" + anim::defaultStateName(entry) +
                             "\", the file or sub-clip name.";
    const float helpWidth = textWidth("(?)") + ImGui::GetStyle().ItemSpacing.x;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s", entry.name.c_str());
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - helpWidth));
    if (ImGui::InputTextWithHint("##clip_name", hint.c_str(), buffer, sizeof(buffer))){
        cmd = nameCommand(buffer);
        CommandHandle::get(project->getSelectedSceneId())->addCommand(cmd);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && cmd){
        cmd->setNoMerge();
        cmd = nullptr;
    }
    ImGui::SameLine();
    helpMarker(help);
}

// The Default Clip combo (states of the player; empty = the first clip). Several GameObjects
// sharing the same state list edit theirs together; otherwise a text row.
void editor::InspectorWindow::drawPlayerDefaultClipRow(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities){
    std::vector<std::string> states;
    std::string defaultClip;
    bool mixedDefault = false;
    bool sameStates = true;
    for (size_t e = 0; e < entities.size(); e++){
        AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entities[e]);
        if (!player) return;
        const std::vector<std::string> own = anim::playerStateNames(player->clips);
        if (e == 0){
            states = own;
            defaultClip = player->defaultClip;
        }else{
            sameStates = sameStates && own == states;
            mixedDefault = mixedDefault || player->defaultClip != defaultClip;
        }
    }
    if (!sameStates){
        RowSettings settings;
        settings.help = "The state that plays automatically; empty = the first clip.";
        propertyRow(RowPropertyType::String, cpType, "defaultClip", "Default Clip", sceneProject, entities, settings);
        return;
    }
    auto setDefault = [&](const std::string& value){
        MultiPropertyCmd* multiCmd = new MultiPropertyCmd();
        for (Entity entity : entities){
            multiCmd->addPropertyCmd<std::string>(project, sceneProject->id, entity, cpType, "defaultClip", value);
        }
        multiCmd->setNoMerge();
        CommandHandle::get(project->getSelectedSceneId())->addCommand(multiCmd);
    };

    prefabRowProperty = "defaultClip";
    if (propertyHeader("Default Clip", -1, !defaultClip.empty() || mixedDefault)){
        setDefault("");
    }

    const bool known = defaultClip.empty() || std::find(states.begin(), states.end(), defaultClip) != states.end();
    const std::string firstLabel = states.empty() ? std::string("None") : "First Clip (" + states[0] + ")";
    const std::string preview = mixedDefault ? std::string("---") :
                                (defaultClip.empty() ? firstLabel : (known ? defaultClip : defaultClip + " (not in the list)"));
    if (!known && !mixedDefault) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::WarningText);
    const bool open = ImGui::BeginCombo("##player_default_clip", preview.c_str());
    if (!known && !mixedDefault) ImGui::PopStyleColor();
    UIProbe::item("player.defaultClip");
    if (open){
        if (ImGui::Selectable(firstLabel.c_str(), defaultClip.empty() && !mixedDefault) && (!defaultClip.empty() || mixedDefault)){
            setDefault("");
        }
        for (size_t i = 0; i < states.size(); i++){
            const std::string label = states[i] + "##default" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), states[i] == defaultClip && !mixedDefault) && (states[i] != defaultClip || mixedDefault)){
                setDefault(states[i]);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The state that plays automatically (Unity's Animation field); empty = the first clip");
}

void editor::InspectorWindow::drawAnimationPlayerBindings(SceneProject* sceneProject, Entity entity){
    InspectorClipState& state = clipState();
    AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(entity);
    if (!player || player->clips.empty()) return;
    const std::vector<AnimationPlayerClip> clips = player->clips;
    const std::vector<std::string> states = anim::playerStateNames(clips);
    const ImVec4 warning = Theme::Colors::WarningText;

    // the reports (cached; validation runs at most every 0.25 s) also while the foldout is
    // closed, so its header can say what is wrong
    struct Row{
        std::shared_ptr<AnimationClipDocument> document;
        const ClipValidationReport* report = nullptr;
        std::string error;
        bool missing = false;
    };
    std::vector<Row> rows(clips.size());
    size_t problems = 0;
    bool anyChecked = false;
    for (size_t i = 0; i < clips.size(); i++){
        Row& row = rows[i];
        if (clips[i].clip.empty()) continue;
        if (!clipExists(project, state, clips[i].clip)){
            row.missing = true;
            problems++;
            continue;
        }
        row.document = findClipDocument(project, state, clips[i].clip, &row.error);
        if (!row.document){
            problems++;
            continue;
        }
        row.report = &clipBindingsOf(project, state, sceneProject, entity, clips[i].clip, *row.document);
        problems += inspectorclips::problemCount(*row.report);
        anyChecked = anyChecked || inspectorclips::reportChecked(*row.report);
    }

    std::string label;
    if (problems > 0){
        label = std::string("Bindings  " ICON_FA_TRIANGLE_EXCLAMATION " ") + std::to_string(problems) + (problems == 1 ? " problem" : " problems");
        // the first time the foldout shows with problems it opens
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        ImGui::PushStyleColor(ImGuiCol_Text, warning);
    }else{
        label = anyChecked ? "Bindings (all bound)" : "Bindings (unchecked)";
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    }
    const bool open = ImGui::TreeNodeEx((label + "##player_bindings").c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
    ImGui::PopStyleColor();
    UIProbe::item("player.bindings");
    if (!open){
        return;
    }
    for (size_t i = 0; i < clips.size(); i++){
        const Row& row = rows[i];
        ImGui::PushID((int)i);
        if (clips[i].clip.empty()){
            ImGui::TextDisabled("%s: no clip", states[i].c_str());
        }else if (row.missing){
            // the row above shows it
            ImGui::TextDisabled("%s: missing clip", states[i].c_str());
        }else if (!row.document){
            ImGui::TextColored(Theme::Colors::ErrorText, "%s: %s", states[i].c_str(), row.error.c_str());
        }else{
            const ClipValidationReport& report = *row.report;
            const std::string summary = inspectorclips::bindingsSummary(report);
            const bool checked = inspectorclips::reportChecked(report);
            if (inspectorclips::problemCount(report) > 0){
                ImGui::TextColored(warning, ICON_FA_TRIANGLE_EXCLAMATION " %s: %s", states[i].c_str(), summary.c_str());
            }else if (!checked){
                ImGui::TextDisabled("%s: %s", states[i].c_str(), summary.c_str());
                ImGui::SetItemTooltip("The bindings are not checked");
            }else{
                ImGui::TextColored(OK_COLOR, ICON_FA_CHECK);
                ImGui::SameLine();
                ImGui::Text("%s: %s", states[i].c_str(), summary.c_str());
            }

            ImGui::Indent();
            size_t shown = 0;
            size_t hidden = 0;
            if (report.legacyModelLayout){
                // R3: the fix reloads the models in the node hierarchy (one scene step); the
                // warnings below say why
                ImGui::BeginDisabled(sceneProject->playState != ScenePlayState::STOPPED);
                if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Reload Models in Node Hierarchy", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
                    ClipActionResult result;
                    std::string failure;
                    if (!ClipSceneActions::reloadModelsForPlayer(project, sceneProject->id, entity, result, failure)){
                        state.playerError = failure.empty() ? std::string("The models could not be reloaded") : failure;
                    }
                }
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Reload the models under this GameObject with their glTF nodes as GameObjects, so clip paths bind (one undo step)");
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
                ImGui::SetItemTooltip("%s", (track.message.empty() ? track.path : track.message).c_str());
                shown++;
            }
            const std::vector<ClipEventData>& events = row.document->data().events;
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
            // a humanoid (or retargeted) state: the rig it plays on (anim-retarget 9)
            std::shared_ptr<const AnimationClip> humanoidClip;
            std::shared_ptr<const anim::RetargetSource> retargetSource;
            std::string retargetError;
            if (row.document->data().isHumanoid()) humanoidClip = row.document->clip();
            else if (clips[i].retarget && row.document->clip()){
                InspectorClipState::ConversionEntry& conversion = state.conversions[clips[i].clip];
                const double now = ImGui::GetTime();
                if (conversion.pool != AnimationClipPool::revision() || (!conversion.source && now >= conversion.retryAt)){
                    conversion.error.clear();
                    conversion.source = anim::clipRetargetSource(*row.document->clip(), clips[i].clip, &conversion.error);
                    conversion.pool = AnimationClipPool::revision();
                    conversion.retryAt = now + CLIP_RETRY_SECONDS;
                }
                if (state.conversions.size() > 256){
                    state.conversions.clear();
                }else if (conversion.source){
                    humanoidClip = row.document->clip();
                    retargetSource = conversion.source;
                }else{
                    retargetError = conversion.error;
                }
            }
            if (!retargetError.empty()) ImGui::TextColored(warning, ICON_FA_TRIANGLE_EXCLAMATION " %s", retargetError.c_str());
            if (humanoidClip && (humanoidClip->isHumanoid() || retargetSource)){
                if (!state.humanoidRig || state.humanoidRoot != entity || state.humanoidRevision != sceneProject->scene->hierarchyRevision() ||
                    state.humanoidPool != AnimationClipPool::revision()){
                    state.humanoidRig = std::make_shared<anim::HumanoidSlots>(anim::resolveHumanoidSlots(sceneProject->scene, entity));
                    state.humanoidRoot = entity;
                    state.humanoidRevision = sceneProject->scene->hierarchyRevision();
                    state.humanoidPool = AnimationClipPool::revision();
                }
                if (!state.humanoidRig->target){
                    ImGui::TextColored(warning, ICON_FA_TRIANGLE_EXCLAMATION " Humanoid clip, but not a humanoid: %s", state.humanoidRig->error.c_str());
                    // the skinned model under the player (it may be a child of the player's GameObject)
                    const Entity model = state.humanoidRig->model;
                    ImGui::BeginDisabled(model == NULL_ENTITY);
                    if (ImGui::SmallButton("Map Bones...")) Backend::getApp().openAvatarWindow(sceneProject->id, model);
                    ImGui::EndDisabled();
                    if (model == NULL_ENTITY) ImGui::SetItemTooltip("No skinned Model on or under this GameObject");
                }else{
                    anim::HumanoidBinding binding;
                    anim::bindHumanoidClip(*humanoidClip, state.humanoidRig->target, binding, retargetSource);
                    const float scale = state.humanoidRig->target->rig->humanScale();
                    const float source = retargetSource ? retargetSource->rig->humanScale() : humanoidClip->getHumanoidInfo().humanScale;
                    ImGui::TextColored(Theme::Colors::SubtleText, ICON_FA_PERSON " Humanoid on %s (human scale %.3f%s)",
                                       FileUtils::pathToUtf8(FileUtils::pathFromUtf8(state.humanoidRig->target->modelPath).filename()).c_str(), scale,
                                       source > 0.0f ? (", " + inspectorclips::formatRatio(scale / source) + "x the source's").c_str() : "");
                    for (const std::string& line : binding.folded) ImGui::TextColored(Theme::Colors::SubtleText, "%s", line.c_str());
                    if (!binding.missingOnTarget.empty()){
                        std::string list;
                        for (const std::string& bone : binding.missingOnTarget) list += (list.empty() ? "" : ", ") + bone;
                        ImGui::TextColored(Theme::Colors::SubtleText, "The rig lacks: %s", list.c_str());
                    }
                }
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    // the layers' masks: how many bound entities each covers (anim layers spec 9)
    drawPlayerMaskCoverage(sceneProject, entity);
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
    AnimationPlayerComponent& component = scene->getComponent<AnimationPlayerComponent>(entity);
    const anim::AnimationPlayerRuntime& runtime = anim::playerRuntime(component);
    // grouped under their layers (anim layers spec 9: a live weight per layer)
    const size_t layerCount = std::max<size_t>(1, runtime.layers.size());
    for (size_t layer = 0; layer < layerCount; layer++){
        if (layerCount > 1) drawPlayerLayerStatesHeader(sceneProject, entity, (int)layer);
        beginTable(cpType, getLabelSize("Play Automatically"), "player_states" + std::to_string(layer));
        for (size_t i = 0; i < states.size(); i++){
            const int stateIndex = runtime.findState(states[i]);
            if (layerCount > 1 && (stateIndex < 0 || runtime.stateLayer(runtime.states[(size_t)stateIndex]) != (int)layer)) continue;
            const AnimationStateInfo info = playerObject.getStateInfo(states[i]);
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (info.playing){
                ImGui::TextUnformatted(ICON_FA_PLAY);
            }else{
                ImGui::TextDisabled(ICON_FA_PAUSE);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(states[i].c_str());
            ImGui::TableNextColumn();
            if (!info.error.empty()){
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(Theme::Colors::ErrorText, "%s", info.error.c_str());
            }else if (!info.loaded){
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Loading...");
            }else{
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "%.2f / %.2f s", info.time, info.length);
                const float weightWidth = textWidth("w 0.00") + ImGui::GetStyle().ItemSpacing.x;
                const float fraction = info.length > 0.0f ? std::clamp(info.time / info.length, 0.0f, 1.0f) : 0.0f;
                ImGui::ProgressBar(fraction, ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - weightWidth), 0), overlay);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
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
}

// ---- the inspected asset (.anim, or a model's clips) ----

bool editor::InspectorWindow::drawInspectedAsset(){
    InspectorClipState& state = clipState();
    const fs::path asset = project->getInspectedAsset();
    if (asset.empty()) return false;
    const std::string file = asset.string();
    // an Animator Controller, Avatar Mask or Animation Montage (InspectorAnimator.cpp)
    if (Util::isAnimatorAssetFile(file)) return fs::exists(asset) && drawAnimatorAsset(asset);
    // a .vat: its own view (InspectorWindowVat.cpp); gone: the scene selection again
    if (Util::isVertexAnimationFile(file)){
        std::error_code ec;
        if (!fs::is_regular_file(asset, ec)) return false;
        drawVatAsset(asset);
        return true;
    }
    // a bitmap font, a BMFont file or a TTF/OTF: the Bitmap Font Inspector (S51 R23)
    if (Util::isFontFile(file)){
        std::error_code ec;
        if (!fs::is_regular_file(asset, ec)) return false;
        if (!bitmapFontInspector) bitmapFontInspector = std::make_shared<BitmapFontInspector>(project);
        bitmapFontInspector->draw(asset);
        return true;
    }
    // a .shake: its own view (InspectorWindowCamera.cpp)
    if (Util::isShakePresetFile(file)){
        std::error_code ec;
        if (!fs::is_regular_file(asset, ec)) return false;
        drawShakePresetAsset(asset);
        return true;
    }
    // a .soundevent: its editor (InspectorSoundEvent.cpp)
    if (Util::isSoundEventFile(file)){
        std::error_code ec;
        if (!fs::is_regular_file(asset, ec)) return false;
        drawSoundEventAsset(asset);
        return true;
    }
    // a .mixer: its summary (audio-mixer spec 9)
    if (asset.extension() == AudioMixerIO::EXTENSION){
        std::error_code ec;
        if (!fs::is_regular_file(asset, ec)) return false;
        drawAudioMixerAsset(asset);
        return true;
    }
    // an audio file: measurements, import settings, provenance, audition (InspectorAudioClip.cpp)
    if (Util::isAudioFile(file)){
        std::error_code ec;
        if (!fs::is_regular_file(asset, ec)) return false;
        drawAudioClipAsset(asset);
        return true;
    }
    // a video (.mp4, .m4v, .mov, .webm, .mkv): probe, platform matrix, preview (InspectorVideo.cpp)
    if (VideoFormats::isPlayableExtension(file)){
        std::error_code ec;
        if (!fs::is_regular_file(asset, ec)) return false;
        drawVideoAsset(asset);
        return true;
    }
    // a native plugin's file (plugins/native): what it is for, read-only
    const std::string nativePlatform = NativePlugins::platformOf(project->getProjectPath(), asset);
    if (!nativePlatform.empty()){
        std::error_code ec;
        if (!fs::exists(asset, ec)) return false;
        static const std::map<std::string, std::string> kinds = {
            {"android", "Native plugin source (Android)"}, {"apple", "Native plugin source (iOS and macOS)"},
            {"ios", "Native plugin source (iOS)"}, {"macos", "Native plugin source (macOS)"},
            {"web", "Native plugin source (Web)"}, {"cpp", "Native plugin source (C++, every platform)"}};
        drawAssetHeader(ICON_FA_PLUG, asset, kinds.at(nativePlatform).c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", nativePlatform == "cpp"
            ? "Compiled into the Play library and every export. Lua calls its JASPERENGINE_NATIVE_PLUGIN by name: Native.call(\"Name\", \"method\", ...)."
            : "Built into that platform's export only (editor Play answers with the plugin's Lua mock in native_mocks). "
              "Lua calls it by class or object name: Native.call(\"Name\", \"method\", ...).");
        ImGui::TextDisabled("Edit it in the code editor or your IDE; Project Settings > Native Plugins lists the SDK settings. "
                            "Guide: jasper://docs/native-bridge.");
        ImGui::PopTextWrapPos();
        return true;
    }
    // a physics material: its four fields (InspectorPhysics2D.cpp)
    if (PhysicsMaterialAssets::isPhysicsMaterialFile(file)){
        std::error_code ec;
        if (!fs::is_regular_file(asset, ec)) return false;
        drawPhysicsMaterialAsset(asset);
        return true;
    }
    // a .material: the rows of a submesh's material (InspectorMaterialPbr.cpp)
    if (Util::isMaterialFile(file)){
        std::error_code ec;
        if (!fs::is_regular_file(asset, ec)) return false;
        drawMaterialAsset(asset);
        return true;
    }
    // a generated normal map or its source image: their own view; any other image keeps the scene selection
    if (Util::isImageFile(file)) return drawSpriteNormalsAsset(asset);
    const bool clip = Util::isAnimationClipFile(file);
    if (!clip && !Util::isModelFile(file)) return false;
    if (state.relativeOf != asset){
        state.relativeOf = asset;
        state.relative = assetRelativePath(project, asset);
    }
    // gone (deleted, moved outside): the scene selection again
    if (state.relative.empty() || !clipExists(project, state, state.relative)) return false;
    if (clip){
        drawClipAsset(asset, state.relative);
        return true;
    }
    if (!modelClipsOf(project, state, state.relative).empty()){
        drawModelClipsAsset(asset, state.relative);
        return true;
    }
    return false;
}

void editor::InspectorWindow::drawAudioMixerAsset(const fs::path& asset){
    drawAssetHeader(ICON_FA_SLIDERS, asset, "Audio Mixer");
    ImGui::Separator();
    AudioMixerDef def;
    std::string error;
    if (!AudioMixerDocument::read(asset, def, &error)){
        ImGui::TextWrapped("Does not read: %s", error.c_str());
        return;
    }
    const bool projectMixer = project->getAudioMixerFile() == asset.lexically_normal();
    if (projectMixer){
        ImGui::TextDisabled("The project's mixer (Project Settings > Audio)");
    } else if (ImGui::Button("Use as Project Mixer")){
        AudioSettings after = project->getAudioSettings();
        after.mixer = project->normalizeToAssetsRelative(asset).generic_string();
        auto* command = new AudioSettingsCmd(project, project->getAudioSettings(), after);
        command->setNoMerge();
        project->getProjectCommandHistory()->addCommand(command);
    }
    if (ImGui::Button(ICON_FA_SLIDERS " Open in Audio Mixer")){
        editor::getEditorHost().openAudioMixerWindow(asset);
    }
    ImGui::SeparatorText("Groups");
    for (const AudioMixerGroupDef* group : def.treeOrder()){
        int depth = 0;
        for (const AudioMixerGroupDef* parent = def.findGroupById(group->parentId); parent && depth < 16; parent = def.findGroupById(parent->parentId)) depth++;
        std::string effects;
        for (const AudioMixerEffectDef& effect : group->effects){
            const AudioEffectInfo* info = AudioEffects::find(effect.type);
            effects += (effects.empty() ? "" : ", ") + std::string(info ? info->label : effect.typeName.c_str());
        }
        ImGui::Text("%*s%s  %.1f dB%s%s", depth * 2, "", group->name.c_str(), group->volumeDb, group->mute ? "  M" : "", group->solo ? "  S" : "");
        if (!effects.empty()){
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", effects.c_str());
        }
    }
    ImGui::SeparatorText("Exposed Parameters");
    if (def.exposed.empty()) ImGui::TextDisabled("None");
    for (const AudioMixerExposedDef& exposed : def.exposed){
        ImGui::Text("%s", exposed.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s", AudioMixerIO::describeTarget(def, exposed.target).c_str(), exposed.remember ? " (remembered)" : "");
    }
    ImGui::SeparatorText("Snapshots");
    if (def.snapshots.empty()) ImGui::TextDisabled("None");
    for (const AudioMixerSnapshotDef& snapshot : def.snapshots){
        ImGui::Text("%s%s", snapshot.name.c_str(), snapshot.name == def.startSnapshot ? "  (start)" : "");
    }
    ImGui::TextDisabled("Update mode: %s time", def.unscaled ? "unscaled" : "scaled");
}

void editor::InspectorWindow::drawAssetHeader(const char* icon, const fs::path& asset, const char* kind){
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(icon);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    const float kindWidth = textWidth(kind) + ImGui::GetStyle().ItemSpacing.x;
    const float nameSpace = ImGui::GetContentRegionAvail().x - kindWidth;
    // a long name is elided (the kind stays while there is room for it)
    const bool showKind = nameSpace > textWidth("M...M.anim");
    const std::string name = inspectorclips::elideMiddle(asset.filename().string(), showKind ? nameSpace : ImGui::GetContentRegionAvail().x, textWidth);
    ImGui::TextUnformatted(name.c_str());
    UIProbe::item("asset.header");
    ImGui::SetItemTooltip("%s", project->normalizeToProjectRelative(asset).generic_string().c_str());
    if (showKind){
        ImGui::SameLine();
        const float space = ImGui::GetContentRegionAvail().x - textWidth(kind);
        if (space > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + space);
        ImGui::TextDisabled("%s", kind);
    }
    ImGui::Separator();
}

void editor::InspectorWindow::drawClipAsset(const fs::path& asset, const std::string& reference){
    drawAssetHeader(ICON_FA_FILM, asset, "Animation Clip");
    std::string error;
    std::shared_ptr<AnimationClipDocument> document = findClipDocument(project, clipState(), asset.generic_string(), &error);
    if (!document){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    drawClipDocument(*document, reference);
}

void editor::InspectorWindow::drawModelClipsAsset(const fs::path& asset, const std::string& model){
    InspectorClipState& state = clipState();
    drawAssetHeader(ICON_FA_CUBE, asset, "Model");
    const std::vector<anim::ModelClipInfo> clips = modelClipsOf(project, state, model);

    // the clip list with its import settings (S77 R48); the first selected clip shows below
    if (!state.modelClipSettings) state.modelClipSettings = std::make_unique<ModelClipSettingsView>(project);
    state.modelClipSettings->draw(asset, model, clips);
    const std::string shown = state.modelClipSettings->shownClip();
    if (shown.empty()) return;

    const std::string reference = anim::formatSubClipReference(state.modelClipSettings->shownModel(), shown);
    ImGui::SeparatorText(shown.c_str());
    std::string error;
    std::shared_ptr<AnimationClipDocument> document = findClipDocument(project, state, reference, &error);
    if (!document){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
        return;
    }
    drawClipDocument(*document, reference);
}

// The settings, tracks, events, users and warnings of one clip document. Edits commit through
// the document (one project undo step each; a drag commits once when it ends).
void editor::InspectorWindow::drawClipDocument(AnimationClipDocument& document, const std::string& reference){
    InspectorClipState& state = clipState();
    const AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    const bool readOnly = document.isReadOnly();
    const bool playing = project->isAnyScenePlaying();
    // the document's data (a commit replaces it: edits copy it first)
    const ClipData& data = document.data();
    const ImVec4 warning = Theme::Colors::WarningText;

    // The info line, printed from ClipReport (the numbers get_animation returns). Once per revision:
    // the pool's load and release are not free
    if (state.infoReference != reference || state.infoRevision != document.revision()){
        state.infoReference = reference;
        state.infoRevision = document.revision();
        anim::ClipReportRequest request;
        anim::SubClipReference parts;
        request.reference = anim::parseSubClipReference(reference, parts)
            ? FileUtils::pathToGenericUtf8(project->resolveAssetPath(FileUtils::pathFromUtf8(parts.model)).lexically_normal()) + "#" + parts.clip
            : FileUtils::pathToGenericUtf8(document.absolutePath());
        state.infoLine = anim::clipInfoLine(anim::analyzeClip(request));
    }
    if (!state.infoLine.empty()){
        // wrapped by hand: a selectable's label is what find_ui_item reads, whole
        std::string wrapped;
        std::string current;
        const float width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x;
        const std::string dot = " \xC2\xB7 ";
        for (size_t at = 0; at <= state.infoLine.size();){
            size_t next = state.infoLine.find(dot, at);
            const std::string part = state.infoLine.substr(at, next == std::string::npos ? std::string::npos : next - at);
            const std::string extended = current.empty() ? part : current + dot + part;
            if (!current.empty() && ImGui::CalcTextSize(extended.c_str()).x > width){
                wrapped += current + dot + "\n";
                current = part;
            }else{
                current = extended;
            }
            if (next == std::string::npos) break;
            at = next + dot.size();
        }
        wrapped += current;
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::SubtleText);
        ImGui::Selectable((wrapped + "##clip_info_line").c_str(), false, ImGuiSelectableFlags_None, ImVec2(0, ImGui::CalcTextSize(wrapped.c_str()).y));
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Memory: the heap this clip takes when loaded. File: its size on disk");
    }

    auto commit = [&](const ClipData& newData, const std::string& undoName){
        std::string error;
        const uint64_t clock = AnimationClipController::clipCommitClock(project);
        if (!document.commit(newData, undoName, "", &error)){
            state.assetError = error.empty() ? std::string("The clip could not be written") : error;
            Out::error("%s: %s", document.path().c_str(), state.assetError.c_str());
        }else{
            state.assetError.clear();
            // a clip commit of the editor UI: Ctrl+Z undoes it whatever has focus (9.R5c)
            AnimationClipController::markClipCommit(clock);
        }
    };

    // Open in Animation window, Extract to .anim (sub-clips)
    if (hooks.openClip || (readOnly && hooks.extractSubClip)){
        ImGui::BeginDisabled(playing);
        if (hooks.openClip){
            if (ImGui::Button(ICON_FA_FILM " Open in Animation Window##clip_asset_open", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
                hooks.openClip(project, 0, NULL_ENTITY, reference);
            }
            ImGui::SetItemTooltip("Edit the clip in the Animation window");
        }
        if (readOnly && hooks.extractSubClip){
            if (ImGui::Button(ICON_FA_FILE_EXPORT " Extract to .anim...##clip_asset_extract", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
                hooks.extractSubClip(project, 0, NULL_ENTITY, reference);
            }
            ImGui::SetItemTooltip("Write the clip to an editable .anim file (keeps its source)");
        }
        ImGui::EndDisabled();
    }
    // Convert to Humanoid Clip (anim-retarget 9): a generic clip as a clip any humanoid plays
    if (!data.isHumanoid()){
        ImGui::BeginDisabled(playing);
        if (ImGui::Button(ICON_FA_PERSON " Convert to Humanoid Clip...##clip_asset_humanoid", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
            HumanoidConvertDialog::open(project, reference);
        }
        ImGui::SetItemTooltip("A humanoid clip (rotations relative to the neutral pose) that plays on any humanoid character");
        ImGui::EndDisabled();
    }else{
        ImGui::TextColored(Theme::Colors::PrefabText, ICON_FA_PERSON " Humanoid clip%s%s", data.humanoid.sourceModel.empty() ? "" : " (made on ",
                           data.humanoid.sourceModel.empty() ? "" : (data.humanoid.sourceModel + ")").c_str());
    }
    // Reduce Keys...: the dialog measures; nothing changes until it writes
    ImGui::BeginDisabled(playing);
    if (ImGui::Button(ICON_FA_COMPRESS " Reduce Keys...##clip_asset_reduce", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
        anim::SubClipReference parts;
        ReduceKeysDialog::open(project, anim::parseSubClipReference(reference, parts)
            ? FileUtils::pathToGenericUtf8(project->normalizeToProjectRelative(project->resolveAssetPath(FileUtils::pathFromUtf8(parts.model)))) + "#" + parts.clip
            : document.path());
    }
    ImGui::SetItemTooltip("Remove keys a skeleton does not need, with the error it causes measured. The preview writes nothing");
    ImGui::EndDisabled();
    if (readOnly){
        ImGui::TextDisabled(ICON_FA_LOCK " Read-only: a clip of %s", data.source.path.empty() ? "a model" : data.source.path.c_str());
    }

    ImGui::SeparatorText("Settings");
    // its own table (beginTable names tables per component: this one is "..._clip_asset_settings")
    beginTable(ComponentType::AnimationPlayerComponent, getLabelSize("Preview Model"), "clip_asset_settings");
    ImGui::BeginDisabled(readOnly);

    // Frame Rate (Unity's Sample Rate, whole frames): the window's frame grid; key times stay in
    // seconds. Committed once when the drag or the typed value ends.
    propertyHeader("Frame Rate");
    int frameRate = state.editingFrameRate ? state.pendingFrameRate : (int)std::lround(data.frameRate);
    if (ImGui::DragInt("##clip_frame_rate", &frameRate, 0.2f, 1, 240, "%d fps", ImGuiSliderFlags_AlwaysClamp)){
        state.editingFrameRate = true;
        state.pendingFrameRate = frameRate;
    }
    if (ImGui::IsItemDeactivated()){
        if (state.editingFrameRate && (float)state.pendingFrameRate != data.frameRate){
            ClipData newData = data;
            newData.frameRate = (float)state.pendingFrameRate;
            commit(newData, "Clip Frame Rate");
        }
        state.editingFrameRate = false;
    }
    UIProbe::item("clipAsset.frameRate");
    ImGui::SetItemTooltip("Samples per second of the Animation window's frame grid (key times are kept in seconds)");

    propertyHeader("Wrap Mode");
    if (ImGui::BeginCombo("##clip_wrap_mode", inspectorclips::wrapLabel(data.wrap))){
        for (ClipWrapMode wrap : {ClipWrapMode::Once, ClipWrapMode::Loop, ClipWrapMode::PingPong, ClipWrapMode::ClampForever}){
            if (ImGui::Selectable(inspectorclips::wrapLabel(wrap), wrap == data.wrap) && wrap != data.wrap){
                ClipData newData = data;
                newData.wrap = wrap;
                commit(newData, "Clip Wrap Mode");
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("How the clip wraps when a player entry's wrap is Default");

    // Length: automatic (the last key or event) or explicit (at least one frame)
    propertyHeader("Length");
    bool automatic = !data.hasExplicitLength();
    if (ImGui::Checkbox("Auto##clip_length_auto", &automatic)){
        ClipData newData = data;
        newData.length = automatic ? -1.0f : std::max(data.effectiveLength(), 1.0f / std::max(data.frameRate, 1.0f));
        commit(newData, automatic ? "Clip Length Auto" : "Clip Length");
    }
    UIProbe::item("clipAsset.lengthAuto");
    ImGui::SetItemTooltip("Auto: the clip ends at its last key or event");
    ImGui::SameLine();
    if (automatic){
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", inspectorclips::formatSeconds(data.effectiveLength()).c_str());
    }else{
        const float minimum = 1.0f / std::max(data.frameRate, 1.0f);
        float length = state.editingLength ? state.pendingLength : data.length;
        ImGui::SetNextItemWidth(-1);
        if (ImGui::DragFloat("##clip_length", &length, 0.01f, minimum, 3600.0f, "%.2f s", ImGuiSliderFlags_AlwaysClamp)){
            state.editingLength = true;
            state.pendingLength = length;
        }
        if (ImGui::IsItemDeactivated()){
            if (state.editingLength && state.pendingLength != data.length){
                ClipData newData = data;
                newData.length = std::max(state.pendingLength, minimum);
                commit(newData, "Clip Length");
            }
            state.editingLength = false;
        }
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
            if (!file.empty() && (relative.empty() || !isModelOrPrefabFile(relative))){
                state.assetError = "The preview model must be a model or a prefab inside the assets folder.";
                return;
            }
            if (relative == data.previewModel) return;
            ClipData newData = data;
            newData.previewModel = relative;
            commit(newData, "Clip Preview Model");
        };
        ImGui::BeginGroup();
        if (data.previewModel.empty()){
            drawFileLabel("clip_preview_model", "< None >", "", &disabled, fieldWidth);
        }else{
            drawFileLabel("clip_preview_model", fs::path(data.previewModel).filename().string(), data.previewModel, nullptr, fieldWidth);
        }
        ImGui::EndGroup();
        if (!readOnly && draggingFiles(isModelOrPrefabFile) && ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                const std::vector<std::string> files = payloadFiles(payload, isModelOrPrefabFile);
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

    // Root Motion (Unity's clip import names): the block a .anim keeps for the Animator. Each toggle
    // is one project undo step through document.commit; a model's clip reads as it is
    if (!data.isHumanoid() && ImGui::CollapsingHeader("Root Motion##clip_asset_root_motion")){
        const anim::ClipRootMotion rootMotion = anim::readClipRootMotion(data.unknownKeys);
        auto commitRootMotion = [&](const anim::ClipRootMotion& changed, const std::string& undoName){
            ClipData newData = data;
            const std::string text = anim::writeClipRootMotion(changed);
            auto at = std::find_if(newData.unknownKeys.begin(), newData.unknownKeys.end(), [](const auto& entry){ return entry.first == "root_motion"; });
            if (at != newData.unknownKeys.end()) at->second = text;
            else newData.unknownKeys.push_back({"root_motion", text});
            commit(newData, undoName);
        };
        beginTable(ComponentType::AnimationPlayerComponent, getLabelSize("Root Transform Position (XZ)"), "clip_asset_root_motion");
        ImGui::BeginDisabled(readOnly);
        propertyHeader("Root Motion Node");
        char bone[256];
        std::snprintf(bone, sizeof(bone), "%s", rootMotion.bone.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##clip_root_motion_bone", "Auto", bone, sizeof(bone), ImGuiInputTextFlags_EnterReturnsTrue) && rootMotion.bone != bone){
            anim::ClipRootMotion changed = rootMotion;
            changed.bone = bone;
            commitRootMotion(changed, "Root Motion Node");
        }
        ImGui::SetItemTooltip("The bone's path. Auto: the Animator's Root Motion bone, the hips, then the shallowest bone the clip moves");
        struct Row{
            const char* label;
            const char* id;
            bool anim::ClipRootMotion::* member;
            const char* tooltip;
        };
        const Row rows[] = {
            {"Root Transform Rotation", "Bake Into Pose##clip_root_rotation", &anim::ClipRootMotion::bakeRotation, "Keep the heading in the pose: no turning root motion"},
            {"Root Transform Position (Y)", "Bake Into Pose##clip_root_y", &anim::ClipRootMotion::bakePositionY, "Keep the height in the pose"},
            {"Root Transform Position (XZ)", "Bake Into Pose##clip_root_xz", &anim::ClipRootMotion::bakePositionXZ, "Keep the horizontal motion in the pose"},
        };
        for (const Row& row : rows){
            propertyHeader(row.label);
            bool value = rootMotion.*row.member;
            if (ImGui::Checkbox(row.id, &value)){
                anim::ClipRootMotion changed = rootMotion;
                changed.*row.member = value;
                commitRootMotion(changed, row.label);
            }
            ImGui::SetItemTooltip("%s", row.tooltip);
        }
        ImGui::EndDisabled();
        endTable();
    }

    if (!state.assetError.empty()){
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
        ImGui::TextWrapped("%s", state.assetError.c_str());
        ImGui::PopStyleColor();
    }

    // Tracks: path : channel (elided from the start, full in the tooltip), key count
    ImGui::SeparatorText(("Tracks (" + std::to_string(data.tracks.size()) + ")").c_str());
    if (data.tracks.empty()){
        ImGui::TextDisabled("No tracks: add properties in the Animation window.");
    }else if (ImGui::BeginTable("clip_asset_tracks", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                                ImVec2(0, std::min((float)data.tracks.size() + 1.0f, 12.0f) * ImGui::GetTextLineHeightWithSpacing() + 4))){
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Path : Channel");
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, textWidth("Keys 0000"));
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin((int)data.tracks.size());
        while (clipper.Step()){
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++){
                const ClipTrackData& track = data.tracks[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                // a property track: "path : component.property" (anim-properties spec 10)
                std::string label = (track.path.empty() ? std::string(clipui::rootLabel()) : track.path) + " : " +
                                    (track.channel == ClipChannel::Property ? track.component + "." + track.property
                                                                            : std::string(inspectorclips::channelLabel(track.channel)));
                if (track.channel == ClipChannel::Property && !track.known) label += " (not animatable: kept, not played)";
                ImGui::TextUnformatted(inspectorclips::elideStart(label, ImGui::GetContentRegionAvail().x, textWidth).c_str());
                ImGui::SetItemTooltip("%s", label.c_str());
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
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, textWidth("0000.00 s"));
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

    // Users: the Animation Player entries in open scenes and loaded prefabs (R45); a scene
    // user is a link that selects its GameObject
    const std::vector<ClipUser> users = clipUsersOf(project, state, reference);
    size_t prefabUsers = 0;
    for (const ClipUser& user : users) prefabUsers += user.prefabAsset ? 1 : 0;
    ImGui::SeparatorText("Users");
    ImGui::TextUnformatted(inspectorclips::usersText(users.size() - prefabUsers, prefabUsers).c_str());
    ImGui::Indent();
    size_t listed = 0;
    for (const ClipUser& user : users){
        if (listed++ >= MAX_PROBLEM_ROWS){
            ImGui::TextDisabled("... and %zu more", users.size() - MAX_PROBLEM_ROWS);
            break;
        }
        ImGui::PushID((int)listed);
        if (user.prefabAsset){
            ImGui::BulletText("%s (prefab asset), clip %zu", fs::path(user.prefabPath).filename().string().c_str(), user.entry);
            ImGui::PopID();
            continue;
        }
        SceneProject* userScene = project->getScene(user.sceneId);
        std::string entityName = "GameObject";
        std::string stateName;
        if (userScene && userScene->scene && userScene->scene->isEntityCreated(user.entity)){
            entityName = userScene->scene->getEntityName(user.entity);
            if (AnimationPlayerComponent* player = userScene->scene->findComponent<AnimationPlayerComponent>(user.entity)){
                const std::vector<std::string> states = anim::playerStateNames(player->clips);
                if (user.entry < states.size()) stateName = states[user.entry];
            }
        }
        const std::string text = (userScene ? userScene->name : std::string("?")) + ": " + entityName +
                                 (stateName.empty() ? std::string() : " (" + stateName + ")");
        ImGui::Bullet();
        if (userScene && userScene->id == project->getSelectedSceneId() && userScene->scene->isEntityCreated(user.entity)){
            if (ImGui::Selectable(text.c_str())){
                project->replaceSelectedEntities(user.sceneId, {user.entity});
                project->setSelectedSceneForProperties(user.sceneId);
                project->clearInspectedAsset();
            }
            ImGui::SetItemTooltip("Select the GameObject");
        }else{
            ImGui::TextUnformatted(text.c_str());
        }
        ImGui::PopID();
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

float editor::InspectorWindow::modelRowClipButtonsWidth() const{
    const AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    float width = 0.0f;
    if (hooks.addToAnimationPlayer) width += iconButtonWidth(ICON_FA_FILM) + ImGui::GetStyle().ItemSpacing.x;
    if (hooks.extractSubClip) width += iconButtonWidth(ICON_FA_FILE_EXPORT) + ImGui::GetStyle().ItemSpacing.x;
    return width;
}

// Add to Animation Player (disabled when the player lists it) and Extract to .anim of one sub-clip
void editor::InspectorWindow::drawModelRowClipButtons(SceneProject* sceneProject, Entity model, const std::string& subClip, const std::string& entityName){
    const AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    ModelComponent* component = sceneProject->scene->findComponent<ModelComponent>(model);
    if (!component) return;
    const std::string reference = anim::formatSubClipReference(component->filename, subClip);
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    const AnimationPlayerComponent* player = sceneProject->scene->findComponent<AnimationPlayerComponent>(model);
    const bool listed = player && std::any_of(player->clips.begin(), player->clips.end(),
                                              [&](const AnimationPlayerClip& entry){ return entry.clip == reference; });
    const std::string which = entityName.empty() || entityName == subClip ? "\"" + subClip + "\"" :
                              "\"" + subClip + "\" (the clip of entity animation \"" + entityName + "\")";
    bool first = true;
    ImGui::BeginDisabled(!stopped);
    if (hooks.addToAnimationPlayer){
        ImGui::BeginDisabled(listed);
        if (ImGui::Button(ICON_FA_FILM "##model_clip_add")){
            hooks.addToAnimationPlayer(project, sceneProject->id, model, reference);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
            ImGui::SetTooltip("%s", listed ? "Already in the Animation Player" :
                              ("Add sub-clip " + which + " to the Animation Player" + (player ? "" : " (adds one)")).c_str());
        }
        first = false;
    }
    if (hooks.extractSubClip){
        if (!first) ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FILE_EXPORT "##model_clip_extract")){
            hooks.extractSubClip(project, sceneProject->id, model, reference);
        }
        ImGui::SetItemTooltip("Extract to .anim...: sub-clip %s into an editable .anim file", which.c_str());
    }
    ImGui::EndDisabled();
}

// Use Animation Player, and the rows of the sub-clips no entity animation row stands for (all
// of them when the model has no entity animations)
void editor::InspectorWindow::drawModelClipRows(ComponentType cpType, SceneProject* sceneProject, Entity entity, const std::vector<std::string>& shownInRows){
    InspectorClipState& state = clipState();
    ModelComponent* model = sceneProject->scene->findComponent<ModelComponent>(entity);
    if (!model || model->filename.empty()) return;
    const std::string filename = model->filename;
    const bool animationEntities = model->animationEntities;
    const std::vector<anim::ModelClipInfo> clips = modelClipsOf(project, state, filename);
    if (clips.empty()) return;
    const AnimationClipInspectorHooks& hooks = animationClipInspectorHooks();
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;

    if (hooks.useAnimationPlayer && animationEntities){
        ImGui::BeginDisabled(!stopped);
        if (ImGui::Button(ICON_FA_FILM " Use Animation Player##model_use_player", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
            hooks.useAnimationPlayer(project, sceneProject->id, entity);
        }
        UIProbe::item("model.usePlayer");
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Play the model's clips with an Animation Player: removes the entity animations (one undo step)");
    }

    std::vector<anim::ModelClipInfo> rest;
    for (const anim::ModelClipInfo& info : clips){
        if (std::find(shownInRows.begin(), shownInRows.end(), info.name) == shownInRows.end()) rest.push_back(info);
    }
    if (rest.empty()) return;

    ImGui::SeparatorText("Model Clips");
    UIProbe::item("model.clips");
    if (!animationEntities){
        ImGui::TextDisabled("Played by an Animation Player (no entity animations).");
    }
    const float buttons = modelRowClipButtonsWidth();
    beginTable(cpType, getLabelSize("Animation 00"), "model_clips");
    for (const anim::ModelClipInfo& info : rest){
        ImGui::PushID(info.index);
        propertyHeader(info.name, -1, false, false);
        // the value cell: the length, the buttons right-aligned
        const float cellStartX = ImGui::GetCursorPosX();
        const float cellWidth = ImGui::GetContentRegionAvail().x;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", inspectorclips::formatSeconds(info.duration).c_str());
        ImGui::SetItemTooltip("%s: %zu channels", anim::formatSubClipReference(filename, info.name).c_str(), info.channels);
        if (buttons > 0.0f){
            ImGui::SameLine();
            const float x = cellStartX + cellWidth - buttons + ImGui::GetStyle().ItemSpacing.x;
            if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
            drawModelRowClipButtons(sceneProject, entity, info.name, "");
        }
        ImGui::PopID();
    }
    endTable();
}

// the sub-clip names of the Model's file by animation index, for the entity animations table
std::vector<std::string> editor::InspectorWindow::modelSubClipNames(const std::string& model){
    std::vector<std::string> names;
    for (const anim::ModelClipInfo& info : modelClipsOf(project, clipState(), model)){
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
    ImGui::SeparatorText("Clip");
    ImGui::BeginDisabled(!stopped);
    if (hooks.openEntityAnimation){
        if (ImGui::Button(ICON_FA_FILM " Open in Animation Window##animation_open", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
            hooks.openEntityAnimation(project, sceneProject->id, entity);
        }
        ImGui::SetItemTooltip("Edit this animation in the Animation window");
    }
    if (hooks.convertToClip){
        if (ImGui::Button(ICON_FA_FILE_EXPORT " Convert to Clip...##animation_convert", ImVec2(ImGui::GetContentRegionAvail().x, 0))){
            hooks.convertToClip(project, sceneProject->id, entity);
        }
        UIProbe::item("animation.convert");
        ImGui::SetItemTooltip("Write its keyframe tracks to a shared .anim clip played by an Animation Player");
    }
    ImGui::EndDisabled();
}
