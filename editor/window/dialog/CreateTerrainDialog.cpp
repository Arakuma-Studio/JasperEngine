// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CreateTerrainDialog.h"
#include "component/RigidbodyComponent.h"
#include "component/TerrainComponent.h"

#include "EditorHost.h"
#include "Out.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/TerrainTextureEditCmd.h"
#include "graph/Graph.h"
#include "jobs/JobSystem.h"
#include "noise/TerrainGeneration.h"
#include "noise/TerrainPresets.h"
#include "physics/JoltShapeBuilder.h"
#include "terrain/RawMapStore.h"
#include "terrain/TerrainOpsUndo.h"
#include "terrain/TerrainSizePresets.h"
#include "util/FileDialogs.h"
#include "util/TerrainMapUtils.h"
#include "window/EditorModal.h"
#include "window/HeightImportFields.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

editor::CreateTerrainDialog& editor::CreateTerrainDialog::instance(){
    static CreateTerrainDialog dialog;
    return dialog;
}

void editor::CreateTerrainDialog::applyPreset(int index){
    if (index < 0 || index >= static_cast<int>(std::size(TERRAIN_SIZE_PRESETS))) return;
    const TerrainSizePreset& p = TERRAIN_SIZE_PRESETS[index];
    settings.preset = index;
    settings.size = p.size;
    settings.height = p.height;
    settings.heightmap = p.heightmap;
    settings.control = p.control;
    settings.detail = p.detail;
    settings.meshResolution = p.meshResolution;
    settings.rootGrid = p.root;
    settings.levels = p.levels;
}

// T2 R61: a preset's recommended size and height become the dialog's
void editor::CreateTerrainDialog::applyNoisePreset(const std::string& id){
    settings.noisePreset = id;
    TerrainPreset preset;
    if (id.empty() || !TerrainPresets::find(id, preset)) return;
    settings.size = preset.terrainSize;
    settings.height = preset.maxHeight;
}

bool editor::CreateTerrainDialog::createGenerated(Project* project, uint32_t sceneId, Entity parent, const Settings& settings,
                                                  std::function<void(Entity, const GenerationResult&)> done, std::string& error){
    TerrainPreset preset;
    if (!TerrainPresets::find(settings.noisePreset, preset)){
        error = "No Noise Graph preset '" + settings.noisePreset + "'.";
        return false;
    }
    graph::LoadReport report;
    GenerationRequest request;
    request.graph = graph::load(TerrainPresets::json(settings.noisePreset), report);
    if (!report.ok){
        error = report.error;
        return false;
    }
    request.displayName = preset.name;
    request.seed = settings.seed;
    request.terrainSize = settings.size;
    // a new terrain has no surface or foliage layers to paint
    request.applyTo.layers = false;
    request.applyTo.density = false;
    request.layerCount = 0;
    // the new height map, as Create will make it: generated first, so Create's one step holds the result
    const terrain::MapId height{terrain::MapKind::Height, 0};
    auto store = std::make_shared<terrain::RawMapStore>(settings.size, settings.height);
    store->addMap(height, settings.heightmap, settings.heightmap, 1, 16);
    auto result = std::make_shared<GenerationResult>();
    std::shared_ptr<TerrainGeneration::Prepared> prepared = TerrainGeneration::prepare(*store, request, *result);
    if (!prepared){
        error = result->error;
        return false;
    }
    const std::string stepName = "Create Terrain (" + preset.name + ")";
    jobs::SubmitOptions options;
    options.lane = jobs::Lane::Background;
    options.gate = jobs::GateClass::Heavy;
    options.name = "create-terrain";
    jobs::submit([project, sceneId, parent, settings, done, store, result, prepared, stepName, height](){
        std::string failure;
        try{
            TerrainGeneration::evaluate(*prepared, nullptr, true, *result);
        }catch (const std::exception& e){
            failure = std::string("The generation failed: ") + e.what();
        }
        getEditorHost().enqueueMainThreadTask([project, sceneId, parent, settings, done, store, result, prepared, stepName, height, failure](){
            std::string why = failure;
            if (why.empty() && !TerrainGeneration::apply(*prepared, *store, *result)){
                why = result->error.empty() ? "The heights could not be written." : result->error;
            }
            Entity entity = NULL_ENTITY;
            if (why.empty()){
                const uint8_t* bytes = store->mapBytes(height);
                const std::vector<uint8_t> heights(bytes, bytes + store->mapByteCount(height));
                entity = create(project, sceneId, parent, settings, why, &heights, stepName);
            }
            if (!why.empty()){
                result->ok = false;
                if (result->error.empty()) result->error = why;
            }
            if (done) done(entity, *result);
        });
    }, options);
    return true;
}

// R58: Start Shape > Import Raw... picks a file and decodes it with R61's detection
void editor::CreateTerrainDialog::chooseImportFile(){
    const std::string chosen = FileDialogs::openFileDialogFiltered(project->getAssetsPath().string(), "Heightmaps", "raw,r16,r8,r32,f32,png,exr");
    if (chosen.empty()) return;
    std::ifstream file(std::filesystem::u8path(chosen), std::ios::binary);
    importBytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    importPath = chosen;
    importOptions = HeightImportOptions();
    settings.imported = TerrainRawIO::decode(chosen, importBytes, importOptions);
    settings.shape = StartShape::ImportRaw;
    useImportedSize();
}

void editor::CreateTerrainDialog::useImportedSize(){
    if (settings.imported.ok && settings.imported.width == settings.imported.height) settings.heightmap = settings.imported.width;
}

void editor::CreateTerrainDialog::open(Project* project, uint32_t sceneId, Entity parent){
    this->project = project;
    this->sceneId = sceneId;
    this->parent = parent;
    settings = Settings();
    importPath.clear();
    importBytes.clear();
    importOptions = HeightImportOptions();
    importSetsHeight = false;
    applyPreset(settings.preset);
    error.clear();
    isOpen = true;
    justOpened = true;
}

std::string editor::CreateTerrainDialog::memoryLine(const Settings& s){
    // the height map is R16 on the CPU and the GPU; blend maps RGBA8, made on first paint
    const double height = static_cast<double>(s.heightmap) * s.heightmap * 2.0;
    const double control = static_cast<double>(s.control) * s.control * 4.0;
    char text[256];
    std::snprintf(text, sizeof(text), "Height map %.1f MB on the CPU and the GPU; each painted blend map %.1f MB.",
                  height / (1024.0 * 1024.0), control / (1024.0 * 1024.0));
    std::string line = text;
    if (s.heightmap > 2049) line += " Over 2049: too big for mobile and web.";
    if (s.heightmap > static_cast<int>(JoltShapeBuilder::MAX_HEIGHTFIELD_SAMPLES)) {
        line += " Collision samples " + std::to_string(JoltShapeBuilder::MAX_HEIGHTFIELD_SAMPLES) + ", not one per texel.";
    }
    return line;
}

Entity editor::CreateTerrainDialog::create(Project* project, uint32_t sceneId, Entity parent, const Settings& s, std::string& error,
                                           const std::vector<uint8_t>* heights, const std::string& stepName){
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene){
        error = "The scene is not open.";
        return NULL_ENTITY;
    }
    if (s.size <= 0.0f || s.height <= 0.0f || s.heightmap < 2){
        error = "Terrain Size and Terrain Height must be above 0, and the height map at least 2 texels.";
        return NULL_ENTITY;
    }
    // R58: Import Raw... starts from the file's heights, which give the height map its size
    const bool importing = !heights && s.shape == StartShape::ImportRaw;
    if (importing && !s.imported.ok){
        error = s.imported.error.empty() ? "Import Raw...: choose a heightmap file first." : "Import Raw...: " + s.imported.error;
        return NULL_ENTITY;
    }
    if (importing && (s.imported.width != s.imported.height || s.imported.width != s.heightmap ||
                      s.imported.normalized.size() != static_cast<size_t>(s.heightmap) * s.heightmap)){
        error = "Import Raw... makes a square height map; this file is " + std::to_string(s.imported.width) + " x " +
                std::to_string(s.imported.height) + ". Import it with the Terrain window's Settings tool (Import Raw...) instead.";
        return NULL_ENTITY;
    }
    CommandHistory* history = CommandHandle::get(sceneId);
    const size_t start = history->mark();
    auto fail = [&](const std::string& why){
        while (history->position() > start && history->canUndo() && history->undo()){}
        error = why;
        return NULL_ENTITY;
    };

    CreateEntityCmd* createCmd = new CreateEntityCmd(project, sceneId, s.name.empty() ? std::string("Terrain") : s.name,
                                                     EntityCreationType::TERRAIN, parent);
    if (!history->addCommandNoMerge(createCmd)) return fail("The terrain could not be made.");
    const Entity entity = createCmd->getEntity();

    auto set = [&](const char* property, auto value){
        using T = decltype(value);
        return history->addCommandNoMerge(new PropertyCmd<T>(project, sceneId, entity, ComponentType::TerrainComponent, property, value));
    };
    if (!set("terrainSize", s.size) || !set("maxHeight", s.height) || !set("resolution", s.meshResolution) ||
        !set("rootGridSize", s.rootGrid) || !set("levels", s.levels)){
        return fail("The terrain's sizes could not be set.");
    }

    // the height map: flat at Base Height, or at the middle of the range
    TerrainComponent& terrain = sceneProject->scene->getComponent<TerrainComponent>(entity);
    const TerrainMapRef ref(TerrainMapTarget::HeightMap);
    Texture* texture = TerrainMapUtils::findTexture(terrain, ref);
    if (!texture) return fail("The height map could not be made.");
    TerrainMapSnapshot before = TerrainMapUtils::captureSnapshot(project, *texture, true);
    TerrainMapSnapshot after;
    after.empty = false;
    after.path = TerrainMapUtils::makeEditableTexturePath(project, sceneId, entity, ref);
    after.id = after.path;
    after.colorFormat = ColorFormat::RED16;
    after.channels = 1;
    after.width = s.heightmap;
    after.height = s.heightmap;
    after.pixels.resize(static_cast<size_t>(s.heightmap) * s.heightmap * 2);
    if (heights && heights->size() == after.pixels.size()){
        std::copy(heights->begin(), heights->end(), after.pixels.begin());
    }else if (importing){
        for (size_t i = 0; i < s.imported.normalized.size(); i++){
            const uint16_t value = static_cast<uint16_t>(std::lround(std::clamp(s.imported.normalized[i], 0.0f, 1.0f) * 65535.0f));
            after.pixels[i * 2] = static_cast<uint8_t>(value & 0xFF);
            after.pixels[i * 2 + 1] = static_cast<uint8_t>(value >> 8);
        }
    }else{
        const float normalized = s.shape == StartShape::FlatAtMiddle ? 0.5f : std::clamp(s.baseHeight / s.height, 0.0f, 1.0f);
        const uint16_t value = static_cast<uint16_t>(std::lround(normalized * 65535.0f));
        for (size_t i = 0; i < after.pixels.size(); i += 2){
            after.pixels[i] = static_cast<uint8_t>(value & 0xFF);
            after.pixels[i + 1] = static_cast<uint8_t>(value >> 8);
        }
    }
    if (!history->addCommandNoMerge(new TerrainTextureEditCmd(project, sceneId, entity, ref, std::move(before), std::move(after), "Create Terrain"))){
        return fail("The height map could not be made.");
    }
    // R58: Base Layer is Terrain Layer 0 over the whole terrain, as a Unity splat map starts filled with
    // its first layer: the layer, and the first blend map at full weight in its red channel
    if (!s.baseLayer.empty()){
        TerrainSurfaceLayer layer;
        layer.colorTexture = Texture(s.baseLayer);
        if (!history->addCommandNoMerge(new PropertyCmd<std::vector<TerrainSurfaceLayer>>(project, sceneId, entity, ComponentType::TerrainComponent,
                                                                                         "surfaceLayers", std::vector<TerrainSurfaceLayer>{layer}))){
            return fail("The Base Layer could not be added.");
        }
        terrain::MapFormat blendFormat;
        blendFormat.bounds = terrain::TexelRect{0, 0, s.control, s.control};
        blendFormat.channels = 4;
        blendFormat.bitsPerChannel = 8;
        blendFormat.exists = true;
        TerrainMadeMap made;
        if (!makeTerrainBlankMap(project, sceneProject, entity, terrain::MapId{terrain::MapKind::Blend, 0}, blendFormat, made)){
            return fail("The Base Layer's blend map could not be made.");
        }
        TerrainMapSnapshot filled = made.blank;
        for (size_t i = 0; i < filled.pixels.size(); i += 4) filled.pixels[i] = 255;
        if (!history->addCommandNoMerge(new TerrainTextureEditCmd(project, sceneId, entity, made.ref, made.before, std::move(filled), "Create Terrain"))){
            return fail("The Base Layer's blend map could not be made.");
        }
    }

    if (s.collider){
        Collider3D shape;
        shape.type = Collider3DType::HEIGHTFIELD;
        shape.source = Collider3DSource::ENTITY_HEIGHTFIELD;
        shape.sourceEntity = entity;
        shape.samplesSize = static_cast<unsigned int>(std::min(s.heightmap, static_cast<int>(JoltShapeBuilder::MAX_HEIGHTFIELD_SAMPLES)));
        if (!history->addCommandNoMerge(new AddComponentCmd(project, sceneId, entity, ComponentType::RigidbodyComponent)) ||
            !history->addCommandNoMerge(new PropertyCmd<std::vector<Collider3D>>(project, sceneId, entity, ComponentType::RigidbodyComponent,
                                                                                   "colliders", std::vector<Collider3D>{shape}))){
            return fail("The terrain collider could not be added.");
        }
    }

    TerrainEditorSettings& ts = project->getTerrainEditorSettings();
    ts.heightMapResolution = s.heightmap;
    ts.blendMapResolution = s.control;
    ts.densityMapResolution = s.detail;
    history->groupSince(start, !stepName.empty() ? stepName : "Create Terrain (" + (s.name.empty() ? std::string("Terrain") : s.name) + ")");
    project->setSelectedEntity(sceneId, entity);
    return entity;
}

void editor::CreateTerrainDialog::show(){
    if (!isOpen || !project){
        return;
    }
    const char* popupName = "Create Terrain##CreateTerrainModal";
    if (justOpened){
        ImGui::OpenPopup(popupName);
        justOpened = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!EditorModal::begin(popupName, &isOpen, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)){
        return;
    }
    EditorModal::message("A new terrain from a preset: its sizes, a 16-bit height map (flat, generated or imported) and a collider, one undo step.");
    EditorModal::error(error);

    const float width = ImGui::GetFontSize() * 12.0f;
    char name[128];
    std::snprintf(name, sizeof(name), "%s", settings.name.c_str());
    ImGui::SetNextItemWidth(width);
    if (EditorModal::inputText("Name", "name", name, sizeof(name))) settings.name = name;

    std::vector<std::string> presets;
    for (const TerrainSizePreset& p : TERRAIN_SIZE_PRESETS) presets.push_back(p.label);
    int preset = settings.preset;
    ImGui::SetNextItemWidth(width);
    if (EditorModal::combo("Preset", "preset", &preset, presets)) applyPreset(preset);

    ImGui::SetNextItemWidth(width);
    EditorModal::inputFloat("Terrain Size", "terrain_size", &settings.size, 0.0f, 0.0f, "%.1f m");
    ImGui::SetNextItemWidth(width);
    EditorModal::inputFloat("Terrain Height", "terrain_height", &settings.height, 0.0f, 0.0f, "%.1f m");
    ImGui::SetNextItemWidth(width);
    EditorModal::inputInt("Heightmap Resolution", "heightmap_resolution", &settings.heightmap, 0, 0);
    ImGui::SetNextItemWidth(width);
    EditorModal::inputInt("Control Texture Resolution", "control_resolution", &settings.control, 0, 0);
    ImGui::SetNextItemWidth(width);
    EditorModal::inputInt("Detail Resolution", "detail_resolution", &settings.detail, 0, 0);
    ImGui::SetNextItemWidth(width);
    EditorModal::inputFloat("Mesh Resolution", "mesh_resolution", &settings.meshResolution, 0.0f, 0.0f, "%.0f");
    ImGui::SetNextItemWidth(width);
    EditorModal::inputInt("Root Grid Size", "root_grid_size", &settings.rootGrid, 0, 0);
    ImGui::SetNextItemWidth(width);
    EditorModal::inputInt("Levels", "levels", &settings.levels, 0, 0);
    int shape = static_cast<int>(settings.shape);
    ImGui::SetNextItemWidth(width);
    if (EditorModal::combo("Start Shape", "start_shape", &shape, {"Flat", "Flat at Middle", "Import Raw..."})){
        if (static_cast<StartShape>(shape) == StartShape::ImportRaw){
            chooseImportFile();
        }else{
            settings.shape = static_cast<StartShape>(shape);
        }
    }
    if (settings.shape == StartShape::Flat){
        ImGui::SetNextItemWidth(width);
        EditorModal::inputFloat("Base Height", "base_height", &settings.baseHeight, 0.0f, 0.0f, "%.1f m");
    }
    if (settings.shape == StartShape::ImportRaw){
        // R61's fields and preview; the file's size becomes Heightmap Resolution
        if (EditorModal::button("Choose File...", "choose_import")) chooseImportFile();
        if (drawHeightImportFields(importPath, importBytes, importOptions, settings.imported, settings.height, &importSetsHeight)) useImportedSize();
        if (importSetsHeight && importOptions.range == HeightRange::Source && importOptions.sourceMax > importOptions.sourceMin){
            settings.height = importOptions.sourceMax - importOptions.sourceMin;
        }
    }
    // R58: Terrain Layer 0, covering the whole terrain until another layer is painted over it
    ImGui::TextUnformatted("Base Layer");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", settings.baseLayer.empty() ? "None" : settings.baseLayer.c_str());
    ImGui::SameLine();
    if (EditorModal::button("Browse...", "base_layer")){
        const std::string chosen = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
        if (!chosen.empty()){
            if (project->isInsideAssetsPath(chosen)){
                settings.baseLayer = project->normalizeToAssetsRelative(chosen).generic_string();
            }else{
                error = "The Base Layer must be an image inside the project's assets.";
            }
        }
    }
    if (!settings.baseLayer.empty()){
        ImGui::SameLine();
        if (EditorModal::button("Clear", "base_layer_clear")) settings.baseLayer.clear();
    }
    // T2 R61: generated after Create, from a preset; its recommended size and height fill the fields above.
    // An imported file is the start shape instead.
    if (settings.shape == StartShape::ImportRaw) settings.noisePreset.clear();
    ImGui::BeginDisabled(settings.shape == StartShape::ImportRaw);
    static const std::vector<TerrainPreset> noisePresets = TerrainPresets::list();
    std::vector<std::string> noiseNames{"None"};
    for (const TerrainPreset& p : noisePresets) noiseNames.push_back(p.name);
    int noise = 0;
    for (size_t i = 0; i < noisePresets.size(); i++){
        if (noisePresets[i].id == settings.noisePreset) noise = static_cast<int>(i) + 1;
    }
    ImGui::SetNextItemWidth(width);
    if (EditorModal::combo("Noise Graph", "noise_graph", &noise, noiseNames)){
        applyNoisePreset(noise > 0 && noise <= static_cast<int>(noisePresets.size()) ? noisePresets[static_cast<size_t>(noise) - 1].id : std::string());
    }
    if (!settings.noisePreset.empty()){
        int seed = static_cast<int>(settings.seed);
        ImGui::SetNextItemWidth(width);
        if (EditorModal::inputInt("Seed", "seed", &seed, 0, 0)) settings.seed = static_cast<uint32_t>(std::max(seed, 1));
    }
    ImGui::EndDisabled();
    EditorModal::checkbox("Add Terrain Collider", "add_terrain_collider", &settings.collider);
    settings.heightmap = std::clamp(settings.heightmap, 2, 8192);
    settings.control = std::clamp(settings.control, 2, 8192);
    settings.detail = std::clamp(settings.detail, 2, 8192);
    settings.rootGrid = std::clamp(settings.rootGrid, 1, 64);
    settings.levels = std::clamp(settings.levels, 1, 16);
    ImGui::TextDisabled("%s", memoryLine(settings).c_str());

    ImGui::Separator();
    if (EditorModal::button("Create", "ok", ImVec2(8.0f * ImGui::GetFontSize(), 0))){
        error.clear();
        // with a preset, Create waits for its heights (one step, made when they are ready)
        const bool made = settings.noisePreset.empty()
            ? create(project, sceneId, parent, settings, error) != NULL_ENTITY
            : createGenerated(project, sceneId, parent, settings, [](Entity, const GenerationResult& result){
                  if (!result.ok) Out::error("Create Terrain: " + result.error);
              }, error);
        if (made){
            isOpen = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel", ImVec2(8.0f * ImGui::GetFontSize(), 0))){
        isOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
