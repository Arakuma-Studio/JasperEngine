// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector > Vertex Colors (docs/specs/V1-vertex-colors.md R5-R7, R10, R61, R62, R100, R132), with
// Unity-style labels: the problem strip, Paint, Clear and Make Unique, the component's fields through
// the Catalog (PropertyCmd: undo, multi-selection, prefab overrides) but Format, which converts the
// file (setFormat), the channel names as R, G, B and A rows with what each one feeds, each mesh's
// vertex count and the vertices its hard edges add, Export as Model... and Bake to Texture....
// Also the Paint Vertex Colors entry the Model and Mesh Inspectors show.

#include "VertexColorEditing.h"
#include "VertexColorEditorInternal.h"
#include "VertexColorInspector.h"

#include "Project.h"
#include "component/ModelComponent.h"
#include "component/VertexColorsComponent.h"
#include "external/IconsFontAwesome6.h"
#include "reflect/Enums.h"
#include "subsystem/VertexColorScene.h"
#include "vertexcolor/VertexColorSplit.h"
#include "window/EditorModal.h"
#include "window/InspectorWindow.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace vcd = jasperengine::editor::vertexcolor::detail;

namespace {

    constexpr ComponentType kVcInspectorType = ComponentType::VertexColorsComponent;
    constexpr const char* kVcChannelName[4] = {"R", "G", "B", "A"};

    // the channels the paint holds something other than the initial value in, per file revision
    struct VcPaintedChannels {
        const jasperengine::vertexcolor::Data* data = nullptr;
        uint64_t revision = 0;
        jasperengine::vertexcolor::ChannelMask mask = 0;
    };

    jasperengine::vertexcolor::ChannelMask vcPaintedMask(Project* project, uint32_t sceneId, Entity entity) {
        static std::map<std::pair<uint32_t, Entity>, VcPaintedChannels> cache;
        const VertexColorsComponent* component = vcd::componentOf(project, sceneId, entity);
        const std::shared_ptr<jasperengine::vertexcolor::Data> data = vcd::resolve(project, sceneId, entity);
        if (!component || !data) return 0;
        VcPaintedChannels& known = cache[{sceneId, entity}];
        if (known.data == data.get() && known.revision == component->revision) return known.mask;
        known.data = data.get();
        known.revision = component->revision;
        known.mask = 0;
        Vector4 custom;
        const jasperengine::vertexcolor::InitialValue initial = vcd::initialFor(project, sceneId, entity, &custom);
        for (const jasperengine::vertexcolor::TargetData& target : data->targets) {
            const std::shared_ptr<const jasperengine::vertexcolor::MeshView> mesh = jasperengine::editor::vertexcolor::meshView(project, sceneId, entity, target.key);
            for (size_t v = 0; v < target.base.size() && known.mask != jasperengine::vertexcolor::CHANNELS_RGBA; v++) {
                const Vector4* asset = mesh && v < mesh->assetColors.size() ? &mesh->assetColors[v] : nullptr;
                const Vector4 start = jasperengine::vertexcolor::quantize(jasperengine::vertexcolor::initialValueFor(initial, custom, asset), data->format);
                const Vector4& value = target.base[v];
                if (value.x != start.x) known.mask |= jasperengine::vertexcolor::CHANNEL_R;
                if (value.y != start.y) known.mask |= jasperengine::vertexcolor::CHANNEL_G;
                if (value.z != start.z) known.mask |= jasperengine::vertexcolor::CHANNEL_B;
                if (value.w != start.w) known.mask |= jasperengine::vertexcolor::CHANNEL_A;
            }
            if (!target.corners.empty()) known.mask |= jasperengine::vertexcolor::CHANNELS_RGB;
        }
        return known.mask;
    }

    void vcErrorText(const std::string& text) {
        if (text.empty()) return;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", text.c_str());
        ImGui::PopTextWrapPos();
    }

    void vcOpenPaint(Project* project, uint32_t sceneId, Entity entity, std::string& error) {
        error.clear();
        jasperengine::editor::vertexcolor::ensureComponent(project, sceneId, entity, &error);
        if (!error.empty()) return;
        if (jasperengine::editor::vertexcolor::paintToolHook()) jasperengine::editor::vertexcolor::paintToolHook()(project, sceneId, entity);
    }

}

const std::vector<VertexColorInspectorRow>& editor::vertexColorInspectorRows() {
    using K = VertexColorInspectorRowKind;
    static const std::vector<VertexColorInspectorRow> rows = {
        {"data", "Data", K::Asset, "The .vcolors asset holding this object's colors; the first edit makes it beside the scene"},
        {"format", "Format", K::Format, "RGBA8, or High Precision for values above 1 and fine data channels. Changing it converts the file"},
        {"initialValue", "Initial Value", K::Enum, "What a mesh holds before its first edit, and what Erase goes back to"},
        {"customInitial", "Custom Initial", K::Color, "The Initial Value when it is Custom (linear)"},
        {"channelNames[0]", "R Channel", K::ChannelName, "A name for what R holds (\"Moss\"), shared by the painter and the material"},
        {"channelNames[1]", "G Channel", K::ChannelName, "A name for what G holds"},
        {"channelNames[2]", "B Channel", K::ChannelName, "A name for what B holds"},
        {"channelNames[3]", "A Channel", K::ChannelName, "A name for what A holds"},
    };
    return rows;
}

std::string editor::vertexColorCount(uint64_t value) {
    std::string digits = std::to_string(value);
    for (int i = (int)digits.size() - 3; i > 0; i -= 3) digits.insert((size_t)i, ",");
    return digits;
}

std::vector<VertexColorTargetRow> editor::vertexColorTargetRows(Project* project, uint32_t sceneId, Entity entity) {
    std::vector<VertexColorTargetRow> rows;
    for (const jasperengine::editor::vertexcolor::TargetInfo& target : jasperengine::editor::vertexcolor::listTargets(project, sceneId, entity)) {
        VertexColorTargetRow row;
        row.name = target.name;
        row.painted = target.painted;
        row.stale = target.stale;
        row.detail = vertexColorCount(target.vertexCount) + " vertices";
        if (!target.painted) {
            row.detail += ", not painted";
        } else if (target.stale) {
            row.detail += ", painted on another version of the mesh";
        } else {
            uint32_t split = target.splitVertices;
            if (split == 0) {
                // the render path may not have bound it yet: count it here
                const jasperengine::vertexcolor::TargetData* data = jasperengine::editor::vertexcolor::targetData(project, sceneId, entity, target.key);
                const std::shared_ptr<const jasperengine::vertexcolor::MeshView> mesh = jasperengine::editor::vertexcolor::meshView(project, sceneId, entity, target.key);
                if (data && mesh && data->hasOverrides()) split = jasperengine::vertexcolor::buildSplit(*data, *mesh, {}).added;
            }
            if (split > 0) row.detail += ", +" + vertexColorCount(split) + " vertices for hard edges";
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<std::string> editor::vertexColorProblemLines(Project* project, uint32_t sceneId, Entity entity) {
    std::vector<std::string> lines;
    std::string error;
    if (vcd::componentOf(project, sceneId, entity) && !vcd::resolve(project, sceneId, entity, &error) && !error.empty()) lines.push_back(error);
    for (const jasperengine::editor::vertexcolor::RemapIssue& issue : jasperengine::editor::vertexcolor::remapIssues(project, sceneId)) {
        if (issue.entity != entity) continue;
        char distance[32];
        std::snprintf(distance, sizeof(distance), "%.4g", (double)issue.meanDistance);
        lines.push_back(issue.unmatched
                            ? issue.key.describe() + " was painted on another version of its mesh and nothing matches it (vertex_colors_unmatched): it draws "
                              "its Initial Value. Clear and repaint it."
                            : issue.key.describe() + " was transferred to its reimported mesh (vertex_colors_remapped, mean distance " + distance +
                              "): check it and save the scene to keep it.");
    }
    for (const jasperengine::vertexcolor::Warning& warning : jasperengine::editor::vertexcolor::materialWarnings(project, sceneId, entity, vcPaintedMask(project, sceneId, entity))) lines.push_back(warning.message);
    return lines;
}

void editor::InspectorWindow::drawPaintVertexColorsButton(SceneProject* sceneProject, const std::vector<Entity>& entities) {
    if (entities.size() != 1 || sceneProject->scene->findComponent<VertexColorsComponent>(entities[0])) return;
    const Entity entity = entities[0];
    if (!jasperengine::editor::vertexcolor::canPaint(project, sceneProject->id, entity, nullptr)) return;
    static std::string error;
    if (ImGui::Button(ICON_FA_PAINTBRUSH " Paint Vertex Colors")) vcOpenPaint(project, sceneProject->id, entity, error);
    ImGui::SetItemTooltip("Adds Vertex Colors to this GameObject (the model is never written) and opens the Vertex Paint window");
    vcErrorText(error);
}

void editor::InspectorWindow::drawVertexColorsComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    const uint32_t sceneId = sceneProject->id;
    const Entity entity = entities[0];
    const bool single = entities.size() == 1;
    static std::string actionError;
    VertexColorsComponent& component = sceneProject->scene->getComponent<VertexColorsComponent>(entity);

    if (single) {
        for (const std::string& line : vertexColorProblemLines(project, sceneId, entity)) vcErrorText(line);
        // R73: the one-click fix of the materials that ignore painted channels
        const std::vector<jasperengine::editor::vertexcolor::vc::Warning> warnings = jasperengine::editor::vertexcolor::materialWarnings(project, sceneId, entity, vcPaintedMask(project, sceneId, entity));
        for (size_t w = 0; w < warnings.size(); w++) {
            ImGui::PushID((int)w);
            if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES " Fix Material")) {
                actionError.clear();
                jasperengine::editor::vertexcolor::fixMaterialWarning(project, sceneId, entity, warnings[w], &actionError);
            }
            ImGui::SetItemTooltip("%s", warnings[w].message.c_str());
            ImGui::PopID();
            if (w + 1 < warnings.size()) ImGui::SameLine();
        }
        std::string why;
        const bool paintable = jasperengine::editor::vertexcolor::canPaint(project, sceneId, entity, &why);
        ImGui::BeginDisabled(!paintable);
        if (ImGui::Button(ICON_FA_PAINTBRUSH " Paint")) vcOpenPaint(project, sceneId, entity, actionError);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", paintable ? "Open the Vertex Paint window with the Paint tool" : why.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(component.data.path.empty());
        if (ImGui::Button(ICON_FA_ERASER " Clear")) {
            actionError.clear();
            jasperengine::editor::vertexcolor::clearAll(project, sceneId, entity, &actionError);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Remove the paint of every mesh: they draw their own colors again. One undo step");
        const int users = jasperengine::editor::vertexcolor::fileUserCount(project, component.data.path);
        if (users > 1) {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_CLONE " Make Unique")) {
                actionError.clear();
                jasperengine::editor::vertexcolor::makeUnique(project, sceneId, entity, &actionError);
            }
            ImGui::SetItemTooltip("Give this GameObject its own copy of the colors, with a new GUID (its first edit makes one anyway)");
            ImGui::SameLine();
            ImGui::TextDisabled("%d GameObjects share these colors", users);
        }
    }

    const std::array<std::string, 4> uses = single ? jasperengine::editor::vertexcolor::channelUses(project, sceneId, entity) : std::array<std::string, 4>();
    std::vector<EnumEntry>* initialValues = InspectorWindow::enumEntries(reflect::enums::kVertexColorInitialValue);
    beginTable(cpType, getLabelSize("Custom Initial"));
    for (const VertexColorInspectorRow& row : vertexColorInspectorRows()) {
        RowSettings settings;
        settings.help = row.help;
        switch (row.kind) {
            case VertexColorInspectorRowKind::Asset:
                propertyRow(RowPropertyType::Asset, cpType, row.property, row.label, sceneProject, entities, settings);
                break;
            case VertexColorInspectorRowKind::Format: {
                // the file's values convert with it: setFormat, not the plain field
                propertyHeader(row.label);
                const bool high = component.format == jasperengine::vertexcolor::Format::RGBA16F;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##vertexColorFormat", high ? "High Precision (RGBA16F)" : "RGBA8")) {
                    for (const jasperengine::vertexcolor::Format format : {jasperengine::vertexcolor::Format::RGBA8, jasperengine::vertexcolor::Format::RGBA16F}) {
                        const bool selected = component.format == format;
                        if (ImGui::Selectable(format == jasperengine::vertexcolor::Format::RGBA16F ? "High Precision (RGBA16F)" : "RGBA8", selected) && !selected) {
                            actionError.clear();
                            for (Entity each : entities) jasperengine::editor::vertexcolor::setFormat(project, sceneId, each, format, &actionError);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("%s", row.help);
                break;
            }
            case VertexColorInspectorRowKind::Enum:
                settings.enumEntries = initialValues;
                propertyRow(RowPropertyType::Enum, cpType, row.property, row.label, sceneProject, entities, settings);
                break;
            case VertexColorInspectorRowKind::Color:
                if (component.initialValue != jasperengine::vertexcolor::InitialValue::Custom) break;
                propertyRow(RowPropertyType::Color4L, cpType, row.property, row.label, sceneProject, entities, settings);
                break;
            case VertexColorInspectorRowKind::ChannelName: {
                const size_t channel = (size_t)(row.property[13] - '0');
                // R7, R72: what the channel feeds, for the person naming it
                if (channel < 4 && !uses[channel].empty()) settings.help = std::string(row.help) + ". Feeds: " + uses[channel];
                propertyRow(RowPropertyType::String, cpType, row.property, row.label, sceneProject, entities, settings);
                break;
            }
        }
    }
    endTable();
    if (!single) return;

    // R100, R124: each mesh, its vertices and what its hard edges add
    const std::vector<VertexColorTargetRow> targets = vertexColorTargetRows(project, sceneId, entity);
    if (!targets.empty()) {
        beginTable(cpType, getLabelSize("Body (node 00, primitive 0)"), "vertexcolor_targets");
        for (const VertexColorTargetRow& row : targets) {
            propertyHeader(row.name);
            ImGui::AlignTextToFramePadding();
            if (row.stale) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s", row.detail.c_str());
            else if (!row.painted) ImGui::TextDisabled("%s", row.detail.c_str());
            else ImGui::TextUnformatted(row.detail.c_str());
        }
        endTable();
    }

    // R61, R62
    static char exportPath[512] = "";
    static bool exportReplace = false;
    static bool exportSplit = true;
    const bool painted = std::any_of(targets.begin(), targets.end(), [](const VertexColorTargetRow& row) { return row.painted; });
    ImGui::BeginDisabled(!painted);
    if (ImGui::Button(ICON_FA_FILE_EXPORT " Export as Model...")) {
        actionError.clear();
        const ModelComponent* model = sceneProject->scene->findComponent<ModelComponent>(entity);
        std::string suggestion;
        if (model && !model->filename.path.empty()) {
            const std::filesystem::path path(model->filename.path);
            suggestion = (path.parent_path() / (path.stem().string() + " (" + sceneProject->scene->getEntityName(entity) + ").glb")).generic_string();
        }
        std::snprintf(exportPath, sizeof(exportPath), "%s", suggestion.c_str());
        ImGui::OpenPopup("Export as Model##vertexcolors");
    }
    ImGui::SetItemTooltip("Write a new .glb with these colors as COLOR_0 (a Mesh's geometry, or the Model's file; empty Path: beside it)");
    ImGui::SameLine();
    static char bakePath[512] = "";
    static int bakeTarget = 0;
    static int bakeSize = 1024;
    static int bakePadding = 4;
    static int bakeUv = 0;
    static bool bakeSrgb = true;
    if (ImGui::Button(ICON_FA_IMAGE " Bake to Texture...")) {
        actionError.clear();
        bakeTarget = 0;
        std::snprintf(bakePath, sizeof(bakePath), "%s", (sceneProject->scene->getEntityName(entity) + " Colors.png").c_str());
        ImGui::OpenPopup("Bake to Texture##vertexcolors");
    }
    ImGui::SetItemTooltip("Rasterize a mesh's colors into a PNG in UV space");
    ImGui::EndDisabled();

    if (EditorModal::begin("Export as Model##vertexcolors", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditorModal::inputText("Path", "path", exportPath, sizeof(exportPath));
        EditorModal::checkbox("Split Hard Edges", "split_hard_edges", &exportSplit);
        ImGui::SetItemTooltip("Faces with their own colors get their own vertices in the file");
        EditorModal::checkbox("Replace in Scene", "replace_in_scene", &exportReplace);
        ImGui::SetItemTooltip("The GameObject draws the new file as its Model (a Mesh becomes a Model) and loses this component, in one undo step");
        EditorModal::error(actionError);
        if (EditorModal::button("Export", "ok")) {
            jasperengine::editor::vertexcolor::ExportModelOptions options;
            options.path = exportPath;
            options.replaceInScene = exportReplace;
            options.splitHardEdges = exportSplit;
            actionError.clear();
            if (jasperengine::editor::vertexcolor::exportAsModel(project, sceneId, entity, options, &actionError)) ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel")) ImGui::CloseCurrentPopup();
        vcErrorText(actionError);
        ImGui::EndPopup();
    }
    if (EditorModal::begin("Bake to Texture##vertexcolors", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::vector<std::string> names;
        for (const VertexColorTargetRow& row : targets) names.push_back(row.name);
        if (!names.empty()) EditorModal::combo("Mesh", "target", &bakeTarget, names);
        EditorModal::inputText("Path", "path", bakePath, sizeof(bakePath));
        EditorModal::inputInt("Size", "size", &bakeSize, 256, 1024);
        EditorModal::inputInt("Padding", "padding", &bakePadding);
        ImGui::SetItemTooltip("Texels grown around each UV island, so filtering never reads the background");
        EditorModal::combo("UV Set", "uv_set", &bakeUv, {"UV0", "UV1"});
        EditorModal::checkbox("sRGB", "srgb", &bakeSrgb);
        ImGui::SetItemTooltip("On for colors seen as colors; off for data channels (the values as they are)");
        EditorModal::error(actionError);
        if (EditorModal::button("Bake", "ok")) {
            const std::vector<jasperengine::editor::vertexcolor::TargetInfo> infos = jasperengine::editor::vertexcolor::listTargets(project, sceneId, entity);
            jasperengine::editor::vertexcolor::BakeTextureOptions options;
            if (bakeTarget >= 0 && (size_t)bakeTarget < infos.size()) options.key = infos[(size_t)bakeTarget].key;
            options.path = bakePath;
            options.size = bakeSize;
            options.padding = bakePadding;
            options.uvSet = bakeUv;
            options.srgb = bakeSrgb;
            actionError.clear();
            if (jasperengine::editor::vertexcolor::bakeToTexture(project, sceneId, entity, options, &actionError)) ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (EditorModal::button("Cancel", "cancel")) ImGui::CloseCurrentPopup();
        vcErrorText(actionError);
        ImGui::EndPopup();
    }
    vcErrorText(actionError);
}
