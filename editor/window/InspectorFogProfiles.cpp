// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's Fog Profile parts (docs/specs/S101-vapor.md R23-R27):
// - the Profile row of a Fog and a Fog Zone: the asset field with its menu (None, the built-ins,
//   the project's .fogprofile files) and a .fogprofile dropped on it, Used by N, New, Duplicate
//   and Unlink, each one undo step (LinkFogProfileCmd), disabled while playing;
// - the Fog's look rows: while linked each gets a leading Set by profile box; a ticked row edits
//   the profile (FogProfileAssetCmd, a drag merged into one step), an unticked one this Fog
//   (PropertyCmd); ticking copies the Fog's value into the profile, unticking removes it; All and
//   None per section;
// - the Effective look line (in Play, or while a Fog Zone or a blend changes the look) and an
//   icon on the rows whose effective value differs from what they show;
// - the Fog Zone: its shape rows, the profile's set fields only, Add Field, its weight now;
// - a .fogprofile picked in the Project window: name, description, every field (an unticked one
//   reads "not set"), and who uses it (a click selects or opens the user). Its edits are project
//   undo steps.

#include "InspectorWindow.h"
#include "widget/ColorField.h"
#include "widget/ColorPickerWindow.h"

#include "Catalog.h"
#include "EditorHost.h"
#include "FogProfileAssets.h"
#include "Out.h"
#include "Project.h"
#include "Scene.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/FogProfileAssetCmd.h"
#include "command/type/LinkFogProfileCmd.h"
#include "component/FogComponent.h"
#include "component/FogZoneComponent.h"
#include "external/IconsFontAwesome6.h"
#include "reflect/Enums.h"
#include "reflect/Reflect.h"
#include "render/FogProfile.h"
#include "render/FogResolve.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace jasperengine::editor {

    struct InspectorFogState {
        // the Fog being drawn (beginFogLookRows): its linked profile, a copy the rows edit
        bool linked = false;
        bool editable = false;      // the file may change now (not while playing)
        std::string assetPath;      // assets-relative
        FogProfileData profile;
        FogComponent own;           // the first Fog's own fields (what ticking copies)
        std::vector<FogField> sectionFields;
        // the look the frame resolves, when something other than the profile layers on it
        bool hasEffective = false;
        FogResolve effective;
        std::vector<FogZoneState> zones;

        // Used by, read again every half second (it walks the open scenes and the index)
        std::string usageKey;
        int usageFrame = -1000;
        fogprofiles::FogProfileUsers usage;

        // the Profile menu's project profiles, read as it opens
        std::vector<fogprofiles::ProjectFogProfile> menuProfiles;

        // the .fogprofile inspector's text fields, refreshed while not edited
        std::string assetKey;
        char nameBuffer[256] = {};
        char descriptionBuffer[2048] = {};
        bool nameActive = false;            // the field was being edited last frame
        bool descriptionActive = false;
    };

}

// file-local, in a namespace of its own: a unity build merges the editor's sources
namespace jasperengine::editor::inspectorfog {
namespace {

    enum class RowWidget { Float, FloatPositive, Float01, Color, ColorHdr, Vector3, Bool, Enum };

    // every look field as the zone and asset inspectors show it: section, widget, format, help
    struct FieldRow {
        FogField field;
        const char* section;
        RowWidget widget;
        float step;
        const char* format;
        const char* help;
    };

    const FieldRow kRows[] = {
        {FogField::Color, "Fog", RowWidget::Color, 0.0f, nullptr,
         "The fog's colour (added light). The Lit colour mode uses Albedo and Emissive instead."},
        {FogField::Density, "Fog", RowWidget::Float, 0.001f, "%.4f", "Exponential types: how fast the fog thickens with distance."},
        {FogField::LinearStart, "Fog", RowWidget::Float, 0.1f, "%.2f", "Linear type: where the fog starts (m)."},
        {FogField::LinearEnd, "Fog", RowWidget::Float, 0.1f, "%.2f", "Linear type: where the fog is full (m)."},
        {FogField::ColorMode, "Height Fog", RowWidget::Enum, 0.0f, nullptr,
         "Color: the Color row. Sky: the sky's horizon. Lit: Albedo times the sun and the scene ambient, plus Emissive."},
        {FogField::Albedo, "Height Fog", RowWidget::Color, 0.0f, nullptr, "Lit: the share of the light the fog scatters, per channel."},
        {FogField::AmbientMultiplier, "Height Fog", RowWidget::FloatPositive, 0.1f, "%.2f", "Lit: the weight of the scene ambient in the fog's light."},
        {FogField::Emissive, "Height Fog", RowWidget::ColorHdr, 0.0f, nullptr, "Lit: light the fog gives off itself (HDR)."},
        {FogField::AttenuationDistance, "Height Fog", RowWidget::FloatPositive, 0.1f, "%.2f",
         "The distance (m) light travels in the fog below the Base Height before 63% of it is absorbed. Smaller: denser fog."},
        {FogField::BaseHeight, "Height Fog", RowWidget::Float, 0.1f, "%.2f", "The fog is uniform below this height (m) and thins above it."},
        {FogField::MaximumHeight, "Height Fog", RowWidget::Float, 0.1f, "%.2f", "The height (m) where the fog has thinned to a thousandth of its density."},
        {FogField::StartDistance, "Height Fog", RowWidget::FloatPositive, 0.1f, "%.2f", "No fog nearer the camera than this (m)."},
        {FogField::MaxFogDistance, "Height Fog", RowWidget::FloatPositive, 0.1f, "%.2f", "No more fog past this distance (m) from the camera."},
        {FogField::MaxOpacity, "Height Fog", RowWidget::Float01, 0.01f, "%.2f", "The most the fog hides of what is behind it (0..1)."},
        {FogField::DirectionalInscattering, "Directional Inscattering", RowWidget::FloatPositive, 0.1f, "%.2f",
         "How much of the main directional light the fog scatters towards the camera."},
        {FogField::Anisotropy, "Directional Inscattering", RowWidget::Float, 0.01f, "%.2f",
         "-1 scatters the light back towards the sun, 0 evenly, 1 forward (a glow around the sun)."},
        {FogField::SkyFill, "Sky", RowWidget::Bool, 0.0f, nullptr,
         "The sky takes the fog's colour near the horizon, so the fogged ground meets it without a seam."},
        {FogField::SkyFillAngle, "Sky", RowWidget::FloatPositive, 0.1f, "%.2f", "How far above the horizon (degrees) the sky fill reaches."},
        {FogField::NoiseIntensity, "Volumetric Fog", RowWidget::Float01, 0.01f, "%.2f",
         "How much a 3D noise varies the fog's density (0..1), fading out towards the volumetric Distance."},
        {FogField::NoiseScale, "Volumetric Fog", RowWidget::FloatPositive, 0.1f, "%.2f", "The size (m) of the noise's features."},
        {FogField::Wind, "Volumetric Fog", RowWidget::Vector3, 0.05f, "%.2f", "The speed (m/s) the noise drifts at."},
    };

    const FieldRow& rowOf(FogField field) {
        for (const FieldRow& row : kRows) {
            if (row.field == field) return row;
        }
        return kRows[0];
    }

    const char* const kNotSet = "not set (each Fog's own value)";

    RowWidget widgetOf(RowPropertyType type, bool hdr) {
        switch (type) {
            case RowPropertyType::FloatPositive: return RowWidget::FloatPositive;
            case RowPropertyType::Float_0_1: return RowWidget::Float01;
            case RowPropertyType::Color3L: return hdr ? RowWidget::ColorHdr : RowWidget::Color;
            case RowPropertyType::Vector3: return RowWidget::Vector3;
            case RowPropertyType::Bool: return RowWidget::Bool;
            case RowPropertyType::Enum: return RowWidget::Enum;
            default: return RowWidget::Float;
        }
    }

    // a key naming a project file (not "builtin:<id>" or a script's "runtime:<n>")
    bool isProfileFile(const std::string& key) {
        return !key.empty() && !FogProfiles::isBuiltinKey(key) && !FogProfileLibrary::isRuntimeKey(key);
    }

    std::string displayName(const FogProfileData& data, const std::string& path) {
        if (!data.name.empty()) return data.name;
        return fs::path(FileUtils::pathFromUtf8(path)).stem().string();
    }

    std::string valueText(const FogComponent& values, FogField field) {
        float v[3] = {0.0f, 0.0f, 0.0f};
        FogFields::read(values, field, v);
        char text[96];
        switch (FogFields::info(field).kind) {
            case FogFieldKind::Color:
            case FogFieldKind::Vector3:
                std::snprintf(text, sizeof(text), "(%.3g, %.3g, %.3g)", v[0], v[1], v[2]);
                return text;
            case FogFieldKind::Bool:
                return v[0] != 0.0f ? "On" : "Off";
            case FogFieldKind::Enum:
                return FogFields::colorModeName(values.colorMode);
            default:
                std::snprintf(text, sizeof(text), "%.4g", v[0]);
                return text;
        }
    }

    // the layer's name as the Effective look line and the tooltips say it
    std::string layerText(Scene* scene, const FogLayerSource& source) {
        switch (source.layer) {
            case FogLayer::Fog: return "this Fog";
            case FogLayer::Profile: return "the profile";
            case FogLayer::Blend: return "the blend";
            case FogLayer::Zone: return "Zone " + scene->getEntityName(source.zone);
            case FogLayer::Runtime: return "a script";
        }
        return std::string();
    }

    std::string profileNameOfKey(const std::string& key) {
        if (key.empty()) return "this Fog's values";
        if (std::shared_ptr<const FogProfileData> data = FogProfileLibrary::get(key)) return displayName(*data, key);
        return fs::path(FileUtils::pathFromUtf8(key)).stem().string();
    }

    // the assets-relative path of the file a reference names (by GUID first, as the editor
    // resolves references; else its path)
    std::string refAssetPath(Project* project, const AssetRef& ref) {
        if (!ref.guid.isNull() && project->getAssetDatabase().isOpen()) {
            const std::string hint = project->assetHintToProjectPath(ref.path, AssetPathBase::Assets);
            const AssetDatabase::Resolution found = project->getAssetDatabase().resolve(ref.guid, hint, AssetType::Unknown, true);
            if (found.found()) return FogProfileLibrary::normalizeKey(project->projectPathToAssetHint(found.path, AssetPathBase::Assets));
        }
        return FogProfileLibrary::normalizeKey(ref.path);
    }

    fogprofiles::FogProfileUsers readUsage(Project* project, const std::string& assetPath) {
        fogprofiles::FogProfileUsers usage = fogprofiles::profileUsers(*project, assetPath);
        // until the index is ready, only the open scenes count: the row says "in open scenes"
        if (!usage.indexReady) usage.closedFiles.clear();
        return usage;
    }

    std::string usageText(const fogprofiles::FogProfileUsers& usage) {
        std::string text = "Used by " + std::to_string(usage.count());
        if (!usage.indexReady) text += " in open scenes";
        return text;
    }

    void usageTooltip(Project* project, const fogprofiles::FogProfileUsers& usage) {
        if (!ImGui::BeginItemTooltip()) return;
        if (usage.count() == 0) ImGui::TextUnformatted("Nothing uses this profile yet.");
        for (const fogprofiles::FogProfileUser& user : usage.open) {
            SceneProject* sceneProject = project->getScene(user.sceneId);
            const std::string name = sceneProject && sceneProject->scene ? sceneProject->scene->getEntityName(user.entity) : std::string("?");
            ImGui::Text("%s  %s / %s", user.zone ? "Fog Zone" : "Fog", user.sceneName.c_str(), name.c_str());
        }
        for (const std::string& file : usage.closedFiles) ImGui::Text("%s (not open)", file.c_str());
        ImGui::TextDisabled("Editing the profile changes every one of them.");
        ImGui::EndTooltip();
    }

    // the next button on this line when it fits, else on the next (a narrow Inspector)
    void sameLineIfFits(const char* label) {
        const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
    }

    struct ValueEdit {
        bool begin = false;     // a colour window opened on it
        bool changed = false;
        bool commit = false;    // the edit is complete (one step)
    };

    // the value widget of a profile row, editing `values`
    ValueEdit valueWidget(FogField field, RowWidget widget, FogComponent& values, float step, const char* format, const char* label,
                          const std::vector<EnumEntry>* enumEntries) {
        ValueEdit edit;
        // room for the row's (?) after it
        const float width = std::max(ImGui::GetFrameHeight(), std::min(ImGui::CalcItemWidth(), ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("(?)").x -
                                                                                               ImGui::GetStyle().ItemSpacing.x * 2.0f));
        const std::string id = std::string("##fog_profile_value_") + FogFields::name(field);
        float v[3] = {0.0f, 0.0f, 0.0f};
        FogFields::read(values, field, v);
        switch (widget) {
            case RowWidget::Float:
            case RowWidget::FloatPositive:
            case RowWidget::Float01: {
                const float minimum = 0.0f;
                const float maximum = widget == RowWidget::Float01 ? 1.0f : (widget == RowWidget::FloatPositive ? FLT_MAX : 0.0f);
                const ImGuiSliderFlags flags = widget == RowWidget::Float ? ImGuiSliderFlags_None : ImGuiSliderFlags_AlwaysClamp;
                ImGui::SetNextItemWidth(width);
                if (ImGui::DragFloat(id.c_str(), &v[0], step > 0.0f ? step : 0.1f, minimum, maximum, format ? format : "%.2f", flags)) {
                    if (widget != RowWidget::Float) v[0] = std::clamp(v[0], 0.0f, maximum);
                    edit.changed = true;
                }
                edit.commit = ImGui::IsItemDeactivatedAfterEdit();
                break;
            }
            case RowWidget::Vector3:
                ImGui::SetNextItemWidth(width);
                edit.changed = ImGui::DragFloat3(id.c_str(), v, step > 0.0f ? step : 0.05f, 0.0f, 0.0f, format ? format : "%.2f");
                edit.commit = ImGui::IsItemDeactivatedAfterEdit();
                break;
            case RowWidget::Color:
            case RowWidget::ColorHdr: {
                ui::ColorFieldOptions options;
                options.alpha = false;
                options.hdr = widget == RowWidget::ColorHdr;
                options.linearStorage = true;
                options.label = label;
                options.width = width;
                Vector3 color(v[0], v[1], v[2]);
                const ui::EditPhase phase = ui::colorField((std::string(label) + id).c_str(), color, options);
                edit.begin = phase == ui::EditPhase::Begin;
                edit.changed = phase == ui::EditPhase::Changing || phase == ui::EditPhase::Applied;
                edit.commit = phase == ui::EditPhase::Applied;
                v[0] = color.x;
                v[1] = color.y;
                v[2] = color.z;
                break;
            }
            case RowWidget::Bool: {
                bool on = v[0] != 0.0f;
                if (ImGui::Checkbox(id.c_str(), &on)) {
                    v[0] = on ? 1.0f : 0.0f;
                    edit.changed = edit.commit = true;
                }
                break;
            }
            case RowWidget::Enum: {
                const int current = static_cast<int>(v[0]);
                const char* preview = "";
                if (enumEntries) {
                    for (const EnumEntry& entry : *enumEntries) {
                        if (entry.value == current) preview = entry.name;
                    }
                }
                ImGui::SetNextItemWidth(width);
                if (ImGui::BeginCombo(id.c_str(), preview)) {
                    if (enumEntries) {
                        for (const EnumEntry& entry : *enumEntries) {
                            if (ImGui::Selectable(entry.name, entry.value == current) && entry.value != current) {
                                v[0] = static_cast<float>(entry.value);
                                edit.changed = edit.commit = true;
                            }
                        }
                    }
                    ImGui::EndCombo();
                }
                break;
            }
        }
        if (edit.changed) FogFields::write(values, field, v);
        return edit;
    }

    // the value a field gets when a zone or an asset ticks it: the scene Fog's own, else the default
    FogComponent sceneFogValues(Scene* scene) {
        const Entity fogEntity = FogScene::findFog(scene);
        if (fogEntity != NULL_ENTITY) return scene->getComponent<FogComponent>(fogEntity);
        return FogComponent();
    }

}
}

editor::InspectorFogState& editor::InspectorWindow::fogProfileState() {
    if (!fogState) fogState = std::make_shared<InspectorFogState>();
    return *fogState;
}

void editor::InspectorWindow::pushFogProfileCommand(CommandHistory* history, Command* command, bool commit) {
    if (command) {
        history->addCommand(command);
        const UndoEntry* top = history->top();
        fogProfileEditCmd = (top && top->cmd.get() == command) ? command : nullptr;
    }
    if (commit) {
        const UndoEntry* top = history->top();
        if (fogProfileEditCmd && top && top->cmd.get() == fogProfileEditCmd) top->cmd->setNoMerge();
        fogProfileEditCmd = nullptr;
    }
}

// ---- the Profile row (R23, R27)

void editor::InspectorWindow::drawFogProfileRow(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities) {
    InspectorFogState& state = fogProfileState();
    Scene* scene = sceneProject->scene;
    const bool zone = cpType == ComponentType::FogZoneComponent;
    AssetRef ref;
    bool mixed = false;
    for (size_t i = 0; i < entities.size(); i++) {
        const AssetRef& own = zone ? scene->getComponent<FogZoneComponent>(entities[i]).profile : scene->getComponent<FogComponent>(entities[i]).profile;
        if (i == 0) ref = own;
        else if (own.path != ref.path || own.guid != ref.guid) mixed = true;
    }
    const std::string key = ref.isSet() ? inspectorfog::refAssetPath(project, ref) : std::string();
    const std::shared_ptr<const FogProfileData> profile = key.empty() ? nullptr : FogProfileLibrary::get(key);
    const bool missing = ref.isSet() && !profile;
    const bool stopped = sceneProject->playState == ScenePlayState::STOPPED;
    CommandHistory* history = CommandHandle::get(sceneProject->id);

    auto submit = [&](Command* command, const std::string& error) {
        if (command) history->addCommand(command);
        else if (!error.empty()) Out::warning("%s", error.c_str());
    };
    auto link = [&](const std::string& name) {
        std::string error;
        submit(LinkFogProfileCmd::link(project, sceneProject->id, entities, cpType, name, error), error);
    };
    auto unlink = [&]() {
        std::string error;
        submit(LinkFogProfileCmd::unlink(project, sceneProject->id, entities, cpType, error), error);
    };

    beginTable(cpType, getLabelSize("Blend Distance"), "fog_profile");
    recordLayoutRow(RowPropertyType::Asset, cpType, "profile", "Profile", sceneProject, entities);
    propertyHeader("Profile");
    std::string shown;
    if (mixed) shown = "\xE2\x80\x94";
    else if (!ref.isSet()) shown = zone ? "None (no effect)" : "None (this Fog's values)";
    else if (missing) shown = "Missing: " + ref.path;
    else shown = inspectorfog::displayName(*profile, key) + (inspectorfog::isProfileFile(key) ? "" : " (read only: Duplicate to edit)");
    // Used by N beside the field (R23): how many a change of the profile changes
    std::string usedBy;
    if (profile && !mixed) {
        if (state.usageKey != key || ImGui::GetFrameCount() - state.usageFrame > 30) {
            state.usage = inspectorfog::readUsage(project, key);
            state.usageKey = key;
            state.usageFrame = ImGui::GetFrameCount();
        }
        usedBy = inspectorfog::usageText(state.usage);
    }
    ImGui::BeginDisabled(!stopped);
    if (missing) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Colors::ErrorText);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    const bool open = ImGui::Button((std::string(ICON_FA_SMOG " ") + shown + "##fog_profile_field").c_str(), ImVec2(-FLT_MIN, 0.0f));
    ImGui::PopStyleVar();
    if (missing) ImGui::PopStyleColor();
    UIProbe::item("inspector.fogProfile.field");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (!stopped) ImGui::SetTooltip("Stop Play to change the profile: its file is shared by every scene that uses it.");
        else if (ref.isSet()) ImGui::SetTooltip("%s\nPick another, or drop a .fogprofile here.", key.c_str());
        else ImGui::SetTooltip("Pick a profile (a built-in is copied into assets/FogProfiles on first use), or drop a .fogprofile here.");
    }
    if (open) {
        state.menuProfiles = fogprofiles::listProjectProfiles(*project);
        ImGui::OpenPopup("##fog_profile_menu");
    }
    if (stopped && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            if (!dropped.empty() && Util::isFogProfileFile(dropped[0]) && project->isInsideAssetsPath(dropped[0]))
                link(project->normalizeToAssetsRelative(dropped[0]).generic_string());
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##fog_profile_menu")) {
        if (ImGui::MenuItem(zone ? "None (no effect)" : "None (this Fog's values)", nullptr, !ref.isSet() && !mixed)) unlink();
        if (!zone) ImGui::SetItemTooltip("Unlink: this Fog keeps the look it shows as its own values.");
        ImGui::SeparatorText("Built-in");
        for (const FogProfileBuiltin& builtin : FogProfiles::builtins()) {
            const std::string copy = FogProfileLibrary::normalizeKey(FogProfiles::builtinCopyPath(builtin.id));
            if (ImGui::MenuItem((builtin.data.name + "##builtin_" + builtin.id).c_str(), nullptr, !mixed && key == copy))
                link(FogProfiles::builtinKey(builtin.id));
            if (!builtin.data.description.empty()) ImGui::SetItemTooltip("%s", builtin.data.description.c_str());
        }
        ImGui::SeparatorText("Project");
        if (state.menuProfiles.empty()) ImGui::TextDisabled("No .fogprofile in the project yet");
        for (const fogprofiles::ProjectFogProfile& entry : state.menuProfiles) {
            const std::string label = inspectorfog::displayName(entry.data, entry.assetPath) + "##project_" + entry.assetPath;
            if (ImGui::MenuItem(label.c_str(), nullptr, !mixed && key == entry.assetPath)) link(entry.assetPath);
            ImGui::SetItemTooltip("%s%s%s", entry.path.c_str(), entry.data.description.empty() ? "" : "\n", entry.data.description.c_str());
        }
        ImGui::EndPopup();
    }

    endTable();

    // New, Duplicate, Unlink and Used by N, the whole width under the field
    ImGui::BeginDisabled(!stopped);
    const Entity first = entities[0];
    if (ImGui::Button(ICON_FA_PLUS " New##fog_profile_new")) {
        FogProfileData data;
        std::string name = scene->getEntityName(first);
        if (zone) {
            data.name = name;
        } else {
            // this Fog's look, every field set (R23)
            if (name.empty() || name == "Fog") name = sceneProject->name + " Fog";
            const FogComponent& fog = scene->getComponent<FogComponent>(first);
            data = FogProfileData::fromFog(LinkFogProfileCmd::resolvedLook(fog, profile.get()), name);
        }
        const std::string path = FogProfileAssetCmd::freePath(*project, "FogProfiles", name.empty() ? std::string("New Fog Profile") : name);
        std::string error;
        submit(LinkFogProfileCmd::linkNew(project, sceneProject->id, entities, cpType, path, data, error), error);
    }
    ImGui::SetItemTooltip("%s", zone ? "A new empty profile for this zone (assets/FogProfiles), linked. Add Field picks what it sets."
                                     : "Saves this Fog's look as a new profile (assets/FogProfiles), every field set, and links it.");
    inspectorfog::sameLineIfFits(ICON_FA_CLONE " Duplicate");
    ImGui::BeginDisabled(!profile || mixed);
    if (ImGui::Button(ICON_FA_CLONE " Duplicate##fog_profile_duplicate")) {
        FogProfileData data = *profile;
        data.name = inspectorfog::displayName(*profile, key) + " Copy";
        std::string folder = "FogProfiles";
        std::string stem = data.name;
        if (inspectorfog::isProfileFile(key)) {
            const fs::path source = FileUtils::pathFromUtf8(key);
            folder = FileUtils::pathToGenericUtf8(source.parent_path());
            stem = source.stem().string() + " Copy";
        }
        const std::string path = FogProfileAssetCmd::freePath(*project, folder, stem);
        std::string error;
        submit(LinkFogProfileCmd::linkNew(project, sceneProject->id, entities, cpType, path, data, error), error);
    }
    ImGui::SetItemTooltip("Links a copy of the profile, so changing it changes only what uses the copy.");
    ImGui::EndDisabled();
    inspectorfog::sameLineIfFits(ICON_FA_LINK_SLASH " Unlink");
    ImGui::BeginDisabled(!ref.isSet() && !mixed);
    if (ImGui::Button(ICON_FA_LINK_SLASH " Unlink##fog_profile_unlink")) unlink();
    ImGui::SetItemTooltip("%s", zone ? "Clears the zone's profile: the zone has no effect without one."
                                     : "Copies the values this Fog shows into its own fields and clears the link: the look stays.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!usedBy.empty()) {
        const float width = ImGui::CalcTextSize(usedBy.c_str()).x;
        // beside the buttons when it fits
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f);
        if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
        ImGui::AlignTextToFramePadding();
        if (state.usage.count() > 1) ImGui::TextColored(Theme::Colors::WarningText, "%s", usedBy.c_str());
        else ImGui::TextDisabled("%s", usedBy.c_str());
        UIProbe::item("inspector.fogProfile.usedBy");
        inspectorfog::usageTooltip(project, state.usage);
    }
    if (missing && !mixed) {
        std::string reason;
        FogProfileLibrary::isMissing(key, &reason);
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        const std::string message = "The profile " + key + " does not read" + (reason.empty() ? std::string() : " (" + reason + ")") +
                                    (zone ? ": this zone has no effect. Pick another or Unlink." : ": this Fog shows its own values. Pick another or Unlink.");
        ImGui::TextWrapped("%s", message.c_str());
    }
}

// ---- the Fog's look rows (R24, R25)

void editor::InspectorWindow::beginFogLookRows(SceneProject* sceneProject, const std::vector<Entity>& entities) {
    InspectorFogState& state = fogProfileState();
    Scene* scene = sceneProject->scene;
    const FogComponent& fog = scene->getComponent<FogComponent>(entities[0]);
    state.linked = false;
    state.sectionFields.clear();
    state.hasEffective = false;
    state.zones.clear();
    state.own = fog;
    state.editable = sceneProject->playState == ScenePlayState::STOPPED;

    bool same = true;
    for (Entity entity : entities) {
        const AssetRef& other = scene->getComponent<FogComponent>(entity).profile;
        if (other.path != fog.profile.path || other.guid != fog.profile.guid) same = false;
    }
    if (same && fog.profile.isSet()) {
        state.assetPath = inspectorfog::refAssetPath(project, fog.profile);
        if (std::shared_ptr<const FogProfileData> profile = FogProfileLibrary::get(state.assetPath)) {
            state.linked = true;
            state.profile = *profile;
        }
        // a built-in named directly (a script's or an agent's "builtin:<id>") has no file to
        // edit: Duplicate makes one
        if (!inspectorfog::isProfileFile(state.assetPath)) state.editable = false;
    }

    // the layers above the profile (R25): what the frame resolves at the scene's camera
    const bool playing = sceneProject->playState != ScenePlayState::STOPPED;
    if (!playing && !FogScene::needsResolve(scene, fog)) return;
    FogScene::resolve(scene, fog, FogScene::mainCameraPosition(scene), state.effective, &state.zones);
    state.hasEffective = true;
    const FogBlendState blend = FogScene::blendState(fog);
    const FogRuntime* runtime = fog.runtime.get();
    std::vector<std::string> parts;
    const std::string profileKey = FogScene::profileKey(fog);
    if (!profileKey.empty()) parts.push_back("Profile " + inspectorfog::profileNameOfKey(profileKey));
    if (blend.active) {
        char text[160];
        std::snprintf(text, sizeof(text), "blending to %s %d%%", inspectorfog::profileNameOfKey(blend.targetKey).c_str(),
                      static_cast<int>(blend.progress * 100.0f + 0.5f));
        parts.push_back(text);
    }
    bool zoneActive = false;
    for (const FogZoneState& zoneState : state.zones) {
        if (zoneState.weight <= 0.0f) continue;
        char text[160];
        std::snprintf(text, sizeof(text), "Zone %s %.2g", scene->getEntityName(zoneState.entity).c_str(), zoneState.weight);
        parts.push_back(text);
        zoneActive = true;
    }
    if (runtime && runtime->writes.set.any()) parts.push_back("script overrides " + std::to_string(runtime->writes.set.count()));
    if (!playing && !zoneActive && !blend.active) return;
    if (parts.empty()) parts.push_back("this Fog's values");
    std::string line;
    for (const std::string& part : parts) line += (line.empty() ? "" : " \xC2\xB7 ") + part;
    ImGui::TextDisabled(ICON_FA_LAYER_GROUP);
    ImGui::SameLine();
    ImGui::TextWrapped("Effective look: %s", line.c_str());
    UIProbe::item("inspector.fogProfile.effective");
    ImGui::SetItemTooltip("What the %s camera sees now. Rows whose value this changes show " ICON_FA_LAYER_GROUP " with the effective value.",
                          playing ? "Game view's" : "Scene view's");
}

void editor::InspectorWindow::fogLookRow(FogField field, RowPropertyType type, const std::string& label, SceneProject* sceneProject,
                                         const std::vector<Entity>& entities, RowSettings settings) {
    InspectorFogState& state = fogProfileState();
    state.sectionFields.push_back(field);
    const ComponentType cpType = ComponentType::FogComponent;
    const char* name = FogFields::name(field);
    const bool ticked = state.linked && state.profile.isSet(field);
    // what the row shows, and what the frame shows when another layer changes it
    const FogComponent& shownValues = ticked ? state.profile.values : state.own;
    const bool differs = state.hasEffective && !FogFields::equal(shownValues, state.effective.resolved, field);
    bool toggled = false;
    if (state.linked || differs) {
        propertyLabelPrefix = [&, ticked, differs]() {
            if (state.linked) {
                bool box = ticked;
                ImGui::BeginDisabled(!state.editable);
                if (ImGui::Checkbox((std::string("##fog_set_by_profile_") + name).c_str(), &box)) toggled = true;
                ImGui::EndDisabled();
                UIProbe::item(std::string("inspector.fogProfile.set.") + name);
                ImGui::SetItemTooltip("%s", ticked ? "Set by profile: this row edits the profile, for every user. Untick to use this Fog's own value."
                                                   : "Not set by the profile: this row edits this Fog. Tick to copy its value into the profile.");
            }
            if (differs) {
                if (state.linked) ImGui::SameLine();
                ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_LAYER_GROUP);
                ImGui::SetItemTooltip("Effective now: %s (from %s)", inspectorfog::valueText(state.effective.resolved, field).c_str(),
                                      inspectorfog::layerText(sceneProject->scene, state.effective.sourceOf(field)).c_str());
            }
        };
    }

    if (!ticked) {
        propertyRow(type, cpType, name, label, sceneProject, entities, settings);
    } else {
        // the profile's value: its edits write the file (one step per edit, a drag merged)
        recordLayoutRow(type, cpType, name, label, sceneProject, entities);
        propertyHeader(label, settings.secondColSize, false, settings.child, settings.indent);
        ImGui::BeginDisabled(!state.editable);
        FogComponent edited = state.profile.values;
        const inspectorfog::ValueEdit edit = inspectorfog::valueWidget(field, inspectorfog::widgetOf(type, settings.colorHdr), edited, settings.stepSize,
                                                                       settings.format, label.c_str(), settings.enumEntries);
        UIProbe::item(std::string("inspector.fogProfile.value.") + name);
        ImGui::EndDisabled();
        if (!settings.help.empty()) {
            ImGui::SameLine();
            helpMarker(settings.help);
        }
        CommandHistory* history = CommandHandle::get(sceneProject->id);
        if (edit.begin) ui::ColorPickerWindow::beginEdit(history, label.c_str());
        if (edit.changed) {
            FogProfileData after = state.profile;
            after.setFrom(edited, field);
            pushFogProfileCommand(history, FogProfileAssetCmd::edit(project, state.assetPath, state.profile, after), edit.commit);
            state.profile = after;
        } else if (edit.commit) {
            pushFogProfileCommand(history, nullptr, true);
        }
    }
    propertyLabelPrefix = nullptr;

    if (toggled) {
        // ticking copies the Fog's value into the profile, unticking removes the override (R24)
        FogProfileData after = state.profile;
        if (ticked) after.clear(field);
        else after.setFrom(state.own, field);
        if (Command* command = FogProfileAssetCmd::edit(project, state.assetPath, state.profile, after)) {
            CommandHandle::get(sceneProject->id)->addCommandNoMerge(command);
            state.profile = after;
        }
    }
}

void editor::InspectorWindow::drawFogLookSectionButtons(SceneProject* sceneProject, const char* id) {
    InspectorFogState& state = fogProfileState();
    const std::vector<FogField> fields = state.sectionFields;
    state.sectionFields.clear();
    if (!state.linked || fields.empty()) return;
    ImGui::PushID(id);
    ImGui::BeginDisabled(!state.editable);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Set by profile:");
    ImGui::SameLine();
    const bool all = ImGui::SmallButton("All");
    ImGui::SetItemTooltip("Every row above edits the profile (their values copied from this Fog where it set none).");
    ImGui::SameLine();
    const bool none = ImGui::SmallButton("None");
    ImGui::SetItemTooltip("Every row above edits this Fog (the profile stops setting them).");
    ImGui::EndDisabled();
    ImGui::PopID();
    if (!all && !none) return;
    FogProfileData after = state.profile;
    for (FogField field : fields) {
        if (all && !after.isSet(field)) after.setFrom(state.own, field);
        if (none) after.clear(field);
    }
    if (Command* command = FogProfileAssetCmd::edit(project, state.assetPath, state.profile, after)) {
        CommandHandle::get(sceneProject->id)->addCommandNoMerge(command);
        state.profile = after;
    }
}

// ---- the Fog Zone (R27, R28)

void editor::InspectorWindow::drawFogZoneComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    InspectorFogState& state = fogProfileState();
    Scene* scene = sceneProject->scene;
    const FogZoneComponent& zone = scene->getComponent<FogZoneComponent>(entities[0]);
    const reflect::ComponentDesc* desc = reflect::find(cpType);
    auto help = [desc](const char* id) {
        const reflect::FieldDesc* field = desc ? reflect::findField(*desc, id) : nullptr;
        return field ? reflect::tooltip(*field) : std::string();
    };
    static std::vector<EnumEntry>& shapes = *enumEntries(reflect::enums::kFogZoneShape);
    static std::vector<EnumEntry>& colorModes = *enumEntries(reflect::enums::kFogColorMode);

    const float labelSize = getLabelSize("Blend Distance");
    RowSettings settingsShape;
    settingsShape.enumEntries = &shapes;
    settingsShape.help = help("shape");
    RowSettings settingsSize;
    settingsSize.help = help("size");
    settingsSize.unit = NumericUnit::Meters;
    RowSettings settingsBlend;
    settingsBlend.secondColSize = 6 * ImGui::GetFontSize();
    settingsBlend.help = help("blendDistance");
    settingsBlend.unit = NumericUnit::Meters;
    RowSettings settingsPriority;
    settingsPriority.secondColSize = 6 * ImGui::GetFontSize();
    settingsPriority.help = help("priority");
    RowSettings settingsWeight;
    settingsWeight.secondColSize = 6 * ImGui::GetFontSize();
    settingsWeight.stepSize = 0.01f;
    settingsWeight.help = help("weight");

    beginTable(cpType, labelSize);
    propertyRow(RowPropertyType::Enum, cpType, "shape", "Shape", sceneProject, entities, settingsShape);
    propertyRow(RowPropertyType::Vector3, cpType, "size", "Size", sceneProject, entities, settingsSize);
    propertyRow(RowPropertyType::FloatPositive, cpType, "blendDistance", "Blend Distance", sceneProject, entities, settingsBlend);
    propertyRow(RowPropertyType::Int, cpType, "priority", "Priority", sceneProject, entities, settingsPriority);
    propertyRow(RowPropertyType::Float_0_1, cpType, "weight", "Weight", sceneProject, entities, settingsWeight);
    endTable();

    drawFogProfileRow(cpType, sceneProject, entities);

    // the zone's own values are its profile's overrides: the set fields, then Add Field
    bool same = true;
    for (Entity entity : entities) {
        const AssetRef& other = scene->getComponent<FogZoneComponent>(entity).profile;
        if (other.path != zone.profile.path || other.guid != zone.profile.guid) same = false;
    }
    const std::string key = same && zone.profile.isSet() ? inspectorfog::refAssetPath(project, zone.profile) : std::string();
    const std::shared_ptr<const FogProfileData> profile = key.empty() ? nullptr : FogProfileLibrary::get(key);
    const bool editable = sceneProject->playState == ScenePlayState::STOPPED && inspectorfog::isProfileFile(key);
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    if (profile) {
        FogProfileData data = *profile;
        std::vector<FogField> unset;
        beginTable(cpType, labelSize, "zone_profile_fields");
        for (size_t i = 0; i < FOG_FIELD_COUNT; i++) {
            const FogField field = static_cast<FogField>(i);
            if (!data.isSet(field)) {
                unset.push_back(field);
                continue;
            }
            const inspectorfog::FieldRow& row = inspectorfog::rowOf(field);
            bool untick = false;
            propertyLabelPrefix = [&]() {
                bool box = true;
                ImGui::BeginDisabled(!editable);
                if (ImGui::Checkbox((std::string("##zone_set_") + FogFields::name(field)).c_str(), &box)) untick = true;
                ImGui::EndDisabled();
                ImGui::SetItemTooltip("Set by the profile. Untick to remove it: the zone stops changing it.");
            };
            propertyHeader(FogFields::info(field).label);
            propertyLabelPrefix = nullptr;
            ImGui::BeginDisabled(!editable);
            FogComponent edited = data.values;
            const inspectorfog::ValueEdit edit = inspectorfog::valueWidget(field, row.widget, edited, row.step, row.format, FogFields::info(field).label,
                                                                           &colorModes);
            ImGui::EndDisabled();
            ImGui::SameLine();
            helpMarker(row.help);
            if (edit.begin) ui::ColorPickerWindow::beginEdit(history, FogFields::info(field).label);
            FogProfileData after = data;
            if (edit.changed) after.setFrom(edited, field);
            if (untick) after.clear(field);
            if (edit.changed || untick) {
                pushFogProfileCommand(history, FogProfileAssetCmd::edit(project, key, data, after), edit.commit || untick);
                data = after;
            } else if (edit.commit) {
                pushFogProfileCommand(history, nullptr, true);
            }
        }
        endTable();
        if (data.set.none()) ImGui::TextDisabled("The profile sets no field: this zone changes nothing yet.");
        ImGui::BeginDisabled(!editable || unset.empty());
        if (ImGui::Button(ICON_FA_PLUS " Add Field " ICON_FA_CARET_DOWN "##zone_add_field")) ImGui::OpenPopup("##zone_add_field_menu");
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Picks a field for the profile to set; it starts at the scene Fog's value.");
        UIProbe::item("inspector.fogZone.addField");
        if (ImGui::BeginPopup("##zone_add_field_menu")) {
            const char* section = nullptr;
            for (FogField field : unset) {
                const inspectorfog::FieldRow& row = inspectorfog::rowOf(field);
                if (!section || std::string(section) != row.section) {
                    section = row.section;
                    ImGui::SeparatorText(section);
                }
                if (ImGui::MenuItem(FogFields::info(field).label)) {
                    FogProfileData after = data;
                    after.setFrom(inspectorfog::sceneFogValues(scene), field);
                    if (Command* command = FogProfileAssetCmd::edit(project, key, data, after)) history->addCommandNoMerge(command);
                }
                ImGui::SetItemTooltip("%s", row.help);
            }
            ImGui::EndPopup();
        }
    } else if (!zone.profile.isSet()) {
        ImGui::TextDisabled("No profile: this zone has no effect. Pick one, or New makes an empty one.");
    }

    // its weight at the camera now (the Scene view's in edit mode, the Game view's in Play)
    if (entities.size() == 1) {
        for (const FogZoneState& zoneState : FogScene::zoneStates(scene, FogScene::mainCameraPosition(scene))) {
            if (zoneState.entity != entities[0]) continue;
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::TextWrapped("Weight now %.2f at the %s camera (%.1f m from the shape)", zoneState.weight,
                               sceneProject->playState == ScenePlayState::STOPPED ? "Scene view's" : "Game view's", zoneState.distance);
            ImGui::PopStyleColor();
            UIProbe::item("inspector.fogZone.weightNow");
        }
    }
    (void)state;
    drawAtmosphereDiagnostics(sceneProject, entities[0], false);
}

// ---- a .fogprofile (R26)

void editor::InspectorWindow::drawFogProfileAsset(const fs::path& asset) {
    InspectorFogState& state = fogProfileState();
    drawAssetHeader(ICON_FA_SMOG, asset, "Fog Profile");
    if (!project->isInsideAssetsPath(asset)) {
        ImGui::TextWrapped("Only a profile inside the assets folder can be used by a Fog or a Fog Zone.");
        return;
    }
    const std::string key = FogProfileLibrary::normalizeKey(project->normalizeToAssetsRelative(asset).generic_string());
    const std::shared_ptr<const FogProfileData> loaded = FogProfileLibrary::get(key);
    if (!loaded) {
        std::string reason;
        FogProfileLibrary::isMissing(key, &reason);
        ImGui::TextColored(Theme::Colors::WarningText, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        const std::string message = "It does not read as a fog profile" + (reason.empty() ? std::string() : " (" + reason + ")") +
                                    ". Fix the file (Open as Text) or delete it.";
        ImGui::TextWrapped("%s", message.c_str());
        return;
    }
    const FogProfileData data = *loaded;
    CommandHistory* history = project->getProjectCommandHistory();
    const bool editable = !project->isAnyScenePlaying();
    auto commitEdit = [&](const FogProfileData& after, bool commit) {
        pushFogProfileCommand(history, FogProfileAssetCmd::edit(project, key, data, after), commit);
    };

    // name and description, written when the field lets go
    const bool refresh = state.assetKey != key;
    state.assetKey = key;
    ImGui::BeginDisabled(!editable);
    const float labelSize = getLabelSize("Attenuation Distance");
    beginTable(ComponentType::FogComponent, labelSize, "fog_profile_asset_text");
    propertyHeader("Name");
    if (refresh || !state.nameActive) std::snprintf(state.nameBuffer, sizeof(state.nameBuffer), "%s", data.name.c_str());
    ImGui::InputText("##fog_profile_name", state.nameBuffer, sizeof(state.nameBuffer));
    state.nameActive = ImGui::IsItemActive();
    UIProbe::item("asset.fogProfile.name");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        FogProfileData after = data;
        after.name = state.nameBuffer;
        commitEdit(after, true);
    }
    propertyHeader("Description");
    if (refresh || !state.descriptionActive)
        std::snprintf(state.descriptionBuffer, sizeof(state.descriptionBuffer), "%s", data.description.c_str());
    ImGui::InputTextMultiline("##fog_profile_description", state.descriptionBuffer, sizeof(state.descriptionBuffer),
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.5f));
    state.descriptionActive = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        FogProfileData after = data;
        after.description = state.descriptionBuffer;
        commitEdit(after, true);
    }
    endTable();

    // every field: ticked ones the profile sets, the others each Fog's own
    static std::vector<EnumEntry>& colorModes = *enumEntries(reflect::enums::kFogColorMode);
    const char* section = nullptr;
    bool tableOpen = false;
    for (const inspectorfog::FieldRow& row : inspectorfog::kRows) {
        if (!section || std::string(section) != row.section) {
            if (tableOpen) endTable();
            section = row.section;
            ImGui::SeparatorText(section);
            beginTable(ComponentType::FogComponent, labelSize, std::string("fog_profile_asset_") + section);
            tableOpen = true;
        }
        const FogField field = row.field;
        const bool ticked = data.isSet(field);
        bool toggled = false;
        propertyLabelPrefix = [&]() {
            bool box = ticked;
            if (ImGui::Checkbox((std::string("##asset_set_") + FogFields::name(field)).c_str(), &box)) toggled = true;
            UIProbe::item(std::string("asset.fogProfile.set.") + FogFields::name(field));
            ImGui::SetItemTooltip("%s", ticked ? "Set by the profile: every Fog and Fog Zone using it takes this value. Untick to leave it to each Fog."
                                               : "Not set: each Fog keeps its own value. Tick to set it here.");
        };
        propertyHeader(FogFields::info(field).label);
        propertyLabelPrefix = nullptr;
        if (ticked) {
            FogComponent edited = data.values;
            const inspectorfog::ValueEdit edit = inspectorfog::valueWidget(field, row.widget, edited, row.step, row.format, FogFields::info(field).label,
                                                                           &colorModes);
            ImGui::SameLine();
            helpMarker(row.help);
            if (edit.begin) ui::ColorPickerWindow::beginEdit(history, FogFields::info(field).label);
            if (edit.changed) {
                FogProfileData after = data;
                after.setFrom(edited, field);
                commitEdit(after, edit.commit);
            } else if (edit.commit) {
                pushFogProfileCommand(history, nullptr, true);
            }
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", inspectorfog::kNotSet);
            ImGui::SetItemTooltip("%s", row.help);
        }
        if (toggled) {
            FogProfileData after = data;
            if (ticked) after.clear(field);
            else after.setFrom(data.values, field);
            commitEdit(after, true);
        }
    }
    if (tableOpen) endTable();
    ImGui::EndDisabled();
    if (!editable) ImGui::TextDisabled("Stop Play to edit the profile.");

    // Used by: a click selects an open user, or opens a file that uses it
    if (state.usageKey != key || ImGui::GetFrameCount() - state.usageFrame > 30) {
        state.usage = inspectorfog::readUsage(project, key);
        state.usageKey = key;
        state.usageFrame = ImGui::GetFrameCount();
    }
    ImGui::SeparatorText(inspectorfog::usageText(state.usage).c_str());
    UIProbe::item("asset.fogProfile.usedBy");
    if (state.usage.count() == 0) ImGui::TextDisabled("No Fog or Fog Zone uses it.");
    for (const fogprofiles::FogProfileUser& user : state.usage.open) {
        SceneProject* sceneProject = project->getScene(user.sceneId);
        if (!sceneProject || !sceneProject->scene) continue;
        const std::string label = std::string(user.zone ? ICON_FA_SMOG "  Fog Zone  " : ICON_FA_SMOG "  Fog  ") + user.sceneName + " / " +
                                  sceneProject->scene->getEntityName(user.entity) + "##fog_user_" + std::to_string(user.sceneId) + "_" +
                                  std::to_string(user.entity);
        if (ImGui::Selectable(label.c_str())) {
            project->setSelectedSceneId(user.sceneId);
            project->setSelectedEntity(user.sceneId, user.entity);
            project->clearInspectedAsset();
        }
    }
    for (const std::string& file : state.usage.closedFiles) {
        if (ImGui::Selectable((std::string(ICON_FA_FILE "  ") + file + " (not open)##fog_file_" + file).c_str())) {
            const fs::path path = project->getProjectPath() / FileUtils::pathFromUtf8(file);
            if (Util::isPrefabFile(file)) getEditorHost().openPrefabAsset(path);
            else project->openScene(path, false);
        }
        ImGui::SetItemTooltip("Opens it");
    }
    if (!state.usage.indexReady) ImGui::TextDisabled("Indexing the project: scenes that are not open are not listed yet.");
}
