// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animation window's clip mode, its drawing (animation clips spec 4.4, 9.R31/R39): toolbar,
// clip dropdown, empty state, the one-line banner (read-only + Extract, Preview on, missing
// bindings, another scene), the sheet (ruler, events lane, binding rows with the value column,
// keys culled to the visible time range, the label splitter, Dopesheet / Curves and the status at
// the bottom left), the menus and popups (keys, rows, Add Property, Rebind) and the shortcuts.

#include "AnimationWindowClip.h"
#include "component/Transform.h"
#include "util/FileUtils.h"
#include "AnimationWindowClipUtil.h"
#include "animation/PropertyBindings.h"
#include "animation/ClipPropertyNames.h"
#include "animation/ClipTangents.h"

#include "imgui_internal.h"
#include "external/IconsFontAwesome6.h"
#include "Project.h"
#include "animation/AnimationClipDocument.h"
#include "animation/AnimationPlayerRuntime.h"
#include "animation/ClipBinding.h"
#include "animation/ModelSubClips.h"
#include "command/type/ClipSceneActions.h"
#include "component/ActionComponent.h"
#include "component/AnimationComponent.h"
#include "component/AnimationPlayerComponent.h"
#include "component/AnimatorComponent.h"
#include "pool/AnimatorAssetPool.h"
#include "animation/CompiledController.h"
#include "component/LatticeComponent.h"
#include "component/MeshComponent.h"
#include "component/ModelComponent.h"
#include "component/SpriteComponent.h"
#include "util/EntityPath.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "shortcuts/CommandCatalog.h"
#include "widget/ColorField.h"

#include <chrono>
#include <functional>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::clipui;

namespace fs = std::filesystem;

namespace {

    // a context menu item's shortcut text: the catalog's binding (S25: rebindable)
    const char* shortcutText(const char* id){
        static std::map<std::string, std::string> texts;
        std::string& text = texts[id];
        text = CommandCatalog::editor().menuShortcutText(id);
        return text.c_str();
    }

    // `text` shortened in its middle ("mixamo...Spinee") to fit `width` (UTF-8 safe)
    std::string elideMiddle(const std::string& text, float width){
        if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
        // code point boundaries
        std::vector<size_t> cuts;
        for (size_t i = 0; i < text.size(); i++){
            if (((unsigned char)text[i] & 0xC0) != 0x80) cuts.push_back(i);
        }
        cuts.push_back(text.size());
        const size_t count = cuts.size() - 1;
        for (size_t keep = count; keep > 2; keep--){
            const size_t head = (keep + 1) / 2, tail = keep / 2;
            const std::string shown = text.substr(0, cuts[head]) + "..." + text.substr(cuts[count - tail]);
            if (ImGui::CalcTextSize(shown.c_str()).x <= width) return shown;
        }
        return "...";
    }

    // what an entity can be animated on, for Rebind ("" = it can)
    std::string channelProblem(Scene* scene, Entity entity, ClipChannel channel){
        switch (channel){
            case ClipChannel::Morph: {
                const MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
                bool morphs = false;
                for (unsigned int s = 0; mesh && s < mesh->numSubmeshes; s++) morphs = morphs || mesh->submeshes[s].hasMorphTarget;
                return morphs ? std::string() : std::string("no blend shapes");
            }
            case ClipChannel::Lattice:
                return scene->findComponent<LatticeComponent>(entity) ? std::string() : std::string("no Lattice");
            default:
                return scene->findComponent<Transform>(entity) ? std::string() : std::string("no Transform");
        }
    }

}

// ---- the frame ----

void AnimationWindowClip::draw(SceneProject* sceneProject, bool focused){
    const auto started = std::chrono::steady_clock::now();
    ImGui::PushID("clip_mode");
    // the label width and the view saved with the editor layout (first frame)
    if (!settingsLoaded) loadSettings();
    // an edit Record does not key (a 2D or UI move of a bound GameObject) says so here
    const std::string recordNote = ClipRecordHooks::takeNote();
    if (!recordNote.empty()) setNotice(recordNote, true);
    // a save, Play or Prefab Mode Auto Save restored the preview, and Record ended with it
    if (controller().takeRecordStoppedNotice()){
        setNotice("Record is off: saving or Play restores the previewed scene, which ends Record (press Record again to go on)", true);
    }
    // the window handles its keys while focused (App: AnimationClipAppHooks::takesKeys)
    controller().setTakesKeys(focused);
    refreshBinding();
    refreshReport();
    refreshSelection();
    resolveSelectedEvent();
    drawToolbar(sceneProject);
    if (!document()){
        drawEmptyState(sceneProject);
    }else{
        drawBanners(sceneProject);
        drawSheet(sceneProject, focused);
        drawEventEditor(sceneProject);
        drawCueEditor();
        drawAddPropertyPopup();
        drawRebindPopup();
        // the catalog's animation.* commands frame the keys over the sheet's width (S25)
        sheetWidth = std::max(1.0f, ImGui::GetWindowWidth() - scaledLabelWidth());
    }
    ImGui::PopID();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    drawMs = drawFrames == 0 ? ms : drawMs * 0.8 + ms * 0.2;
    drawMsMax = std::max(drawMsMax, ms);
    drawFrames++;
}

// ---- toolbar ----

void AnimationWindowClip::drawToolbar(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    const bool stopped = sceneProject && sceneProject->playState == ScenePlayState::STOPPED;
    const std::string blocker = previewBlocker(sceneProject);

    // Preview: the clip at the playhead in the Scene view (blue timeline), restored on stop
    {
        const bool on = c.isPreviewing();
        ImGui::BeginDisabled(!on && !blocker.empty());
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
        if (ImGui::Button("Preview##clip_preview")) togglePreview(sceneProject);
        // UI tests: the toggle and whether it is on (its highlight)
        UIProbe::item(on ? "clip.preview.on" : "clip.preview.off");
        if (on) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
            ImGui::SetTooltip("%s", on ? "Stop previewing: the scene goes back to how it was before"
                                       : !blocker.empty() ? blocker.c_str()
                                       : "Preview the clip at the playhead in the Scene view (the scene is not changed)");
        }
    }
    // Record: edits of the bound hierarchy (Scene view gizmo, Inspector rows, lattice points) key
    // the clip at the playhead (red timeline); leaving it restores the scene
    ImGui::SameLine();
    {
        const bool on = c.isRecording();
        const bool editableClip = doc && !doc->isReadOnly();
        // a humanoid clip keys the rig's joint rotations: only on a humanoid character (anim-retarget 9)
        const std::string recordBlocker = !blocker.empty() ? blocker
            : (doc && doc->data().isHumanoid() && c.getRoot() != NULL_ENTITY && !c.boundHumanoid()) ? std::string("Bind a humanoid character to record this humanoid clip")
            : std::string();
        ImGui::BeginDisabled(!on && (!editableClip || !recordBlocker.empty() || !stopped));
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, mix(Theme::Colors::ErrorText, ImVec4(0, 0, 0, 1), 0.45f));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? ImVec4(1, 1, 1, 1) : Theme::Colors::ErrorText);
        if (ImGui::Button(ICON_FA_CIRCLE "##clip_record")){
            std::string error;
            if (!setRecording(sceneProject, !on, &error)) setNotice(error, true);
        }
        UIProbe::item(on ? "clip.record.on" : "clip.record.off");
        ImGui::PopStyleColor(on ? 2 : 1);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
            ImGui::SetTooltip("%s", on ? (previewBeforeRecord ? "Stop recording (R): the preview stays on"
                                                              : "Stop recording (R): the preview stops and the scene goes back to how it was before")
                                    : !doc ? "Record: open a clip first"
                                    : doc->isReadOnly() ? "Record: the clip is read-only (extract it to edit it)"
                                    : !recordBlocker.empty() ? recordBlocker.c_str()
                                    : "Record (R): moving the bound GameObject or its children (Scene view, Inspector) keys the clip at the playhead");
        }
    }
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    if (doc){
        // transport
        if (ImGui::Button(ICON_FA_BACKWARD_FAST "##clip_first")) seek(0.0f);
        ImGui::SetItemTooltip("First frame");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_BACKWARD_STEP "##clip_prev_key")) stepKey(-1);
        ImGui::SetItemTooltip("Previous key (Alt+,)");
        ImGui::SameLine();
        if (c.isPlaying()){
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
            if (ImGui::Button(ICON_FA_PAUSE "##clip_play")) togglePlay(sceneProject);
            ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Pause (Space)");
        }else{
            ImGui::BeginDisabled(!stopped || (!c.isPreviewing() && !blocker.empty()));
            if (ImGui::Button(ICON_FA_PLAY "##clip_play")) togglePlay(sceneProject);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
                ImGui::SetTooltip("%s", !c.isPreviewing() && !blocker.empty() ? blocker.c_str() : "Play the preview (Space)");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FORWARD_STEP "##clip_next_key")) stepKey(1);
        ImGui::SetItemTooltip("Next key (Alt+.)");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FORWARD_FAST "##clip_last")) seek(snapToFrame(doc->data().effectiveLength()));
        ImGui::SetItemTooltip("Last frame");
        ImGui::SameLine();

        // the playhead in frames or seconds (a typed time lands on a frame)
        const float fps = frameRate();
        if (showFrames){
            int frame = (int)std::lround(c.getTime() * fps);
            ImGui::SetNextItemWidth(Theme::dpi(56.0f));
            if (ImGui::DragInt("##clip_frame", &frame, 0.2f, 0, (int)(kMaxTime * fps))) seek((float)std::max(0, frame) / fps);
        }else{
            float seconds = c.getTime();
            ImGui::SetNextItemWidth(Theme::dpi(64.0f));
            if (ImGui::DragFloat("##clip_seconds", &seconds, 1.0f / fps, 0.0f, kMaxTime, "%.3fs")) seek(snapToFrame(seconds));
        }
        ImGui::SetItemTooltip("%s", formatBoth(c.getTime(), fps, showFrames).c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(78.0f));
        if (ImGui::BeginCombo("##clip_units", showFrames ? "Frames" : "Seconds")){
            if (ImGui::Selectable("Frames", showFrames)) showFrames = true;
            if (ImGui::Selectable("Seconds", !showFrames)) showFrames = false;
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Times in frames or in seconds");
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
    }

    drawClipCombo(sceneProject);

    // Preview over (anim layers spec 9): for a state above the base layer, a base state under it
    if (c.getStateLayer() > 0){
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Preview over");
        ImGui::SameLine();
        const std::string over = c.getPreviewOver();
        ImGui::SetNextItemWidth(std::max(Theme::dpi(90.0f), ImGui::CalcTextSize(over.c_str()).x + ImGui::GetFrameHeight() * 2.0f));
        if (ImGui::BeginCombo("##clip_preview_over", over.empty() ? "None" : over.c_str())){
            if (ImGui::Selectable("None", over.empty())) c.setPreviewOver("");
            for (const std::string& state : c.previewOverChoices()){
                if (ImGui::Selectable((state + "##over").c_str(), state == over)) c.setPreviewOver(state);
            }
            ImGui::EndCombo();
        }
        UIProbe::item("clip.previewOver");
        ImGui::SetItemTooltip("Preview this clip on its layer (with its mask) over a base layer state at the playhead, as in the game");
    }

    if (doc){
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        ImGui::BeginDisabled(doc->isReadOnly());
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Samples");
        ImGui::SameLine();
        int samples = (int)std::lround(frameRate());
        ImGui::SetNextItemWidth(Theme::dpi(44.0f));
        ImGui::InputInt("##clip_samples", &samples, 0, 0);
        if (ImGui::IsItemDeactivatedAfterEdit()){
            std::string error;
            if (!setFrameRate((float)samples, &error)) setNotice(error, true);
        }
        ImGui::SetItemTooltip("Samples: the clip's frame rate, 1 to 240 (the grid keys snap to)");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_MAGNET "##clip_snap")){
            std::string error;
            if (!snapKeysToFrames(&error)) setNotice(error, true);
        }
        ImGui::SetItemTooltip("Snap keys to frames (keys closer than half a frame merge, with a warning)");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_DIAMOND "+##clip_key_all")){
            std::string error;
            if (!keyAt(false, &error)) setNotice(error, true);
        }
        ImGui::SetItemTooltip("Add Key: key every property at the playhead with its current value (K)");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FLAG "+##clip_add_event")){
            std::string error;
            if (!addEvent(snapToFrame(c.getTime()), "", {}, &error)) setNotice(error, true);
            else openEventEditor = true;
        }
        ImGui::SetItemTooltip("Add Event at the playhead");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ICON_FA_MAGNIFYING_GLASS);
        ImGui::SameLine();
        float zoom = pixelsPerSecond / 160.0f;
        ImGui::SetNextItemWidth(Theme::dpi(80.0f));
        if (ImGui::SliderFloat("##clip_zoom", &zoom, 0.0625f, 25.0f, "%.2fx", ImGuiSliderFlags_Logarithmic)){
            pixelsPerSecond = std::clamp(zoom * 160.0f, 10.0f, 4000.0f);
        }
        ImGui::SetItemTooltip("Zoom. The wheel over the ruler or the events lane, or Ctrl+wheel anywhere, zooms at the mouse;\n"
                              "Shift+wheel and the middle mouse button pan; F frames the selected keys, A all");
    }
    ImGui::Separator();
}

void AnimationWindowClip::drawClipCombo(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    // a player with layers: its states grouped by layer ("Holding ▸ Hold", anim layers spec 9)
    const std::vector<std::pair<std::string, std::vector<std::string>>> groups = c.getStatesByLayer();
    const bool layered = groups.size() > 1;
    std::string current = !c.getStateName().empty() ? c.getStateName()
                        : doc ? FileUtils::pathToUtf8(FileUtils::pathFromUtf8(doc->path()).filename()) : std::string("(No Clip)");
    if (doc && doc->isReadOnly()) current = ICON_FA_LOCK " " + current;
    const float width = std::max(Theme::dpi(150.0f), ImGui::CalcTextSize(current.c_str()).x + ImGui::GetFrameHeight() * 2.0f);
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo("##clip_combo", current.c_str())){
        int id = 0;
        for (const auto& group : groups){
            if (layered){
                ImGui::SeparatorText(group.first.c_str());
            }
            for (const std::string& state : group.second){
                ImGui::PushID(id++);
                const std::string label = layered ? group.first + " \xe2\x96\xb8 " + state : state;
                if (ImGui::Selectable(label.c_str(), state == c.getStateName())){
                    std::string error;
                    if (!openState(state, &error)) setNotice(error, true);
                }
                ImGui::PopID();
            }
        }
        SceneProject* bound = boundScene();
        const Entity root = c.getRoot();
        // the clips of the root's Animator controller (S21 R27): opened on the same root
        if (bound && root != NULL_ENTITY && bound->scene->isEntityCreated(root)){
            if (const AnimatorComponent* animator = bound->scene->findComponent<AnimatorComponent>(root);
                    animator && !animator->controller.path.empty()){
                std::shared_ptr<const anim::CompiledController> compiled = AnimatorAssetPool::controller(animator->controller.path);
                if (compiled && !compiled->clips.empty()){
                    ImGui::Separator();
                    ImGui::TextDisabled("Animator: %s", FileUtils::pathToUtf8(FileUtils::pathFromUtf8(animator->controller.path).filename()).c_str());
                    for (size_t i = 0; i < compiled->clips.size(); i++){
                        const std::string& clip = compiled->clips[i].path;
                        anim::SubClipReference subClip;
                        const std::string name = anim::parseSubClipReference(clip, subClip) ? subClip.clip
                                               : FileUtils::pathToUtf8(FileUtils::pathFromUtf8(clip).stem());
                        const bool currentClip = doc && c.getStateName().empty() && doc->path() == clip;
                        if (ImGui::Selectable((name + "##animator_clip" + std::to_string(i)).c_str(), currentClip)){
                            std::string error;
                            if (!open(bound->id, root, clip, &error)) setNotice(error, true);
                        }
                        ImGui::SetItemTooltip("%s", clip.c_str());
                    }
                }
            }
        }
        // the entity Animations targeting the root stay reachable (R31)
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

std::vector<std::string> AnimationWindowClip::entityModeEntries(SceneProject* sceneProject, Entity selected) const{
    std::vector<std::string> entries;
    if (!sceneProject || selected == NULL_ENTITY || !sceneProject->scene->isEntityCreated(selected)) return entries;
    Scene* scene = sceneProject->scene;
    if (scene->findComponent<AnimationComponent>(selected)) return entries;
    const Entity playerRoot = AnimationClipController::findPlayerRoot(scene, selected);
    if (playerRoot != NULL_ENTITY){
        for (const std::string& state : anim::playerStateNames(scene->getComponent<AnimationPlayerComponent>(playerRoot).clips)){
            entries.push_back(state + " (clip)");
        }
    }
    entries.push_back("Create New Clip...");
    return entries;
}

bool AnimationWindowClip::drawEntityModeEntries(SceneProject* sceneProject, Entity selected){
    const std::vector<std::string> entries = entityModeEntries(sceneProject, selected);
    if (entries.empty()) return false;
    Scene* scene = sceneProject->scene;
    const Entity playerRoot = AnimationClipController::findPlayerRoot(scene, selected);
    const std::vector<std::string> states = playerRoot != NULL_ENTITY
        ? anim::playerStateNames(scene->getComponent<AnimationPlayerComponent>(playerRoot).clips) : std::vector<std::string>();
    bool picked = false;
    for (size_t i = 0; i < states.size(); i++){
        ImGui::PushID((int)i + 100000);
        if (ImGui::Selectable(entries[i].c_str(), false)){
            entityModeRequest = EntityModeRequest::State;
            entityModeState = states[i];
            picked = true;
        }
        UIProbe::item("entity.clipEntry." + std::to_string(i));
        ImGui::PopID();
    }
    const bool canCreate = sceneProject->playState == ScenePlayState::STOPPED;
    if (ImGui::Selectable("Create New Clip...##entity_mode", false, canCreate ? 0 : ImGuiSelectableFlags_Disabled)){
        entityModeRequest = EntityModeRequest::Create;
        picked = true;
    }
    UIProbe::item("entity.clipEntry." + std::to_string(states.size()));
    ImGui::Separator();
    return picked;
}

void AnimationWindowClip::runEntityModeRequest(SceneProject* sceneProject, Entity selected){
    const EntityModeRequest request = entityModeRequest;
    entityModeRequest = EntityModeRequest::None;
    if (request == EntityModeRequest::None || !sceneProject) return;
    bind(sceneProject->id, selected);
    std::string error;
    if (request == EntityModeRequest::State){
        if (!openState(entityModeState, &error)) setNotice(error, true);
    }else{
        askCreateNewClip(sceneProject);
    }
}

// ---- empty state, banner ----

void AnimationWindowClip::drawEmptyState(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    SceneProject* bound = boundScene();
    const Entity root = c.getRoot();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::Dummy(ImVec2(0.0f, std::max(0.0f, avail.y * 0.18f)));
    auto centered = [&](const std::string& text, const ImVec4* textColor, float scale){
        ImGui::SetWindowFontScale(scale);
        const float w = ImGui::CalcTextSize(text.c_str()).x;
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (avail.x - w) * 0.5f));
        if (textColor) ImGui::PushStyleColor(ImGuiCol_Text, *textColor);
        ImGui::TextUnformatted(text.c_str());
        if (textColor) ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
    };
    centered(ICON_FA_FILM, &Theme::Colors::SubtleText, 2.5f);
    if (!bound || root == NULL_ENTITY || !bound->scene->isEntityCreated(root)){
        centered("Select a GameObject to animate it, or open an Animation Clip from the Project window.", &Theme::Colors::SubtleText, 1.0f);
        return;
    }
    const std::string name = bound->scene->getEntityName(root);
    if (!c.getStateName().empty()){
        // a state whose clip file can't be opened
        centered("The clip of state '" + c.getStateName() + "' can't be opened (missing or not a clip).", &Theme::Colors::WarningText, 1.0f);
    }
    centered("To begin animating " + name + ", create an Animation Clip.", nullptr, 1.0f);
    const char* button = "Create...";
    const float bw = ImGui::CalcTextSize(button).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), (avail.x - bw) * 0.5f));
    const bool canCreate = sceneProject && sceneProject->id == bound->id && sceneProject->playState == ScenePlayState::STOPPED;
    ImGui::BeginDisabled(!canCreate);
    if (ImGui::Button(button)) askCreateNewClip(sceneProject);
    UIProbe::item("clip.emptyState.create");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Adds an Animation Player to %s (when it has none) and a new clip file (one undo step)", name.c_str());
}

void AnimationWindowClip::drawBanners(SceneProject* sceneProject){
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc) return;
    SceneProject* bound = boundScene();
    const bool otherScene = bound && sceneProject && bound->id != sceneProject->id && c.getRoot() != NULL_ENTITY;
    const bool targetless = c.getRoot() == NULL_ENTITY;
    const bool missing = report.root != NULL_ENTITY && report.total > report.resolved;
    if (!doc->isReadOnly() && doc->exists() && !otherScene && !targetless && !missing) return;
    // everything on one line: icon + short text + action, the explanation in the tooltips
    bool first = true;
    auto next = [&](){
        if (!first){
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
        }
        first = false;
    };
    ImGui::AlignTextToFramePadding();
    if (doc->isReadOnly()){
        next();
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_LOCK " Read-Only");
        ImGui::SetItemTooltip("%s is an animation of a model: extract it to an .anim file to edit it", doc->path().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_FILE_EXPORT " Extract to .anim...")) askExtract(sceneProject);
        ImGui::SetItemTooltip(c.getStateName().empty() ? "Writes the animation to a new .anim file (one project undo step) and opens it"
                                                       : "Writes the animation to a new .anim file and switches the Animation Player's state to it (one undo step)");
    }
    if (!doc->exists()){
        next();
        ImGui::TextColored(Theme::Colors::ErrorText, ICON_FA_TRIANGLE_EXCLAMATION " Deleted");
        ImGui::SetItemTooltip("%s was deleted", doc->path().c_str());
    }
    if (otherScene){
        next();
        ImGui::TextColored(Theme::Colors::WarningText, "Bound to %s in %s", bound->scene->getEntityName(c.getRoot()).c_str(), bound->name.c_str());
        ImGui::SetItemTooltip("The clip is bound to a GameObject of another scene: select its scene tab to preview it, or select a GameObject here");
    }
    if (targetless){
        // the file is edited; the preview runs on an instance of the preview model
        next();
        ImGui::TextColored(Theme::Colors::SubtleText, "Preview Model");
        ImGui::SameLine();
        static char buffer[512];
        const std::string model = doc->isReadOnly() ? previewOverride : doc->data().previewModel;
        const ImGuiID fieldId = ImGui::GetID("##clip_preview_model");
        if (ImGui::GetActiveID() != fieldId) std::snprintf(buffer, sizeof(buffer), "%s", model.c_str());
        ImGui::SetNextItemWidth(Theme::dpi(220.0f));
        const std::string hint = previewModelOf().empty() ? std::string("a model or prefab (drop one here)") : previewModelOf();
        ImGui::InputTextWithHint("##clip_preview_model", hint.c_str(), buffer, sizeof(buffer));
        if (ImGui::IsItemDeactivatedAfterEdit()){
            std::string error;
            if (!setPreviewModel(buffer, &error)) setNotice(error, true);
        }
        if (ImGui::BeginDragDropTarget()){
            const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files", ImGuiDragDropFlags_AcceptBeforeDelivery);
            if (payload){
                const std::vector<std::string> paths = Util::getStringsFromPayload(payload);
                const bool accepted = !paths.empty() && (anim::isModelFilePath(paths.front()) || editor::FileUtils::pathFromUtf8(paths.front()).extension() == ".prefab");
                if (accepted && payload->IsDelivery()){
                    std::string error;
                    if (!setPreviewModel(paths.front(), &error)) setNotice(error, true);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SetItemTooltip(doc->isReadOnly()
            ? "The model or prefab to preview this model animation on (an instance in the open scene; for this session, the clip is not written)"
            : "The model or prefab the clip previews on when no GameObject is selected (an instance in the open scene)");
    }
    if (missing){
        next();
        if (report.legacyModelLayout){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Legacy model layout");
            ImGui::SetItemTooltip("The model is loaded without its node hierarchy, so its bones can't be animated by the clip");
            ImGui::SameLine();
            const bool canFix = sceneProject && sceneProject->id == c.getSceneId() && sceneProject->playState == ScenePlayState::STOPPED;
            ImGui::BeginDisabled(!canFix);
            if (ImGui::SmallButton("Reload Models in Node Hierarchy")){
                ClipActionResult result;
                std::string error;
                c.stopPreview();
                if (ClipSceneActions::reloadModelsForPlayer(project, c.getSceneId(), c.getRoot(), result, error)){
                    setNotice(result.status, false);
                }else{
                    setNotice(error, true);
                }
                reportRevision = ~uint64_t(0);
            }
            ImGui::EndDisabled();
        }else{
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %zu of %zu missing", report.total - report.resolved, report.total);
            ImGui::SetItemTooltip("Bindings that don't resolve on %s: right-click a yellow row to Rebind or Remove it",
                                  report.root != NULL_ENTITY && valueScene() ? valueScene()->getEntityName(report.root).c_str() : "the root");
        }
    }
}

// ---- the sheet ----

void AnimationWindowClip::drawRuler(ImDrawList* drawList, ImVec2 origin, float width, float height){
    const SheetMetrics m = sheetMetrics();
    drawList->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), color(Theme::Colors::ToolbarBackground));
    const float fps = frameRate();
    // labels at least ~70 px apart: frame multiples, or round seconds
    const float minSpacing = 70.0f * m.s;
    float step = 1.0f / fps;
    if (showFrames){
        const float frames[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800, 3600, 18000, 36000};
        for (float f : frames){
            step = f / fps;
            if (step * pixelsPerSecond >= minSpacing) break;
        }
    }else{
        const float seconds[] = {0.05f, 0.1f, 0.25f, 0.5f, 1, 2, 5, 10, 30, 60, 300, 600, 1800};
        for (float sec : seconds){
            step = sec;
            if (step * pixelsPerSecond >= minSpacing) break;
        }
    }
    const float left = origin.x + m.timePad;
    const long firstTick = std::max(0L, (long)std::floor((scrollX / pixelsPerSecond) / step));
    const long lastTick = (long)std::ceil(((scrollX + width) / pixelsPerSecond) / step);
    drawList->PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    for (long i = firstTick; i <= lastTick; i++){
        const float t = (float)i * step;
        const float x = left + t * pixelsPerSecond - scrollX;
        drawList->AddLine(ImVec2(x, origin.y + height * 0.45f), ImVec2(x, origin.y + height), color(Theme::Colors::SubtleText));
        const std::string label = formatTime(t, fps, showFrames);
        // a label that would be cut by the right edge is left out
        if (x + 3.0f * m.s + ImGui::CalcTextSize(label.c_str()).x <= origin.x + width){
            drawList->AddText(ImVec2(x + 3.0f * m.s, origin.y + 1.0f * m.s), color(Theme::Colors::SubtleText), label.c_str());
        }
        for (int k = 1; k < 5; k++){
            const float xm = x + step * pixelsPerSecond * (float)k / 5.0f;
            drawList->AddLine(ImVec2(xm, origin.y + height * 0.75f), ImVec2(xm, origin.y + height), color(Theme::Colors::SubtleText, 0.5f));
        }
    }
    drawList->PopClipRect();
    drawList->AddLine(ImVec2(origin.x, origin.y + height - 1.0f), ImVec2(origin.x + width, origin.y + height - 1.0f),
                      color(Theme::Colors::ToolbarBorder));
}

void AnimationWindowClip::drawRowLabel(const Row& row, size_t index, ImVec2 pos, float width, float height){
    const SheetMetrics m = sheetMetrics();
    std::shared_ptr<AnimationClipDocument> doc = document();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float textY = pos.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
    // labels end 8 px before the value column (no run-in)
    const float labelEnd = pos.x + width - m.valueWidth - 8.0f * m.s;
    if (row.kind == Row::Kind::Summary){
        const std::string clip = doc ? FileUtils::pathToUtf8(FileUtils::pathFromUtf8(doc->path()).filename()) : std::string();
        drawList->PushClipRect(pos, ImVec2(labelEnd, pos.y + height), true);
        drawList->AddText(ImVec2(pos.x + m.indent * 0.5f, textY), color(Theme::Colors::SubtleText), clip.c_str());
        drawList->PopClipRect();
        return;
    }
    const ClipTrackData& track = doc->data().tracks[(size_t)row.track];
    if (row.kind == Row::Kind::Track){
        // a property folds when it has components (a colour's r g b a, a vector's x y z w)
        const bool foldable = track.channel != ClipChannel::Property || !componentLabels((size_t)row.track).empty();
        const bool open = expanded.count(rowKey(track)) > 0;
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 2.0f * m.s, pos.y));
        ImGui::PushID((int)index);
        if (foldable){
            if (ImGui::InvisibleButton("##fold", ImVec2(m.indent, height))){
                if (open) expanded.erase(rowKey(track));
                else expanded.insert(rowKey(track));
            }
            ImGui::RenderArrow(drawList, ImVec2(pos.x + 3.0f * m.s, textY), color(Theme::Colors::SubtleText),
                               open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
        }
        // the label: right-click for the row menu (Rebind / Retarget / Remove)
        ImGui::SetCursorScreenPos(ImVec2(pos.x + m.indent + 2.0f * m.s, pos.y));
        ImGui::InvisibleButton("##label", ImVec2(std::max(1.0f, labelEnd - pos.x - m.indent - 2.0f * m.s), height));
        UIProbe::item("clip.rowLabel." + std::to_string(row.track));
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)){
            // opened by drawRowMenu: OpenPopup here would use this row's ID scope
            rowMenuTrack = row.track;
            rowMenuRequested = true;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)){
            // selects the row's keys (Unity)
            std::vector<KeyId> keys;
            for (const ClipKey& key : track.keys) keys.push_back(keyIdOf(track, key.time));
            const ImGuiIO& io = ImGui::GetIO();
            selectKeys(keys, io.KeyShift || io.KeyCtrl);
        }
        if (!row.message.empty()) ImGui::SetItemTooltip("%s\n%s", row.label.c_str(), row.message.c_str());
        else if (!row.masked.empty()) ImGui::SetItemTooltip("%s\n%s", row.label.c_str(), row.masked.c_str());
        else ImGui::SetItemTooltip("%s", row.label.c_str());
        ImGui::PopID();
        // the leaf name, elided in its middle; " : Channel" and the missing mark always show
        const std::string icon = row.missing ? std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") : std::string();
        const std::string suffix = std::string(" : ") + row.channelText + (row.missing ? " (Missing!)" : "");
        const std::string name = row.leaf.substr(0, row.leaf.size() - (std::string(" : ") + row.channelText).size());
        const float x = pos.x + m.indent + 4.0f * m.s;
        const float room = labelEnd - x - ImGui::CalcTextSize((icon + suffix).c_str()).x;
        const std::string shown = icon + elideMiddle(name, std::max(room, ImGui::CalcTextSize("...").x)) + suffix;
        drawList->PushClipRect(pos, ImVec2(labelEnd, pos.y + height), true);
        // a track the layer's mask hides is dimmed (anim layers spec 9)
        drawList->AddText(ImVec2(x, textY), row.missing ? color(Theme::Colors::WarningText) :
                          !row.masked.empty() ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : ImGui::GetColorU32(ImGuiCol_Text), shown.c_str());
        drawList->PopClipRect();
        return;
    }
    // a component row (x / y / z, a blend shape, the lattice point count)
    drawList->PushClipRect(pos, ImVec2(labelEnd, pos.y + height), true);
    drawList->AddText(ImVec2(pos.x + m.indent * 2.0f + 4.0f * m.s, textY),
                      row.missing ? color(Theme::Colors::WarningText) :
                      !row.masked.empty() ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : color(Theme::Colors::SubtleText), row.label.c_str());
    drawList->PopClipRect();
}

void AnimationWindowClip::drawValueField(const Row& row, ImVec2 pos, float width, float height){
    if (row.kind == Row::Kind::Summary || row.track < 0) return;
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    const SheetMetrics m = sheetMetrics();
    const ClipTrackData& track = doc->data().tracks[(size_t)row.track];
    if (track.channel == ClipChannel::Property){
        drawPropertyValueField(row, pos, width, height);
        return;
    }
    // the lattice point count shows on the track row only
    if (row.kind == Row::Kind::Component && track.channel == ClipChannel::Lattice) return;
    // what the Scene view shows while previewing (incl. an unkeyed pink value), else the clip's value
    std::vector<float> value;
    bool shown = false;
    if (c.isPreviewing() && !row.missing && isHumanoidChannel(track.channel)){
        // the bound rig's pose as the humanoid value (anim-retarget 9)
        std::shared_ptr<const anim::HumanoidSlots> humanoid = c.boundHumanoid();
        shown = humanoid && anim::readHumanoidValue(valueScene(), *humanoid->target, track.channel, track.humanBone, value);
    }else if (c.isPreviewing() && !row.missing){
        Scene* scene = valueScene();
        const Entity entity = scene ? anim::resolvePath(scene, valueRoot(), track.path) : NULL_ENTITY;
        shown = entity != NULL_ENTITY && anim::readChannelValue(scene, entity, track.channel, track.points, value);
    }
    if (!shown) value = sampledValue((size_t)row.track, c.getTime());
    const bool pending = c.hasPendingValue((size_t)row.track);
    const float right = pos.x + width - 4.0f * m.s;
    ImGui::PushID(row.track * 64 + row.component + 1);
    if (row.kind == Row::Kind::Track){
        char text[96] = "";
        if (isAngleTrack(track)){
            const Vector3 e = anglesOf(track, value);
            std::snprintf(text, sizeof(text), "%.1f  %.1f  %.1f", e.x, e.y, e.z);
        }else if (track.channel == ClipChannel::Morph){
            std::snprintf(text, sizeof(text), "%zu weights", value.size());
        }else if (track.channel == ClipChannel::Lattice){
            std::snprintf(text, sizeof(text), "%zu points", track.points.size());
        }else if (value.size() >= 3){
            std::snprintf(text, sizeof(text), "%.2f  %.2f  %.2f", value[0], value[1], value[2]);
        }
        // right-aligned in the value column
        const float w = ImGui::CalcTextSize(text).x;
        ImGui::GetWindowDrawList()->AddText(ImVec2(std::max(pos.x + width - m.valueWidth, right - w), pos.y + (height - ImGui::GetTextLineHeight()) * 0.5f),
                                            color(pending ? pendingTextColor() : Theme::Colors::SubtleText), text);
        ImGui::PopID();
        return;
    }
    // an editable component: unkeyed edits preview in pink until K keys them
    ImGui::SetCursorScreenPos(ImVec2(pos.x + width - m.valueWidth, pos.y + (height - ImGui::GetFrameHeight()) * 0.5f));
    float v = 0.0f;
    const bool editingEuler = isAngleTrack(track) && eulerEditTrack == row.track;
    if (isAngleTrack(track)){
        const Vector3 e = editingEuler ? eulerEdit : anglesOf(track, value);
        v = row.component == 0 ? e.x : row.component == 1 ? e.y : e.z;
    }else if ((size_t)row.component < value.size()){
        v = value[(size_t)row.component];
    }
    if (pending) ImGui::PushStyleColor(ImGuiCol_FrameBg, pendingFieldColor());
    ImGui::SetNextItemWidth(m.valueWidth - 4.0f * m.s);
    ImGui::BeginDisabled(doc->isReadOnly() || row.missing);
    const float speed = isAngleTrack(track) ? 0.5f : 0.01f;
    const bool changed = ImGui::DragFloat("##value", &v, speed, 0.0f, 0.0f, "%.3f");
    if (ImGui::IsItemActivated()){
        valueMergeKey.clear();
        if (isAngleTrack(track)){
            // the triple stays as dragged while the field is active (M13)
            eulerEditTrack = row.track;
            eulerEdit = anglesOf(track, value);
        }
    }
    if (changed){
        std::string error;
        if (!setComponentValue((size_t)row.track, row.component, v, ImGui::IsItemActivated(), &error)) setNotice(error, true);
    }
    if (ImGui::IsItemDeactivated() && eulerEditTrack == row.track) eulerEditTrack = -1;
    ImGui::EndDisabled();
    if (pending) ImGui::PopStyleColor();
    ImGui::SetItemTooltip(pending ? "Not keyed: K keys it (the value goes when the playhead moves)"
                                  : "The value at the playhead. Editing it previews the change; K keys it");
    ImGui::PopID();
}

void AnimationWindowClip::drawPropertyValueField(const Row& row, ImVec2 pos, float width, float height){
    AnimationClipController& c = controller();
    std::shared_ptr<AnimationClipDocument> doc = document();
    const SheetMetrics m = sheetMetrics();
    const ClipTrackData& track = doc->data().tracks[(size_t)row.track];
    if (!track.known) return;
    // what the Scene view shows while previewing (incl. a pink value), else the clip's value;
    // the whole property in key space (a swizzle track its component)
    std::vector<float> value;
    bool shown = false;
    anim::PropertyBinding binding;
    if (c.isPreviewing() && !row.missing){
        Scene* scene = valueScene();
        const Entity entity = scene ? anim::resolvePath(scene, valueRoot(), track.path) : NULL_ENTITY;
        anim::PropertyRef ref;
        anim::parsePropertyRef(track.component, track.property, ref);
        if (entity != NULL_ENTITY) binding = anim::bindProperty(scene, entity, ref, track.width);
        float whole[4] = {0, 0, 0, 0};
        if (binding.status == anim::PropertyBindStatus::Bound && anim::readPropertyKeySpace(scene, entity, binding, whole)){
            value = binding.swizzle >= 0 ? std::vector<float>{whole[binding.swizzle]} : std::vector<float>(whole, whole + binding.width);
            shown = true;
        }
    }
    if (!shown) value = sampledValue((size_t)row.track, c.getTime());
    if (value.empty()) return;
    const bool pending = c.hasPendingValue((size_t)row.track);
    const bool readOnly = doc->isReadOnly() || row.missing;
    const PropertyKind kind = track.kind;
    ImGui::PushID(row.track * 64 + row.component + 1);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + width - m.valueWidth, pos.y + (height - ImGui::GetFrameHeight()) * 0.5f));
    if (pending) ImGui::PushStyleColor(ImGuiCol_FrameBg, pendingFieldColor());
    ImGui::BeginDisabled(readOnly);
    std::string error;
    const bool colour = kind == PropertyKind::Color3 || kind == PropertyKind::Color4;
    if (row.kind == Row::Kind::Component){
        // one component of a colour or vector
        float v = (size_t)row.component < value.size() ? value[(size_t)row.component] : 0.0f;
        ImGui::SetNextItemWidth(m.valueWidth - 4.0f * m.s);
        const bool changed = ImGui::DragFloat("##value", &v, 0.01f, 0.0f, 0.0f, "%.3f");
        if (ImGui::IsItemActivated()) valueMergeKey.clear();
        if (changed && !setComponentValue((size_t)row.track, row.component, v, ImGui::IsItemActivated(), &error)) setNotice(error, true);
    }else if (kind == PropertyKind::Bool){
        bool on = value[0] > 0.5f;
        if (ImGui::Checkbox("##value", &on)){
            valueMergeKey.clear();
            if (!setPropertyValue((size_t)row.track, {on ? 1.0f : 0.0f}, true, &error)) setNotice(error, true);
        }
    }else if (kind == PropertyKind::Int || kind == PropertyKind::Frame){
        int v = (int)std::lround(value[0]);
        ImGui::SetNextItemWidth(m.valueWidth - 4.0f * m.s);
        const bool changed = ImGui::DragInt("##value", &v, 0.1f);
        if (ImGui::IsItemActivated()) valueMergeKey.clear();
        if (changed && !setPropertyValue((size_t)row.track, {(float)v}, ImGui::IsItemActivated(), &error)) setNotice(error, true);
    }else if (colour && track.swizzle < 0 && value.size() >= 3){
        // the Inspector's colour widget, in sRGB (the keys' space)
        Vector4 rgba(value[0], value[1], value[2], value.size() > 3 ? value[3] : 1.0f);
        ui::ColorFieldOptions options;
        options.alpha = kind == PropertyKind::Color4;
        options.linearStorage = false;
        options.label = "Key Color";
        options.width = m.valueWidth - 4.0f * m.s;
        const ui::EditPhase phase = ui::colorField("##value", rgba, options);
        // the Color window's session is one merged value edit (a cancel puts the original back)
        if (phase == ui::EditPhase::Begin) valueMergeKey.clear();
        if (phase == ui::EditPhase::Changing || phase == ui::EditPhase::Cancelled || phase == ui::EditPhase::Applied){
            const float channels[4] = {rgba.x, rgba.y, rgba.z, rgba.w};
            std::vector<float> next(channels, channels + (kind == PropertyKind::Color4 ? 4 : 3));
            if (!setPropertyValue((size_t)row.track, next, false, &error)) setNotice(error, true);
        }
    }else if (value.size() == 1){
        float v = value[0];
        ImGui::SetNextItemWidth(m.valueWidth - 4.0f * m.s);
        const bool changed = ImGui::DragFloat("##value", &v, 0.01f, 0.0f, 0.0f, "%.3f");
        if (ImGui::IsItemActivated()) valueMergeKey.clear();
        if (changed && !setPropertyValue((size_t)row.track, {v}, ImGui::IsItemActivated(), &error)) setNotice(error, true);
    }else{
        // a vector: its components fold out
        std::string text;
        for (size_t i = 0; i < value.size(); i++){
            char number[32];
            std::snprintf(number, sizeof(number), "%.2f", value[i]);
            text += (i ? "  " : "") + std::string(number);
        }
        const float right = pos.x + width - 4.0f * m.s;
        const float w = ImGui::CalcTextSize(text.c_str()).x;
        ImGui::GetWindowDrawList()->AddText(ImVec2(std::max(pos.x + width - m.valueWidth, right - w), pos.y + (height - ImGui::GetTextLineHeight()) * 0.5f),
                                            color(pending ? pendingTextColor() : Theme::Colors::SubtleText), text.c_str());
    }
    ImGui::EndDisabled();
    if (pending) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()){
        ImGui::SetTooltip(pending ? "Not keyed: K keys it (the value goes when the playhead moves)"
                                  : track.discrete ? "The value at the playhead (stepped: it changes at its keys). Editing it previews the change; K keys it"
                                  : "The value at the playhead. Editing it previews the change; K keys it");
    }
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
    const bool readOnly = doc->isReadOnly();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float labelW = std::clamp(scaledLabelWidth(), 160.0f * m.s, std::max(160.0f * m.s, avail.x - 120.0f * m.s));
    const float bottomHeight = std::max(ImGui::GetStyle().ScrollbarSize, ImGui::GetFrameHeight());
    // the Audio lane under the events lane (S40 R9): a row per cue kind in use
    const float audioHeight = audioLaneHeight(data);
    const float headerHeight = m.rulerHeight + m.eventsHeight + audioHeight;
    // at least the header and four rows
    const float sheetHeight = std::max(headerHeight + 4.0f * m.rowHeight, avail.y - bottomHeight - ImGui::GetStyle().ItemSpacing.y);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float keyAreaX = origin.x + labelW;
    const float keyAreaWidth = std::max(1.0f, avail.x - labelW);
    const float timeX = keyAreaX + m.timePad;
    auto timeAt = [&](float x){ return (x - timeX + scrollX) / pixelsPerSecond; };
    auto xAt = [&](float t){ return timeX + t * pixelsPerSecond - scrollX; };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // header: Add Property over the labels, the ruler (scrub); the "Events" caption, the lane
    drawList->AddRectFilled(origin, ImVec2(keyAreaX, origin.y + headerHeight), color(Theme::Colors::ToolbarBackground));
    drawRuler(drawList, ImVec2(keyAreaX, origin.y), keyAreaWidth, m.rulerHeight);
    // the ruler turns red while recording (Unity)
    if (c.isRecording()){
        drawList->AddRectFilled(ImVec2(keyAreaX, origin.y), ImVec2(keyAreaX + keyAreaWidth, origin.y + m.rulerHeight), color(Theme::Colors::ErrorText, 0.3f));
    }
    ImGui::SetCursorScreenPos(ImVec2(keyAreaX, origin.y));
    ImGui::InvisibleButton("##clip_ruler", ImVec2(keyAreaWidth, m.rulerHeight));
    if (ImGui::IsItemActive() && (ImGui::IsItemActivated() || io.MouseDelta.x != 0.0f)){
        seek(snapToFrame(std::max(0.0f, timeAt(io.MousePos.x))));
    }
    const bool rulerHovered = ImGui::IsItemHovered();
    drawEventsLane(drawList, ImVec2(keyAreaX, origin.y + m.rulerHeight), keyAreaWidth, m.eventsHeight, timeX);
    const bool eventsHovered = ImGui::IsItemHovered();
    if (audioHeight > 0.0f){
        drawAudioLaneLabels(drawList, ImVec2(origin.x, origin.y + m.rulerHeight + m.eventsHeight), labelW);
        drawAudioLane(drawList, ImVec2(keyAreaX, origin.y + m.rulerHeight + m.eventsHeight), keyAreaWidth, audioHeight, timeX);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 4.0f * m.s, origin.y + (m.rulerHeight - ImGui::GetFrameHeight()) * 0.5f + 1.0f * m.s));
    ImGui::BeginDisabled(readOnly);
    if (ImGui::SmallButton(ICON_FA_PLUS " Add Property")) openAddProperty = true;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
        ImGui::SetTooltip("%s", readOnly ? "Read-only clip: extract it to edit it" : "Animate a property of the bound GameObject or one under it");
    }
    drawList->AddText(ImVec2(origin.x + 8.0f * m.s, origin.y + m.rulerHeight + (m.eventsHeight - ImGui::GetTextLineHeight()) * 0.5f),
                      color(Theme::Colors::SubtleText), "Events");

    // the label column's splitter (on the header: the rows' child window is above the parent)
    ImGui::SetCursorScreenPos(ImVec2(keyAreaX - 4.0f * m.s, origin.y));
    ImGui::InvisibleButton("##clip_splitter", ImVec2(8.0f * m.s, headerHeight));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) dragStartLabelWidth = labelWidth;
    if (ImGui::IsItemActive()){
        labelWidth = std::clamp(dragStartLabelWidth + ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x / m.s, 160.0f, 900.0f);
    }
    // the width is kept with the editor layout
    if (ImGui::IsItemDeactivated()) saveSettings();
    ImGui::SetItemTooltip("Drag to resize the property column");

    // zoom and pan with the wheel (over the header, or Ctrl: zoom at the mouse; Shift: pan)
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

    const float playX = xAt(c.getTime());
    const ImU32 playheadColour = color(playheadColor());
    if (view == View::Curves){
        // the curves of the bound channels in place of the key rows (AnimationWindowClipCurves.cpp)
        drawCurves(sceneProject, ImVec2(origin.x, origin.y + headerHeight), labelW, avail.x, sheetHeight - headerHeight);
    }else{
        // the rows: labels + value column on the left, keys on the right (scrolls vertically)
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + headerHeight));
        ImGuiWindowFlags childFlags = (io.KeyCtrl || io.KeyShift) ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None;
        ImGui::BeginChild("##clip_rows", ImVec2(avail.x, sheetHeight - headerHeight), ImGuiChildFlags_None, childFlags);
        ImDrawList* rowsList = ImGui::GetWindowDrawList();
        const ImVec2 top = ImGui::GetCursorScreenPos();
        const float contentHeight = std::max((float)rows.size() * m.rowHeight + m.rowHeight, ImGui::GetContentRegionAvail().y);
        const ImVec2 clipMin = ImGui::GetWindowPos();
        const ImVec2 clipMax = ImVec2(clipMin.x + ImGui::GetWindowSize().x, clipMin.y + ImGui::GetWindowSize().y);

        // backgrounds: the timeline is red while recording, blue while previewing (Unity)
        const ImU32 keyBackground = c.isRecording() ? color(Theme::Colors::ErrorText, 0.22f)
                                  : c.isPreviewing() ? color(Theme::Colors::Selection, 0.28f) : color(ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
        rowsList->AddRectFilled(ImVec2(keyAreaX, clipMin.y), ImVec2(keyAreaX + keyAreaWidth, clipMax.y), keyBackground);
        const float clipEnd = xAt(data.effectiveLength());
        if (clipEnd < keyAreaX + keyAreaWidth){
            rowsList->AddRectFilled(ImVec2(std::max(keyAreaX, clipEnd), clipMin.y), ImVec2(keyAreaX + keyAreaWidth, clipMax.y), IM_COL32(0, 0, 0, 60));
        }
        // the visible time range (keys outside it are not visited)
        const float visibleFrom = std::max(0.0f, timeAt(keyAreaX) - m.keyHit / pixelsPerSecond);
        const float visibleTo = timeAt(keyAreaX + keyAreaWidth) + m.keyHit / pixelsPerSecond;
        // frame grid lines when frames are wide enough
        if (pixelsPerSecond / fps >= 8.0f * m.s){
            const long first = (long)std::floor(visibleFrom * fps);
            const long last = (long)std::ceil(visibleTo * fps);
            for (long f = std::max(0L, first); f <= last; f++){
                const float x = xAt((float)f / fps);
                rowsList->AddLine(ImVec2(x, clipMin.y), ImVec2(x, clipMax.y), IM_COL32(255, 255, 255, 14));
            }
        }

        // the keys of track `t` in the visible range
        auto visibleKeys = [&](size_t t, size_t& begin, size_t& end){
            const std::vector<ClipKey>& keys = data.tracks[t].keys;
            begin = (size_t)(std::lower_bound(keys.begin(), keys.end(), visibleFrom, [](const ClipKey& k, float time){ return k.time < time; }) - keys.begin());
            end = (size_t)(std::upper_bound(keys.begin(), keys.end(), visibleTo, [](float time, const ClipKey& k){ return time < k.time; }) - keys.begin());
        };
        struct Visible{ float time; size_t track; size_t key; };

        int hoveredRow = -1;
        hoveredKeyIds.clear();
        const ImVec4 selectedColor = selectedKeyColor();
        const float keyAlpha = readOnly ? 0.5f : 1.0f;
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
            drawRowLabel(row, r, rowPos, labelW, m.rowHeight);
            drawValueField(row, rowPos, labelW, m.rowHeight);

            // the visible keys: one diamond per time (the summary row merges every track's times)
            std::vector<Visible> keys;
            if (row.kind == Row::Kind::Summary){
                for (size_t t = 0; t < data.tracks.size(); t++){
                    size_t b = 0, e = 0;
                    visibleKeys(t, b, e);
                    for (size_t k = b; k < e; k++) keys.push_back({data.tracks[t].keys[k].time, t, k});
                }
                std::sort(keys.begin(), keys.end(), [](const Visible& a, const Visible& b){ return a.time < b.time; });
            }else{
                size_t b = 0, e = 0;
                visibleKeys((size_t)row.track, b, e);
                for (size_t k = b; k < e; k++) keys.push_back({data.tracks[(size_t)row.track].keys[k].time, (size_t)row.track, k});
            }
            rowsList->PushClipRect(ImVec2(keyAreaX, clipMin.y), ImVec2(keyAreaX + keyAreaWidth, clipMax.y), true);
            const float cy = rowPos.y + m.rowHeight * 0.5f;
            // property rows (anim-properties spec 10): a bool's on bars, an int's or frame's values,
            // a colour's sampled gradient under its keys
            if (row.kind == Row::Kind::Track && data.tracks[(size_t)row.track].channel == ClipChannel::Property){
                const ClipTrackData& track = data.tracks[(size_t)row.track];
                const float keyAreaRight = keyAreaX + keyAreaWidth;
                if (track.kind == PropertyKind::Bool && track.known){
                    for (size_t k = 0; k < track.keys.size(); k++){
                        if (track.keys[k].value.empty() || track.keys[k].value[0] <= 0.5f) continue;
                        const float x0 = std::max(keyAreaX, xAt(track.keys[k].time));
                        const float x1 = k + 1 < track.keys.size() ? xAt(track.keys[k + 1].time) : keyAreaRight;
                        if (x1 <= keyAreaX || x0 >= keyAreaRight) continue;
                        rowsList->AddRectFilled(ImVec2(x0, cy - m.keyRadius * 0.45f), ImVec2(std::min(x1, keyAreaRight), cy + m.keyRadius * 0.45f),
                                                color(Theme::Colors::Selection, 0.45f));
                    }
                }else if ((track.kind == PropertyKind::Int || track.kind == PropertyKind::Frame) && track.known){
                    for (size_t k = 0; k < track.keys.size(); k++){
                        if (track.keys[k].value.empty()) continue;
                        const float x = xAt(track.keys[k].time);
                        if (x < keyAreaX - 40.0f * m.s || x > keyAreaRight) continue;
                        const std::string text = std::to_string((long)std::lround(track.keys[k].value[0]));
                        rowsList->AddText(ImVec2(x + m.keyRadius + 2.0f * m.s, rowPos.y + (m.rowHeight - ImGui::GetTextLineHeight()) * 0.5f),
                                          color(Theme::Colors::SubtleText), text.c_str());
                    }
                }else if ((track.kind == PropertyKind::Color3 || track.kind == PropertyKind::Color4) && track.known && track.swizzle < 0 &&
                          !track.keys.empty()){
                    // the sampled colour every few pixels over the keyed span (the keys are sRGB)
                    std::shared_ptr<const AnimationClip> clip = doc->clip();
                    const int built = clip ? builtTrack(*clip, track) : -1;
                    if (built >= 0){
                        const float from = std::max(keyAreaX, xAt(track.keys.front().time));
                        const float to = std::min(keyAreaRight, xAt(track.keys.back().time));
                        const float step = 4.0f * m.s;
                        anim::TrackSample sample;
                        for (float x = from; x < to; x += step){
                            const float t = timeAt(x + step * 0.5f);
                            if (!anim::sampleTrack(clip->getTracks()[(size_t)built], t, sample) || sample.weights.size() < 3) continue;
                            const float a = sample.weights.size() > 3 ? std::clamp(sample.weights[3], 0.0f, 1.0f) : 1.0f;
                            const ImVec4 c(std::clamp(sample.weights[0], 0.0f, 1.0f), std::clamp(sample.weights[1], 0.0f, 1.0f),
                                           std::clamp(sample.weights[2], 0.0f, 1.0f), 0.35f + 0.55f * a);
                            rowsList->AddRectFilled(ImVec2(x, rowPos.y + m.rowHeight * 0.68f), ImVec2(std::min(x + step, to), rowPos.y + m.rowHeight - 2.0f * m.s),
                                                    ImGui::GetColorU32(c));
                        }
                    }
                }
            }
            const bool rowHovered = ImGui::IsWindowHovered() && io.MousePos.y >= rowPos.y && io.MousePos.y < rowPos.y + m.rowHeight &&
                                    io.MousePos.x >= keyAreaX;
            float lastX = -1e9f;
            bool lastSelected = false;
            for (size_t i = 0; i < keys.size();){
                const float t = keys[i].time;
                size_t j = i;
                bool selected = true;
                while (j < keys.size() && std::fabs(keys[j].time - t) <= kKeyEpsilon){
                    selected = selected && isSelected(keys[j].track, keys[j].key);
                    j++;
                }
                const float x = xAt(t);
                // keys closer than a pixel to the last one drawn (zoomed out on a dense clip) share its diamond
                if (x - lastX < 1.0f * m.s && selected == lastSelected && !(rowHovered && std::fabs(io.MousePos.x - x) <= m.keyHit)){
                    i = j;
                    continue;
                }
                lastX = x;
                lastSelected = selected;
                const float radius = row.kind == Row::Kind::Component ? m.keyRadius * 0.8f : m.keyRadius;
                ImVec4 fill = selected ? selectedColor : keyColor();
                if (!selected && row.missing) fill = Theme::Colors::WarningText;
                if (!selected && row.kind == Row::Kind::Summary) fill = Theme::Colors::SubtleText;
                const ImVec2 pts[4] = {ImVec2(x, cy - radius), ImVec2(x + radius, cy), ImVec2(x, cy + radius), ImVec2(x - radius, cy)};
                rowsList->AddConvexPolyFilled(pts, 4, color(fill, fill.w * keyAlpha));
                rowsList->AddPolyline(pts, 4, IM_COL32(20, 20, 20, (int)(255 * keyAlpha)), ImDrawFlags_Closed, 1.0f * m.s);
                // UI tests click a track's key where the layout puts it ("clip.key.<track>.<key>")
                if (row.kind == Row::Kind::Track && UIProbe::enabled() && x >= keyAreaX && x <= keyAreaX + keyAreaWidth){
                    UIProbe::rect("clip.key." + std::to_string(keys[i].track) + "." + std::to_string(keys[i].key), x - radius, cy - radius, x + radius, cy + radius);
                }
                if (rowHovered && std::fabs(io.MousePos.x - x) <= m.keyHit){
                    hoveredRow = (int)r;
                    hoveredTime = t;
                    hoveredKeyIds.clear();
                    for (size_t k = i; k < j; k++){
                        const ClipTrackData& track = data.tracks[keys[k].track];
                        hoveredKeyIds.push_back(keyIdOf(track, keys[k].time));
                    }
                }
                i = j;
            }
            rowsList->PopClipRect();
        }

        // the key area's input: click / Shift / Ctrl select, box select, drag, right-click menu
        ImGui::SetCursorScreenPos(ImVec2(keyAreaX, top.y));
        ImGui::InvisibleButton("##clip_keys", ImVec2(keyAreaWidth, contentHeight), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const int mouseRow = (int)std::floor((io.MousePos.y - top.y) / m.rowHeight);
        auto allSelected = [&](const std::vector<KeyId>& ids){
            // selection membership by identity (the mask is per revision)
            for (const KeyId& id : ids){
                bool found = false;
                for (const AnimationClipController::KeyRef& ref : selectedRefs){
                    const ClipTrackData& track = data.tracks[ref.track];
                    if (track.channel == id.channel && std::fabs(track.keys[ref.key].time - id.time) <= kKeyEpsilon && track.path == id.path){
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            return true;
        };
        if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            const bool additive = io.KeyShift || io.KeyCtrl;
            dragStart = io.MousePos;
            if (!hoveredKeyIds.empty()){
                const bool already = allSelected(hoveredKeyIds);
                if (io.KeyCtrl && already){
                    // Ctrl toggles
                    std::vector<KeyId> kept;
                    for (const KeyId& id : selection){
                        bool hit = false;
                        for (const KeyId& other : hoveredKeyIds){
                            hit = hit || (other.path == id.path && other.channel == id.channel && std::fabs(other.time - id.time) <= kKeyEpsilon);
                        }
                        if (!hit) kept.push_back(id);
                    }
                    selection = kept;
                    refreshSelection();
                    drag = Drag::None;
                }else{
                    if (!already) selectKeys(hoveredKeyIds, additive);
                    dragGrabTime = hoveredTime;
                    dragAdditive = additive;
                    drag = Drag::PendingKeys;
                }
            }else{
                if (!additive){
                    selection.clear();
                    selectEvent(-1);
                    refreshSelection();
                }
                dragAdditive = additive;
                drag = Drag::Box;
            }
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            if (drag == Drag::PendingKeys && std::fabs(io.MousePos.x - dragStart.x) > 3.0f * m.s){
                std::string error;
                if (!readOnly && beginDrag(&error)){
                    drag = Drag::Keys;
                }else{
                    setNotice(readOnly ? std::string("Read-only clip: extract it to edit it") : error, true);
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
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && (drag == Drag::Keys || drag == Drag::Box || drag == Drag::PendingKeys)){
            if (drag == Drag::Keys){
                endDrag();
            }else if (drag == Drag::Box){
                if (std::fabs(io.MousePos.x - dragStart.x) > 2.0f * m.s || std::fabs(io.MousePos.y - dragStart.y) > 2.0f * m.s){
                    const int rowA = (int)std::floor((dragStart.y - top.y) / m.rowHeight);
                    const int rowB = (int)std::floor((io.MousePos.y - top.y) / m.rowHeight);
                    boxSelect(rowA, rowB, timeAt(dragStart.x), timeAt(io.MousePos.x), dragAdditive);
                }
            }else if (drag == Drag::PendingKeys && !dragAdditive && !hoveredKeyIds.empty()){
                // a click (no drag) on one of several selected keys selects only those
                selectKeys(hoveredKeyIds, false);
            }
            drag = Drag::None;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && drag == Drag::None){
            contextRow = mouseRow;
            contextTime = snapToFrame(std::max(0.0f, timeAt(io.MousePos.x)));
            if (!hoveredKeyIds.empty()){
                if (!allSelected(hoveredKeyIds)) selectKeys(hoveredKeyIds, false);
                contextTime = hoveredTime;
            }
            ImGui::OpenPopup("##clip_key_menu");
        }
        if (ImGui::IsItemHovered() && hoveredRow >= 0 && drag == Drag::None){
            ImGui::SetTooltip("%s", formatBoth(hoveredTime, fps, showFrames).c_str());
        }
        drawKeyContextMenu();
        drawRowMenu();

        // the playhead
        if (playX >= keyAreaX && playX <= keyAreaX + keyAreaWidth){
            rowsList->AddLine(ImVec2(playX, clipMin.y), ImVec2(playX, clipMax.y), playheadColour, 1.5f * m.s);
        }
        ImGui::SetCursorScreenPos(top);
        ImGui::Dummy(ImVec2(avail.x, contentHeight));
        ImGui::EndChild();
    }

    if (playX >= keyAreaX && playX <= keyAreaX + keyAreaWidth){
        drawList->AddLine(ImVec2(playX, origin.y), ImVec2(playX, origin.y + headerHeight), playheadColour, 1.5f * m.s);
        const ImVec2 tri[3] = {ImVec2(playX - m.playheadHalf, origin.y), ImVec2(playX + m.playheadHalf, origin.y),
                               ImVec2(playX, origin.y + m.playheadHalf * 1.6f)};
        drawList->AddConvexPolyFilled(tri, 3, playheadColour);
    }
    drawList->AddLine(ImVec2(keyAreaX, origin.y), ImVec2(keyAreaX, origin.y + sheetHeight), color(Theme::Colors::ToolbarBorder));

    // the bottom row: Dopesheet / Curves and the status on the left, the time scrollbar right
    const ImVec2 bottom = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 2.0f * m.s, bottom.y));
    ImGui::PushStyleColor(ImGuiCol_Button, view == View::Dopesheet ? Theme::Colors::ButtonActivated : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::SmallButton("Dopesheet") && view != View::Dopesheet){
        view = View::Dopesheet;
        saveSettings();
    }
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("The keys of every property in rows");
    ImGui::SameLine(0.0f, 2.0f * m.s);
    ImGui::PushStyleColor(ImGuiCol_Button, view == View::Curves ? Theme::Colors::ButtonActivated : ImGui::GetStyleColorVec4(ImGuiCol_Button));
    if (ImGui::SmallButton("Curves") && view != View::Curves){
        view = View::Curves;
        saveSettings();
    }
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("The curves of the properties: drag key points in time and value; the wheel zooms the values,\n"
                          "right-click a key or a curve for its interpolation; F frames the selected keys, A all");
    ImGui::SameLine();
    // the notice, else what Record does now (short; the sentence in the tooltip)
    const std::string recording = notice.empty() ? recordStatus(false) : std::string();
    if (!notice.empty() || !recording.empty()){
        const std::string& text = notice.empty() ? recording : notice;
        const float room = keyAreaX - ImGui::GetCursorScreenPos().x - 6.0f * m.s;
        const std::string shown = elideMiddle(text, std::max(10.0f, room));
        const ImVec4 colour = !notice.empty() ? (noticeWarning ? Theme::Colors::WarningText : Theme::Colors::SubtleText) : Theme::Colors::ErrorText;
        ImGui::TextColored(colour, "%s", shown.c_str());
        if (notice.empty()) UIProbe::item("clip.recordStatus");
        const std::string tip = notice.empty() ? recordStatus(true) : notice;
        if (shown != text || notice.empty()) ImGui::SetItemTooltip("%s", tip.c_str());
    }
    const float contentWidth = std::max(keyAreaWidth, (std::max(viewLength(), c.getTime()) + 1.0f) * pixelsPerSecond);
    const float barY = bottom.y + (bottomHeight - ImGui::GetStyle().ScrollbarSize) * 0.5f;
    ImRect barRect(ImVec2(keyAreaX, barY), ImVec2(keyAreaX + keyAreaWidth, barY + ImGui::GetStyle().ScrollbarSize));
    ImS64 scrollValue = (ImS64)std::llround(scrollX);
    const ImS64 visibleValue = std::max<ImS64>(1, (ImS64)std::llround(keyAreaWidth));
    const ImS64 contentValue = std::max(visibleValue, (ImS64)std::llround(std::max(contentWidth, scrollX + keyAreaWidth)));
    ImGui::ScrollbarEx(barRect, ImGui::GetID("##clip_hscroll"), ImGuiAxis_X, &scrollValue, visibleValue, contentValue);
    scrollX = std::max(0.0f, (float)scrollValue);
    ImGui::SetCursorScreenPos(bottom);
    ImGui::Dummy(ImVec2(avail.x, bottomHeight));
}

// ---- menus and popups ----

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
    // Unity's key tangents (the curves; the track becomes a Hermite one, eases drop)
    if (ImGui::BeginMenu("Tangents")){
        ClipTangentMode common = ClipTangentMode::None;
        bool uniform = true, first = true;
        // anim-properties: the per-side items need keys of scalar curves (not rotations, not
        // stepped properties)
        bool sideKeys = false;
        if (doc){
            for (const AnimationClipController::KeyRef& ref : selectedRefs){
                if (ref.track >= doc->data().tracks.size() || ref.key >= doc->data().tracks[ref.track].keys.size()) continue;
                const ClipTangentMode mode = doc->data().tracks[ref.track].keys[ref.key].tangentMode;
                if (first) common = mode;
                else if (mode != common) uniform = false;
                first = false;
                sideKeys = sideKeys || cliptangents::editable(doc->data().tracks[ref.track]);
            }
        }
        std::string notice;
        const ClipTangentMode modes[] = {ClipTangentMode::ClampedAuto, ClipTangentMode::Auto, ClipTangentMode::Free,
                                         ClipTangentMode::Flat, ClipTangentMode::Linear, ClipTangentMode::Constant,
                                         ClipTangentMode::Broken};
        for (ClipTangentMode mode : modes){
            if (mode == ClipTangentMode::Linear) ImGui::Separator();
            if (ImGui::MenuItem(clipui::tangentModeLabel(mode), nullptr, uniform && !first && common == mode)){
                if (!setTangentMode(mode, &error)) setNotice(error, true);
            }
        }
        // Unity's Left / Right / Both Tangents: one side Free, Linear or Constant (the key Broken)
        if (sideKeys){
            ImGui::Separator();
            AnimationClipController& ctl = controller();
            struct SideChoice{ const char* label; cliptangents::Side side; };
            const SideChoice sides[] = {{"Left Tangent", cliptangents::Side::Left}, {"Right Tangent", cliptangents::Side::Right},
                                        {"Both Tangents", cliptangents::Side::Both}};
            struct TypeChoice{ const char* label; cliptangents::SideType type; };
            const TypeChoice types[] = {{"Free", cliptangents::SideType::Free}, {"Linear", cliptangents::SideType::Linear},
                                        {"Constant", cliptangents::SideType::Constant}};
            for (const SideChoice& side : sides){
                if (!ImGui::BeginMenu(side.label)) continue;
                for (const TypeChoice& type : types){
                    if (ImGui::MenuItem(type.label)){
                        if (!ctl.setSideTangent(selectedRefs, side.side, type.type, &error, &notice)) setNotice(error, true);
                        else if (!notice.empty()) setNotice(notice, true);
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndMenu();
    }
    UIProbe::item("clip.keyMenu.tangents");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::Separator();
    if (ImGui::MenuItem("Copy", shortcutText("animation.copy"), false, !selectedRefs.empty())) copySelected();
    if (ImGui::MenuItem("Paste", shortcutText("animation.paste"), false, editable && !clipboard.empty())){
        if (!paste(contextTime, &error)) setNotice(error, true);
    }
    if (ImGui::MenuItem("Duplicate", shortcutText("animation.duplicate"), false, editable && !selectedRefs.empty())){
        if (!duplicateSelected(&error)) setNotice(error, true);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Move Playhead Here")) seek(contextTime);
    ImGui::EndPopup();
}

void AnimationWindowClip::drawRowMenu(){
    if (rowMenuRequested){
        rowMenuRequested = false;
        ImGui::OpenPopup("##clip_row_menu");
    }
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
    if (ImGui::MenuItem(missing ? "Rebind..." : "Retarget...")) openRebindFor(rowMenuTrack);
    UIProbe::item("clip.rowMenu.retarget");
    ImGui::SetItemTooltip("Rewrites the property's path in the clip: it changes for every Animation Player using the clip");
    if (ImGui::MenuItem("Add Key at Playhead")){
        if (!addKeysAt(rowMenuTrack, controller().getTime(), &error)) setNotice(error, true);
    }
    if (ImGui::MenuItem("Remove Property")){
        if (!controller().removeTrack((size_t)rowMenuTrack, &error)) setNotice(error, true);
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
    const std::string needle = lower(addPropertySearch);
    const ClipData& data = doc->data();
    std::string error;
    bool done = false;

    // the channels an entity offers
    auto channelsOf = [&](Entity entity){
        std::vector<ClipChannel> channels = {ClipChannel::Position, ClipChannel::Rotation, ClipChannel::Scale};
        if (channelProblem(scene, entity, ClipChannel::Morph).empty()) channels.push_back(ClipChannel::Morph);
        if (channelProblem(scene, entity, ClipChannel::Lattice).empty()) channels.push_back(ClipChannel::Lattice);
        return channels;
    };
    auto addChannels = [&](Entity entity, const std::string& path){
        for (ClipChannel channel : channelsOf(entity)){
            const bool exists = data.findTrack(path, channel) >= 0;
            std::string label = std::string(channelLabel(channel)) + (exists ? "  (animated)" : "");
            ImGui::PushID((int)channel);
            if (ImGui::Selectable(label.c_str(), false, exists ? ImGuiSelectableFlags_Disabled : 0)){
                // the key lands on the Samples grid (I3)
                AnimationClipController& c = controller();
                c.setTime(snapToFrame(c.getTime()));
                size_t track = 0;
                if (!c.addProperty(path, channel, &track, &error)){
                    setNotice(error, true);
                }else{
                    std::shared_ptr<AnimationClipDocument> now = document();
                    if (now && track < now->data().tracks.size()) expanded.insert(rowKey(now->data().tracks[track]));
                }
                done = true;
            }
            ImGui::PopID();
        }
        // the component properties (anim-properties spec 10): GameObject Active, then one group
        // per component in the Inspector's names (Enabled and its registry rows; a colour also
        // as Alpha only), a sprite's material colour as Sprite Renderer : Color, Lua scripts
        const std::vector<anim::AnimatablePropertyInfo> properties = anim::listAnimatableProperties(scene, entity);
        std::vector<std::string> groups;
        for (const anim::AnimatablePropertyInfo& info : properties){
            if (std::find(groups.begin(), groups.end(), info.component) == groups.end()) groups.push_back(info.component);
        }
        auto addPropertyItem = [&](const std::string& component, const std::string& property, const std::string& label){
            const bool exists = data.findPropertyTrack(path, component, property) >= 0;
            const std::string text = label + (exists ? "  (animated)" : "");
            ImGui::PushID(property.c_str());
            if (ImGui::Selectable(text.c_str(), false, exists ? ImGuiSelectableFlags_Disabled : 0)){
                AnimationClipController& c = controller();
                c.setTime(snapToFrame(c.getTime()));
                size_t track = 0;
                if (!c.addPropertyTrack(path, component, property, &track, &error)) setNotice(error, true);
                done = true;
            }
            ImGui::PopID();
        };
        for (const std::string& group : groups){
            if (done) break;
            ImGui::PushID(group.c_str());
            const std::string groupLabel = ClipPropertyNames::groupLabel(scene, entity, group);
            if (ImGui::TreeNodeEx("##group", 0, "%s", groupLabel.c_str())){
                std::string submeshGroup;
                bool submeshOpen = true;
                for (const anim::AnimatablePropertyInfo& info : properties){
                    if (info.component != group || done) continue;
                    std::string label = info.label;
                    // a sprite's colour (Lua Sprite.color) is its material's base colour: Sprite Renderer : Color
                    const bool spriteColor = group == "mesh" && info.property == "submeshes[0].material.baseColorFactor" &&
                                             scene->findComponent<SpriteComponent>(entity);
                    if (spriteColor) label = "Color";
                    // "Material [N] : X" rows fold under Material [N]
                    const size_t colon = label.find(" : ");
                    if (!spriteColor && colon != std::string::npos && label.compare(0, 9, "Material ") == 0){
                        const std::string sub = label.substr(0, colon);
                        if (sub != submeshGroup){
                            if (!submeshGroup.empty() && submeshOpen) ImGui::TreePop();
                            submeshGroup = sub;
                            submeshOpen = ImGui::TreeNodeEx(sub.c_str(), 0, "%s", sub.c_str());
                        }
                        if (!submeshOpen) continue;
                        label = label.substr(colon + 3);
                    }
                    addPropertyItem(info.component, info.property, label);
                    if (info.kind == PropertyKind::Color4) addPropertyItem(info.component, info.property + ".a", label + " : Alpha only");
                }
                if (!submeshGroup.empty() && submeshOpen) ImGui::TreePop();
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    };
    ImGui::BeginChild("##tree", ImVec2(Theme::dpi(380.0f), Theme::dpi(320.0f)), ImGuiChildFlags_None);
    // audio cues first (S40 R10): Audio Cue, Snapshot Cue; a GameObject's Source Cue under it
    if (drawAddCueItems(needle)) done = true;
    addPropertyCueKind = -1;
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
                        if (drawSourceCueItem(entity, path)) done = true;
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
                visit(child, childPath, childOk);
            }
        }else if (!ok){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " %s (can't be animated)", name.c_str());
            ImGui::SetItemTooltip("%s", reason.empty() ? "a GameObject above it can't be addressed by an animation path" : reason.c_str());
        }else{
            const std::string label = entity == root ? name + " (root)" : name;
            if (ImGui::TreeNodeEx("##node", entity == root ? ImGuiTreeNodeFlags_DefaultOpen : 0, "%s", label.c_str())){
                if (drawSourceCueItem(entity, path)) done = true;
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

    // humanoid tracks (anim-retarget 9): every slot and extra bone, the body position and the
    // foot goals, keyed from the bound humanoid rig
    std::shared_ptr<const anim::HumanoidSlots> humanoid = controller().boundHumanoid();
    if (!done && (humanoid || data.isHumanoid())){
        ImGui::PushID("humanoid");
        if (!humanoid){
            ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION " Humanoid (bind a humanoid character)");
            ImGui::SetItemTooltip("Humanoid tracks are keyed from a humanoid rig: bind a character whose model maps the humanoid bones");
        }else if (ImGui::TreeNodeEx("##humanoid", needle.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen, "Humanoid")){
            auto humanoidItem = [&](ClipChannel channel, int bone){
                const std::string label = humanoidTrackLabel(channel, bone).substr(std::string("Humanoid : ").size());
                if (!needle.empty() && lower(label).find(needle) == std::string::npos) return;
                const bool exists = data.findHumanoidTrack(channel, bone) >= 0;
                std::vector<float> value;
                const bool available = anim::readHumanoidValue(scene, *humanoid->target, channel, bone, value);
                ImGui::PushID(bone + (channel == ClipChannel::HumanoidPosition ? 1000 : 0));
                const std::string text = label + (exists ? "  (animated)" : available ? "" : "  (not on this rig)");
                if (ImGui::Selectable(text.c_str(), false, exists || !available ? ImGuiSelectableFlags_Disabled : 0)){
                    AnimationClipController& c = controller();
                    c.setTime(snapToFrame(c.getTime()));
                    size_t track = 0;
                    if (!c.addProperty(anim::humanoidTrackBoneName(channel, bone), channel, &track, &error)){
                        setNotice(error, true);
                    }else{
                        std::shared_ptr<AnimationClipDocument> now = document();
                        if (now && track < now->data().tracks.size()) expanded.insert(rowKey(now->data().tracks[track]));
                    }
                    done = true;
                }
                ImGui::PopID();
            };
            humanoidItem(ClipChannel::HumanoidPosition, HUMANOID_POSITION_HIPS);
            for (int b = 0; b < anim::HUMANOID_TRACK_BONE_COUNT && !done; b++) humanoidItem(ClipChannel::Humanoid, b);
            humanoidItem(ClipChannel::HumanoidPosition, HUMANOID_POSITION_LEFT_FOOT);
            humanoidItem(ClipChannel::HumanoidPosition, HUMANOID_POSITION_RIGHT_FOOT);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (done) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void AnimationWindowClip::openRebindFor(int track){
    std::shared_ptr<AnimationClipDocument> doc = document();
    rebindCandidates.clear();
    rebindReasons.clear();
    rebindNearest = 0;
    rebindSceneUsers = rebindPrefabUsers = 0;
    if (!doc || track < 0 || track >= (int)doc->data().tracks.size()) return;
    rebindTrack = track;
    rebindSearch[0] = '\0';
    openRebind = true;
    rebindMissing = false;
    for (const Row& row : buildRows()){
        if (row.kind == Row::Kind::Track && row.track == track) rebindMissing = row.missing;
    }
    // computed once, when the popup opens
    for (const ClipUser& user : findClipUsers(project, doc->path())){
        if (user.prefabAsset) rebindPrefabUsers++;
        else rebindSceneUsers++;
    }
    Scene* scene = valueScene();
    const Entity root = valueRoot();
    if (!scene || root == NULL_ENTITY) return;
    const ClipTrackData& data = doc->data().tracks[(size_t)track];
    rebindCandidates = ClipValidation::nearestPaths(scene, root, data.path, 5);
    rebindNearest = rebindCandidates.size();
    const std::vector<std::pair<std::string, Entity>> paths = anim::descendantPaths(scene, root);
    for (const auto& entry : paths){
        if (std::find(rebindCandidates.begin(), rebindCandidates.end(), entry.first) == rebindCandidates.end()) rebindCandidates.push_back(entry.first);
    }
    for (const std::string& path : rebindCandidates){
        const Entity entity = anim::resolvePath(scene, root, path);
        rebindReasons.push_back(entity == NULL_ENTITY ? std::string("not found") : channelProblem(scene, entity, data.channel));
    }
}

void AnimationWindowClip::drawRebindPopup(){
    if (openRebind){
        openRebind = false;
        ImGui::OpenPopup("##clip_rebind");
    }
    if (!ImGui::BeginPopup("##clip_rebind")) return;
    std::shared_ptr<AnimationClipDocument> doc = document();
    if (!doc || rebindTrack < 0 || rebindTrack >= (int)doc->data().tracks.size()){
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const ClipTrackData& track = doc->data().tracks[(size_t)rebindTrack];
    const size_t users = rebindSceneUsers + rebindPrefabUsers;
    ImGui::Text("%s %s", rebindMissing ? "Rebind" : "Retarget", ClipValidation::trackLabel(track).c_str());
    UIProbe::item("clip.rebind.title");
    ImGui::TextColored(Theme::Colors::WarningText, "This rewrites the path in the clip: it changes the clip for %zu Animation %s (%zu in open scenes, %zu in prefabs).",
                       users, users == 1 ? "Player" : "Players", rebindSceneUsers, rebindPrefabUsers);
    if (rebindCandidates.empty()){
        ImGui::TextColored(Theme::Colors::SubtleText, "Bind a GameObject (or preview on the clip's model) to pick the new path.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(Theme::dpi(380.0f));
    ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS " Search", rebindSearch, sizeof(rebindSearch));
    const std::string needle = lower(rebindSearch);
    ImGui::BeginChild("##paths", ImVec2(Theme::dpi(380.0f), Theme::dpi(260.0f)), ImGuiChildFlags_Borders);
    std::string error;
    for (size_t i = 0; i < rebindCandidates.size(); i++){
        const std::string& path = rebindCandidates[i];
        if (!needle.empty() && lower(path).find(needle) == std::string::npos) continue;
        if (i == rebindNearest && rebindNearest > 0 && needle.empty()) ImGui::Separator();
        const std::string& reason = i < rebindReasons.size() ? rebindReasons[i] : std::string();
        std::string label = (path.empty() ? std::string("(root)") : path) + (i < rebindNearest ? "  (nearest)" : "");
        if (!reason.empty()) label += "  (" + reason + ")";
        ImGui::PushID((int)i);
        if (ImGui::Selectable(label.c_str(), false, reason.empty() ? 0 : ImGuiSelectableFlags_Disabled)){
            if (!controller().retargetTrack((size_t)rebindTrack, path, &error)){
                setNotice(error, true);
            }else{
                setNotice("Rebound to '" + lastSegment(path) + "' (changes the clip for " + std::to_string(users) +
                          (users == 1 ? " Animation Player)" : " Animation Players)"), false);
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

// ---- shortcuts ----

void AnimationWindowClip::runKeyCommand(std::string_view id){
    AnimationClipController& c = controller();
    if (!document()) return;
    std::string error;
    auto report = [&](bool ok){
        if (!ok && !error.empty()) setNotice(error, true);
        error.clear();
    };
    if (id == "animation.playPause") togglePlay(project->getSelectedScene());
    else if (id == "animation.addKey") report(keyAt(false, &error));
    else if (id == "animation.addKeyModified") report(keyAt(true, &error));
    else if (id == "animation.previousFrame") stepFrame(-1);
    else if (id == "animation.nextFrame") stepFrame(1);
    else if (id == "animation.previousKey") stepKey(-1);
    else if (id == "animation.nextKey") stepKey(1);
    else if (id == "animation.delete"){
        resolveSelectedCue();
        if (!selectedRefs.empty()) report(deleteSelected(&error));
        else if (selectedEvent >= 0) report(deleteEvent(selectedEvent, &error));
        else if (selectedCue >= 0) report(deleteCue(selectedCue, &error));
    }
    // the audio cues (S40 R10): the selected cue to the playhead, Add Property's cue sections
    else if (id == "animation.alignCueStart" || id == "animation.alignCuePeak" || id == "animation.alignCueEnd"){
        resolveSelectedCue();
        const CueAlign align = id == "animation.alignCueStart" ? CueAlign::Start : id == "animation.alignCuePeak" ? CueAlign::Peak : CueAlign::End;
        report(alignCue(selectedCue, align, &error));
    }
    else if (id == "animation.addAudioCue" || id == "animation.addSourceCue" || id == "animation.addSnapshotCue"){
        addPropertyCueKind = (int)(id == "animation.addAudioCue" ? ClipCueKind::Sound : id == "animation.addSourceCue" ? ClipCueKind::Source : ClipCueKind::Snapshot);
        openAddProperty = true;
    }
    else if (id == "animation.copy") copySelected();
    else if (id == "animation.paste") report(paste(snapToFrame(c.getTime()), &error));
    else if (id == "animation.duplicate") report(duplicateSelected(&error));
    // F / A: the time range, and in the curves view the value range too
    else if (id == "animation.frameSelected" || id == "animation.frameAll"){
        const bool selectedOnly = id == "animation.frameSelected";
        frameKeys(selectedOnly, sheetWidth);
        if (view == View::Curves) frameCurveValues(selectedOnly);
    }
    // R: Record on / off
    else if (id == "animation.record"){
        if (!setRecording(project->getSelectedScene(), !c.isRecording(), &error)) report(false);
        error.clear();
    }
}

void AnimationWindowClip::bindCommands(CommandCatalog& catalog, std::function<AnimationWindowClip*()> target){
    for (const CommandDef& existing : std::vector<CommandDef>(catalog.commands())){
        if (existing.id.rfind("animation.", 0) != 0) continue;
        CommandDef def = existing;
        const std::string id = def.id;
        // what the entity mode runs too (S40 R22): the Align commands on a selected audio block,
        // the Add ... action commands; in clip mode the clip window runs them
        const bool shared = id.rfind("animation.alignCue", 0) == 0 || id == "animation.addAudioAction" ||
                            id == "animation.addSoundEventAction" || id == "animation.addSnapshotAction";
        def.run = [target, id, shared](const CommandArgs&){
            AnimationWindowClip* clip = target();
            if (!clip) return;
            // the entity mode first while it has the keys: a clip opened before stays the controller's
            // document behind it, and its cue must not move
            if (shared && clip->entityKeys && clip->entityCommands) clip->entityCommands(id, true);
            else if (clip->document() != nullptr) clip->runKeyCommand(id);
            else if (shared && clip->entityCommands) clip->entityCommands(id, true);
        };
        def.enabled = [target, id, shared](){
            AnimationWindowClip* clip = target();
            if (!clip) return false;
            if (shared && clip->entityKeys && clip->entityCommands) return clip->entityCommands(id, false);
            if (clip->document() != nullptr) return true;
            return shared && clip->entityCommands && clip->entityCommands(id, false);
        };
        // while the window has focus (the controller's takesKeys, or the entity mode's), not while typing in it
        def.keyGate = [target, shared](){
            AnimationWindowClip* clip = target();
            return clip && (clip->controller().takesKeys() || (shared && clip->entityKeys)) && !ImGui::GetIO().WantTextInput;
        };
        catalog.add(std::move(def));
    }
}
