// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Inspector rows of the shader library (docs/superpowers/specs/2026-10-10-shader-library-phase1-design.md):
// the Shader picker, the rows a shader's manifest declares (parameters, textures, features,
// keywords), a material's Surface and Lighting rows, and the render state of UI, points and
// lines. Every edit is a property command on the Catalog paths MCP uses too.

#include "InspectorWindow.h"

#include "App.h"
#include "Backend.h"
#include "command/CommandHandle.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "external/IconsFontAwesome6.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"
#include "util/Color.h"
#include "util/FileDialogs.h"
#include "util/Util.h"

#include <algorithm>
#include <filesystem>
#include <map>

using namespace jasperengine;

namespace {

    using ShaderTextures = std::vector<std::pair<std::string, Texture>>;

    const char* blendLabel(BlendMode mode) {
        switch (mode) {
            case BlendMode::NONE: return "Opaque";
            case BlendMode::ALPHA: return "Alpha";
            case BlendMode::PREMULTIPLIED: return "Premultiplied";
            case BlendMode::ADDITIVE: return "Additive";
            case BlendMode::MULTIPLY: return "Multiply";
        }
        return "Alpha";
    }

    const char* depthCompareLabel(DepthCompare compare) {
        switch (compare) {
            case DepthCompare::LESS: return "Less";
            case DepthCompare::LESS_EQUAL: return "Less Equal";
            case DepthCompare::EQUAL: return "Equal";
            case DepthCompare::GREATER_EQUAL: return "Greater Equal";
            case DepthCompare::GREATER: return "Greater";
            case DepthCompare::NOT_EQUAL: return "Not Equal";
            case DepthCompare::ALWAYS: return "Always";
        }
        return "Less Equal";
    }

    const char* lightingLabel(MaterialLighting mode) {
        switch (mode) {
            case MaterialLighting::AUTO: return "Auto";
            case MaterialLighting::PIXEL: return "Per Pixel";
            case MaterialLighting::VERTEX: return "Per Vertex";
            case MaterialLighting::MIXED: return "Mixed";
            case MaterialLighting::NONE: return "None (unlit)";
        }
        return "Auto";
    }

    const char* textureDefaultLabel(ShaderTextureDefault value) {
        switch (value) {
            case ShaderTextureDefault::WHITE: return "Default (white)";
            case ShaderTextureDefault::BLACK: return "Default (black)";
            case ShaderTextureDefault::GRAY: return "Default (gray)";
            case ShaderTextureDefault::NORMAL: return "Default (flat normal)";
        }
        return "Default";
    }

    // The label a manifest row shows
    std::string rowLabel(const std::string& label, const std::string& fallback) {
        return label.empty() ? fallback : label;
    }

    // Enum entries whose Auto entry names what it resolves to; the strings outlive the row
    struct AutoEnumEntries {
        std::string autoLabel;
        std::vector<editor::EnumEntry> entries;
    };

}

void editor::InspectorWindow::drawLibraryShaderCombo(const std::string& currentShader, ShaderType type,
                                                     const std::function<void(const std::string&)>& setShader,
                                                     const std::string& popupName){
    const char* builtIn = type == ShaderType::MESH ? "Lit (built-in)" : "Built-in";

    std::string preview = builtIn;
    if (ShaderReference::isLibrary(currentShader)) {
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(currentShader);
        preview = (manifest && !manifest->name.empty()) ? manifest->name : currentShader;
    } else if (!currentShader.empty()) {
        preview = "Custom: " + currentShader;
    }

    // the library shaders of this type by category, in name order within each
    std::map<std::string, std::vector<std::pair<std::string, std::shared_ptr<const ShaderManifest>>>> categories;
    for (const std::string& name : ShaderManifests::getLibraryShaderNames()) {
        const std::string reference = ShaderReference::LIBRARY_PREFIX + name;
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(reference);
        if (!manifest || manifest->type != type)
            continue;
        categories[manifest->category.empty() ? "Other" : manifest->category].push_back({reference, manifest});
    }

    // the files popup opens after the combo closes: opened from inside the combo's own popup
    // window, its id would not match the BeginPopup of the row (drawShaderFilesPopup)
    bool openFiles = false;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo(("##shader_picker_" + popupName).c_str(), preview.c_str())) {
        if (ImGui::Selectable(builtIn, currentShader.empty()) && !currentShader.empty())
            setShader("");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(type == ShaderType::MESH ? "The engine's physically based shader, with the material's PBR values"
                                                       : "The engine's own shader for this component");
        for (const auto& [category, shaders] : categories) {
            ImGui::SeparatorText(category.c_str());
            for (const auto& [reference, manifest] : shaders) {
                const std::string label = (manifest->name.empty() ? reference : manifest->name) + "##" + reference;
                if (ImGui::Selectable(label.c_str(), currentShader == reference) && currentShader != reference)
                    setShader(reference);
                if (!manifest->description.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", manifest->description.c_str());
            }
        }
        ImGui::Separator();
        const bool fork = !currentShader.empty() && !ShaderReference::isLibrary(currentShader);
        if (ImGui::Selectable("Custom file...", fork))
            openFiles = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A shader of this project: pick its .vert and .frag files");
        ImGui::EndCombo();
    }
    if (ShaderReference::isLibrary(currentShader) && ImGui::IsItemHovered()) {
        std::shared_ptr<const ShaderManifest> manifest = ShaderManifests::get(currentShader);
        if (manifest && !manifest->description.empty())
            ImGui::SetTooltip("%s\n\n%s", currentShader.c_str(), manifest->description.c_str());
        else if (!manifest)
            ImGui::SetTooltip("%s is not a library shader of this engine", currentShader.c_str());
    }
    if (openFiles)
        ImGui::OpenPopup(popupName.c_str());
}

bool editor::InspectorWindow::drawShaderManifestRows(ComponentType cpType, SceneProject* sceneProject, Entity entity, const std::string& shader,
                                                     const std::string& paramsProperty, const std::string& texturesProperty,
                                                     const std::string& featuresProperty, const std::function<void()>& onValueChanged){
    std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
    if (!manifest || !manifest->present)
        return false;

    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    ShaderUniformValues* paramsRef = Catalog::getPropertyRef<ShaderUniformValues>(scene, entity, cpType, paramsProperty);
    ShaderTextures* texturesRef = Catalog::getPropertyRef<ShaderTextures>(scene, entity, cpType, texturesProperty);
    std::vector<std::string>* featuresRef = Catalog::getPropertyRef<std::vector<std::string>>(scene, entity, cpType, featuresProperty);
    if (!paramsRef || !texturesRef || !featuresRef)
        return false;
    // copies: a command may change them while the rows are drawn
    const ShaderUniformValues params = *paramsRef;
    const ShaderTextures textures = *texturesRef;
    const std::vector<std::string> features = *featuresRef;

    // a discrete change is its own undo step; a drag merges until released
    auto setFeatures = [&](const std::vector<std::string>& value) {
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new PropertyCmd<std::vector<std::string>>(project, sceneId, entity, cpType, featuresProperty, value, onValueChanged));
    };
    auto setParams = [&](const ShaderUniformValues& value, bool continuous) {
        Command* paramsCmd = new PropertyCmd<ShaderUniformValues>(project, sceneId, entity, cpType, paramsProperty, value, onValueChanged);
        if (continuous) {
            cmd = paramsCmd;
            CommandHandle::get(sceneId)->addCommand(paramsCmd);
        } else {
            CommandHandle::get(sceneId)->addCommandNoMerge(paramsCmd);
        }
    };
    auto setTextures = [&](const ShaderTextures& value) {
        CommandHandle::get(sceneId)->addCommandNoMerge(
            new PropertyCmd<ShaderTextures>(project, sceneId, entity, cpType, texturesProperty, value, onValueChanged));
    };
    auto finishDrag = [&]() {
        if (ImGui::IsItemDeactivatedAfterEdit() && cmd) {
            cmd->setNoMerge();
            cmd = nullptr;
        }
    };

    // ---- features and keywords (compile-time: a change builds another variant, off the UI thread)
    for (const ShaderManifestFeature& feature : manifest->features) {
        ImGui::PushID(("feature_" + feature.define).c_str());
        prefabRowProperty = featuresProperty;
        propertyHeader(rowLabel(feature.label, feature.define));
        bool enabled = ShaderLibraryValues::isFeatureEnabled(features, shader, feature.define);
        if (ImGui::Checkbox(("##feature_" + feature.define).c_str(), &enabled)) {
            std::vector<std::string> changed = features;
            ShaderLibraryValues::setFeature(changed, shader, feature.define, enabled);
            setFeatures(changed);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s", feature.define.c_str(), feature.defaultOn ? " (on by default)" : "");
        ImGui::PopID();
    }
    for (const ShaderManifestKeywords& group : manifest->keywords) {
        ImGui::PushID(("keywords_" + group.group).c_str());
        prefabRowProperty = featuresProperty;
        propertyHeader(rowLabel(group.label, group.group));
        std::string current = group.values.empty() ? std::string() : group.values[0];
        for (const std::string& value : group.values) {
            if (ShaderLibraryValues::isFeatureEnabled(features, shader, group.group + "_" + value))
                current = value;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo(("##keywords_" + group.group).c_str(), current.c_str())) {
            for (const std::string& value : group.values) {
                if (ImGui::Selectable(value.c_str(), value == current) && value != current) {
                    std::vector<std::string> changed = features;
                    ShaderLibraryValues::setFeature(changed, shader, group.group + "_" + value, true);
                    setFeatures(changed);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    auto featureOn = [&](const std::string& feature) {
        return feature.empty() || ShaderLibraryValues::isFeatureEnabled(features, shader, feature);
    };

    // ---- parameters
    for (const ShaderManifestProperty& property : manifest->properties) {
        if (!featureOn(property.feature))
            continue;
        const bool hasValue = ShaderUniforms::has(params, property.member);
        const Vector4 value = hasValue ? ShaderUniforms::get(params, property.member) : property.defaultValue;

        ImGui::PushID(("param_" + property.member).c_str());
        prefabRowProperty = paramsProperty;
        if (propertyHeader(rowLabel(property.label, property.member), -1, hasValue)) {
            // back to the manifest default
            ShaderUniformValues changed = params;
            ShaderUniforms::remove(changed, property.member);
            setParams(changed, false);
            ImGui::PopID();
            continue;
        }
        const std::string id = "##param_value_" + property.member;
        Vector4 edited = value;
        bool changed = false;
        bool continuous = true;
        ImGui::SetNextItemWidth(-1);
        switch (property.kind) {
            case ShaderPropertyKind::COLOR:
            case ShaderPropertyKind::HDRCOLOR: {
                // edited as sRGB, stored linear (alpha as is)
                const bool hdr = property.kind == ShaderPropertyKind::HDRCOLOR;
                Vector4 srgb = Color::linearTosRGB(value);
                float color[4] = {srgb.x, srgb.y, srgb.z, srgb.w};
                ImGuiColorEditFlags flags = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf;
                if (hdr) flags |= ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float;
                if (ImGui::ColorEdit4(id.c_str(), color, flags)) {
                    const Vector4 linear = Color::sRGBToLinear(Vector4(color[0], color[1], color[2], color[3]));
                    edited = Vector4(linear.x, linear.y, linear.z, color[3]);
                    changed = true;
                }
                break;
            }
            case ShaderPropertyKind::RANGE: {
                float v = value.x;
                if (ImGui::SliderFloat(id.c_str(), &v, property.min, property.max)) {
                    edited = Vector4(v, 0.0f, 0.0f, 0.0f);
                    changed = true;
                }
                break;
            }
            case ShaderPropertyKind::TOGGLE: {
                bool on = value.x != 0.0f;
                if (ImGui::Checkbox(id.c_str(), &on)) {
                    edited = Vector4(on ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
                    changed = true;
                    continuous = false;
                }
                break;
            }
            case ShaderPropertyKind::INT: {
                int v = (int)value.x;
                if (ImGui::DragInt(id.c_str(), &v, 0.1f)) {
                    edited = Vector4((float)v, 0.0f, 0.0f, 0.0f);
                    changed = true;
                }
                break;
            }
            default: {
                const int components = property.kind == ShaderPropertyKind::VECTOR2 ? 2
                                     : property.kind == ShaderPropertyKind::VECTOR3 ? 3
                                     : property.kind == ShaderPropertyKind::VECTOR4 ? 4 : 1;
                float fields[4] = {value.x, value.y, value.z, value.w};
                if (ImGui::DragScalarN(id.c_str(), ImGuiDataType_Float, fields, components, 0.01f)) {
                    edited = Vector4(fields[0], fields[1], fields[2], fields[3]);
                    changed = true;
                }
                break;
            }
        }
        if (changed) {
            ShaderUniformValues newParams = params;
            ShaderUniforms::set(newParams, property.member, edited);
            setParams(newParams, continuous);
        }
        finishDrag();
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("%s", property.member.c_str());
        ImGui::PopID();
    }

    // ---- textures
    for (const ShaderManifestTexture& declared : manifest->textures) {
        if (!featureOn(declared.feature))
            continue;
        const std::string current = ShaderLibraryValues::getTexture(textures, declared.uniform);

        ImGui::PushID(("texture_" + declared.uniform).c_str());
        prefabRowProperty = texturesProperty;
        if (propertyHeader(rowLabel(declared.label, declared.uniform), -1, !current.empty())) {
            ShaderTextures changed = textures;
            ShaderLibraryValues::setTexture(changed, declared.uniform, "");
            setTextures(changed);
            ImGui::PopID();
            continue;
        }

        auto assign = [&](const std::string& absolutePath) {
            if (!project->isInsideAssetsPath(absolutePath)) {
                Backend::getApp().registerOutsideAssetsAlert(absolutePath);
                return;
            }
            ShaderTextures changed = textures;
            if (ShaderLibraryValues::setTexture(changed, declared.uniform, project->normalizeToAssetsRelative(absolutePath).generic_string()))
                setTextures(changed);
        };

        const float buttonWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const std::string shown = current.empty() ? textureDefaultLabel(declared.defaultTexture)
                                                  : std::filesystem::path(current).filename().string();
        ImGui::PushStyleColor(ImGuiCol_Text, current.empty() ? ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] : ImGui::GetStyle().Colors[ImGuiCol_Text]);
        ImGui::Button((shown + "##texture_slot_" + declared.uniform).c_str(),
                      ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x), 0));
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s%s%s\nDrop an image here", declared.uniform.c_str(), current.empty() ? "" : ": ", current.c_str());
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
                std::vector<std::string> received = Util::getStringsFromPayload(payload);
                if (!received.empty() && Util::isImageFile(received[0]))
                    assign(received[0]);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (ImGui::Button((std::string(ICON_FA_FOLDER_OPEN "##texture_pick_") + declared.uniform).c_str())) {
            const std::string selected = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
            if (!selected.empty())
                assign(selected);
        }
        ImGui::PopID();
    }
    prefabRowProperty.clear();

    return true;
}

bool editor::InspectorWindow::drawMaterialShaderRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                     unsigned int submesh, const RowSettings& settingsMaterial){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const std::string prefix = "submeshes[" + std::to_string(submesh) + "].material.";
    const std::string shaderProperty = prefix + "shader";
    std::string* shaderRef = Catalog::getPropertyRef<std::string>(scene, entities[0], cpType, shaderProperty);
    if (!shaderRef)
        return true;
    const std::string currentShader = *shaderRef;
    bool mixed = false;
    for (Entity entity : entities) {
        std::string* other = Catalog::getPropertyRef<std::string>(scene, entity, cpType, shaderProperty);
        mixed = mixed || !other || *other != currentShader;
    }

    // the same shader on every selected submesh, one undo step; a lighting mode the shader
    // the submesh then draws with does not list goes back to Auto (its combo lists only those)
    auto setShader = [&](const std::string& value) {
        auto* multi = new MultiPropertyCmd();
        for (Entity entity : entities) {
            multi->addPropertyCmd<std::string>(project, sceneId, entity, cpType, shaderProperty, value, settingsMaterial.onValueChanged);
            MeshComponent* mesh = scene->findComponent<MeshComponent>(entity);
            if (!mesh || submesh >= mesh->numSubmeshes)
                continue;
            Submesh drawn = mesh->submeshes[submesh];
            drawn.material.shader = value;
            const std::string shader = RenderSystem::getSubmeshShader(*mesh, drawn, scene->getDefaultCustomShader(ShaderType::MESH));
            std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
            if (!Catalog::supportsLighting(drawn.material.lighting, manifest.get()))
                multi->addPropertyCmd<int>(project, sceneId, entity, cpType, prefix + "lighting", (int)MaterialLighting::AUTO,
                                           settingsMaterial.onValueChanged);
        }
        CommandHandle::get(sceneId)->addCommandNoMerge(multi);
    };

    const std::string popupName = "Edit Shader Files##material_shader_" + std::to_string(submesh);
    prefabRowProperty = shaderProperty;
    propertyHeader("Shader", -1, false, true);
    if (mixed)
        ImGui::TextDisabled("(different shaders)");
    drawLibraryShaderCombo(mixed ? std::string() : currentShader, ShaderType::MESH, setShader, popupName);
    const Util::CustomShaderPaths paths = (!currentShader.empty() && !ShaderReference::isLibrary(currentShader))
        ? Util::resolveCustomShaderPaths(currentShader) : Util::CustomShaderPaths();
    drawShaderFilesPopup(popupName, setShader, paths.vert, paths.frag);
    prefabRowProperty.clear();

    if (mixed)
        return false;

    // the shader this submesh draws with (the mesh's or the scene's when the material has none)
    MeshComponent& mesh = scene->getComponent<MeshComponent>(entities[0]);
    const std::string drawnShader = RenderSystem::getSubmeshShader(mesh, mesh.submeshes[submesh], scene->getDefaultCustomShader(ShaderType::MESH));
    std::shared_ptr<const ShaderManifest> manifest = drawnShader.empty() ? nullptr : ShaderManifests::get(drawnShader);
    if (manifest && manifest->present) {
        if (entities.size() == 1) {
            drawShaderManifestRows(cpType, sceneProject, entities[0], drawnShader, prefix + "shaderParams", prefix + "shaderTextures",
                                   prefix + "shaderFeatures", settingsMaterial.onValueChanged);
        } else {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("Select one GameObject to edit the shader's values");
        }
    } else if (ShaderReference::isLibrary(drawnShader) && !manifest) {
        // a reference the engine does not have (renamed, mistyped): it draws with the built-in one
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s is not a library shader of this engine", drawnShader.c_str());
    }
    // a library shader has its own parameters; the built-in Lit and forks of it read the PBR ones
    return !ShaderReference::isLibrary(drawnShader);
}

void editor::InspectorWindow::drawMaterialSurfaceRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                      unsigned int submesh, const RowSettings& settingsMaterial){
    Scene* scene = sceneProject->scene;
    const std::string prefix = "submeshes[" + std::to_string(submesh) + "].material.";
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entities[0]);
    if (!mesh || submesh >= mesh->numSubmeshes)
        return;
    const Submesh& sub = mesh->submeshes[submesh];
    const std::string drawnShader = RenderSystem::getSubmeshShader(*mesh, sub, scene->getDefaultCustomShader(ShaderType::MESH));
    std::shared_ptr<const ShaderManifest> manifest = drawnShader.empty() ? nullptr : ShaderManifests::get(drawnShader);
    const ShaderManifest* stateManifest = (manifest && manifest->present) ? manifest.get() : nullptr;

    // what Auto resolves to now: the manifest's @state, else the engine's behaviour
    Material autoMaterial = sub.material;
    autoMaterial.blendMode = MaterialBlendMode::AUTO;
    autoMaterial.cullMode = MaterialCullMode::AUTO;
    autoMaterial.depthWrite = MaterialDepthWrite::AUTO;
    autoMaterial.depthTest = MaterialDepthTest::AUTO;
    Submesh autoSubmesh;
    autoSubmesh.material = autoMaterial;
    autoSubmesh.faceCulling = sub.faceCulling;
    bool blended = false;
    bool transparentQueue = false;
    const RenderState resolved = RenderSystem::resolveSubmeshRenderState(*mesh, autoSubmesh, stateManifest, blended, transparentQueue);

    auto withAuto = [](AutoEnumEntries& entries, const std::string& resolvedLabel, std::initializer_list<EnumEntry> others) {
        entries.autoLabel = "Auto (" + resolvedLabel + ")";
        entries.entries.push_back({0, entries.autoLabel.c_str()});
        for (const EnumEntry& entry : others) entries.entries.push_back(entry);
    };

    AutoEnumEntries blend, cull, depthWrite, depthTest, lighting;
    withAuto(blend, blended ? blendLabel(resolved.blend) : "Opaque", {
        {(int)MaterialBlendMode::NONE, "Opaque"}, {(int)MaterialBlendMode::ALPHA, "Alpha"},
        {(int)MaterialBlendMode::PREMULTIPLIED, "Premultiplied"}, {(int)MaterialBlendMode::ADDITIVE, "Additive"},
        {(int)MaterialBlendMode::MULTIPLY, "Multiply"}});
    withAuto(cull, !resolved.faceCulling ? "Off" : (resolved.cullingMode == CullingMode::FRONT ? "Front" : "Back"), {
        {(int)MaterialCullMode::BACK, "Back"}, {(int)MaterialCullMode::FRONT, "Front"}, {(int)MaterialCullMode::OFF, "Off"}});
    withAuto(depthWrite, resolved.depthWrite ? "On" : "Off", {
        {(int)MaterialDepthWrite::ENABLED, "On"}, {(int)MaterialDepthWrite::DISABLED, "Off"}});
    withAuto(depthTest, depthCompareLabel(resolved.depthCompare), {
        {(int)MaterialDepthTest::LESS, "Less"}, {(int)MaterialDepthTest::LESS_EQUAL, "Less Equal"},
        {(int)MaterialDepthTest::EQUAL, "Equal"}, {(int)MaterialDepthTest::GREATER_EQUAL, "Greater Equal"},
        {(int)MaterialDepthTest::GREATER, "Greater"}, {(int)MaterialDepthTest::NOT_EQUAL, "Not Equal"},
        {(int)MaterialDepthTest::ALWAYS, "Always"}});

    // only the lighting modes the shader lists (the built-in Lit: per pixel, per vertex,
    // mixed), its default first: what Auto draws with
    const std::vector<MaterialLighting> modes = Catalog::getShaderLightingModes(stateManifest);
    lighting.autoLabel = std::string("Auto (") + lightingLabel(modes.front()) + ")";
    lighting.entries.push_back({(int)MaterialLighting::AUTO, lighting.autoLabel.c_str()});
    for (MaterialLighting mode : modes)
        lighting.entries.push_back({(int)mode, lightingLabel(mode)});

    RowSettings settingsEnum = settingsMaterial;
    RowSettings settingsFloat = settingsMaterial;
    settingsFloat.stepSize = 0.01f;
    RowSettings settingsInt = settingsMaterial;

    beginTable(cpType, getLabelSize("Depth Offset Units"), "submeshes_material_surface");
    settingsEnum.enumEntries = &blend.entries;
    settingsEnum.help = "How the colour mixes with what is behind. Anything but Opaque draws in the transparent queue, sorted by Sort Priority, then far to near.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "blendMode", "Blend Mode", sceneProject, entities, settingsEnum);
    settingsEnum.enumEntries = &cull.entries;
    settingsEnum.help = "Which faces are left out. Auto follows the shader, else the submesh's Face Culling and the mesh's Culling Mode.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "cullMode", "Cull", sceneProject, entities, settingsEnum);
    settingsEnum.enumEntries = &depthWrite.entries;
    settingsEnum.help = "Whether it writes depth. Auto: on when opaque, off when blended.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "depthWrite", "Depth Write", sceneProject, entities, settingsEnum);
    settingsEnum.enumEntries = &depthTest.entries;
    settingsEnum.help = "The depth comparison that keeps a fragment. A camera with depth test off draws it always.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "depthTest", "Depth Test", sceneProject, entities, settingsEnum);
    settingsFloat.help = "Polygon offset slope factor; negative pulls the surface towards the camera (decals). 0 with Units 0 uses the shader's.";
    propertyRow(RowPropertyType::Float, cpType, prefix + "depthOffsetFactor", "Depth Offset Factor", sceneProject, entities, settingsFloat);
    settingsFloat.help = "Polygon offset constant, in depth units.";
    propertyRow(RowPropertyType::Float, cpType, prefix + "depthOffsetUnits", "Depth Offset Units", sceneProject, entities, settingsFloat);
    settingsInt.help = "Order among transparent objects: lower draws first.";
    propertyRow(RowPropertyType::Int, cpType, prefix + "sortPriority", "Sort Priority", sceneProject, entities, settingsInt);

    settingsEnum.enumEntries = &lighting.entries;
    settingsEnum.help = "Which stage evaluates the lights: Per Pixel (every light per pixel, specular and shadows), "
                        "Per Vertex (diffuse only, cheapest), Mixed (each light's Shading decides), None (unlit). "
                        "Only the modes this shader supports are listed; Auto is its default.";
    propertyRow(RowPropertyType::Enum, cpType, prefix + "lighting", "Lighting", sceneProject, entities, settingsEnum);

    // @state shadows=off (or a @state[...] rule of its features): the mesh's Cast Shadows does
    // not bring this submesh into the shadow maps
    if (stateManifest && stateManifest->resolveState(stateManifest->resolveFeatures(sub.material.shaderFeatures)).shadows == ShaderShadowCasting::OFF){
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("Casts no shadows (the shader's @state)");
    }
    endTable();
}

void editor::InspectorWindow::drawComponentStateRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities){
    if (!Catalog::findProperty(sceneProject->scene, entities[0], cpType, "blendMode").ref)
        return;

    // what Auto resolves to with the shader the component draws with (its @state, else the
    // component's legacy state: alpha blending, depth write on), as RenderSystem loads it
    const std::string shader = Catalog::getShaderReferenceForProperty(sceneProject->scene, entities[0], cpType, "customShader");
    std::shared_ptr<const ShaderManifest> manifest = shader.empty() ? nullptr : ShaderManifests::get(shader);
    // the component's features select the manifest's @state[<DEFINE>] rules
    static const std::vector<std::string> noFeatures;
    const std::vector<std::string>* features = Catalog::getPropertyRef<std::vector<std::string>>(sceneProject->scene, entities[0], cpType, "shaderFeatures");
    const RenderState resolved = RenderSystem::resolveComponentRenderState(MaterialBlendMode::AUTO, MaterialDepthWrite::AUTO,
        (manifest && manifest->present) ? manifest.get() : nullptr, features ? *features : noFeatures, false);

    AutoEnumEntries blendEntries, depthWriteEntries;
    blendEntries.autoLabel = std::string("Auto (") + blendLabel(resolved.blend) + ")";
    blendEntries.entries = {
        {(int)MaterialBlendMode::AUTO, blendEntries.autoLabel.c_str()}, {(int)MaterialBlendMode::NONE, "Opaque"},
        {(int)MaterialBlendMode::ALPHA, "Alpha"}, {(int)MaterialBlendMode::PREMULTIPLIED, "Premultiplied"},
        {(int)MaterialBlendMode::ADDITIVE, "Additive"}, {(int)MaterialBlendMode::MULTIPLY, "Multiply"}};
    depthWriteEntries.autoLabel = resolved.depthWrite ? "Auto (On)" : "Auto (Off)";
    depthWriteEntries.entries = {
        {(int)MaterialDepthWrite::AUTO, depthWriteEntries.autoLabel.c_str()}, {(int)MaterialDepthWrite::ENABLED, "On"},
        {(int)MaterialDepthWrite::DISABLED, "Off"}};

    RowSettings settings;
    settings.enumEntries = &blendEntries.entries;
    settings.help = "How the colour mixes with what is behind. Auto: the shader's @state, else alpha blending.";
    propertyRow(RowPropertyType::Enum, cpType, "blendMode", "Blend Mode", sceneProject, entities, settings);
    settings.enumEntries = &depthWriteEntries.entries;
    settings.help = "Whether it writes depth. Auto: the shader's @state, else on.";
    propertyRow(RowPropertyType::Enum, cpType, "depthWrite", "Depth Write", sceneProject, entities, settings);
}
