// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptValueEditor.h"

#include "Out.h"
#include "Project.h"
#include "dataasset/DataAssets.h"
#include "external/IconsFontAwesome6.h"
#include "util/AssetDatabase.h"
#include "util/EntityPayload.h"
#include "util/FileUtils.h"
#include "util/Util.h"
#include "widget/ColorField.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // a text field over a std::string
    bool editString(const char* id, std::string& value, bool& editing) {
        char buffer[1024];
        std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool changed = ImGui::InputText(id, buffer, sizeof(buffer));
        if (changed) value = buffer;
        editing |= ImGui::IsItemActive();
        return changed;
    }

    // the label of a reference's button and its tooltip; missing: red
    struct ReferenceLabel {
        std::string text;
        std::string tooltip;
        bool missing = false;
    };

    ReferenceLabel assetLabel(Project* project, const AssetRef& ref, AssetType expected, const std::string& typeLabel) {
        ReferenceLabel label;
        if (!ref.isSet()) {
            label.text = "None (" + typeLabel + ")";
            return label;
        }
        const AssetDatabase::Resolution found = project->getAssetDatabase().resolve(ref.guid, ref.path, expected, true);
        if (!found.found()) {
            label.missing = true;
            label.text = "Missing (" + fs::path(ref.path).filename().string() + ")";
            label.tooltip = "Missing (GUID " + ref.guid.str() + ")\nLast path: " + ref.path;
        } else {
            label.text = fs::path(found.path).stem().string();
            label.tooltip = found.path + "\nGUID " + found.guid.str();
        }
        return label;
    }

    // the assets a reference of `field` may hold (a Data Asset's: of its class), project-relative
    std::vector<std::string> fittingAssets(Project* project, const ScriptProperty& field, bool prefab) {
        std::vector<std::string> paths;
        const AssetType expected = prefab ? AssetType::Prefab : assetTypeFromName(field.assetType);
        project->getAssetDatabase().forEachAsset([&](const AssetDatabase::AssetInfo& info) {
            if (expected != AssetType::Unknown && info.type != expected) return;
            paths.push_back(info.path);
        });
        if (expected == AssetType::DataAsset && !field.ptrTypeName.empty()) {
            paths.erase(std::remove_if(paths.begin(), paths.end(), [&](const std::string& path) {
                return DataAssets::classOfFile(project->getProjectPath() / FileUtils::pathFromUtf8(path)) != field.ptrTypeName;
            }), paths.end());
        }
        std::sort(paths.begin(), paths.end());
        return paths;
    }

    // a dropped or picked file as the reference `field` takes; false (warned) when it does not fit
    bool referenceFromFile(Project* project, const ScriptProperty& field, const std::string& file, ScriptPropertyValue& out) {
        const std::string relative = project->normalizeToProjectRelative(file).generic_string();
        std::string error;
        if (!DataAssets::fromJson(project, field, nlohmann::json(relative), true, out, error)) {
            Out::warning("%s: %s", field.displayName.empty() ? field.name.c_str() : field.displayName.c_str(), error.c_str());
            return false;
        }
        return true;
    }

    // a prefab or asset reference: its button (a drop target), the picker and None
    bool editReference(Project* project, const ScriptProperty& field, ScriptPropertyValue& value) {
        const bool prefab = field.type == ScriptPropertyType::Prefab;
        const AssetType expected = prefab ? AssetType::Prefab : assetTypeFromName(field.assetType);
        std::string typeLabel = prefab ? "Prefab" : field.assetType.empty() ? "Asset" : field.assetType;
        if (!prefab && !field.ptrTypeName.empty()) typeLabel = field.ptrTypeName;
        AssetRef ref;
        if (const PrefabAsset* p = std::get_if<PrefabAsset>(&value)) {
            ref = AssetRef(p->getPath(), Guid::isValidText(p->getGuid()) ? Guid::parse(p->getGuid()) : Guid());
        } else if (const AssetRef* a = std::get_if<AssetRef>(&value)) {
            ref = *a;
        }
        const ReferenceLabel label = assetLabel(project, ref, expected, typeLabel);
        bool changed = false;

        const float buttonWidth = ImGui::GetFrameHeight();
        const float fieldWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - 2 * (buttonWidth + ImGui::GetStyle().ItemSpacing.x));
        if (label.missing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.35f, 0.35f, 1.0f));
        ImGui::Button((std::string(prefab ? ICON_FA_CUBE " " : ICON_FA_FILE " ") + label.text + "##ref").c_str(), ImVec2(fieldWidth, 0));
        if (label.missing) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", label.tooltip.empty() ? ("Drag a " + typeLabel + " here from the Project window").c_str() : label.tooltip.c_str());
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
                if (!dropped.empty() && referenceFromFile(project, field, dropped[0], value)) changed = true;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_CIRCLE_DOT "##pick", ImVec2(buttonWidth, 0))) ImGui::OpenPopup("pick");
        ImGui::SetItemTooltip("Select %s", typeLabel.c_str());
        if (ImGui::BeginPopup("pick")) {
            if (ImGui::Selectable("None", !ref.isSet())) {
                value = prefab ? ScriptPropertyValue(PrefabAsset()) : ScriptPropertyValue(AssetRef());
                changed = true;
            }
            for (const std::string& path : fittingAssets(project, field, prefab)) {
                if (ImGui::Selectable(path.c_str(), path == ref.path) &&
                        referenceFromFile(project, field, (project->getProjectPath() / FileUtils::pathFromUtf8(path)).string(), value)) {
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!ref.isSet());
        if (ImGui::Button(ICON_FA_XMARK "##none", ImVec2(buttonWidth, 0))) {
            value = prefab ? ScriptPropertyValue(PrefabAsset()) : ScriptPropertyValue(AssetRef());
            changed = true;
        }
        ImGui::SetItemTooltip("Set to None");
        ImGui::EndDisabled();
        return changed;
    }

    // a GameObject reference: its name, a drop target from the Hierarchy, None
    bool editEntity(Project* project, SceneProject* scene, ScriptPropertyValue& value) {
        EntityReference ref = std::get_if<EntityReference>(&value) ? std::get<EntityReference>(value) : EntityReference{};
        std::string name = "None (GameObject)";
        if (ref.entity != NULL_ENTITY && scene && scene->scene && ref.sceneId == 0) {
            name = scene->scene->isEntityCreated(ref.entity) ? scene->scene->getEntityName(ref.entity) : "Missing (" + std::to_string(ref.entity) + ")";
        } else if (ref.entity != NULL_ENTITY) {
            name = "GameObject " + std::to_string(ref.entity) + (ref.sceneId ? " of scene " + std::to_string(ref.sceneId) : std::string());
        }
        bool changed = false;
        const float buttonWidth = ImGui::GetFrameHeight();
        ImGui::Button((ICON_FA_CUBES " " + name + "##entity").c_str(), ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x), 0));
        ImGui::SetItemTooltip("Drag a GameObject here from the Hierarchy");
        if (project && scene && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("entity", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
                const EntityPayload* entity = reinterpret_cast<const EntityPayload*>(payload->Data);
                // the Inspector's reference rule: this scene or one loaded with it (its parents,
                // children and siblings), else the reference finds nothing when the game runs;
                // another scene's GameObject keeps its scene id
                const std::string problem = referenceDropProblem(*project, *entity, scene->id);
                if (!problem.empty()) {
                    ImGui::SetTooltip("%s", problem.c_str());
                } else if (payload->IsDelivery()) {
                    const uint32_t droppedSceneId = payloadSceneId(*entity, project->getSelectedSceneId());
                    value = EntityReference{entity->entity, droppedSceneId != scene->id ? droppedSceneId : 0u};
                    changed = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(ref.entity == NULL_ENTITY);
        if (ImGui::Button(ICON_FA_XMARK "##none", ImVec2(buttonWidth, 0))) {
            value = EntityReference{};
            changed = true;
        }
        ImGui::EndDisabled();
        return changed;
    }

    // the colour field whose Color window session is open (its edits merge into one undo step)
    ImGuiID scriptValueColorSession = 0;

    // a Color3 / Color4 value through the shared colour field (its window, eyedropper and
    // linear storage, as the Inspector's script colours): changed on each live change and on Esc
    // (the original back), editing while its window's session is open
    template <class V>
    bool editScriptColor(V& v, bool alpha, bool& editing) {
        ui::ColorFieldOptions options;
        options.alpha = alpha;
        const ImGuiID id = ImGui::GetID("##v");
        const ui::EditPhase phase = ui::colorField("##v", v, options);
        if (phase == ui::EditPhase::Begin) scriptValueColorSession = id;
        if (phase == ui::EditPhase::Committed || phase == ui::EditPhase::Cancelled) scriptValueColorSession = 0;
        editing |= scriptValueColorSession == id;
        return phase == ui::EditPhase::Changing || phase == ui::EditPhase::Applied || phase == ui::EditPhase::Cancelled;
    }

    // one value (not a list)
    bool editSingle(Project* project, SceneProject* scene, const ScriptProperty& field, ScriptPropertyValue& value, bool& editing) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        bool changed = false;
        switch (field.type) {
            case ScriptPropertyType::Bool: {
                bool v = std::get_if<bool>(&value) ? std::get<bool>(value) : false;
                if ((changed = ImGui::Checkbox("##v", &v))) value = v;
                break;
            }
            case ScriptPropertyType::Int: {
                int v = std::get_if<int>(&value) ? std::get<int>(value) : 0;
                if ((changed = ImGui::DragInt("##v", &v))) value = v;
                break;
            }
            case ScriptPropertyType::Float: {
                float v = std::get_if<float>(&value) ? std::get<float>(value) : 0.0f;
                if ((changed = ImGui::DragFloat("##v", &v, 0.01f))) value = v;
                break;
            }
            case ScriptPropertyType::String: {
                std::string v = std::get_if<std::string>(&value) ? std::get<std::string>(value) : std::string();
                if ((changed = editString("##v", v, editing))) value = v;
                return changed;
            }
            case ScriptPropertyType::Vector2: {
                Vector2 v = std::get_if<Vector2>(&value) ? std::get<Vector2>(value) : Vector2(0, 0);
                if ((changed = ImGui::DragFloat2("##v", &v.x, 0.01f))) value = v;
                break;
            }
            case ScriptPropertyType::Vector3: {
                Vector3 v = std::get_if<Vector3>(&value) ? std::get<Vector3>(value) : Vector3(0, 0, 0);
                if ((changed = ImGui::DragFloat3("##v", &v.x, 0.01f))) value = v;
                break;
            }
            case ScriptPropertyType::Color3: {
                Vector3 v = std::get_if<Vector3>(&value) ? std::get<Vector3>(value) : Vector3(0, 0, 0);
                if ((changed = editScriptColor(v, false, editing))) value = v;
                break;
            }
            case ScriptPropertyType::Vector4: {
                Vector4 v = std::get_if<Vector4>(&value) ? std::get<Vector4>(value) : Vector4(0, 0, 0, 1);
                if ((changed = ImGui::DragFloat4("##v", &v.x, 0.01f))) value = v;
                break;
            }
            case ScriptPropertyType::Color4: {
                Vector4 v = std::get_if<Vector4>(&value) ? std::get<Vector4>(value) : Vector4(0, 0, 0, 1);
                if ((changed = editScriptColor(v, true, editing))) value = v;
                break;
            }
            case ScriptPropertyType::Prefab:
            case ScriptPropertyType::Asset:
                return editReference(project, field, value);
            case ScriptPropertyType::EntityReference:
                return editEntity(project, scene, value);
            case ScriptPropertyType::List:
                break;
        }
        editing |= ImGui::IsItemActive();
        return changed;
    }

}

ScriptValueEdit jasperengine::editor::drawScriptValue(Project* project, SceneProject* scene, const ScriptProperty& field, ScriptPropertyValue& value,
                                                      const std::string& id) {
    ScriptValueEdit edit;
    ImGui::PushID(id.c_str());
    if (field.type != ScriptPropertyType::List) {
        edit.changed = editSingle(project, scene, field, value, edit.editing);
        ImGui::PopID();
        return edit;
    }

    // a list: its size, then one row per element (Unity's reorderable list)
    ScriptPropertyList list = std::get_if<ScriptPropertyList>(&value) ? std::get<ScriptPropertyList>(value) : ScriptPropertyList();
    ScriptProperty element = field;
    element.type = field.elementType;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%zu %s", list.items.size(), list.items.size() == 1 ? "element" : "elements");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_PLUS "##add")) {
        list.items.push_back(list.items.empty() ? scriptPropertyElementDefault(field.elementType) : list.items.back());
        edit.changed = true;
    }
    ImGui::SetItemTooltip("Add an element (a copy of the last)");
    int move = -1, moveTo = -1, remove = -1;
    for (size_t i = 0; i < list.items.size(); i++) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%zu", i);
        ImGui::SameLine();
        const float buttons = 3 * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::BeginGroup();
        ImGui::PushItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - buttons));
        ImGui::BeginChild("##element", ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - buttons), ImGui::GetFrameHeight()),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        ScriptPropertyValue item = scriptPropertyValueOf(list.items[i]);
        if (editSingle(project, scene, element, item, edit.editing)) {
            list.items[i] = scriptPropertyElementOf(item);
            edit.changed = true;
        }
        ImGui::EndChild();
        ImGui::PopItemWidth();
        ImGui::EndGroup();
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::Button(ICON_FA_ARROW_UP "##up", ImVec2(ImGui::GetFrameHeight(), 0))) {
            move = static_cast<int>(i);
            moveTo = static_cast<int>(i) - 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == list.items.size());
        if (ImGui::Button(ICON_FA_ARROW_DOWN "##down", ImVec2(ImGui::GetFrameHeight(), 0))) {
            move = static_cast<int>(i);
            moveTo = static_cast<int>(i) + 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_XMARK "##remove", ImVec2(ImGui::GetFrameHeight(), 0))) remove = static_cast<int>(i);
        ImGui::SetItemTooltip("Remove element %zu", i);
        ImGui::PopID();
    }
    if (move >= 0) {
        std::swap(list.items[static_cast<size_t>(move)], list.items[static_cast<size_t>(moveTo)]);
        edit.changed = true;
    }
    if (remove >= 0) {
        list.items.erase(list.items.begin() + remove);
        edit.changed = true;
    }
    if (edit.changed) value = std::move(list);
    ImGui::PopID();
    return edit;
}
