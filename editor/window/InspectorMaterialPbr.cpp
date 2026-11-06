// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Advanced Surface rows of a built-in Lit material (docs/specs/S11-materials-lighting.md):
// one collapsing header per lobe with its enabled checkbox in the header, the lobe's values
// and maps from MaterialPbr's field table, and a footer with the variant the material draws
// with, its extension map budget and the maps it leaves out. Every edit is a property command
// on the Catalog paths (submeshes[N].material.pbr.<lobe>.<field>) MCP uses too, named after
// the lobe ("Set Clear Coat Roughness").

#include "InspectorWindow.h"

#include "imgui_internal.h"

#include "Backend.h"
#include "DocumentFile.h"
#include "EditorHost.h"
#include "Out.h"
#include "Catalog.h"
#include "Project.h"
#include "Stream.h"
#include "Theme.h"
#include "Widgets.h"
#include "command/CommandHandle.h"
#include "command/UndoTimeline.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/WriteFilesCmd.h"
#include "external/IconsFontAwesome6.h"
#include "util/ProjectFileWrites.h"
#include "util/UIProbe.h"
#include "window/CodeEditor.h"
#include "pool/ShaderPool.h"
#include "shader/ShaderManifest.h"
#include "subsystem/RenderSystem.h"
#include "texture/MaterialPbr.h"
#include "util/Util.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace jasperengine;

namespace {

    const char* lobeHelp(PbrLobe lobe) {
        switch (lobe) {
            case PbrLobe::CLEARCOAT: return "A clear dielectric coat over the base (car paint, lacquer): its own reflection, with its roughness and an optional coat normal map (KHR_materials_clearcoat).";
            case PbrLobe::SHEEN: return "The soft rim light of cloth and velvet: a Charlie lobe tinted by its colour (KHR_materials_sheen).";
            case PbrLobe::ANISOTROPY: return "Highlights stretched along a direction of the surface (brushed metal, hair): Strength, Rotation from the tangent, an optional direction map. Needs tangents or UVs (KHR_materials_anisotropy).";
            case PbrLobe::IRIDESCENCE: return "A thin film's interference colours (soap bubbles, oil, beetles): the film's IOR and thickness range in nanometres (KHR_materials_iridescence). Left out of the Reduced mobile tier.";
            case PbrLobe::TRANSMISSION: return "Light through the surface (glass, water): it shows the opaque scene behind it, refracted, and draws after the opaque objects. Objects drawn after it do not show through (KHR_materials_transmission).";
            case PbrLobe::VOLUME: return "The volume behind a transmissive surface: its thickness bends the refraction, and light is absorbed toward the attenuation colour over the attenuation distance (KHR_materials_volume). Needs Transmission.";
            case PbrLobe::SPECULAR: return "The reflectance of the dielectric base: its strength and tint, and the index of refraction that sets it (1.5 is glTF's default, 1.33 water, 2.4 diamond) (KHR_materials_specular, KHR_materials_ior).";
            case PbrLobe::HAIR: return "Kajiya-Kay strands for hair cards: two highlights along the strands, shifted towards the root and the tip, with their colours and widths. The strands follow the mesh's tangent turned by Strand Rotation, or a Flow Map. Two specular lobes per light per pixel; use with Alpha Clip or Order Independent transparency.";
            case PbrLobe::HEX: return "The textures of UV set 0 sampled on three randomly rotated copies on a hex grid, which hides the repetition of a tiled texture (Mikkelsen 2022). Three taps per texture.";
            default: return "";
        }
    }

    const char* hairFieldHelp(const char* name) {
        const std::string field = name;
        if (field == "primaryColor") return "The light's own highlight along the strands (linear).";
        if (field == "primaryShift") return "Moves the primary highlight along the strands, towards the root (positive) or the tip (negative).";
        if (field == "primaryWidth") return "How wide the primary highlight spreads: narrow is glossy, wide is dull.";
        if (field == "secondaryColor") return "The second, coloured highlight light makes inside the strands (linear, tinted by the base colour).";
        if (field == "secondaryShift") return "Moves the secondary highlight along the strands, usually the other way from the primary.";
        if (field == "secondaryWidth") return "How wide the secondary highlight spreads; usually wider than the primary.";
        if (field == "rotation") return "Radians the strands turn from the mesh's tangent towards the bitangent (no Flow Map).";
        if (field == "flowMap") return "The strands' direction per texel in tangent space (RG, 0.5 is none) instead of the tangent.";
        return "";
    }

    // a fork of the built-in Lit keeps the lobes when it has the S11 hooks (its fragment stage
    // includes includes/pbr_ext_slots.glsl, the lobes' values ending its u_fs_pbrParams); checked once per file version
    bool forkHasPbrHooks(const std::filesystem::path& fragFile) {
        static std::map<std::string, std::pair<std::filesystem::file_time_type, bool>> cache;
        std::error_code ec;
        const std::filesystem::file_time_type time = std::filesystem::last_write_time(fragFile, ec);
        if (ec) return false;
        auto it = cache.find(fragFile.string());
        if (it != cache.end() && it->second.first == time) return it->second.second;
        std::ifstream file(fragFile, std::ios::binary);
        std::stringstream content;
        content << file.rdbuf();
        const std::string text = content.str();
        const bool hooks = text.find("pbr_ext_slots.glsl") != std::string::npos || text.find("u_fs_pbrExt") != std::string::npos;
        cache[fragFile.string()] = {time, hooks};
        return hooks;
    }

}

namespace {

    // one project undo step, through what update_material writes
    bool writeMaterialStep(editor::Project* project, const std::filesystem::path& file, const Material& material) {
        std::vector<editor::WriteFilesCmd::Item> items = {{file, project->materialFileText(file, material)}};
        auto failure = std::make_shared<std::string>();
        auto* command = new editor::WriteFilesCmd(std::move(items), [project](const std::vector<std::filesystem::path>& changed) {
            editor::ProjectFileWrites::filesWritten(project, changed);
        });
        command->setName("Edit Material " + file.stem().string());
        command->reportFailureTo(failure);
        if (project->getProjectCommandHistory()->addCommandNoMerge(command)) return true;
        editor::Out::error("Writing the material failed: %s", failure->c_str());
        return false;
    }

}

editor::InspectorWindow::MaterialAssetView::~MaterialAssetView(){
    UndoTimeline::setAssetEditHistory(nullptr);
    project->setMaterialAssetScene(nullptr);
    CommandHandle::remove(host.id);
    delete host.scene;
}

bool editor::InspectorWindow::showsProjectAsset() const{
    return std::max(materialAssetFrame, particleAssetFrame) >= ImGui::GetFrameCount() - 1;
}

void editor::InspectorWindow::resetMaterialAsset(){
    if (materialAsset && !materialAsset->path.empty()) leaveMaterialAsset();
    materialAsset.reset();
}

void editor::InspectorWindow::leaveMaterialAsset(){
    MaterialAssetView& view = *materialAsset;
    if (view.host.scene->getComponent<MeshComponent>(view.entity).submeshes[0].material != view.inFile) writeMaterialAsset();
    CommandHandle::get(view.host.id)->clear();
    view.path.clear();
}

void editor::InspectorWindow::readMaterialAsset(const std::filesystem::path& asset){
    MaterialAssetView& view = *materialAsset;
    std::error_code ec;
    view.path = asset;
    view.time = std::filesystem::last_write_time(asset, ec);
    view.size = std::filesystem::file_size(asset, ec);
    view.error.clear();
    try {
        Material material = Stream::decodeMaterial(DocumentFiles::read(asset, DocumentKind::Material).root);
        material.name = project->normalizeToProjectRelative(asset).generic_string();
        view.host.scene->getComponent<MeshComponent>(view.entity).submeshes[0].material = material;
        view.inFile = material;
        view.inLinked = material;
    } catch (const std::exception& e) {
        view.error = e.what();
    }
}

void editor::InspectorWindow::writeMaterialAsset(){
    MaterialAssetView& view = *materialAsset;
    Material& edited = view.host.scene->getComponent<MeshComponent>(view.entity).submeshes[0].material;
    const Material material = edited;
    // what cannot be written shows as the file has it, and the linked submeshes go back to that
    auto keepFile = [this, &view, &edited]() {
        edited = view.inFile;
        project->applyLinkedMaterialFile(view.inFile.name, view.inFile);
        view.inLinked = view.inFile;
    };
    // the file changed or went while this edit was held: what it has now wins, and the edit says so
    {
        std::error_code timeError, sizeError;
        const std::filesystem::file_time_type time = std::filesystem::last_write_time(view.path, timeError);
        const std::uintmax_t size = std::filesystem::file_size(view.path, sizeError);
        if (timeError || sizeError || time != view.time || size != view.size) {
            editor::Out::warning("%s changed on disk during this edit: the edit was not written", view.path.filename().string().c_str());
            keepFile();
            return;
        }
    }
    // a file that did not read completely: the editor's lossy-save modal decides (S03)
    const LoadReport report = DocumentFiles::materialFileReport(view.path);
    if (report.dropped() > 0) {
        keepFile();
        DegradedSaveRequest request;
        request.documentName = view.path.filename().string();
        request.path = view.inFile.name;
        for (const LoadIssue& issue : report.issues) {
            if (issue.fate == LoadIssueFate::Dropped) request.dropped.push_back(issue);
        }
        Project* owner = project;
        const std::filesystem::path file = view.path;
        request.saveAnyway = [owner, file, material]() { writeMaterialStep(owner, file, material); };
        editor::getEditorHost().confirmDegradedSave(request);
        return;
    }
    if (writeMaterialStep(project, view.path, material)) {
        view.inFile = material;
        std::error_code ec;
        view.time = std::filesystem::last_write_time(view.path, ec);
        view.size = std::filesystem::file_size(view.path, ec);
    } else {
        keepFile();
    }
}

void editor::InspectorWindow::drawMaterialAsset(const std::filesystem::path& asset){
    drawAssetHeader(ICON_FA_PALETTE, asset, "Material");
    ImGui::Separator();
    materialAssetFrame = ImGui::GetFrameCount();

    if (!materialAsset) {
        materialAsset = std::make_unique<MaterialAssetView>();
        MaterialAssetView& created = *materialAsset;
        created.project = project;
        created.host.id = project->allocateSceneId();
        created.host.name = "Material";
        created.host.sceneType = SceneType::SCENE_3D;
        created.host.scene = new Scene(EntityPool::System);
        created.entity = created.host.scene->createSystemEntity();
        created.host.scene->addComponent<MeshComponent>(created.entity, {});
        created.host.scene->getComponent<MeshComponent>(created.entity).numSubmeshes = 1;
        created.host.entities.push_back(created.entity);
        project->setMaterialAssetScene(&created.host);
        UndoTimeline::setAssetEditHistory(CommandHandle::get(created.host.id));
    }
    MaterialAssetView& view = *materialAsset;
    auto edited = [&view]() -> Material& { return view.host.scene->getComponent<MeshComponent>(view.entity).submeshes[0].material; };

    // a selection change ends an edit: it goes to the file it was made on
    // (a Color window's session stays open across the items it activates: it is one gesture)
    CommandHistory* rowHistory = CommandHandle::get(view.host.id);
    auto gestureOpen = [rowHistory]() { return ImGui::IsAnyItemActive() || rowHistory->hasOpenInteractive(); };
    const bool pending = !view.path.empty() && edited() != view.inFile;
    if (view.path != asset) {
        if (!view.path.empty()) leaveMaterialAsset();
        readMaterialAsset(asset);
    } else if (!pending && !gestureOpen()) {
        // an undo, an agent or another program changed the file: the rows follow it
        std::error_code ec;
        const std::filesystem::file_time_type time = std::filesystem::last_write_time(asset, ec);
        if (time != view.time || std::filesystem::file_size(asset, ec) != view.size) readMaterialAsset(asset);
    }
    if (!view.error.empty()) {
        ImGui::TextColored(Theme::Colors::WarningText, "Cannot read this material: %s", view.error.c_str());
    } else {
        const float previewSize = ImGui::GetFrameHeight() * 6;
        Texture preview = getMaterialPreview(edited(), "asset:" + asset.generic_string(), true, std::string());
        Widgets::image(Backend::getImGuiTexture(preview.getRender()), ImVec2(previewSize, previewSize));
        UIProbe::item("asset.material.preview");
    }
    if (ImGui::Button(ICON_FA_FILE_CODE " Open as Text")) {
        Backend::getApp().getCodeEditor()->openFile(asset.string(), true);
    }
    if (!view.error.empty()) return;

    const bool locked = drawVcsLockBanner({asset});
    if (locked) ImGui::BeginDisabled(true);
    {
        // their commands only keep the rows' own state: the undo step is the file's
        CommandHandle::Redirect rowCommands(project->getSelectedSceneId(), view.host.id);
        drawSubmeshMaterialRows(ComponentType::MeshComponent, &view.host, {view.entity}, 0, nullptr);
    }
    if (locked) ImGui::EndDisabled();

    // as Unity's: what uses it follows while the value is dragged; the file takes it at the release
    if (edited() != view.inLinked) {
        if (view.inLinked == view.inFile) {
            view.scenesModified.clear();
            for (const SceneProject* scene : project->getScenesAndPrefabStages()) view.scenesModified[scene->id] = scene->isModified;
        }
        project->applyLinkedMaterialFile(view.inFile.name, edited());
        view.inLinked = edited();
        // back to what the file has (Esc, a drag back): nothing changed, so no scene did (docs/undo.md)
        if (view.inLinked == view.inFile) {
            for (const auto& [sceneId, modified] : view.scenesModified) {
                if (SceneProject* scene = project->getScene(sceneId)) scene->isModified = modified;
            }
        }
    }
    if (!gestureOpen() && edited() != view.inFile) writeMaterialAsset();
}

// The value and map rows of one lobe (its enabled flag is its header's, or the Shading Model's)
void editor::InspectorWindow::drawMaterialPbrLobeRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                      const std::string& prefix, PbrLobe lobe, const RowSettings& settingsMaterial){
    for (const PbrField& field : MaterialPbr::getFields()){
        if (field.lobe != lobe || field.type == PbrFieldType::Bool)
            continue;
        const std::string path = prefix + MaterialPbr::getFieldPath(field);
        RowSettings settings = settingsMaterial;
        settings.stepSize = 0.01f;
        if (lobe == PbrLobe::HAIR)
            settings.help = hairFieldHelp(field.name);
        switch (field.type){
            case PbrFieldType::Float:{
                RowPropertyType type = RowPropertyType::Float;
                if (field.minValue == 0.0f && field.maxValue == 1.0f)
                    type = RowPropertyType::Float_0_1;
                else if (field.minValue >= 0.0f)
                    type = RowPropertyType::FloatPositive;
                if (std::string(field.name) == "ior")
                    settings.help = "1.5: glTF's default; 1.0 air, 1.33 water, 2.4 diamond. 0: no dielectric falloff (a full mirror).";
                if (std::string(field.name) == "rotation" && lobe == PbrLobe::ANISOTROPY)
                    settings.help = "Radians from the tangent toward the bitangent.";
                if (std::string(field.name) == "attenuationDistance")
                    settings.help = "World units over which light falls to the attenuation colour; 0 or empty: no absorption.";
                propertyRow(type, cpType, path, field.label, sceneProject, entities, settings);
                break;
            }
            case PbrFieldType::Vector2:
                settings.stepSize = 1.0f;
                settings.format = "%.0f";
                propertyRow(RowPropertyType::Vector2, cpType, path, field.label, sceneProject, entities, settings);
                break;
            case PbrFieldType::Color:
                propertyRow(RowPropertyType::Color3L, cpType, path, field.label, sceneProject, entities, settings);
                break;
            case PbrFieldType::Texture:
                propertyRow(RowPropertyType::Texture, cpType, path, field.label, sceneProject, entities, settings);
                break;
            case PbrFieldType::TexCoord:
                settings.intMin = 0;
                settings.intMax = 1;
                settings.help = "The UV set the map samples: 0 the first, 1 the second.";
                propertyRow(RowPropertyType::Int, cpType, path, field.label, sceneProject, entities, settings);
                break;
            default:
                break;
        }
    }
}

void editor::InspectorWindow::drawMaterialShadingModelRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                           unsigned int submesh, const RowSettings& settingsMaterial){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    const std::string prefix = "submeshes[" + std::to_string(submesh) + "].material.pbr.";
    const std::string enabledPath = prefix + "hair.enabled";

    // the hair lobe's flag across the selection (mixed shows as a dash); a Terrain draws its layers
    bool hair = false;
    bool mixed = false;
    bool first = true;
    bool terrain = false;
    for (Entity entity : entities){
        terrain = terrain || scene->findComponent<TerrainComponent>(entity) != nullptr;
        bool* value = Catalog::getPropertyRef<bool>(scene, entity, cpType, enabledPath);
        if (!value) continue;
        if (first) hair = *value;
        else if (*value != hair) mixed = true;
        first = false;
    }
    if (first || terrain)
        return;

    const char* const modelNames[] = {"Standard", "Hair"};
    auto setModel = [&](ShadingModel model){
        auto* multi = new MultiPropertyCmd();
        for (Entity entity : entities)
            multi->addPropertyCmd<bool>(project, sceneId, entity, cpType, enabledPath, model == ShadingModel::HAIR, settingsMaterial.onValueChanged);
        CommandHandle::get(sceneId)->addCommandNoMerge(multi);
    };

    const ShadingModel current = hair ? ShadingModel::HAIR : ShadingModel::STANDARD;
    if (propertyHeader("Shading Model", -1, !mixed && current != ShadingModel::STANDARD, settingsMaterial.child))
        setModel(ShadingModel::STANDARD);
    ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("(?)").x + ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::BeginCombo("##shading_model", mixed ? "—" : modelNames[(int)current])){
        for (int m = 0; m < 2; m++){
            if (ImGui::Selectable(modelNames[m], !mixed && (int)current == m) && (mixed || (int)current != m))
                setModel((ShadingModel)m);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    helpMarker(std::string("Standard: the metallic-roughness surface (and its Advanced Surface lobes). Hair: ") + lobeHelp(PbrLobe::HAIR));

    if (hair && !mixed){
        RowSettings settings = settingsMaterial;
        settings.indent = true;
        drawMaterialPbrLobeRows(cpType, sceneProject, entities, prefix, PbrLobe::HAIR, settings);
    }
}

void editor::InspectorWindow::drawMaterialAdvancedSurfaceRows(ComponentType cpType, SceneProject* sceneProject, const std::vector<Entity>& entities,
                                                              unsigned int submesh, const RowSettings& settingsMaterial){
    Scene* scene = sceneProject->scene;
    const uint32_t sceneId = sceneProject->id;
    MeshComponent* mesh = scene->findComponent<MeshComponent>(entities[0]);
    if (!mesh || submesh >= mesh->numSubmeshes)
        return;
    const Submesh& sub = mesh->submeshes[submesh];
    const std::string prefix = "submeshes[" + std::to_string(submesh) + "].material.pbr.";

    ImGui::SeparatorText("Advanced Surface");

    // what the lobes cannot apply to: a Terrain (its layers are its surface) and a fork of the
    // built-in Lit written before them
    bool terrain = false;
    for (Entity entity : entities)
        terrain = terrain || scene->findComponent<TerrainComponent>(entity) != nullptr;
    const std::string drawnShader = RenderSystem::getSubmeshShader(*mesh, sub, scene->getDefaultCustomShader(ShaderType::MESH));
    bool forkWithoutHooks = false;
    if (!drawnShader.empty() && !ShaderReference::isLibrary(drawnShader)){
        const Util::CustomShaderPaths paths = project->resolveCustomShaderFiles(drawnShader);
        const std::filesystem::path frag = paths.frag.empty() ? std::filesystem::path()
            : Project::findShaderFile(project->getProjectPath(), project->getAssetsPath(), paths.frag);
        forkWithoutHooks = frag.empty() || !forkHasPbrHooks(frag);
    }
    if (forkWithoutHooks)
        ImGui::TextDisabled("This shader does not implement advanced surface");

    ImGui::BeginDisabled(terrain);
    for (const PbrLobeInfo& lobe : MaterialPbr::getLobes()){
        // Hair is a shading model, not a layer: the Surface section's Shading Model row picks it
        if (lobe.lobe == PbrLobe::HAIR)
            continue;
        const std::string enabledPath = prefix + lobe.name + ".enabled";
        // the enabled flag across the selection (mixed shows as a dash)
        bool enabled = false;
        bool mixed = false;
        bool first = true;
        for (Entity entity : entities){
            bool* value = Catalog::getPropertyRef<bool>(scene, entity, cpType, enabledPath);
            if (!value) continue;
            if (first) enabled = *value;
            else if (*value != enabled) mixed = true;
            first = false;
        }
        if (first) continue;

        // Volume refines Transmission and does nothing without it
        bool transmission = false;
        if (lobe.lobe == PbrLobe::VOLUME){
            bool* value = Catalog::getPropertyRef<bool>(scene, entities[0], cpType, prefix + "transmission.enabled");
            transmission = value && *value;
        }
        const bool lobeDisabled = lobe.lobe == PbrLobe::VOLUME && !transmission;

        ImGui::PushID(lobe.name);
        ImGui::BeginDisabled(lobeDisabled);
        const bool open = ImGui::TreeNodeEx("##lobe", ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
        ImGui::SameLine();
        if (mixed)
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
        bool checked = enabled;
        if (ImGui::Checkbox(lobe.label, &checked)){
            auto* multi = new MultiPropertyCmd();
            for (Entity entity : entities)
                multi->addPropertyCmd<bool>(project, sceneId, entity, cpType, enabledPath, checked, settingsMaterial.onValueChanged);
            CommandHandle::get(sceneId)->addCommandNoMerge(multi);
        }
        if (mixed)
            ImGui::PopItemFlag();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", lobeDisabled ? "Needs Transmission" : lobeHelp(lobe.lobe));
        ImGui::EndDisabled();

        if (open){
            beginTable(cpType, getLabelSize("Attenuation Distance"), (std::string("submeshes_material_pbr_") + lobe.name).c_str());
            ImGui::BeginDisabled(lobeDisabled || (!enabled && !mixed));
            drawMaterialPbrLobeRows(cpType, sceneProject, entities, prefix, lobe.lobe, settingsMaterial);
            ImGui::EndDisabled();
            endTable();
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (terrain && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("A Terrain draws its painted layers: their Hex Tiling is in the Terrain's layers");

    // the variant: its basename, built or still building, the map budget and what is left out
    if (entities.size() != 1 || terrain)
        return;
    const PbrVariant variant = RenderSystem::getLoadedPbrVariant(sub, MaterialPbr::getRuntimeTier());
    const std::vector<std::string> loaded = MaterialPbr::getDefinesFromBits(sub.litDefineBits);
    ImGui::Spacing();
    if (variant.defines.empty() && loaded.empty()){
        ImGui::TextDisabled("Variant: builtin:lit (no extended lobes)");
    }else{
        const std::vector<std::string>& shown = variant.defines.empty() ? loaded : variant.defines;
        // the variant asked for at the last load: built (drawn), still building (the plain
        // variant stands in, RenderSystem::loadMesh), or failed
        const std::string reference = RenderSystem::getSubmeshShaderReference(*mesh, sub, scene->getDefaultCustomShader(ShaderType::MESH), loaded);
        const bool failed = sub.customShaderId == 0 && !loaded.empty() &&
            ShaderPool::isShaderBuildFailed(ShaderType::MESH, sub.shaderProperties, ShaderPool::registerCustomShader(reference));
        const bool building = !mesh->loaded || mesh->needReload || mesh->needVariantReload || (!loaded.empty() && sub.customShaderId == 0);
        const std::string status = failed ? "Failed (drawn with the plain variant, see the Console)" : building ? "Compiling..." : "Ready";
        ImGui::TextDisabled("Variant: %s, %s", MaterialPbr::getVariantBasename(shown).c_str(), status.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", ShaderReference::withFeatures(ShaderReference::BUILTIN_LIT, shown).c_str());
    }
    const int budget = MaterialPbr::MAX_EXTENSION_MAPS - (((sub.shaderProperties & (1u << 23)) != 0) ? 1 : 0);
    ImGui::TextDisabled("Extension maps %d / %d", (int)variant.maps.size(), budget);
    if (!variant.droppedMaps.empty()){
        std::string dropped;
        for (const std::string& map : variant.droppedMaps)
            dropped += (dropped.empty() ? "" : ", ") + map;
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Over the map budget, drawn with their factors: %s", dropped.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("A variant binds at most %d extension maps (16 fragment textures on mobile and web). They are kept by priority: clear coat, specular, transmission, sheen, anisotropy, iridescence, clear coat normal.", MaterialPbr::MAX_EXTENSION_MAPS);
    }
    // what the mobile and web exports leave out (Project Settings > Graphics > Mobile PBR Tier)
    const PbrVariant mobile = RenderSystem::getLoadedPbrVariant(sub, MaterialPbr::resolveTier(true, project->getMobilePbrTier()));
    if (!mobile.strippedMaps.empty()){
        std::string stripped;
        for (const std::string& map : mobile.strippedMaps)
            stripped += (stripped.empty() ? "" : ", ") + map;
        ImGui::TextDisabled("Mobile and web (Reduced tier) leave out: %s", stripped.c_str());
    }
}
