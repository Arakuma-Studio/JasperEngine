// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "object/TerrainEdit.h"
#include "terrain/TerrainHoles.h"
#include "terrain/TerrainQuality.h"
#include "TerrainQualityPanel.h"
#include "TerrainEditWindow.h"
#include "TerrainFoliagePanel.h"
#include "foliage/TreeDataFile.h"
#include "component/TerrainComponent.h"
#include "component/MeshComponent.h"
#include "component/PrefabComponent.h"
#include "component/InstancedMeshComponent.h"
#include "component/ModelComponent.h"
#include "component/Transform.h"
#include "widget/ColorPickerWindow.h"
#include "DocumentFile.h"
#include "util/FileUtils.h"
#include "util/SafeWrite.h"

#include "Backend.h"
#include "Catalog.h"
#include "Out.h"
#include "Stream.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/CommandNames.h"
#include "command/PrefabEditUndo.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/ImportPrefabCmd.h"
#include "command/type/ModelLoadCmd.h"
#include "command/type/PropertyCmd.h"
#include "command/type/TerrainMapDeltaCmd.h"
#include "command/type/TerrainTextureEditCmd.h"
#include "external/IconsFontAwesome6.h"
#include "subsystem/MeshSystem.h"
#include "terrain/TerrainLayers.h"
#include "util/Angle.h"
#include "util/Color.h"
#include "util/FileDialogs.h"
#include "util/TerrainMapFileWriter.h"
#include "util/TerrainMapUtils.h"
#include "terrain/TerrainMapPersistence.h"
#include "terrain/TerrainPlacement.h"
#include "terrain/TerrainSurface.h"
#include "util/NormalMapTilt.h"
#include "noise/NoiseCompat.h"
#include "util/UIUtils.h"
#include "util/Util.h"
#include "window/ProjectWindow.h"
#include "window/dialog/CreateTerrainDialog.h"
#include "window/TerrainGenerateSection.h"
#include "window/Widgets.h"
#include "util/FileUtils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <random>
#include <span>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;


void editor::TerrainEditWindow::showTooltip(const char* text, ImGuiHoveredFlags flags){
    if (ImGui::IsItemHovered(flags)){
        ImGui::SetTooltip("%s", text);
    }
}

// Brush and command buttons carry only an icon, so they get a step above the frame height to stay readable.
static ImVec2 commandButtonSize(){
    const float size = ImGui::GetFrameHeight() * 1.5f;
    return ImVec2(size, size);
}

bool editor::TerrainEditWindow::iconButton(const char* icon, const char* id, const char* tooltip, bool selected, const ImVec2& size){
    std::string label = "##" + std::string(id);

    if (selected){
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    }

    bool clicked = Widgets::iconButton(label.c_str(), icon, size);

    if (selected){
        ImGui::PopStyleColor(3);
    }

    showTooltip(tooltip, ImGuiHoveredFlags_AllowWhenDisabled);
    return clicked;
}

// Extracts terrain-editor map filenames ("terrain_edit_<stem>.png") from a text blob.
// A plain token scan is component-agnostic: it finds the reference no matter which
// component or property holds the path, and never breaks when the YAML schema evolves.
static void collectTerrainMapTokens(const std::string& content, std::unordered_set<std::string>& out){
    static const std::string prefix = "terrain_edit_";
    size_t pos = content.find(prefix);
    while (pos != std::string::npos){
        size_t end = pos + prefix.size();
        while (end < content.size() && (std::isalnum(static_cast<unsigned char>(content[end])) || content[end] == '_')){
            end++;
        }
        if (content.compare(end, 4, ".png") == 0){
            out.insert(content.substr(pos, end + 4 - pos));
        }else if (content.compare(end, 8, ".jetrees") == 0){
            out.insert(content.substr(pos, end + 8 - pos));
        }
        pos = content.find(prefix, end);
    }
}

// Scans every project file that can reference textures — scenes, prefabs
// (.prefab via Util::isPrefabFile) and materials — and collects the terrain map
// filenames they mention. Returns false when any file could not be read; the caller
// must then not delete anything, since an unreadable file may hold the only
// reference to a map.
static bool collectTerrainMapDiskReferences(const fs::path& projectPath, std::unordered_set<std::string>& out){
    if (projectPath.empty()){
        return false;
    }

    std::error_code ec;
    fs::recursive_directory_iterator it(projectPath, fs::directory_options::skip_permission_denied, ec);
    if (ec){
        return false;
    }

    const fs::recursive_directory_iterator endIt;
    while (it != endIt){
        const fs::directory_entry& entry = *it;

        if (entry.is_directory(ec)){
            // Dot-directories hold caches and VCS data (.jasperengine export copies, .git);
            // nothing in them defines a live reference.
            if (entry.path().filename().string().rfind(".", 0) == 0){
                it.disable_recursion_pending();
            }
        }else if (entry.is_regular_file(ec)){
            const std::string entryPath = entry.path().string();
            if (Util::isSceneFile(entryPath) || Util::isMaterialFile(entryPath) || Util::isPrefabFile(entryPath)){
                std::ifstream file(entry.path(), std::ios::binary);
                if (!file){
                    return false;
                }
                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                if (file.bad()){
                    return false;
                }
                collectTerrainMapTokens(content, out);
            }
        }

        it.increment(ec);
        if (ec){
            return false;
        }
    }

    return true;
}

bool editor::TerrainEditWindow::cleanUnusedTerrainMaps(Project* project){
    if (!project || project->getProjectPath().empty()){
        return true;
    }

    // Settle pending background writes first, so a late write can't resurrect a
    // file this scan is about to delete. This runs on every scene save, so a
    // failure here means the save is incomplete — say so, loudly.
    bool flushOk = TerrainMapPersistence::get().flushAll();
    flushOk = TreeDataFile::flushAll(project) && flushOk;
    if (!flushOk){
        Out::error("Scene saved, but %zu terrain map(s) could not be written to disk; the scene stays marked as unsaved — save again after checking free disk space and permissions.",
                   TerrainMapFileWriter::get().failedCount());
    }

    // Pass 1 — live references: loaded scenes are inspected through their in-memory
    // components, so maps repointed by still-unsaved edits stay protected. Closed
    // scenes keep a null Scene* (deleteSceneProject) and are covered by pass 2.
    std::unordered_set<std::string> activeFiles;
    for (SceneProject* sceneProjectPtr : project->getScenesAndPrefabStages()){
        SceneProject& sceneProject = *sceneProjectPtr;
        if (!sceneProject.scene){
            continue;
        }

        for (int i = 0; i < sceneProject.entities.size(); i++){
            Entity entity = sceneProject.entities[i];
            TerrainComponent* terrain = sceneProject.scene->findComponent<TerrainComponent>(entity);
            if (terrain){
                if (!terrain->heightMap.getPath(0).empty() && TerrainMapUtils::isEditableTexturePath(terrain->heightMap.getPath(0))){
                    activeFiles.insert(fs::path(terrain->heightMap.getPath(0)).filename().string());
                }
                for (const Texture& blendMap : terrain->blendMaps){
                    if (!blendMap.getPath(0).empty() && TerrainMapUtils::isEditableTexturePath(blendMap.getPath(0))){
                        activeFiles.insert(fs::path(blendMap.getPath(0)).filename().string());
                    }
                }
                // T5 R61: the tree file and the baked atlases are the maps folder's too
                if (!terrain->treeData.empty()){
                    activeFiles.insert(fs::path(terrain->treeData.str()).filename().string());
                }
                for (const TerrainTreePrototype& prototype : terrain->treePrototypes){
                    if (!prototype.impostorAlbedo.getPath(0).empty()) activeFiles.insert(fs::path(prototype.impostorAlbedo.getPath(0)).filename().string());
                    if (!prototype.impostorNormal.getPath(0).empty()) activeFiles.insert(fs::path(prototype.impostorNormal.getPath(0)).filename().string());
                }
                for (const TerrainFoliageLayer& layer : terrain->foliageLayers){
                    const std::string& densityPath = layer.densityMap.getPath(0);
                    if (!densityPath.empty() && TerrainMapUtils::isEditableTexturePath(densityPath)){
                        activeFiles.insert(fs::path(densityPath).filename().string());
                    }
                }
            }
        }
    }

    fs::path absoluteBaseDir = project->getTerrainMapsDir();

    std::error_code ec;
    if (!fs::exists(absoluteBaseDir, ec) || !fs::is_directory(absoluteBaseDir, ec)){
        return flushOk;
    }

    // The maps folder resolving outside the project (a link to another project's folder): its
    // files are not this project's to collect
    if (!safewrite::isInsideResolved(absoluteBaseDir, project->getProjectPath()) && !safewrite::isInsideResolved(absoluteBaseDir, project->getAssetsPath())){
        return flushOk;
    }

    // Candidate orphans: editable maps on disk that no loaded scene references.
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(absoluteBaseDir, ec)){
        if (entry.is_regular_file(ec)){
            const std::string filename = entry.path().filename().string();
            if (filename.rfind("terrain_edit_", 0) == 0 && activeFiles.find(filename) == activeFiles.end()){
                candidates.push_back(entry.path());
            }
        }
    }
    if (candidates.empty()){
        return flushOk;
    }

    // Pass 2 — on-disk references: closed scenes (and prefabs/materials) can still
    // reference candidate maps, so scan the project files before deleting. When the
    // scan cannot complete, delete nothing: a false "unused" verdict permanently
    // loses painted terrain, while a kept orphan only wastes a little disk.
    if (!collectTerrainMapDiskReferences(project->getProjectPath(), activeFiles)){
        return flushOk;
    }

    for (const fs::path& candidate : candidates){
        if (activeFiles.find(candidate.filename().string()) == activeFiles.end()){
            // the entry itself (in a write scope: beneath its root)
            FileUtils::removePath(candidate, ec);
            Out::info("Garbage collected old terrain map: %s", candidate.filename().string().c_str());
        }
    }

    return flushOk;
}

// Texel coordinate of a terrain-local position, centres at (i + 0.5) / size like the terrain shader and MeshSystem
static float terrainTexel(float local, float terrainSize, int size){
    return MeshSystem::terrainTexelCoord(local, terrainSize, size);
}

// A map stroke on the height map: the ray stays on the surface it started from
static bool strokeHoldsHeights(const ActiveStroke* activeStroke){
    return activeStroke && activeStroke->active && activeStroke->session && activeStroke->ref.target == TerrainMapTarget::HeightMap;
}

bool editor::TerrainEditWindow::raycastTerrainStrokeSurface(const Ray& localRay, TerrainComponent& terrain, const ActiveStroke* activeStroke, Vector3& localPoint, float& localHeight) const{
    if (!strokeHoldsHeights(activeStroke)){
        return false;
    }

    const unsigned char* currentPixels = nullptr;
    int currentWidth = 0;
    int currentHeight = 0;
    int currentChannels = 0;
    int currentBytesPerChannel = 1;

    if (!terrain.heightMap.empty() && TerrainMapUtils::hasLoadedData(terrain.heightMap)){
        TextureData& heightData = terrain.heightMap.getData();
        currentPixels = static_cast<const unsigned char*>(heightData.getData());
        currentWidth = heightData.getWidth();
        currentHeight = heightData.getHeight();
        currentChannels = heightData.getChannels();
        currentBytesPerChannel = TextureData::getBytesPerChannel(heightData.getColorFormat());
    }

    auto sampleHeightPixels = [](const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float terrainSize, float maxHeight, float localX, float localZ, float& sampledHeight){
        if (terrainSize <= std::numeric_limits<float>::epsilon()){
            return false;
        }

        const float halfSize = terrainSize * 0.5f;
        if (localX < -halfSize || localX > halfSize || localZ < -halfSize || localZ > halfSize){
            return false;
        }

        sampledHeight = terrain::sampleHeightTexels(pixels, width, height, channels, bytesPerChannel,
                                             terrainTexel(localX, terrainSize, width), terrainTexel(localZ, terrainSize, height)) * maxHeight;
        return true;
    };

    auto sampleCurrentHeight = [&](float localX, float localZ, float& sampledHeight){
        return sampleHeightPixels(currentPixels, currentWidth, currentHeight, currentChannels, currentBytesPerChannel, terrain.terrainSize, terrain.maxHeight, localX, localZ, sampledHeight);
    };

    // The cursor stays on the ground as the stroke found it, or a raise would climb under the pointer
    auto sampleRaycastHeight = [&](float localX, float localZ, float& sampledHeight){
        const float half = terrain.terrainSize * 0.5f;
        if (terrain.terrainSize <= std::numeric_limits<float>::epsilon() || localX < -half || localX > half || localZ < -half || localZ > half){
            return false;
        }
        sampledHeight = activeStroke->session->heightBefore(localX, localZ) * terrain.maxHeight;
        return true;
    };

    const float halfSize = terrain.terrainSize * 0.5f;
    const float surfaceMinHeight = std::min(0.0f, terrain.maxHeight);
    const float surfaceMaxHeight = std::max(0.0f, terrain.maxHeight);
    const Vector3 rayOrigin = localRay.getOrigin();
    const Vector3 rayDirection = localRay.getDirection();

    float rayEntry = 0.0f;
    float rayExit = 1.0f;
    auto clipAxis = [&](float axisOrigin, float axisDirection, float minValue, float maxValue){
        if (std::abs(axisDirection) <= std::numeric_limits<float>::epsilon()){
            return axisOrigin >= minValue && axisOrigin <= maxValue;
        }

        float nearParameter = (minValue - axisOrigin) / axisDirection;
        float farParameter = (maxValue - axisOrigin) / axisDirection;
        if (nearParameter > farParameter){
            std::swap(nearParameter, farParameter);
        }
        rayEntry = std::max(rayEntry, nearParameter);
        rayExit = std::min(rayExit, farParameter);
        return rayEntry <= rayExit;
    };

    if (!clipAxis(rayOrigin.x, rayDirection.x, -halfSize, halfSize) ||
        !clipAxis(rayOrigin.y, rayDirection.y, surfaceMinHeight, surfaceMaxHeight) ||
        !clipAxis(rayOrigin.z, rayDirection.z, -halfSize, halfSize)){
        return false;
    }

    auto signedDistanceToSurface = [&](float rayParameter, float& signedDistance, Vector3& point){
        point = localRay.getPoint(rayParameter);
        float sampledHeight = 0.0f;
        if (!sampleRaycastHeight(point.x, point.z, sampledHeight)){
            return false;
        }
        signedDistance = point.y - sampledHeight;
        return true;
    };

    const int raycastSteps = 160;
    const float surfaceEpsilon = std::max(0.001f, std::abs(surfaceMaxHeight - surfaceMinHeight) * 0.0005f);
    float previousParameter = rayEntry;
    float previousDistance = 0.0f;
    Vector3 previousPoint;
    if (!signedDistanceToSurface(previousParameter, previousDistance, previousPoint)){
        return false;
    }

    float closestDistance = previousDistance;
    Vector3 closestPoint = previousPoint;
    if (std::abs(previousDistance) <= surfaceEpsilon){
        closestDistance = 0.0f;
    }

    for (int sampleIndex = 1; sampleIndex <= raycastSteps; sampleIndex++){
        const float rayParameter = rayEntry + (rayExit - rayEntry) * (static_cast<float>(sampleIndex) / static_cast<float>(raycastSteps));
        float currentDistance = 0.0f;
        Vector3 currentPoint;
        if (!signedDistanceToSurface(rayParameter, currentDistance, currentPoint)){
            continue;
        }

        if (std::abs(currentDistance) < std::abs(closestDistance)){
            closestDistance = currentDistance;
            closestPoint = currentPoint;
        }

        const bool crossedSurface = (previousDistance <= 0.0f && currentDistance >= 0.0f) ||
                                    (previousDistance >= 0.0f && currentDistance <= 0.0f);
        if (crossedSurface){
            float lowerParameter = previousParameter;
            float upperParameter = rayParameter;
            float lowerDistance = previousDistance;
            for (int refineIndex = 0; refineIndex < 12; refineIndex++){
                const float midParameter = (lowerParameter + upperParameter) * 0.5f;
                float midDistance = 0.0f;
                Vector3 midPoint;
                if (!signedDistanceToSurface(midParameter, midDistance, midPoint)){
                    break;
                }
                const bool sameSide = (lowerDistance <= 0.0f && midDistance <= 0.0f) ||
                                      (lowerDistance >= 0.0f && midDistance >= 0.0f);
                if (sameSide){
                    lowerParameter = midParameter;
                    lowerDistance = midDistance;
                }else{
                    upperParameter = midParameter;
                }
            }
            closestPoint = localRay.getPoint((lowerParameter + upperParameter) * 0.5f);
            closestDistance = 0.0f;
            break;
        }

        previousParameter = rayParameter;
        previousDistance = currentDistance;
    }

    if (std::abs(closestDistance) > surfaceEpsilon * 4.0f){
        return false;
    }

    if (!sampleCurrentHeight(closestPoint.x, closestPoint.z, localHeight)){
        return false;
    }

    localPoint = Vector3(closestPoint.x, localHeight, closestPoint.z);
    return true;
}

editor::TerrainEditWindow::TerrainEditWindow(Project* project){
    this->project = project;
    foliagePanel = std::make_unique<TerrainFoliagePanel>(project, this);
    windowOpen = false;
    focusRequested = false;
    brushActive = false;
    selectedSceneId = NULL_PROJECT_SCENE;
    selectedEntity = NULL_ENTITY;
    brushMode     = TerrainBrushMode::Raise;
    brushShape    = TerrainBrushShape::Circle;
    brushFalloff  = TerrainBrushFalloff::Smooth;
    brushSize     = 4.0f;
    brushStrength = 0.3f;
    brushRotation = 0.0f;
    flattenHeight = 0.5f;
    terraceSteps  = 8;
    noiseSize     = 4.0f;
    brushMaskWidth = 0;
    brushMaskHeight = 0;
    heightMapResolution = 512;
    blendMapResolution  = 512;
    densityMapResolution = 512;
    selectedFoliageLayer = 0;
    selectedTextureLayer = 0;
    normalizeBlendPaint = true;
    heightMapStartAtMiddle = true;
    flattenPickOnStroke = true;
    placeInstanced = true;
    placeSpacing = 2.0f;
    placeMinScale = 1.0f;
    placeMaxScale = 1.0f;
    placeRotationJitter = 1.0f;
    placeAlignToNormal = 0.0f;
    paintUseMask = false;
    paintMinSlope = 0.0f;
    paintMaxSlope = 90.0f;
    paintMinHeight = 0.0f;
    paintMaxHeight = 1.0f;
    placementSeed = std::random_device{}();
}

editor::TerrainEditWindow::~TerrainEditWindow(){
    setOpen(false);
    // Drain pending map writes while the app is still fully alive (the writer's
    // own static destructor runs during late shutdown, where it makes one final
    // synchronous attempt at anything still failing).
    TreeDataFile::flushAll(project);
    if (!TerrainMapPersistence::get().flushAll()){
        Out::error("%zu terrain map(s) could not be written to disk; one final attempt will be made at exit, but the sculpt may be lost. Check free disk space and permissions.",
                   TerrainMapFileWriter::get().failedCount());
    }
}

// Instancing draws the host entity's own mesh, so an asset only qualifies when the load
// puts all of its geometry on the root.
static bool isInstanceableHost(Scene* scene, Entity host){
    ModelComponent* model = scene->findComponent<ModelComponent>(host);
    MeshComponent* mesh = scene->findComponent<MeshComponent>(host);
    if (!model || !mesh || mesh->numSubmeshes == 0){
        return false;
    }
    return model->nodesIdMapping.empty() && model->meshNodesMapping.empty() &&
           model->bonesIdMapping.empty() && model->animations.empty() &&
           model->skeleton == NULL_ENTITY;
}

// The brush has to know whether an asset can be instanced before it places the stamp.
static bool executeSync(editor::Command* command){
    const bool wasAsync = Engine::isAsyncLoading();
    Engine::setAsyncLoading(false);
    const bool done = command->execute();
    Engine::setAsyncLoading(wasAsync);
    return done;
}

class editor::TerrainEditWindow::TerrainInstancePlaceCmd: public editor::Command{
private:
    Project* project;
    uint32_t sceneId;
    Entity terrainEntity;
    std::string assetPath;
    InstanceData instance;

    CreateEntityCmd* createHostCmd = nullptr;
    ModelLoadCmd* loadHostCmd = nullptr;
    ModelLoadCmd* mergeHostCmd = nullptr;
    Entity host = NULL_ENTITY;
    size_t insertedIndex = 0;
    bool wasModified = false;

    bool createHost(SceneProject* sceneProject){
        Scene* scene = sceneProject->scene;

        if (!createHostCmd){
            std::string name = fs::path(assetPath).stem().string();
            createHostCmd = new CreateEntityCmd(project, sceneId, name.empty() ? "Terrain Instances" : name, EntityCreationType::MODEL, terrainEntity);
            createHostCmd->setQuiet(true);
        }
        if (!createHostCmd->execute()){
            return false;
        }

        host = createHostCmd->getEntity();
        scene->addComponent<InstancedMeshComponent>(host, {});

        if (!loadHostCmd){
            loadHostCmd = new ModelLoadCmd(project, sceneId, host, assetPath);
        }
        bool usable = executeSync(loadHostCmd);

        // A multi-node asset spreads over child entities until it is merged as one static mesh.
        if (usable && !isInstanceableHost(scene, host)){
            if (!mergeHostCmd){
                mergeHostCmd = new ModelLoadCmd(project, sceneId, host, assetPath, true);
            }
            usable = executeSync(mergeHostCmd) && isInstanceableHost(scene, host);
        }

        if (!usable){
            loadHostCmd->undo();
            createHostCmd->undo();
            host = NULL_ENTITY;
            return false;
        }

        return true;
    }

public:
    TerrainInstancePlaceCmd(Project* project, uint32_t sceneId, Entity terrainEntity, const std::string& assetPath, const InstanceData& instance):
        project(project), sceneId(sceneId), terrainEntity(terrainEntity), assetPath(assetPath), instance(instance){}

    ~TerrainInstancePlaceCmd() override{
        delete mergeHostCmd;
        delete loadHostCmd;
        delete createHostCmd;
    }

    bool execute() override{
        SceneProject* sceneProject = project->getScene(sceneId);
        if (!sceneProject){
            return false;
        }
        wasModified = sceneProject->isModified;

        host = findInstanceHost(sceneProject, terrainEntity, assetPath);
        if (host == NULL_ENTITY && !createHost(sceneProject)){
            return false;
        }

        Scene* scene = sceneProject->scene;
        InstancedMeshComponent& instmesh = scene->getComponent<InstancedMeshComponent>(host);
        insertedIndex = instmesh.instances.size();
        instmesh.instances.push_back(instance);
        instmesh.needUpdateInstances = true;

        // Grows with headroom, and the mesh reloads to get the larger buffer
        const size_t needed = instmesh.instances.size();
        if (instmesh.maxInstances < needed){
            instmesh.maxInstances = static_cast<unsigned int>(needed + needed / 4 + 8);
            scene->getComponent<MeshComponent>(host).needReload = true;
        }

        sceneProject->isModified = true;
        return true;
    }

    void undo() override{
        SceneProject* sceneProject = project->getScene(sceneId);
        if (!sceneProject){
            return;
        }
        Scene* scene = sceneProject->scene;

        if (host != NULL_ENTITY && scene->isEntityCreated(host)){
            if (InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(host)){
                if (insertedIndex < instmesh->instances.size()){
                    instmesh->instances.erase(instmesh->instances.begin() + insertedIndex);
                    instmesh->needUpdateInstances = true;
                }
            }
        }

        // Only the stamp that created the host takes it away again.
        if (createHostCmd){
            createHostCmd->undo();
            host = NULL_ENTITY;
        }

        sceneProject->isModified = wasModified;
    }

    bool affectsStructure() const override{
        return createHostCmd != nullptr;
    }

    // "Paint Trees (Island)"
    std::string describe() const override{
        return "Paint Trees (" + editor::CommandNames::entity(project, sceneId, terrainEntity) + ")";
    }

    const char* typeName() const override{ return "TerrainInstancePlaceCmd"; }

    size_t memoryBytes() const override{
        return sizeof(*this) + assetPath.size() + (createHostCmd ? createHostCmd->memoryBytes() : 0) +
            (loadHostCmd ? loadHostCmd->memoryBytes() : 0) + (mergeHostCmd ? mergeHostCmd->memoryBytes() : 0);
    }
};

class editor::TerrainEditWindow::TerrainInstanceEraseCmd: public editor::Command{
private:
    Project* project;
    uint32_t sceneId;
    Entity host;
    std::vector<size_t> indices; //ascending
    std::vector<InstanceData> removed;
    bool wasModified = false;

public:
    TerrainInstanceEraseCmd(Project* project, uint32_t sceneId, Entity host, const std::vector<size_t>& indices):
        project(project), sceneId(sceneId), host(host), indices(indices){}

    bool execute() override{
        SceneProject* sceneProject = project->getScene(sceneId);
        if (!sceneProject || indices.empty()){
            return false;
        }
        InstancedMeshComponent* instmesh = sceneProject->scene->findComponent<InstancedMeshComponent>(host);
        if (!instmesh){
            return false;
        }
        wasModified = sceneProject->isModified;

        // Back to front, so the indices still address what they did at collection.
        removed.clear();
        for (size_t i = indices.size(); i > 0; i--){
            const size_t index = indices[i - 1];
            if (index >= instmesh->instances.size()){
                continue;
            }
            removed.push_back(instmesh->instances[index]);
            instmesh->instances.erase(instmesh->instances.begin() + index);
        }
        std::reverse(removed.begin(), removed.end());

        instmesh->needUpdateInstances = true;
        sceneProject->isModified = true;
        return !removed.empty();
    }

    void undo() override{
        SceneProject* sceneProject = project->getScene(sceneId);
        if (!sceneProject){
            return;
        }
        InstancedMeshComponent* instmesh = sceneProject->scene->findComponent<InstancedMeshComponent>(host);
        if (!instmesh){
            return;
        }

        for (size_t i = 0; i < indices.size() && i < removed.size(); i++){
            const size_t index = std::min(indices[i], instmesh->instances.size());
            instmesh->instances.insert(instmesh->instances.begin() + index, removed[i]);
        }
        instmesh->needUpdateInstances = true;
        sceneProject->isModified = wasModified;
    }

    bool affectsStructure() const override{
        return false;
    }

    // "Erase Trees (Rock)": the instanced GameObject they are erased from
    std::string describe() const override{
        return "Erase Trees (" + editor::CommandNames::entity(project, sceneId, host) + ")";
    }

    const char* typeName() const override{ return "TerrainInstanceEraseCmd"; }

    size_t memoryBytes() const override{
        return sizeof(*this) + indices.capacity() * sizeof(size_t) + removed.capacity() * sizeof(InstanceData);
    }

    // Off-thread release: the destructor only frees index and instance vectors; no editor/engine/GPU state (S24 R8).
    bool releaseOffThread() const override{ return true; }
};

// One undo step per stroke: each stamp merges into the previous one while the stroke id
// matches, so a drag that drops thirty rocks undoes as a single action.
class editor::TerrainEditWindow::TerrainObjectStrokeCmd: public editor::Command{
private:
    uint64_t strokeId;
    std::vector<editor::Command*> commands;
    size_t executedCount = 0;

public:
    TerrainObjectStrokeCmd(uint64_t strokeId, editor::Command* command): strokeId(strokeId){
        commands.push_back(command);
    }

    ~TerrainObjectStrokeCmd() override{
        for (editor::Command* command : commands){
            delete command;
        }
    }

    bool execute() override{
        bool applied = false;
        while (executedCount < commands.size()){
            editor::Command* command = commands[executedCount];
            if (!command->execute()){
                // Dropped, so undo never runs against something that was not created.
                delete command;
                commands.erase(commands.begin() + executedCount);
                continue;
            }
            executedCount++;
            applied = true;
        }
        return applied;
    }

    void undo() override{
        for (size_t i = executedCount; i > 0; i--){
            commands[i - 1]->undo();
        }
        executedCount = 0;
    }

    bool mergeWith(editor::Command* otherCommand) override{
        TerrainObjectStrokeCmd* otherCmd = dynamic_cast<TerrainObjectStrokeCmd*>(otherCommand);
        if (!otherCmd || otherCmd->strokeId != strokeId){
            return false;
        }
        // The history deletes the older command after a merge, so its placements move out.
        commands.insert(commands.begin(), otherCmd->commands.begin(), otherCmd->commands.end());
        executedCount = commands.size();
        otherCmd->commands.clear();
        otherCmd->executedCount = 0;
        return true;
    }

    // Read before an undo runs, so it describes the whole stroke.
    bool affectsStructure() const override{
        for (editor::Command* command : commands){
            if (command->affectsStructure()){
                return true;
            }
        }
        return false;
    }

    // A stroke's stamps are all of one kind: named by the first
    std::string describe() const override{
        return commands.empty() ? std::string("Paint Trees") : commands.front()->describe();
    }

    const char* typeName() const override{ return "TerrainObjectStrokeCmd"; }

    size_t memoryBytes() const override{
        size_t bytes = sizeof(*this) + commands.capacity() * sizeof(editor::Command*);
        for (const editor::Command* command : commands){
            bytes += command->memoryBytes();
        }
        return bytes;
    }
};

SceneProject* editor::TerrainEditWindow::findSceneProject(Scene* scene) const{
    if (!project || !scene){
        return nullptr;
    }
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()){
        if (sceneProject->scene == scene){
            return sceneProject;
        }
    }
    return nullptr;
}

SceneProject* editor::TerrainEditWindow::getTargetSceneProject() const{
    if (!project || selectedSceneId == NULL_PROJECT_SCENE){
        return nullptr;
    }
    return project->getScene(selectedSceneId);
}

bool editor::TerrainEditWindow::updateTargetFromSelection(){
    if (!project){
        selectedSceneId = NULL_PROJECT_SCENE;
        selectedEntity = NULL_ENTITY;
        return false;
    }

    uint32_t sceneId = project->getSelectedSceneForProperties();
    SceneProject* sceneProject = project->getScene(sceneId);
    if (!sceneProject){
        sceneProject = project->getSelectedScene();
    }
    if (!sceneProject || !sceneProject->scene){
        selectedSceneId = NULL_PROJECT_SCENE;
        selectedEntity = NULL_ENTITY;
        return false;
    }

    std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    if (selected.size() == 1 && sceneProject->scene->findComponent<TerrainComponent>(selected[0])){
        selectedSceneId = sceneProject->id;
        selectedEntity = selected[0];
        return true;
    }

    selectedSceneId = NULL_PROJECT_SCENE;
    selectedEntity = NULL_ENTITY;
    brushActive = false;
    return false;
}

bool editor::TerrainEditWindow::hasValidTarget(SceneProject* sceneProject) const{
    SceneProject* targetScene = sceneProject ? sceneProject : getTargetSceneProject();
    return targetScene &&
           targetScene->scene &&
           targetScene->playState == ScenePlayState::STOPPED &&
           selectedEntity != NULL_ENTITY &&
           targetScene->scene->isEntityCreated(selectedEntity) &&
           targetScene->scene->findComponent<TerrainComponent>(selectedEntity) &&
           targetScene->scene->findComponent<Transform>(selectedEntity);
}

bool editor::TerrainEditWindow::ensureDensityMap(Project* project, SceneProject* sceneProject, Entity entity, int layer){
    const TerrainMapRef ref(TerrainMapTarget::DensityMap, layer);
    TerrainMapUtils::ensureMapSlot(sceneProject->scene->getComponent<TerrainComponent>(entity), ref);
    return TerrainMapUtils::ensureEditableMap(project, sceneProject, entity, ref, mapResolutionFor(TerrainMapTarget::DensityMap), false);
}

int editor::TerrainEditWindow::mapResolutionFor(TerrainMapTarget target) const{
    if (target == TerrainMapTarget::HeightMap){
        return heightMapResolution;
    }
    if (target == TerrainMapTarget::HoleMap){
        // the height map's (T3 R18), which a query takes in UV whatever it comes to
        return heightMapResolution;
    }
    return target == TerrainMapTarget::DensityMap ? densityMapResolution : blendMapResolution;
}

bool editor::TerrainEditWindow::isHoleBrush() const{
    return brushMode == TerrainBrushMode::CutHole || brushMode == TerrainBrushMode::FillHole;
}

editor::TerrainMapRef editor::TerrainEditWindow::getBrushMapRef() const{
    if (isHoleBrush()){
        return TerrainMapRef(TerrainMapTarget::HoleMap);
    }
    if (isHeightBrush()){
        return TerrainMapRef(TerrainMapTarget::HeightMap);
    }
    if (isDensityBrush()){
        return TerrainMapRef(TerrainMapTarget::DensityMap, selectedFoliageLayer);
    }
    // three layers share a blend map, so the selected one picks which map the stroke edits
    return TerrainMapRef(TerrainMapTarget::BlendMap, selectedTextureLayer / 3);
}

bool editor::TerrainEditWindow::isHeightBrush() const{
    return brushMode == TerrainBrushMode::Raise ||
           brushMode == TerrainBrushMode::Lower ||
           brushMode == TerrainBrushMode::Smooth ||
           brushMode == TerrainBrushMode::Flatten ||
           brushMode == TerrainBrushMode::Sharpen ||
           brushMode == TerrainBrushMode::Noise ||
           brushMode == TerrainBrushMode::Terrace ||
           brushMode == TerrainBrushMode::Stamp ||
           brushMode == TerrainBrushMode::Erode ||
           brushMode == TerrainBrushMode::Ramp;
}

bool editor::TerrainEditWindow::isBlendBrush() const{
    return brushMode == TerrainBrushMode::PaintBase || brushMode == TerrainBrushMode::PaintLayer;
}

bool editor::TerrainEditWindow::isDensityBrush() const{
    return brushMode == TerrainBrushMode::PaintDensity || brushMode == TerrainBrushMode::EraseDensity;
}

bool editor::TerrainEditWindow::isPlacementBrush() const{
    return brushMode == TerrainBrushMode::PlaceObject || brushMode == TerrainBrushMode::EraseObject;
}

bool editor::TerrainEditWindow::isPlacementReady() const{
    return brushMode == TerrainBrushMode::EraseObject || !placeAssetPath.empty();
}

bool editor::TerrainEditWindow::findTerrainHit(Scene* scene, const Ray& ray, Entity& entity, Vector3& localPoint, Vector3& worldPoint, float& localHeight, const ActiveStroke* activeStroke) const{
    SceneProject* sceneProject = findSceneProject(scene);
    if (!sceneProject || sceneProject->id != selectedSceneId || !hasValidTarget(sceneProject)){
        return false;
    }

    entity = selectedEntity;
    Transform& transform = scene->getComponent<Transform>(entity);
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    Matrix4 inverseModel = transform.modelMatrix.inverse();
    if (!strokeHoldsHeights(activeStroke)){
        if (!scene->getSystem<MeshSystem>()->raycastTerrainSurface(ray, terrain, transform, worldPoint, true)){
            return false;
        }

        localPoint = inverseModel * worldPoint;
        localHeight = localPoint.y;
        return true;
    }

    Vector3 localOrigin = inverseModel * ray.getOrigin();
    Vector3 localEnd = inverseModel * (ray.getOrigin() + ray.getDirection());
    Ray localRay(localOrigin, localEnd - localOrigin);

    if (!raycastTerrainStrokeSurface(localRay, terrain, activeStroke, localPoint, localHeight)){
        return false;
    }

    worldPoint = transform.modelMatrix * localPoint;
    return true;
}

// T1 R38: stamping on the main thread per event, so a large brush keeps the editor responsive
static constexpr double STROKE_FRAME_BUDGET_SECONDS = 0.006;
// Longest interval a single event may deposit, so hitches don't cause spikes.
static constexpr float BRUSH_MAX_STAMP_SECONDS = 0.05f;
// T1 R39: a test-only log of mouse strokes as the edit_terrain calls that replay them
static const char* terrainStrokeLogPath(){
    const char* path = std::getenv("JASPER_TERRAIN_STROKE_LOG");
    return path && path[0] ? path : nullptr;
}

// Objects any closer than this would stack on top of each other
static constexpr float MIN_PLACE_SPACING = 0.05f;

// The brush places models, instanced hosts and prefab roots. A light or a camera parented
// to the terrain is not a prop, so it neither blocks spacing nor answers to the erase brush.
static bool isPlacedObject(Scene* scene, Entity entity){
    return scene->findComponent<MeshComponent>(entity) != nullptr ||
           scene->findComponent<PrefabComponent>(entity) != nullptr;
}

Entity editor::TerrainEditWindow::findInstanceHost(SceneProject* sceneProject, Entity terrainEntity, const std::string& assetPath){
    Scene* scene = sceneProject->scene;
    for (Entity entity : sceneProject->entities){
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || transform->parent != terrainEntity){
            continue;
        }
        if (!scene->findComponent<InstancedMeshComponent>(entity)){
            continue;
        }
        ModelComponent* model = scene->findComponent<ModelComponent>(entity);
        if (model && model->filename == assetPath){
            return entity;
        }
    }
    return NULL_ENTITY;
}

// A prefab is a hierarchy of entities, so there is nothing to instance it into
bool editor::TerrainEditWindow::useInstancedPlacement() const{
    return placeInstanced && !Util::isPrefabFile(placeAssetPath) && placeAssetPath != instancingRejectedAsset;
}

// Props are direct children of the terrain, and an instanced host counts as its instances
std::vector<Vector2> editor::TerrainEditWindow::collectPlacedPoints(SceneProject* sceneProject, Entity terrainEntity) const{
    std::vector<Vector2> points;
    Scene* scene = sceneProject->scene;
    for (Entity entity : sceneProject->entities){
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || transform->parent != terrainEntity || !isPlacedObject(scene, entity)){
            continue;
        }
        if (InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(entity)){
            for (const InstanceData& instance : instmesh->instances){
                points.push_back(Vector2(transform->position.x + instance.position.x,
                                         transform->position.z + instance.position.z));
            }
            continue;
        }
        points.push_back(Vector2(transform->position.x, transform->position.z));
    }
    return points;
}

editor::Command* editor::TerrainEditWindow::makePlacementCommand(SceneProject* sceneProject, Entity terrainEntity, const Vector3& localPosition, const Quaternion& rotation, const Vector3& scale){
    return placementCommand(project, sceneProject->id, placeAssetPath, terrainEntity, localPosition, rotation, scale);
}

editor::Command* editor::TerrainEditWindow::placementCommand(Project* project, uint32_t sceneId, const std::string& asset, Entity terrainEntity,
                                                             const Vector3& localPosition, const Quaternion& rotation, const Vector3& scale){
    // Prefab paths are stored relative to the project, asset paths relative to the assets directory
    if (Util::isPrefabFile(asset)){
        ImportPrefabCmd* command = new ImportPrefabCmd(project, sceneId, fs::path(asset), terrainEntity, true);
        command->setQuiet(true);
        command->setPlacement(localPosition, rotation, scale);
        return command;
    }

    if (!Util::isModelFile(asset)){
        return nullptr;
    }

    std::string name = fs::path(asset).stem().string();
    if (name.empty()){
        name = "GameObject";
    }
    return new ModelLoadCmd(project, sceneId, name, terrainEntity, localPosition, rotation, scale, asset);
}

std::vector<std::pair<Entity, Vector2>> editor::TerrainEditWindow::placedGameObjects(SceneProject* sceneProject, Entity terrainEntity){
    std::vector<std::pair<Entity, Vector2>> placed;
    Scene* scene = sceneProject->scene;
    for (Entity entity : sceneProject->entities){
        Transform* transform = scene->findComponent<Transform>(entity);
        if (!transform || transform->parent != terrainEntity || !isPlacedObject(scene, entity) || scene->findComponent<InstancedMeshComponent>(entity)){
            continue;
        }
        placed.emplace_back(entity, Vector2(transform->position.x, transform->position.z));
    }
    return placed;
}

void editor::TerrainEditWindow::addStrokeObjectCommand(SceneProject* sceneProject, Command* command){
    CommandHandle::get(sceneProject->id)->addCommand(new TerrainObjectStrokeCmd(stroke.placementStrokeId, command));
}

bool editor::TerrainEditWindow::applyPlacement(SceneProject* sceneProject, Entity entity, const Vector3& localPoint){
    Scene* scene = sceneProject->scene;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    const float halfSize = std::max(terrain.terrainSize, std::numeric_limits<float>::epsilon()) * 0.5f;

    // One attempt per spaced stamp, scattered inside the brush: a click drops one, a drag lays a trail
    // (T1 R21). The plan is pure, and the commands below are the existing ones.
    const terrain::PlacementStamp attempt{localPoint.x, localPoint.z, brushSize, brushShape == TerrainBrushShape::Square};
    terrain::PlacementParams params;
    params.spacing = std::max(placeSpacing, MIN_PLACE_SPACING);
    params.minScale = placeMinScale;
    params.maxScale = placeMaxScale;
    params.rotationJitter = placeRotationJitter;
    params.seed = placementSeed++;
    const terrain::PlacementPlan plan = terrain::planPlacement(std::span<const terrain::PlacementStamp>(&attempt, 1), params, stroke.placedPoints,
                                                               Vector2(-halfSize, -halfSize), Vector2(halfSize, halfSize));
    if (plan.objects.empty()){
        return false;
    }
    const terrain::PlacedObject& placedObject = plan.objects.front();
    const float localX = placedObject.x;
    const float localZ = placedObject.z;

    float height = 0.0f;
    Vector3 normal(0.0f, 1.0f, 0.0f);
    scene->getSystem<MeshSystem>()->sampleTerrainSurface(terrain, localX, localZ, height, normal);

    // Same jitter the foliage scatter uses, so grass and props sit on the surface alike
    const Vector3 scale = Vector3(placedObject.scale);
    Quaternion rotation(Angle::radToDefault(placedObject.yaw), Vector3(0.0f, 1.0f, 0.0f));

    if (placeAlignToNormal > 0.0f){
        const float slope = Angle::radToDefault(std::acos(std::clamp(normal.y, -1.0f, 1.0f)));
        const Vector3 axis = Vector3(0.0f, 1.0f, 0.0f).crossProduct(normal);
        if (axis.length() > std::numeric_limits<float>::epsilon()){
            const Quaternion tilt(slope, axis.normalized());
            rotation = Quaternion::slerp(placeAlignToNormal, Quaternion(), tilt) * rotation;
        }
    }

    const Vector3 position(localX, height, localZ);

    if (stroke.instanced){
        InstanceData instance;
        instance.position = position;
        instance.rotation = rotation;
        instance.scale = scale;

        addStrokeObjectCommand(sceneProject, new TerrainInstancePlaceCmd(project, sceneProject->id, entity, placeAssetPath, instance));

        // A host that could not be built leaves nothing behind, so the stroke drops back to entities
        if (findInstanceHost(sceneProject, entity, placeAssetPath) != NULL_ENTITY){
            stroke.placedPoints.push_back(Vector2(localX, localZ));
            return true;
        }

        stroke.instanced = false;
        instancingRejectedAsset = placeAssetPath;
        Out::warning("Cannot instance '%s', placing separate entities instead", placeAssetPath.c_str());
    }

    Command* command = makePlacementCommand(sceneProject, entity, position, rotation, scale);
    if (!command){
        return false;
    }

    // A command the history dropped placed nothing, so its spot stays open for the stroke
    const size_t before = sceneProject->entities.size();
    addStrokeObjectCommand(sceneProject, command);
    if (sceneProject->entities.size() == before){
        return false;
    }

    stroke.placedPoints.push_back(Vector2(localX, localZ));
    return true;
}

// A drag makes one attempt per spaced stamp along its path, however many events it took (T1 R21)
bool editor::TerrainEditWindow::placeAlongStroke(SceneProject* sceneProject, Entity entity, const Vector3& localPoint){
    if (!stroke.placementPath){
        const TerrainComponent& terrain = sceneProject->scene->getComponent<TerrainComponent>(entity);
        // an empty map is still placed on: spaced as a brush on the terrain's default map
        float texelSize = terrain.terrainSize / 512.0f;
        if (!terrain.heightMap.empty() && terrain.heightMap.hasData() && terrain.heightMap.getData().getWidth() > 0){
            texelSize = terrain.terrainSize / static_cast<float>(terrain.heightMap.getData().getWidth());
        }
        stroke.placementPath.emplace(terrain::StrokeResampler::defaultSpacing(brushSize, texelSize));
    }
    terrain::StrokeSample sample;
    sample.x = localPoint.x;
    sample.z = localPoint.z;
    std::vector<terrain::StrokeStamp> stamps;
    stroke.placementPath->add(sample, stamps);
    bool placed = false;
    for (const terrain::StrokeStamp& stamp : stamps){
        placed = applyPlacement(sceneProject, entity, Vector3(stamp.x, 0.0f, stamp.z)) || placed;
    }
    return placed;
}

bool editor::TerrainEditWindow::applyObjectErase(SceneProject* sceneProject, Entity entity, const Vector3& localPoint){
    Scene* scene = sceneProject->scene;
    const float radiusSquared = brushSize * brushSize;

    auto underBrush = [&](float x, float z){
        const float dx = x - localPoint.x;
        const float dz = z - localPoint.z;
        if (brushShape == TerrainBrushShape::Circle){
            return (dx * dx + dz * dz) <= radiusSquared;
        }
        return std::abs(dx) <= brushSize && std::abs(dz) <= brushSize;
    };

    std::vector<Entity> targets;
    // One command per host, since each edits its own array
    std::vector<std::pair<Entity, std::vector<size_t>>> instanceTargets;

    for (Entity candidate : sceneProject->entities){
        Transform* transform = scene->findComponent<Transform>(candidate);
        if (!transform || transform->parent != entity || !isPlacedObject(scene, candidate)){
            continue;
        }

        if (InstancedMeshComponent* instmesh = scene->findComponent<InstancedMeshComponent>(candidate)){
            std::vector<size_t> indices;
            for (size_t i = 0; i < instmesh->instances.size(); i++){
                const Vector3& position = instmesh->instances[i].position;
                if (underBrush(transform->position.x + position.x, transform->position.z + position.z)){
                    indices.push_back(i);
                }
            }
            if (!indices.empty()){
                instanceTargets.emplace_back(candidate, std::move(indices));
            }
            continue;
        }

        if (underBrush(transform->position.x, transform->position.z)){
            targets.push_back(candidate);
        }
    }

    if (targets.empty() && instanceTargets.empty()){
        return false;
    }

    for (const auto& hostTarget : instanceTargets){
        addStrokeObjectCommand(sceneProject, new TerrainInstanceEraseCmd(project, sceneProject->id, hostTarget.first, hostTarget.second));
    }
    if (!targets.empty()){
        addStrokeObjectCommand(sceneProject, new DeleteEntityCmd(project, sceneProject->id, targets));
    }

    // What is left is what the spacing test should see for the rest of the stroke.
    stroke.placedPoints = collectPlacedPoints(sceneProject, entity);
    return true;
}

void editor::TerrainEditWindow::clearStroke(){
    stroke.edit.commit();
    stroke = ActiveStroke();
}

void editor::TerrainEditWindow::openStrokeEdit(SceneProject* sceneProject){
    stroke.wasModified = sceneProject->isModified;
    stroke.edit = CommandHandle::get(sceneProject->id)->beginInteractive(std::string());
    stroke.editStarted = true;
}

bool editor::TerrainEditWindow::strokeCancelled(){
    if (!stroke.editStarted || stroke.edit.isOpen()){
        return false;
    }
    // Placement commands were undone by the cancel; a map stroke goes back through its before tiles
    // (T1 R27), and a first stroke on a map it had to move, to the map it found
    SceneProject* sceneProject = project->getScene(stroke.sceneId);
    if (!stroke.placement && sceneProject && sceneProject->scene->isEntityCreated(stroke.entity)){
        if (TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(stroke.entity)){
            if (stroke.session){
                stroke.store->rebind(*terrain);
                stroke.session->cancel();
            }
            Texture* texture = TerrainMapUtils::findTexture(*terrain, stroke.ref);
            if (texture && stroke.rebound){
                TerrainMapUtils::applySnapshotToTexture(project, *texture, stroke.rebindSnapshot);
                TerrainMapUtils::refresh(sceneProject, stroke.entity, stroke.ref);
                TerrainMapUtils::refreshSharing(project, stroke.sceneId, stroke.entity, stroke.ref);
            }
        }
        sceneProject->isModified = stroke.wasModified;
    }
    stroke = ActiveStroke();
    return true;
}

TerrainStrokeBrush editor::TerrainEditWindow::makeStrokeBrush(TerrainBrushMode mode, const TerrainComponent& terrain, float pickedHeight, bool scripted) const{
    TerrainStrokeBrush brush;
    brush.mode = mode;
    brush.shape = brushShape;
    brush.falloff = brushFalloff;
    brush.size = brushSize;
    brush.strength = brushStrength;
    brush.rotation = brushRotation;
    // R56: an alternate takes its height from under the stroke's first point, as Pick and Flatten does
    const bool alternateOn = !scripted && isAlternateOn();
    brush.flattenTarget = std::clamp(flattenPickOnStroke || alternateOn ? pickedHeight : flattenHeight, 0.0f, 1.0f);
    brush.alternate = alternateOn && (mode == TerrainBrushMode::Raise || mode == TerrainBrushMode::Lower);
    brush.terraceSteps = std::clamp(terraceSteps, MIN_TERRACE_STEPS, MAX_TERRACE_STEPS);
    brush.noiseSize = noiseSize;
    brush.textureLayer = selectedTextureLayer;
    brush.foliageLayer = selectedFoliageLayer;
    brush.normalizeBlend = normalizeBlendPaint;
    if (!brushMaskPixels.empty()){
        brush.mask = brushMaskPixels;
        brush.maskWidth = brushMaskWidth;
        brush.maskHeight = brushMaskHeight;
    }
    if (paintUseMask){
        brush.slope = terrain::FilterRange{paintMinSlope, paintMaxSlope};
        brush.height = terrain::FilterRange{paintMinHeight * terrain.maxHeight, paintMaxHeight * terrain.maxHeight};
        brush.filterAllMaps = filterAllTools;
        brush.invertSlope = filterInvert;
        brush.invertHeight = heightFilterInvert;
    }
    brush.accumulateRamp = scripted;
    return brush;
}

bool editor::TerrainEditWindow::openMapStroke(SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref, TerrainBrushMode mode,
                                              const Vector3& startPoint, float pickedHeight, bool scripted){
    Scene* scene = sceneProject->scene;
    TerrainMapUtils::ensureMapSlot(scene->getComponent<TerrainComponent>(entity), ref);
    Texture* texture = TerrainMapUtils::findTexture(scene->getComponent<TerrainComponent>(entity), ref);
    if (!texture){
        return false;
    }
    // A map this terrain does not own yet moves to a file of its own first (T1 R32): that step keeps
    // whole-map snapshots, and it alone does (R28). Every later stroke is a delta.
    stroke.rebound = TerrainMapUtils::needsEditableCopy(*texture, sceneProject->id, entity, ref);
    if (stroke.rebound){
        const bool forcePixels = texture->getPath(0).empty() || TerrainMapUtils::isOwnedEditableTexturePath(texture->getPath(0), sceneProject->id, entity, ref);
        stroke.rebindSnapshot = TerrainMapUtils::captureSnapshot(project, *texture, forcePixels);
    }
    if (!TerrainMapUtils::ensureEditableMap(project, sceneProject, entity, ref, mapResolutionFor(ref.target), heightMapStartAtMiddle)){
        return false;
    }
    if (stroke.rebound){
        TerrainMapUtils::refresh(sceneProject, entity, ref);
    }

    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    stroke.store = std::make_unique<EditorTerrainStore>(project, sceneProject->id, scene, entity, terrain);
    stroke.store->beginStroke();
    // T3 R29: the last fill keeps the hole map through the stroke; its end takes the map away as one step
    stroke.store->setKeepsMaps(true);
    stroke.session = std::make_unique<TerrainStrokeSession>(*stroke.store, TerrainMapUtils::mapOfRef(ref), terrain.terrainSize,
                                                            makeStrokeBrush(mode, terrain, pickedHeight, scripted));
    stroke.session->setRampStart(startPoint.x, startPoint.z);
    return true;
}

// The stroke's store follows the component if adding a terrain moved the component array
bool editor::TerrainEditWindow::strokeTerrainIsThere(SceneProject* sceneProject){
    TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(stroke.entity);
    if (!terrain || !stroke.store){
        return false;
    }
    stroke.store->rebind(*terrain);
    return true;
}

// T1 R33-R35, R38: the pointer's samples become spaced stamps, so neither the event rate nor the
// pointer's speed changes the stroke, and at most a frame's budget of them runs per event; the rest
// wait for the next frame, or for the stroke's end
bool editor::TerrainEditWindow::paintMapStroke(SceneProject* sceneProject, const Vector3& localPoint){
    if (!strokeTerrainIsThere(sceneProject)){
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!stroke.started){
        stroke.started = true;
        stroke.startTime = now;
    }
    const float time = std::chrono::duration<float>(now - stroke.startTime).count();
    stroke.session->addSample(time, localPoint.x, localPoint.z, 1.0f);
    if (terrainStrokeLogPath()){
        stroke.logSamples.push_back(terrain::StrokeSample{time, localPoint.x, localPoint.z, 1.0f});
    }
    stroke.session->run(STROKE_FRAME_BUDGET_SECONDS);
    return stroke.session->flush();
}

bool editor::TerrainEditWindow::createMapForTarget(const TerrainMapRef& ref, int width, int height){
    SceneProject* sceneProject = getTargetSceneProject();
    if (!hasValidTarget(sceneProject)){
        return false;
    }

    TerrainComponent& terrain = sceneProject->scene->getComponent<TerrainComponent>(selectedEntity);
    TerrainMapUtils::ensureMapSlot(terrain, ref);
    Texture* texture = TerrainMapUtils::findTexture(terrain, ref);
    if (!texture){
        return false;
    }

    const bool forceBeforePixels = texture->getPath(0).empty() || TerrainMapUtils::isOwnedEditableTexturePath(texture->getPath(0), sceneProject->id, selectedEntity, ref);
    TerrainMapSnapshot before = TerrainMapUtils::captureSnapshot(project, *texture, forceBeforePixels);
    TerrainMapSnapshot after;
    after.empty = false;
    after.path = TerrainMapUtils::makeEditableTexturePath(project, sceneProject->id, selectedEntity, ref);
    after.id = after.path;
    after.minFilter = texture->getMinFilter();
    after.magFilter = texture->getMagFilter();
    after.wrapU = texture->getWrapU();
    after.wrapV = texture->getWrapV();
    after.colorFormat = TerrainMapUtils::expectedFormat(ref.target);
    after.channels = TerrainMapUtils::expectedChannels(ref.target);
    after.width = std::max(2, width);
    after.height = std::max(2, height);
    after.pixels = TerrainMapUtils::makeInitialMapPixels(ref.target, after.width, after.height, heightMapStartAtMiddle);

    if (TerrainMapUtils::snapshotsEqual(before, after)){
        return false;
    }

    CommandHandle::get(sceneProject->id)->addCommandNoMerge(new TerrainTextureEditCmd(project, sceneProject->id, selectedEntity, ref, std::move(before), std::move(after)));
    return true;
}

bool editor::TerrainEditWindow::convertHeightMapTo16Bit(uint32_t sceneId, Entity entity, std::string& error){
    SceneProject* sceneProject = project->getScene(sceneId);
    TerrainComponent* terrain = sceneProject ? sceneProject->scene->findComponent<TerrainComponent>(entity) : nullptr;
    if (!terrain){
        error = "The GameObject has no Terrain.";
        return false;
    }
    const TerrainMapRef ref(TerrainMapTarget::HeightMap);
    Texture& texture = terrain->heightMap;
    TerrainMapSnapshot before = TerrainMapUtils::captureSnapshot(project, texture, true);
    if (before.empty || before.pixels.empty()){
        error = "The terrain has no heightmap.";
        return false;
    }
    if (TextureData::getBytesPerChannel(before.colorFormat) >= 2){
        error = "The heightmap is 16-bit already.";
        return false;
    }
    // the same heights, each 8-bit level at its 16-bit equal (v * 257), in the same file
    TerrainMapSnapshot after = before;
    after.colorFormat = ColorFormat::RED16;
    after.channels = 1;
    after.pixels.assign(static_cast<size_t>(before.width) * static_cast<size_t>(before.height) * 2, 0);
    for (size_t texel = 0; texel < static_cast<size_t>(before.width) * static_cast<size_t>(before.height); texel++){
        TerrainMapUtils::encodeHeightTexel(after.pixels.data(), texel, 2, terrain::heightTexelAt(before.pixels.data(), texel, before.channels, 1));
    }
    if (after.path.empty()){
        after.path = TerrainMapUtils::makeEditableTexturePath(project, sceneId, entity, ref);
        after.id = after.path;
    }
    CommandHandle::get(sceneId)->addCommandNoMerge(new TerrainTextureEditCmd(project, sceneId, entity, ref, std::move(before), std::move(after),
                                                                             "Convert Heightmap to 16-bit"));
    return true;
}

bool editor::TerrainEditWindow::deleteMapForTarget(const TerrainMapRef& ref){
    SceneProject* sceneProject = getTargetSceneProject();
    if (!hasValidTarget(sceneProject)){
        return false;
    }

    TerrainComponent& terrain = sceneProject->scene->getComponent<TerrainComponent>(selectedEntity);
    Texture* texture = TerrainMapUtils::findTexture(terrain, ref);
    if (!texture || texture->empty()){
        return false;
    }

    const bool forceBeforePixels = texture->getPath(0).empty() || TerrainMapUtils::isOwnedEditableTexturePath(texture->getPath(0), sceneProject->id, selectedEntity, ref);
    TerrainMapSnapshot before = TerrainMapUtils::captureSnapshot(project, *texture, forceBeforePixels);
    TerrainMapSnapshot after;

    if (TerrainMapUtils::snapshotsEqual(before, after)){
        return false;
    }

    CommandHandle::get(sceneProject->id)->addCommandNoMerge(new TerrainTextureEditCmd(project, sceneProject->id, selectedEntity, ref, std::move(before), std::move(after)));
    return true;
}

bool editor::TerrainEditWindow::setFoliageLayers(const std::vector<TerrainFoliageLayer>& layers){
    SceneProject* sceneProject = getTargetSceneProject();
    if (!hasValidTarget(sceneProject)){
        return false;
    }

    CommandHandle::get(sceneProject->id)->addCommandNoMerge(new PropertyCmd<std::vector<TerrainFoliageLayer>>(
        project, sceneProject->id, selectedEntity, ComponentType::TerrainComponent, "foliageLayers", layers));
    return true;
}

template<typename T>
bool editor::TerrainEditWindow::setFoliageLayerProperty(const char* field, const T& value){
    SceneProject* sceneProject = getTargetSceneProject();
    if (!hasValidTarget(sceneProject)){
        return false;
    }

    const std::string property = "foliageLayers[" + std::to_string(selectedFoliageLayer) + "]." + field;
    CommandHandle::get(sceneProject->id)->addCommand(new PropertyCmd<T>(
        project, sceneProject->id, selectedEntity, ComponentType::TerrainComponent, property, value));
    return true;
}

void editor::TerrainEditWindow::updateFoliagePreview(){
    if (!project){
        return;
    }

    const bool editing = windowOpen && !project->isAnyScenePlaying() && hasValidTarget();
    for (SceneProject* sceneProjectPtr : project->getScenesAndPrefabStages()){
        SceneProject& sceneProject = *sceneProjectPtr;
        if (!sceneProject.scene){
            continue;
        }
        const Entity preview = editing && sceneProject.id == selectedSceneId ? selectedEntity : NULL_ENTITY;
        if (sceneProject.scene->getSystem<MeshSystem>()->setFoliagePreviewEntity(preview)){
            sceneProject.needUpdateRender = true;
        }
    }
}

static bool beginTerrainProperties(const char* id){
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)){
        return false;
    }
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, std::min(ImGui::GetFontSize() * 8.0f, ImGui::GetContentRegionAvail().x * 0.4f));
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

static void terrainPropertyRow(const char* label, const char* tooltip = nullptr, bool child = false, bool indent = false){
    ImGui::TableNextRow();
    if (child){
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_PopupBg]));
    }
    ImGui::TableSetColumnIndex(0);
    if (indent){
        ImGui::Indent();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip){
        ImGui::SetItemTooltip("%s", tooltip);
    }
    if (indent){
        ImGui::Unindent();
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1.0f);
}

void editor::TerrainEditWindow::drawMapSettings(const TerrainMapRef& ref, const char* label, int& resolution){
    TerrainComponent& terrain = getTargetSceneProject()->scene->getComponent<TerrainComponent>(selectedEntity);
    Texture* texture = TerrainMapUtils::findTexture(terrain, ref);
    const bool mapPresent = texture && !texture->empty();
    int mapWidth = 0;
    int mapHeight = 0;
    if (mapPresent){
        if (TerrainMapUtils::hasLoadedData(*texture)){
            TextureData& data = texture->getData();
            mapWidth = data.getWidth();
            mapHeight = data.getHeight();
        }else{
            mapWidth = static_cast<int>(texture->getWidth());
            mapHeight = static_cast<int>(texture->getHeight());
        }
    }
    const bool sizeKnown = mapWidth > 0 && mapHeight > 0;
    const bool heightMap = ref.target == TerrainMapTarget::HeightMap;
    const ImVec2 buttonSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int actionCount = (mapPresent ? 2 : 1) + (heightMap ? 1 : 0);

    ImGui::PushID(label);
    terrainPropertyRow(label, mapPresent ? "Recreate or remove this map. Map changes can be undone." : "Choose a resolution, then create a map to enable painting.");
    const float valueWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - (buttonSize.x + spacing) * actionCount);
    if (mapPresent){
        ImGui::BeginChild("size", ImVec2(valueWidth, buttonSize.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::AlignTextToFramePadding();
        if (sizeKnown){
            ImGui::Text("%d x %d", mapWidth, mapHeight);
        }else{
            ImGui::TextDisabled("Unknown size");
        }
        ImGui::EndChild();
        showTooltip(texture->getPath(0).empty() ? "Editable map" : texture->getPath(0).c_str());
    }else{
        resolution = std::clamp(resolution, 2, 8192);
        ImGui::SetNextItemWidth(valueWidth);
        ImGui::DragInt("##resolution", &resolution, 1.0f, 2, 8192, "%d px", ImGuiSliderFlags_AlwaysClamp);
        showTooltip("New map resolution (width and height)");
    }
    if (heightMap){
        ImGui::SameLine();
        if (iconButton(ICON_FA_CIRCLE_HALF_STROKE, "middle", "Start new heightmaps at middle height", heightMapStartAtMiddle, buttonSize)){
            heightMapStartAtMiddle = !heightMapStartAtMiddle;
        }
    }
    ImGui::SameLine();
    if (mapPresent){
        ImGui::BeginDisabled(!sizeKnown);
        if (iconButton(ICON_FA_ARROWS_ROTATE, "recreate", "Recreate map at its current resolution", false, buttonSize)){
            endStroke();
            createMapForTarget(ref, mapWidth, mapHeight);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (iconButton(ICON_FA_TRASH_CAN, "remove", "Remove map", false, buttonSize)){
            endStroke();
            deleteMapForTarget(ref);
        }
    }else if (iconButton(ICON_FA_PLUS, "create", "Create map", false, buttonSize)){
        endStroke();
        createMapForTarget(ref, resolution, resolution);
    }
    ImGui::PopID();
}

// Returns the size it drew at, which decides whether the details fit beside it
float editor::TerrainEditWindow::drawAssetThumbnail(const std::string& path, const char* id, bool selected, float scale){
    const float thumbSize = std::min(ImGui::GetFrameHeight() * scale, std::max(1.0f, ImGui::GetContentRegionAvail().x));
    int width = 0;
    int height = 0;
    ImTextureID thumbnail = Backend::getApp().getProjectWindow()->getAssetThumbnail(path, width, height);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const float rounding = ImGui::GetStyle().FrameRounding;
    const ImVec4 background = selected ? Theme::Colors::ButtonActivated : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    drawList->AddRectFilled(p, ImVec2(p.x + thumbSize, p.y + thumbSize), ImGui::GetColorU32(background), rounding);
    if (thumbnail && width > 0 && height > 0){
        const float fit = thumbSize / std::max(width, height);
        const ImVec2 min(p.x + (thumbSize - width * fit) * 0.5f, p.y + (thumbSize - height * fit) * 0.5f);
        Widgets::addImageRounded(drawList, thumbnail, min, ImVec2(min.x + width * fit, min.y + height * fit), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, rounding, ImDrawFlags_RoundCornersAll);
    }else{
        const ImVec2 size = ImGui::CalcTextSize(ICON_FA_CUBE);
        drawList->AddText(ImVec2(p.x + (thumbSize - size.x) * 0.5f, p.y + (thumbSize - size.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_FA_CUBE);
    }
    if (selected){
        drawList->AddRect(p, ImVec2(p.x + thumbSize, p.y + thumbSize), ImGui::GetColorU32(ImGuiCol_NavHighlight), rounding, 0, 2.0f);
    }
    ImGui::InvisibleButton(id, ImVec2(thumbSize, thumbSize));
    if (ImGui::IsItemHovered() && thumbnail){
        ImGui::BeginTooltip();
        const float scale = std::min(1.0f, ImGui::GetFontSize() * 18.0f / std::max(width, height));
        Widgets::image(thumbnail, ImVec2(width * scale, height * scale));
        ImGui::TextUnformatted(path.c_str());
        ImGui::EndTooltip();
    }
    return thumbSize;
}

// The blend map channels as a material list, so nobody has to remember which one is the rock
void editor::TerrainEditWindow::drawTextureLayers(TerrainComponent& terrain){
    SceneProject* sceneProject = getTargetSceneProject();
    // T3 R39: what the layers cost, for the effective tier or another one
    static int shownTier = -1;
    drawTerrainCostStrip(terrain, shownTier);
    const ImVec2 buttonSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
    const ImVec2 detailsSpacing(spacing.x * 0.5f, spacing.y * 0.5f);
    const float labelHeight = ImGui::GetTextLineHeight();
    const float previewHeight = buttonSize.y * 2.0f + spacing.y;

    // The whole vector is set at once, so assigning a layer can also grow it
    auto assignLayer = [&](int index, const fs::path& path){
        if (!path.empty() && !project->isInsideAssetsPath(path)){
            Backend::getApp().registerOutsideAssetsAlert(path.string());
            return;
        }
        endStroke();
        std::vector<TerrainSurfaceLayer> layers = terrain.surfaceLayers;
        if (index >= static_cast<int>(layers.size())){
            layers.resize(index + 1);
        }
        // Only the color changes: a PBR layer keeps the rest of its surface
        layers[index].colorTexture = path.empty() ? Texture() : Texture(project->normalizeToAssetsRelative(path).generic_string());
        setSurfaceLayers(layers);
    };

    // index is -1 for the base, whose texture belongs to the material, not the terrain
    auto layerRow = [&](const char* label, const char* tooltip, const std::string& path, int index){
        terrainPropertyRow(label, tooltip);
        ImGui::PushID(label);
        ImGui::BeginGroup();

        const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        const float startY = ImGui::GetCursorPosY();
        const bool selected = brushActive && (index < 0 ? brushMode == TerrainBrushMode::PaintBase
                                                       : (brushMode == TerrainBrushMode::PaintLayer && selectedTextureLayer == index));
        const float thumbSize = drawAssetThumbnail(path, "##thumb", selected, previewHeight / buttonSize.y);
        // Marked on the preview, so the list shows at a glance which layers carry a surface
        if (index >= 0 && terrain.surfaceLayers[index].pbr){
            const float inset = ImGui::GetStyle().FramePadding.x * 0.5f;
            const ImVec2 thumbMax = ImGui::GetItemRectMax();
            const ImVec2 textSize = ImGui::CalcTextSize("PBR");
            const ImVec2 badgeMax(thumbMax.x - inset, thumbMax.y - inset);
            const ImVec2 badgeMin(badgeMax.x - textSize.x - inset * 2.0f, badgeMax.y - textSize.y);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(badgeMin, badgeMax, ImGui::GetColorU32(Theme::Colors::ButtonActivated), ImGui::GetStyle().FrameRounding);
            drawList->AddText(ImVec2(badgeMin.x + inset, badgeMin.y), ImGui::GetColorU32(ImGuiCol_Text), "PBR");
        }
        if (ImGui::IsItemClicked()){
            endStroke();
            brushMode = (index < 0) ? TerrainBrushMode::PaintBase : TerrainBrushMode::PaintLayer;
            if (index >= 0){
                selectedTextureLayer = index;
            }
            brushActive = !selected;
        }

        const char* emptyLabel = (index >= 0) ? "No texture" : "Material base color";
        const float detailsWidth = std::max(ImGui::CalcTextSize(emptyLabel).x, buttonSize.x * 3.0f + detailsSpacing.x * 2.0f);
        if (available >= thumbSize + spacing.x + detailsWidth){
            ImGui::SameLine(0.0f, spacing.x);
            const float detailsHeight = labelHeight + ((index >= 0) ? detailsSpacing.y + buttonSize.y : 0.0f);
            ImGui::SetCursorPosY(startY + std::floor(std::max(0.0f, (thumbSize - detailsHeight) * 0.5f)));
        }
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, detailsSpacing);
        // Give the label its own baseline and clip long filenames to the value column.
        if (ImGui::BeginChild("##texture_name", ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x), labelHeight), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)){
            if (path.empty()){
                ImGui::TextDisabled("%s", emptyLabel);
            }else{
                ImGui::TextUnformatted(fs::path(path).filename().string().c_str());
            }
        }
        ImGui::EndChild();
        if (!path.empty()){
            showTooltip(path.c_str());
        }
        if (index >= 0){
            if (iconButton(ICON_FA_FOLDER_OPEN, "browse", "Choose layer texture", false, buttonSize)){
                const std::string chosen = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
                if (!chosen.empty()){
                    assignLayer(index, chosen);
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(path.empty());
            if (iconButton(ICON_FA_XMARK, "clear", "Clear layer texture", false, buttonSize)){
                assignLayer(index, {});
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::ArrowButton("##toggle_layer", expandedLayers[index] ? ImGuiDir_Up : ImGuiDir_Down)){
                expandedLayers[index] = !expandedLayers[index];
            }
            showTooltip("Layer settings");
        }
        ImGui::PopStyleVar();
        ImGui::EndGroup();
        ImGui::EndGroup();

        // An image is the layer color; a material fills every map it can
        if (index >= 0 && ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
                if (!dropped.empty() && Util::isImageFile(dropped[0])){
                    assignLayer(index, dropped[0]);
                }else if (!dropped.empty() && Util::isMaterialFile(dropped[0])){
                    applyLayerMaterial(terrain, index, dropped[0]);
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (index >= 0 && expandedLayers[index]){
            drawLayerSurface(terrain, index);
        }
        ImGui::PopID();
    };

    std::string basePath;
    if (MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(selectedEntity)){
        if (mesh->numSubmeshes > 0){
            basePath = mesh->submeshes[0].material.baseColorTexture.getPath(0);
        }
    }

    layerRow("Base", "The material's base color. Painting it clears the blend map the selected layer sits on.", basePath, -1);

    // The renderer samples this many, so the list never offers a layer that cannot paint
    const int layerCount = std::min(static_cast<int>(terrain.surfaceLayers.size()), MAX_TERRAIN_LAYERS);
    for (int i = 0; i < layerCount; i++){
        const std::string label = "Layer " + std::to_string(i + 1);
        layerRow(label.c_str(), "Click to paint this layer. Every three layers share a blend map.",
                 terrain.surfaceLayers[i].colorTexture.getPath(0), i);
    }

    terrainPropertyRow("Layers", "Up to nine, three per blend map.");
    const ImVec2 commandSize = commandButtonSize();
    ImGui::BeginDisabled(layerCount >= MAX_TERRAIN_LAYERS);
    if (iconButton(ICON_FA_PLUS, "add_texture_layer", "Add texture layer", false, commandSize)){
        endStroke();
        std::vector<TerrainSurfaceLayer> layers = terrain.surfaceLayers;
        layers.resize(layers.size() + 1);
        setSurfaceLayers(layers);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(layerCount == 0);
    if (iconButton(ICON_FA_TRASH_CAN, "remove_texture_layer", "Remove the last texture layer", false, commandSize)){
        endStroke();
        std::vector<TerrainSurfaceLayer> layers = terrain.surfaceLayers;
        layers.pop_back();
        setSurfaceLayers(layers);
        selectedTextureLayer = std::clamp(selectedTextureLayer, 0, std::max(0, static_cast<int>(layers.size()) - 1));
    }
    ImGui::EndDisabled();
}

// Merging is for values dragged in the UI, so one drag is one undo step
bool editor::TerrainEditWindow::setSurfaceLayers(const std::vector<TerrainSurfaceLayer>& layers, bool merge){
    SceneProject* sceneProject = getTargetSceneProject();
    if (!sceneProject){
        return false;
    }
    auto* command = new PropertyCmd<std::vector<TerrainSurfaceLayer>>(
        project, sceneProject->id, selectedEntity, ComponentType::TerrainComponent, "surfaceLayers", layers);
    if (merge){
        CommandHandle::get(sceneProject->id)->addCommand(command);
    }else{
        CommandHandle::get(sceneProject->id)->addCommandNoMerge(command);
    }
    return true;
}

// A material is copied into the layer, not linked: the asset system has no way to keep a
// live reference here, and the layer only renders part of what a material can hold.
void editor::TerrainEditWindow::applyLayerMaterial(TerrainComponent& terrain, int index, const std::string& materialPath){
    if (index < 0){
        return;
    }

    Material material;
    try{
        material = Stream::decodeMaterial(DocumentFiles::read(project->getProjectPath() / materialPath, DocumentKind::Material).root);
    }catch (const std::exception& e){
        Out::error("Failed to read material '%s': %s", materialPath.c_str(), e.what());
        return;
    }

    endStroke();
    std::vector<TerrainSurfaceLayer> layers = terrain.surfaceLayers;
    if (index >= static_cast<int>(layers.size())){
        layers.resize(index + 1);
    }

    layers[index] = terrainLayerFromMaterial(material, layers[index]);
    setSurfaceLayers(layers);

    if (!material.emissiveTexture.empty() || material.emissiveFactor != Vector3(0, 0, 0)){
        Out::info("Terrain layers do not render emission, so it was left out of layer %d", index + 1);
    }
}

// The layer's own settings, as rows of the layer list. A color layer shows only what it
// can use; the rest appears once it is a PBR layer.
void editor::TerrainEditWindow::drawLayerSurface(TerrainComponent& terrain, int index){
    if (index < 0 || index >= static_cast<int>(terrain.surfaceLayers.size())){
        return;
    }

    const TerrainSurfaceLayer& layer = terrain.surfaceLayers[index];
    const ImVec2 buttonSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    auto edit = [&](const std::function<void(TerrainSurfaceLayer&)>& change, bool merge = false){
        endStroke();
        std::vector<TerrainSurfaceLayer> layers = terrain.surfaceLayers;
        change(layers[index]);
        setSurfaceLayers(layers, merge);
    };

    auto assignMap = [&](Texture TerrainSurfaceLayer::*map, const std::string& file){
        if (!project->isInsideAssetsPath(file)){
            Backend::getApp().registerOutsideAssetsAlert(file);
            return;
        }
        const std::string relative = project->normalizeToAssetsRelative(file).generic_string();
        edit([&](TerrainSurfaceLayer& target){ target.*map = Texture(relative); });
    };

    auto mapRow = [&](const char* label, const char* tooltip, Texture TerrainSurfaceLayer::*map){
        terrainPropertyRow(label, tooltip, true);
        ImGui::PushID(label);
        const std::string path = (layer.*map).getPath(0);
        const float fieldWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - (buttonSize.x + spacing) * 2.0f);

        // Read-only field rather than plain text: it lines up with the buttons beside it
        // and clips a long file name on its own
        char name[256];
        snprintf(name, sizeof(name), "%s", path.empty() ? "" : fs::path(path).filename().string().c_str());
        ImGui::SetNextItemWidth(fieldWidth);
        ImGui::InputTextWithHint("##map", "None", name, sizeof(name), ImGuiInputTextFlags_ReadOnly);
        if (!path.empty()){
            showTooltip(path.c_str());
        }
        if (ImGui::BeginDragDropTarget()){
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
                const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
                if (!dropped.empty() && Util::isImageFile(dropped[0])){
                    assignMap(map, dropped[0]);
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::SameLine();
        if (iconButton(ICON_FA_FOLDER_OPEN, "browse", "Choose map", false, buttonSize)){
            const std::string chosen = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
            if (!chosen.empty()){
                assignMap(map, chosen);
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(path.empty());
        if (iconButton(ICON_FA_XMARK, "clear", "Clear map", false, buttonSize)){
            edit([&](TerrainSurfaceLayer& target){ target.*map = Texture(); });
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    };

    auto factorRow = [&](const char* label, const char* tooltip, float TerrainSurfaceLayer::*factor, float maxValue){
        terrainPropertyRow(label, tooltip, true);
        ImGui::PushID(label);
        float value = layer.*factor;
        if (ImGui::SliderFloat("##factor", &value, 0.0f, maxValue, "%.2f", ImGuiSliderFlags_AlwaysClamp)){
            edit([&](TerrainSurfaceLayer& target){ target.*factor = value; }, true);
        }
        ImGui::PopID();
    };

    terrainPropertyRow("Enable PBR", "Off blends only this layer's color and leaves the rest to the terrain material. On gives the layer its own normal, roughness, metallic and occlusion.", true);
    bool pbr = layer.pbr;
    if (ImGui::Checkbox("##pbr", &pbr)){
        edit([&](TerrainSurfaceLayer& target){ target.pbr = pbr; });
    }

    terrainPropertyRow("Material", "Fill this layer from a material file. Emission and transparency are not copied.", true);
    if (ImGui::Button(ICON_FA_FILE_IMPORT " Copy from material", ImVec2(-1, 0))){
        const std::string chosen = FileDialogs::openFileDialog(project->getProjectPath().string(), FILE_DIALOG_MATERIAL);
        if (!chosen.empty()){
            std::error_code ec;
            const fs::path relative = fs::relative(chosen, project->getProjectPath(), ec);
            if (!ec){
                applyLayerMaterial(terrain, index, relative.lexically_normal().generic_string());
            }
        }
    }

    // Tiling rides on every layer, so only the maps below wait for the PBR surface
    if (layer.pbr){
        terrainPropertyRow("Tint", "Multiplies the layer color", true);
        Vector4 tint = layer.colorFactor;
        ui::ColorFieldOptions tintOptions;
        tintOptions.label = "Tint";
        const ui::EditPhase tintPhase = ui::colorField("##tint", tint, tintOptions);
        if (tintPhase == ui::EditPhase::Begin){
            if (SceneProject* target = getTargetSceneProject()) ui::ColorPickerWindow::beginEdit(CommandHandle::get(target->id), "Tint");
        }else if (tintPhase == ui::EditPhase::Changing){
            edit([&](TerrainSurfaceLayer& target){ target.colorFactor = tint; }, true);
        }

        mapRow("Normal", "Tangent-space normal map, projected with the layer", &TerrainSurfaceLayer::normalTexture);
        factorRow("Normal Strength", "How far the normal map tilts the surface", &TerrainSurfaceLayer::normalStrength, 2.0f);
        mapRow("Roughness", "Read from green, or from the only channel of a grayscale map", &TerrainSurfaceLayer::roughnessTexture);
        factorRow("Roughness Factor", "Multiplies the roughness map", &TerrainSurfaceLayer::roughnessFactor, 1.0f);
        mapRow("Metallic", "Read from blue, or from the only channel of a grayscale map", &TerrainSurfaceLayer::metallicTexture);
        factorRow("Metallic Factor", "Multiplies the metallic map", &TerrainSurfaceLayer::metallicFactor, 1.0f);
        mapRow("Occlusion", "Read from red. White is unoccluded", &TerrainSurfaceLayer::occlusionTexture);
        factorRow("Occlusion Strength", "How far the occlusion map darkens ambient light", &TerrainSurfaceLayer::occlusionStrength, 1.0f);
        mapRow("Height", "Biases the blend toward this layer where it is taller. It never moves geometry", &TerrainSurfaceLayer::heightTexture);
    }

    terrainPropertyRow("UV Scale", "Multiplies the shared detail tiling for this layer alone", true);
    Vector2 uvScale = layer.uvScale;
    if (ImGui::DragFloat2("##uv_scale", &uvScale.x, 0.01f, 0.0f, 0.0f, "%.2f")){
        edit([&](TerrainSurfaceLayer& target){ target.uvScale = uvScale; }, true);
    }

    terrainPropertyRow("UV Offset", "Shifts this layer inside its tile", true);
    Vector2 uvOffset = layer.uvOffset;
    if (ImGui::DragFloat2("##uv_offset", &uvOffset.x, 0.01f, 0.0f, 0.0f, "%.2f")){
        edit([&](TerrainSurfaceLayer& target){ target.uvOffset = uvOffset; }, true);
    }

    terrainPropertyRow("Stochastic Tiling", "Hides the repetition of this layer's tiled maps: hexagonal cells each read them at a random "
                       "offset and blend at their borders, every map of the layer alike. Up to three reads per map where "
                       "cells meet. Suits organic textures (grass, rock, sand); bricks and tiles lose their grid.", true);
    bool stochastic = layer.stochasticTiling;
    if (ImGui::Checkbox("##stochastic", &stochastic)){
        edit([&](TerrainSurfaceLayer& target){ target.stochasticTiling = stochastic; });
    }
    if (layer.stochasticTiling){
        auto stochasticRow = [&](const char* label, const char* tooltip, float TerrainSurfaceLayer::*field, float minValue, float maxValue,
                                 ImGuiSliderFlags flags){
            terrainPropertyRow(label, tooltip, true, true);
            ImGui::PushID(label);
            float value = layer.*field;
            if (ImGui::SliderFloat("##stochastic_param", &value, minValue, maxValue, "%.2f", flags | ImGuiSliderFlags_AlwaysClamp)){
                edit([&](TerrainSurfaceLayer& target){ target.*field = value; }, true);
            }
            ImGui::PopID();
        };
        stochasticRow("Cell Size", "Width of one cell in texture repeats. Smaller cells hide the repetition better but cut up "
                      "large features of the texture", &TerrainSurfaceLayer::stochasticCellSize, 0.25f, 4.0f, ImGuiSliderFlags_Logarithmic);
        stochasticRow("Blend Sharpness", "How narrow the blend between neighbouring cells is: low blends wide and soft, high "
                      "keeps more of each cell whole", &TerrainSurfaceLayer::stochasticBlendSharpness, 0.0f, 1.0f, ImGuiSliderFlags_None);
        stochasticRow("Contrast", "Keeps the texture's contrast where cells blend, which a plain blend washes out. Too high "
                      "shows the cell borders. 0 blends linearly", &TerrainSurfaceLayer::stochasticContrast, 0.0f, 1.0f, ImGuiSliderFlags_None);
        stochasticRow("Rotation", "Turns each cell by a random angle, up to +-180 degrees at 1. Leave at 0 for textures with a "
                      "direction (planks, stripes, light baked in) and for normal maps whose average normal leans",
                      &TerrainSurfaceLayer::stochasticRotation, 0.0f, 1.0f, ImGuiSliderFlags_None);
        // a normal map leaning on average looks tilted cell by cell once the cells turn (its
        // tilt measured once per file, off this thread)
        const std::string normalPath = layer.pbr && !layer.normalTexture.empty() ? layer.normalTexture.getPath(0) : std::string();
        if (layer.stochasticRotation > 0.0f && !normalPath.empty()){
            const fs::path normalFile = project->resolveAssetPath(editor::FileUtils::pathFromUtf8(normalPath));
            const NormalMapTiltResult tilt = NormalMapTilt::request(normalFile);
            if (tilt.ready && tilt.valid && tilt.degrees >= NormalMapTilt::WARNING_DEGREES){
                ImGui::TableNextRow();
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_PopupBg]));
                ImGui::TableSetColumnIndex(1);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.6f, 0.2f, 1.0f));
                ImGui::TextWrapped("%s: rotated tiles will look tilted. Keep Rotation at 0.", NormalMapTilt::describe(normalFile, tilt.degrees).c_str());
                ImGui::PopStyleColor();
                if (ImGui::SmallButton("Set Rotation to 0##stochastic_rotation")){
                    edit([&](TerrainSurfaceLayer& target){ target.stochasticRotation = 0.0f; });
                }
            }
        }
    }
    terrainPropertyRow("Hex Tiling", "Samples the flat projection on three randomly rotated copies on a hex grid: no visible tiling from a distance (three taps per map)", false);
    bool hexTiling = layer.hexTiling;
    if (ImGui::Checkbox("##hex_tiling", &hexTiling)){
        edit([&](TerrainSurfaceLayer& target){ target.hexTiling = hexTiling; }, false);
    }
    if (layer.hexTiling){
        terrainPropertyRow("Rotation", "How far a hex tile may turn: 1 a full turn, 0 none", true);
        float hexRotation = layer.hexRotation;
        if (ImGui::SliderFloat("##hex_rotation", &hexRotation, 0.0f, 1.0f, "%.2f")){
            edit([&](TerrainSurfaceLayer& target){ target.hexRotation = hexRotation; }, true);
        }
    }
}

void editor::TerrainEditWindow::drawFoliageMesh(const TerrainFoliageLayer& layer){
    terrainPropertyRow("Mesh", "Choose a model or drag one from the Project window.");
    ImGui::BeginGroup();
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float startY = ImGui::GetCursorPosY();
    const float thumbSize = drawAssetThumbnail(layer.meshPath, "##foliage_preview");
    if (available > thumbSize + ImGui::GetFrameHeight() * 4.0f){
        ImGui::SameLine();
        const float detailsHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight();
        ImGui::SetCursorPosY(startY + std::max(0.0f, (thumbSize - detailsHeight) * 0.5f));
    }
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    if (layer.meshPath.empty()){
        ImGui::TextDisabled("No mesh selected");
    }else{
        ImGui::TextUnformatted(fs::path(layer.meshPath).filename().string().c_str());
        showTooltip(layer.meshPath.c_str());
    }
    ImGui::PopTextWrapPos();

    auto assignMesh = [&](const fs::path& path){
        if (!path.empty() && !project->isInsideAssetsPath(path)){
            Backend::getApp().registerOutsideAssetsAlert(path.string());
            return;
        }

        endStroke();
        SceneProject* sceneProject = getTargetSceneProject();
        const std::string property = "foliageLayers[" + std::to_string(selectedFoliageLayer) + "].meshPath";
        const std::string meshPath = path.empty() ? std::string() : project->normalizeToAssetsRelative(path).generic_string();
        // An FBX loads from its import, created here the first time
        if (!project->prepareModelFile(meshPath)){
            return;
        }
        CommandHandle::get(sceneProject->id)->addCommandNoMerge(new PropertyCmd<std::string>(
            project, sceneProject->id, selectedEntity, ComponentType::TerrainComponent, property, meshPath));
    };
    const ImVec2 buttonSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    if (iconButton(ICON_FA_FOLDER_OPEN, "browse_foliage_mesh", "Choose foliage mesh", false, buttonSize)){
        const std::string path = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_MODEL);
        if (!path.empty()){
            assignMesh(path);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(layer.meshPath.empty());
    if (iconButton(ICON_FA_XMARK, "clear_foliage_mesh", "Clear foliage mesh", false, buttonSize)){
        assignMesh({});
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::EndGroup();
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            if (!dropped.empty() && Util::isModelFile(dropped[0])){
                assignMesh(dropped[0]);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void editor::TerrainEditWindow::setBrushMask(const std::string& path){
    endStroke();
    brushMaskPath = path;
    brushMaskLoadedPath.clear();
    brushMaskPixels.clear();
    brushMaskWidth = 0;
    brushMaskHeight = 0;
}

// Decoded once per path: the brush reads a plain float field, whatever the file was.
bool editor::TerrainEditWindow::loadBrushMask(){
    if (brushMaskPath.empty()){
        return false;
    }
    if (brushMaskLoadedPath == brushMaskPath){
        return !brushMaskPixels.empty();
    }

    brushMaskLoadedPath = brushMaskPath;
    brushMaskPixels.clear();
    brushMaskWidth = 0;
    brushMaskHeight = 0;

    TextureData data;
    if (!TerrainMapUtils::loadTextureDataFromPath(project, brushMaskPath, data)){
        Out::error("Could not load brush mask '%s'", brushMaskPath.c_str());
        return false;
    }

    const unsigned char* pixels = static_cast<const unsigned char*>(data.getData());
    const int channels = data.getChannels();
    const int bytesPerChannel = TextureData::getBytesPerChannel(data.getColorFormat());
    brushMaskWidth = data.getWidth();
    brushMaskHeight = data.getHeight();
    brushMaskPixels.resize(static_cast<size_t>(brushMaskWidth) * static_cast<size_t>(brushMaskHeight));
    for (size_t i = 0; i < brushMaskPixels.size(); i++){
        brushMaskPixels[i] = terrain::heightTexelAt(pixels, i, channels, bytesPerChannel);
    }

    return true;
}

void editor::TerrainEditWindow::drawBrushMask(){
    terrainPropertyRow("Mask", "Grayscale texture shaping the brush, and the relief the Stamp Terrain tool lays down. Drag one from the Project window.");
    ImGui::BeginGroup();
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float startY = ImGui::GetCursorPosY();
    const float thumbSize = drawAssetThumbnail(brushMaskPath, "##brush_mask_preview");
    if (available > thumbSize + ImGui::GetFrameHeight() * 4.0f){
        ImGui::SameLine();
        const float detailsHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight();
        ImGui::SetCursorPosY(startY + std::max(0.0f, (thumbSize - detailsHeight) * 0.5f));
    }
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    if (brushMaskPath.empty()){
        ImGui::TextDisabled("No mask");
    }else{
        ImGui::TextUnformatted(fs::path(brushMaskPath).filename().string().c_str());
        showTooltip(brushMaskPath.c_str());
    }
    ImGui::PopTextWrapPos();

    auto assignMask = [&](const fs::path& path){
        if (path.empty()){
            setBrushMask({});
            return;
        }
        if (!project->isInsideAssetsPath(path)){
            Backend::getApp().registerOutsideAssetsAlert(path.string());
            return;
        }
        setBrushMask(project->normalizeToAssetsRelative(path).generic_string());
    };

    const ImVec2 buttonSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    if (iconButton(ICON_FA_FOLDER_OPEN, "browse_brush_mask", "Choose brush mask", false, buttonSize)){
        const std::string path = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_IMAGE);
        if (!path.empty()){
            assignMask(path);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(brushMaskPath.empty());
    if (iconButton(ICON_FA_XMARK, "clear_brush_mask", "Clear brush mask", false, buttonSize)){
        assignMask({});
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::EndGroup();
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            if (!dropped.empty() && Util::isImageFile(dropped[0])){
                assignMask(dropped[0]);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void editor::TerrainEditWindow::drawPlacementAsset(){
    terrainPropertyRow("Asset", "Model or Prefab the Paint Trees brush places. Drag one from the Project window.");
    ImGui::BeginGroup();
    const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float startY = ImGui::GetCursorPosY();
    const float thumbSize = drawAssetThumbnail(placeAssetPath, "##place_preview");
    if (available > thumbSize + ImGui::GetFrameHeight() * 4.0f){
        ImGui::SameLine();
        const float detailsHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight();
        ImGui::SetCursorPosY(startY + std::max(0.0f, (thumbSize - detailsHeight) * 0.5f));
    }
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    if (placeAssetPath.empty()){
        ImGui::TextDisabled("No asset selected");
    }else{
        ImGui::TextUnformatted(fs::path(placeAssetPath).filename().string().c_str());
        showTooltip(placeAssetPath.c_str());
    }
    ImGui::PopTextWrapPos();

    // Prefabs are stored relative to the project, models relative to the assets directory
    auto assignAsset = [&](const fs::path& path){
        endStroke();
        instancingRejectedAsset.clear();
        if (path.empty()){
            placeAssetPath.clear();
            return;
        }
        if (Util::isPrefabFile(path.string())){
            std::error_code ec;
            const fs::path relative = fs::relative(path, project->getProjectPath(), ec);
            if (ec || relative.empty() || *relative.begin() == ".."){
                Backend::getApp().registerOutsideAssetsAlert(path.string());
                return;
            }
            placeAssetPath = relative.generic_string();
            return;
        }
        if (!project->isInsideAssetsPath(path)){
            Backend::getApp().registerOutsideAssetsAlert(path.string());
            return;
        }
        const std::string assetPath = project->normalizeToAssetsRelative(path).generic_string();
        if (project->prepareModelFile(assetPath)){
            placeAssetPath = assetPath;
        }
    };

    const ImVec2 buttonSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    if (iconButton(ICON_FA_FOLDER_OPEN, "browse_place_asset", "Choose model or Prefab", false, buttonSize)){
        const std::string path = FileDialogs::openFileDialog(project->getAssetsPath().string(), FILE_DIALOG_MODEL | FILE_DIALOG_PREFAB);
        if (!path.empty()){
            assignAsset(path);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(placeAssetPath.empty());
    if (iconButton(ICON_FA_XMARK, "clear_place_asset", "Clear placement asset", false, buttonSize)){
        assignAsset({});
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::EndGroup();
    if (ImGui::BeginDragDropTarget()){
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")){
            const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            if (!dropped.empty() && (Util::isModelFile(dropped[0]) || Util::isPrefabFile(dropped[0]))){
                assignAsset(dropped[0]);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

// Unity's names for the sculpt tools, shown as the Paint Terrain title
static const char* sculptToolName(editor::TerrainBrushMode mode){
    switch (mode){
        case editor::TerrainBrushMode::Raise:
        case editor::TerrainBrushMode::Lower: return "Raise or Lower Terrain";
        case editor::TerrainBrushMode::Smooth: return "Smooth Height";
        case editor::TerrainBrushMode::Flatten: return "Set Height";
        case editor::TerrainBrushMode::Sharpen: return "Sharpen Peaks";
        case editor::TerrainBrushMode::Noise: return "Noise";
        case editor::TerrainBrushMode::Terrace: return "Terrace";
        case editor::TerrainBrushMode::Stamp: return "Stamp Terrain";
        case editor::TerrainBrushMode::Erode: return "Erosion";
        case editor::TerrainBrushMode::Ramp: return "Ramp";
        case editor::TerrainBrushMode::CutHole:
        case editor::TerrainBrushMode::FillHole: return "Paint Holes";
        default: return "Paint Terrain";
    }
}

static editor::TerrainToolCategory toolCategoryFor(editor::TerrainBrushMode mode){
    switch (mode){
        case editor::TerrainBrushMode::PaintBase:
        case editor::TerrainBrushMode::PaintLayer: return editor::TerrainToolCategory::Texture;
        case editor::TerrainBrushMode::PaintDensity:
        case editor::TerrainBrushMode::EraseDensity: return editor::TerrainToolCategory::Details;
        case editor::TerrainBrushMode::PlaceObject:
        case editor::TerrainBrushMode::EraseObject: return editor::TerrainToolCategory::Trees;
        default: return editor::TerrainToolCategory::Sculpt;
    }
}

void editor::TerrainEditWindow::show(){
    // the saved settings before the first frame writes the members back (opened from the menu or
    // the layout, not only from the Inspector)
    loadSettings();
    // T1 R38: stamps a held, still pointer left waiting run in this frame's budget
    if (stroke.active && stroke.session && stroke.session->hasQueued()){
        if (SceneProject* sceneProject = project->getScene(stroke.sceneId)){
            if (strokeTerrainIsThere(sceneProject)){
                stroke.session->run(STROKE_FRAME_BUDGET_SECONDS);
                stroke.session->flush();
            }
        }
    }
    if (windowOpen){
        updateTargetFromSelection();
    }
    updateFoliagePreview();
    if (!windowOpen){
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 27.0f, ImGui::GetFontSize() * 44.0f), ImGuiCond_FirstUseEver);
    if (focusRequested){
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)){
        ImGui::End();
        if (!windowOpen){
            setOpen(false);
        }
        return;
    }

    SceneProject* sceneProject = getTargetSceneProject();
    if (!hasValidTarget(sceneProject)){
        brushActive = false;
        endStroke();
        ImGui::TextWrapped("%s", project->isAnyScenePlaying() ? "Stop the scene to edit terrain." : "Select a Terrain in the Hierarchy to sculpt, paint, or place details and trees.");
        // T1 R58: the empty state makes one
        SceneProject* selected = project->getSelectedScene();
        if (!project->isAnyScenePlaying() && selected && selected->sceneType == SceneType::SCENE_3D && ImGui::Button(ICON_FA_MOUNTAIN " Create Terrain...")){
            CreateTerrainDialog::instance().open(project, selected->id, NULL_ENTITY);
        }
        ImGui::End();
        if (!windowOpen){
            setOpen(false);
        }
        return;
    }

    TerrainComponent& terrain = sceneProject->scene->getComponent<TerrainComponent>(selectedEntity);
    ImGui::TextWrapped(ICON_FA_MOUNTAIN "  %s", sceneProject->scene->getEntityName(selectedEntity).c_str());
    // T1 R72: what is wrong, with its fix, before anything else
    drawIssuesBanner(sceneProject, terrain);
    ImGui::Spacing();
    const int layerCount = static_cast<int>(terrain.foliageLayers.size());
    selectedFoliageLayer = std::clamp(selectedFoliageLayer, 0, std::max(0, layerCount - 1));
    selectedTextureLayer = std::clamp(selectedTextureLayer, 0, std::max(0, static_cast<int>(terrain.surfaceLayers.size()) - 1));
    const ImVec2 buttonSize(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    const ImVec2 commandSize = commandButtonSize();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    auto brushButton = [&](TerrainBrushMode mode, const char* icon, const char* id, const char* tooltip){
        const bool selected = brushActive && brushMode == mode;
        if (iconButton(icon, id, tooltip, selected, commandSize)){
            endStroke();
            brushMode = mode;
            brushActive = !selected;
        }
    };

    // Unity's terrain tool bar: one tool at a time, then its settings and the brush
    struct TerrainTool{
        TerrainToolCategory category;
        const char* icon;
        const char* name;
        const char* description;
    };
    static const TerrainTool terrainTools[] = {
        {TerrainToolCategory::Sculpt, ICON_FA_MOUNTAIN_SUN, "Paint Terrain",
         "Sculpt the heightmap: raise or lower, set height, smooth, stamp and more. Ctrl inverts, Shift smooths."},
        {TerrainToolCategory::Texture, ICON_FA_PAINTBRUSH, "Paint Texture",
         "Paint texture layers onto the terrain surface."},
        {TerrainToolCategory::Details, ICON_FA_SEEDLING, "Paint Details",
         "Paint grass and other detail meshes by density. Ctrl erases."},
        {TerrainToolCategory::Trees, ICON_FA_TREE, "Paint Trees",
         "Place trees, rocks and other models or Prefabs along the drag. Ctrl erases."},
        {TerrainToolCategory::Settings, ICON_FA_GEAR, "Terrain Settings",
         "Size and height, maps, Import Raw and Export Raw, collision, runtime edits."},
    };
    if (brushActive && toolCategoryFor(brushMode) != toolCategory){
        // A brush chosen elsewhere (shortcut or AI) brings its tool's settings up
        toolCategory = toolCategoryFor(brushMode);
    }
    {
        const float toolSize = std::round(ImGui::GetFrameHeight() * 1.6f);
        const float toolsWidth = toolSize * IM_ARRAYSIZE(terrainTools) + spacing * (IM_ARRAYSIZE(terrainTools) - 1);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - toolsWidth) * 0.5f));
        for (int i = 0; i < IM_ARRAYSIZE(terrainTools); ++i){
            const TerrainTool& tool = terrainTools[i];
            if (i > 0){
                ImGui::SameLine();
            }
            const bool selected = toolCategory == tool.category;
            ImGui::PushStyleColor(ImGuiCol_Button, selected ? Theme::Colors::ButtonActivated : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
            const bool clicked = Widgets::iconButton((std::string("##terrain_tool_") + tool.name).c_str(), tool.icon, ImVec2(toolSize, toolSize));
            ImGui::PopFont();
            ImGui::PopStyleColor();
            showTooltip(tool.name, ImGuiHoveredFlags_AllowWhenDisabled);
            if (clicked && toolCategory != tool.category){
                endStroke();
                toolCategory = tool.category;
                brushActive = false;
            }
            // T5 R46: Paint Trees with Place As Tree is its brush, as Unity's: choosing it arms it
            if (clicked && tool.category == TerrainToolCategory::Trees && foliagePanel->placesTrees()){
                brushMode = TerrainBrushMode::PlaceObject;
                brushActive = true;
            }
        }
        ImGui::Spacing();
        const TerrainTool* currentTool = &terrainTools[0];
        for (const TerrainTool& tool : terrainTools){
            if (tool.category == toolCategory){
                currentTool = &tool;
            }
        }
        const char* toolTitle = currentTool->name;
        if (toolCategory == TerrainToolCategory::Sculpt){
            toolTitle = sculptToolName(brushMode);
        }
        const ImVec2 titlePos = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(ImVec2(titlePos.x + 1.0f, titlePos.y), ImGui::GetColorU32(ImGuiCol_Text), toolTitle);
        ImGui::TextUnformatted(toolTitle);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", currentTool->description);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // T3 R41: the Holes overlay comes on with the brush, and goes with it
    {
        static bool overlayFromBrush = false;
        if (isHoleBrush() && terrain::TerrainQualitySettings::getDebugView() == terrain::TerrainDebugView::SHADED && !overlayFromBrush){
            terrain::TerrainQualitySettings::setDebugView(terrain::TerrainDebugView::HOLES);
            overlayFromBrush = true;
        }else if (!isHoleBrush() && overlayFromBrush){
            terrain::TerrainQualitySettings::setDebugView(terrain::TerrainDebugView::SHADED);
            overlayFromBrush = false;
        }
        if (isHoleBrush()){
            drawHoleBrushInfo(terrain);
        }
    }

    if (toolCategory == TerrainToolCategory::Sculpt && beginTerrainProperties("sculpt_properties")){
        drawMapSettings(TerrainMapTarget::HeightMap, "Heightmap", heightMapResolution);
        terrainPropertyRow("Tool");
        ImGui::BeginDisabled(terrain.heightMap.empty());
        brushButton(TerrainBrushMode::Raise, ICON_FA_ARROW_UP, "terrain_raise", "Raise Terrain (Ctrl lowers, Shift smooths)");
        ImGui::SameLine();
        brushButton(TerrainBrushMode::Lower, ICON_FA_ARROW_DOWN, "terrain_lower", "Lower Terrain (Ctrl raises, Shift smooths)");
        ImGui::SameLine();
        brushButton(TerrainBrushMode::Smooth, ICON_FA_WATER, "terrain_smooth", "Smooth Height");
        ImGui::SameLine();
        brushButton(TerrainBrushMode::Flatten, ICON_FA_GRIP_LINES, "terrain_flatten", "Set Height (Shift smooths)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!hasAlternate());
        // R56: Lift Floors (Ctrl: Flatten Peaks) for Raise and Lower, Pick and Flatten for Set Height
        if (iconButton(ICON_FA_ARROWS_UP_TO_LINE, "terrain_alternate",
                       brushMode == TerrainBrushMode::Flatten ? "Alternate (C): Pick and Flatten, from the height under the click"
                                                              : "Alternate (C): Lift Floors up to the height under the click (Ctrl: Flatten Peaks)",
                       isAlternateOn(), commandButtonSize())){
            toggleAlternate();
        }
        ImGui::EndDisabled();

        brushButton(TerrainBrushMode::Sharpen, ICON_FA_CHART_LINE, "terrain_sharpen", "Sharpen terrain, pulling ridges back out of a smoothed surface");
        ImGui::SameLine();
        brushButton(TerrainBrushMode::Noise, ICON_FA_HILL_ROCKSLIDE, "terrain_noise", "Break up the surface with fractal noise");
        ImGui::SameLine();
        brushButton(TerrainBrushMode::Terrace, ICON_FA_STAIRS, "terrain_terrace", "Step the terrain into flat terraces");
        ImGui::SameLine();
        ImGui::BeginDisabled(brushMaskPath.empty());
        brushButton(TerrainBrushMode::Stamp, ICON_FA_STAMP, "terrain_stamp", "Stamp Terrain: press the brush mask into the terrain as relief");
        ImGui::EndDisabled();
        ImGui::SameLine();
        brushButton(TerrainBrushMode::Erode, ICON_FA_DROPLET, "terrain_erode", "Erode with water and slope, cutting channels and settling scree");
        ImGui::SameLine();
        brushButton(TerrainBrushMode::Ramp, ICON_FA_ROAD, "terrain_ramp", "Drag a ramp between the height where the stroke starts and where it ends");
        brushButton(TerrainBrushMode::CutHole, ICON_FA_CIRCLE_NOTCH, "terrain_cut_hole", "Cut Hole: no falloff or strength (Ctrl fills)");
        ImGui::SameLine();
        brushButton(TerrainBrushMode::FillHole, ICON_FA_CIRCLE, "terrain_fill_hole", "Fill Hole (Ctrl cuts)");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }

    if (toolCategory == TerrainToolCategory::Texture && beginTerrainProperties("texture_paint_properties")){
        const int blendMapIndex = selectedTextureLayer / 3;
        drawMapSettings(TerrainMapRef(TerrainMapTarget::BlendMap, blendMapIndex), "Blendmap", blendMapResolution);
        const bool blendMapReady = blendMapIndex < static_cast<int>(terrain.blendMaps.size()) &&
                                   !terrain.blendMaps[blendMapIndex].empty();
        ImGui::BeginDisabled(!blendMapReady);
        drawTextureLayers(terrain);
        terrainPropertyRow("Normalize", "Fade the other layers sharing this blend map. Layers on the other maps keep their weight.");
        ImGui::BeginDisabled(brushMode == TerrainBrushMode::PaintBase);
        ImGui::Checkbox("##normalize_blend", &normalizeBlendPaint);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::EndTable();
        drawPaintByRules(sceneProject, terrain);
    }

    if (toolCategory == TerrainToolCategory::Details && beginTerrainProperties("foliage_properties")){
        terrainPropertyRow("Layer");
        auto layerLabel = [&](int index){
            const std::string& path = terrain.foliageLayers[index].meshPath;
            return std::to_string(index + 1) + "  " + (path.empty() ? "Empty layer" : fs::path(path).stem().string());
        };
        const std::string preview = layerCount ? layerLabel(selectedFoliageLayer) : "No layers";
        ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x - (buttonSize.x + spacing) * 2.0f));
        ImGui::BeginDisabled(layerCount == 0);
        if (ImGui::BeginCombo("##foliage_layer", preview.c_str())){
            for (int i = 0; i < layerCount; ++i){
                const bool selected = i == selectedFoliageLayer;
                if (ImGui::Selectable(layerLabel(i).c_str(), selected)){
                    endStroke();
                    selectedFoliageLayer = i;
                }
                if (selected){
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        bool layersChanged = false;
        ImGui::SameLine();
        if (iconButton(ICON_FA_PLUS, "add_foliage_layer", "Add detail layer", false, buttonSize)){
            endStroke();
            std::vector<TerrainFoliageLayer> layers = terrain.foliageLayers;
            layers.emplace_back();
            if (setFoliageLayers(layers)){
                selectedFoliageLayer = static_cast<int>(layers.size()) - 1;
                layersChanged = true;
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(layerCount == 0 || layersChanged);
        if (iconButton(ICON_FA_TRASH_CAN, "remove_foliage_layer", "Remove detail layer", false, buttonSize)){
            endStroke();
            std::vector<TerrainFoliageLayer> layers = terrain.foliageLayers;
            layers.erase(layers.begin() + selectedFoliageLayer);
            layersChanged = setFoliageLayers(layers);
            if (layersChanged){
                brushActive = false;
            }
        }
        ImGui::EndDisabled();

        if (layerCount > 0 && !layersChanged){
            TerrainFoliageLayer layer = terrain.foliageLayers[selectedFoliageLayer];
            drawFoliageMesh(layer);
            drawMapSettings(TerrainMapRef(TerrainMapTarget::DensityMap, selectedFoliageLayer), "Density map", densityMapResolution);
            terrainPropertyRow("Paint");
            ImGui::BeginDisabled(terrain.foliageLayers[selectedFoliageLayer].densityMap.empty());
            brushButton(TerrainBrushMode::PaintDensity, ICON_FA_BRUSH, "terrain_paint_density", "Paint detail density (Ctrl erases)");
            ImGui::SameLine();
            brushButton(TerrainBrushMode::EraseDensity, ICON_FA_ERASER, "terrain_erase_density", "Erase detail density (Ctrl paints)");
            ImGui::EndDisabled();

            terrainPropertyRow("Density", "Instances per square world unit at full painted density.");
            if (ImGui::DragFloat("##foliage_density", &layer.density, 0.05f, 0.0f, 128.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)){
                // the detail rules' range (the engine draws at most 8192 instances per 8 m cell)
                setFoliageLayerProperty("density", std::clamp(layer.density, 0.0f, 128.0f));
            }
            const TerrainFoliageLayer& currentLayer = terrain.foliageLayers[selectedFoliageLayer];
            terrainPropertyRow("Min/Max Width", "Minimum and maximum random scale per instance.");
            if (ImGui::DragFloatRange2("##foliage_scale", &layer.minScale, &layer.maxScale, 0.01f, 0.01f, 20.0f, "%.2f", "%.2f", ImGuiSliderFlags_AlwaysClamp)){
                layer.minScale = std::max(0.01f, layer.minScale);
                layer.maxScale = std::max(layer.minScale, layer.maxScale);
                if (layer.minScale != currentLayer.minScale){
                    setFoliageLayerProperty("minScale", layer.minScale);
                }
                if (layer.maxScale != currentLayer.maxScale){
                    setFoliageLayerProperty("maxScale", layer.maxScale);
                }
            }
            terrainPropertyRow("Slope range", "Allowed ground slope in degrees, from flat (0) to vertical (90).");
            if (ImGui::DragFloatRange2("##foliage_slope", &layer.minSlope, &layer.maxSlope, 0.5f, 0.0f, 90.0f, "%.0f deg", "%.0f deg", ImGuiSliderFlags_AlwaysClamp)){
                if (layer.minSlope != currentLayer.minSlope){
                    setFoliageLayerProperty("minSlope", layer.minSlope);
                }
                if (layer.maxSlope != currentLayer.maxSlope){
                    setFoliageLayerProperty("maxSlope", layer.maxSlope);
                }
            }
            terrainPropertyRow("Height range", "Allowed ground height, from the terrain base (0) to its max height (1).");
            if (ImGui::DragFloatRange2("##foliage_height", &layer.minHeight, &layer.maxHeight, 0.01f, 0.0f, 1.0f, "%.2f", "%.2f", ImGuiSliderFlags_AlwaysClamp)){
                if (layer.minHeight != currentLayer.minHeight){
                    setFoliageLayerProperty("minHeight", layer.minHeight);
                }
                if (layer.maxHeight != currentLayer.maxHeight){
                    setFoliageLayerProperty("maxHeight", layer.maxHeight);
                }
            }
            terrainPropertyRow("Rotation", "Random yaw as a share of a full turn.");
            float rotation = layer.rotationJitter * 100.0f;
            if (ImGui::SliderFloat("##foliage_rotation", &rotation, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)){
                setFoliageLayerProperty("rotationJitter", rotation / 100.0f);
            }
            terrainPropertyRow("Normal alignment", "Blend from upright (0%) to aligned with the terrain normal (100%).");
            float alignment = layer.alignToNormal * 100.0f;
            if (ImGui::SliderFloat("##foliage_align", &alignment, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)){
                setFoliageLayerProperty("alignToNormal", alignment / 100.0f);
            }
            terrainPropertyRow("Draw range", "Instances show between these distances, in world units, fading in after Min and out before Max. "
                               "A copy of the layer with a cheaper mesh whose Min equals this layer's Max takes over with a cross-fade. "
                               "Scaled by Project Settings > Quality > Foliage Distance. Ignored while editing terrain; restored when editing ends or play starts.");
            if (ImGui::DragFloatRange2("##foliage_distance", &layer.minDrawDistance, &layer.drawDistance, 1.0f, 0.0f, 500.0f, "Min %.0f", "Max %.0f", ImGuiSliderFlags_AlwaysClamp)){
                if (layer.minDrawDistance != currentLayer.minDrawDistance){
                    setFoliageLayerProperty("minDrawDistance", std::clamp(layer.minDrawDistance, 0.0f, std::max(1.0f, layer.drawDistance)));
                }
                if (layer.drawDistance != currentLayer.drawDistance){
                    setFoliageLayerProperty("drawDistance", std::max(1.0f, layer.drawDistance));
                }
            }
            terrainPropertyRow("Cast shadows", "Turn off for grass-scale scatter so it skips the shadow cascades.");
            if (ImGui::Checkbox("##foliage_cast_shadows", &layer.castShadows)){
                setFoliageLayerProperty("castShadows", layer.castShadows);
            }
            terrainPropertyRow("Seed", "Change the seed to reshuffle instance positions.");
            if (ImGui::InputScalar("##foliage_seed", ImGuiDataType_U32, &layer.seed)){
                setFoliageLayerProperty("seed", layer.seed);
            }
        }
        ImGui::EndTable();
        // T5 R47: its Unity rows, Show Density, the settings and the stats
        foliagePanel->drawDetails(sceneProject, selectedEntity, terrain, selectedFoliageLayer);
    }

    // T5: the prototypes, the tree brush and what goes with them; GameObjects are below it
    const bool placesTrees = toolCategory == TerrainToolCategory::Trees && foliagePanel->drawTrees(sceneProject, selectedEntity, terrain, brushSize);

    if (toolCategory == TerrainToolCategory::Trees && !placesTrees && beginTerrainProperties("object_properties")){
        drawPlacementAsset();
        terrainPropertyRow("Place", "Placed GameObjects are parented to the terrain, so they show in the Hierarchy and can be transformed one by one.");
        ImGui::BeginDisabled(placeAssetPath.empty());
        brushButton(TerrainBrushMode::PlaceObject, ICON_FA_TREE, "terrain_place_object", "Place along the drag (Ctrl erases)");
        ImGui::EndDisabled();
        ImGui::SameLine();
        brushButton(TerrainBrushMode::EraseObject, ICON_FA_ERASER, "terrain_erase_object", "Erase the terrain's placed GameObjects under the brush (Ctrl places)");

        terrainPropertyRow("Instanced", "Batch every placement of this asset into one draw. Instances can be moved but carry no components of their own, so turn this off for props that need Colliders, scripts or animation.");
        ImGui::BeginDisabled(Util::isPrefabFile(placeAssetPath));
        ImGui::Checkbox("##place_instanced", &placeInstanced);
        ImGui::EndDisabled();
        terrainPropertyRow("Spacing", "Closest two placements are allowed to get, in world units.");
        if (ImGui::DragFloat("##place_spacing", &placeSpacing, 0.05f, MIN_PLACE_SPACING, 100.0f, "%.2f")){
            placeSpacing = std::clamp(placeSpacing, MIN_PLACE_SPACING, 100.0f);
        }
        terrainPropertyRow("Scale range", "Random scale per placement. Left at 1, a model keeps the scale its file authored.");
        if (ImGui::DragFloatRange2("##place_scale", &placeMinScale, &placeMaxScale, 0.01f, 0.01f, 20.0f, "%.2f", "%.2f")){
            placeMinScale = std::max(0.01f, placeMinScale);
            placeMaxScale = std::max(placeMinScale, placeMaxScale);
        }
        terrainPropertyRow("Rotation", "Random yaw as a share of a full turn.");
        float placeRotation = placeRotationJitter * 100.0f;
        if (ImGui::SliderFloat("##place_rotation", &placeRotation, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)){
            placeRotationJitter = placeRotation / 100.0f;
        }
        terrainPropertyRow("Normal alignment", "Blend from upright (0%) to aligned with the terrain normal (100%).");
        float placeAlignment = placeAlignToNormal * 100.0f;
        if (ImGui::SliderFloat("##place_align", &placeAlignment, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)){
            placeAlignToNormal = placeAlignment / 100.0f;
        }
        ImGui::EndTable();
    }

    const bool placementBrush = isPlacementBrush();
    Texture* brushTexture = placementBrush ? nullptr : TerrainMapUtils::findTexture(terrain, getBrushMapRef());
    // the tree brush needs a prototype, not the GameObject brush's asset (T5 R46)
    const bool treeBrushReady = foliagePanel->placesTrees() && (brushMode == TerrainBrushMode::EraseObject || foliagePanel->isReady(terrain));
    const bool brushTargetAvailable = placementBrush ? (foliagePanel->placesTrees() ? treeBrushReady : isPlacementReady()) : (brushTexture && !brushTexture->empty());
    if (brushActive && !brushTargetAvailable){
        brushActive = false;
        endStroke();
    }
    if (toolCategory == TerrainToolCategory::Settings){
        drawSettingsTool(sceneProject, terrain);
    }else if (Theme::foldoutHeader("Brushes", ImGuiTreeNodeFlags_DefaultOpen) && beginTerrainProperties("brush_properties")){
        ImGui::BeginDisabled(!brushTargetAvailable);
        terrainPropertyRow("Shape");
        if (iconButton(ICON_FA_CIRCLE, "shape_circle", "Circle brush", brushShape == TerrainBrushShape::Circle, commandSize)){
            brushShape = TerrainBrushShape::Circle;
        }
        ImGui::SameLine();
        if (iconButton(ICON_FA_SQUARE, "shape_square", "Square brush", brushShape == TerrainBrushShape::Square, commandSize)){
            brushShape = TerrainBrushShape::Square;
        }
        ImGui::BeginDisabled(placementBrush);
        terrainPropertyRow("Falloff", "How brush strength fades from its center to its edge.");
        int falloff = static_cast<int>(brushFalloff);
        if (ImGui::Combo("##falloff", &falloff, "Smooth\0Linear\0Constant\0")){
            brushFalloff = static_cast<TerrainBrushFalloff>(falloff);
        }
        ImGui::EndDisabled();
        const char* sizeTooltip = placementBrush ?
            "How far placements scatter from the cursor, in world units. Adjust with [ and ] while painting." :
            "Brush size in world units. Adjust with [ and ] while painting.";
        terrainPropertyRow("Size", sizeTooltip);
        // R60: the slider ends at max(50, size / 4); a typed value reaches the whole terrain
        float minSize = MIN_BRUSH_SIZE, maxSize = MAX_BRUSH_SIZE;
        brushSizeLimits(terrain, minSize, maxSize);
        brushSize = std::clamp(brushSize, minSize, maxSize);
        UIUtils::sliderFloatInput("##brush_size", &brushSize, minSize, std::min(maxSize, std::max(50.0f, terrain.terrainSize * 0.25f)), "%.2f",
                                  ImGuiSliderFlags_Logarithmic);
        brushSize = std::clamp(brushSize, minSize, maxSize);
        ImGui::BeginDisabled(placementBrush);
        terrainPropertyRow("Strength", "Brush flow while held. Adjust with Shift+[ and Shift+] while painting.");
        float strength = std::clamp(brushStrength, MIN_BRUSH_STRENGTH, MAX_BRUSH_STRENGTH) * 100.0f;
        if (UIUtils::sliderFloatInput("##brush_strength", &strength, MIN_BRUSH_STRENGTH * 100.0f, MAX_BRUSH_STRENGTH * 100.0f, "%.0f%%")){
            brushStrength = strength / 100.0f;
        }
        ImGui::EndDisabled();
        // T1 R66: the Slope Filter, Paint Texture's unless All Tools
        if (isBlendBrush() || (!placementBrush && filterAllTools)){
            terrainPropertyRow("Slope Filter", "Paint only where the slope and height are in range.");
            ImGui::Checkbox("##paint_mask", &paintUseMask);
            ImGui::BeginDisabled(!paintUseMask);
            terrainPropertyRow("Invert Slope", "Paint where the slope is outside its range instead (Y).");
            ImGui::Checkbox("##filter_invert", &filterInvert);
            terrainPropertyRow("Invert Height", "Paint where the height is outside its range instead.");
            ImGui::Checkbox("##filter_invert_height", &heightFilterInvert);
            terrainPropertyRow("All Tools", "The filter also limits sculpting and details.");
            ImGui::Checkbox("##filter_all_tools", &filterAllTools);
            // R63: an eyedropper on each end takes it from the next click on the terrain
            auto pickButton = [&](const char* id, const char* tooltip, TerrainPick target){
                if (iconButton(ICON_FA_EYE_DROPPER, id, tooltip, pickTarget == target, commandSize)){
                    pickTarget = pickTarget == target ? TerrainPick::None : target;
                }
                ImGui::SameLine();
            };
            terrainPropertyRow("Slope range", "Allowed ground slope in degrees, from flat (0) to vertical (90).");
            pickButton("pick_slope_min", "Pick the lowest slope from the terrain", TerrainPick::SlopeMin);
            pickButton("pick_slope_max", "Pick the highest slope from the terrain", TerrainPick::SlopeMax);
            ImGui::DragFloatRange2("##paint_slope", &paintMinSlope, &paintMaxSlope, 0.5f, 0.0f, 90.0f, "%.0f deg", "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
            terrainPropertyRow("Height range", "Allowed ground height, from the terrain base (0) to its max height (1).");
            pickButton("pick_height_min", "Pick the lowest height from the terrain", TerrainPick::HeightMin);
            pickButton("pick_height_max", "Pick the highest height from the terrain", TerrainPick::HeightMax);
            ImGui::DragFloatRange2("##paint_height", &paintMinHeight, &paintMaxHeight, 0.01f, 0.0f, 1.0f, "%.2f", "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::EndDisabled();
        }
        if (!placementBrush){
            drawBrushMask();
            ImGui::BeginDisabled(brushMaskPath.empty());
            terrainPropertyRow("Mask rotation", "Turns the mask under the brush.");
            UIUtils::sliderFloatInput("##brush_rotation", &brushRotation, 0.0f, 360.0f, "%.0f deg");
            ImGui::EndDisabled();
        }
        if (brushMode == TerrainBrushMode::Noise){
            terrainPropertyRow("Noise size", "Width of one noise feature in world units.");
            UIUtils::sliderFloatInput("##noise_size", &noiseSize, 0.1f, 50.0f, "%.2f");
        }
        if (brushMode == TerrainBrushMode::Terrace){
            terrainPropertyRow("Terrace steps", "How many flat levels the height range is cut into.");
            ImGui::SliderInt("##terrace_steps", &terraceSteps, MIN_TERRACE_STEPS, MAX_TERRACE_STEPS);
        }
        if (brushMode == TerrainBrushMode::Flatten){
            terrainPropertyRow("Sample height", "Pick the flatten height from the terrain at the start of each stroke.");
            ImGui::Checkbox("##flatten_pick", &flattenPickOnStroke);
            terrainPropertyRow("Flatten height", "Normalized terrain height. The eyedropper takes it from the next click on the terrain.");
            if (iconButton(ICON_FA_EYE_DROPPER, "pick_height", "Pick from Terrain (I)", pickTarget == TerrainPick::Height, commandSize)){
                pickTarget = pickTarget == TerrainPick::Height ? TerrainPick::None : TerrainPick::Height;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(flattenPickOnStroke);
            UIUtils::sliderFloatInput("##flatten_height", &flattenHeight, 0.0f, 1.0f, "%.3f");
            ImGui::EndDisabled();
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    // R62: what the pointer is over
    drawInfoPanel(sceneProject, terrain);
    drawTerrainGenerateSection(project, selectedSceneId, selectedEntity);
    ImGui::End();

    // only over the settings this window read: never constructor defaults or another project's
    if (loadedSettingsGeneration == project->getTerrainEditorSettingsGeneration()){
        TerrainEditorSettings& ts = project->getTerrainEditorSettings();
        ts.brushMode = static_cast<int>(brushMode);
        ts.brushShape = static_cast<int>(brushShape);
        ts.brushFalloff = static_cast<int>(brushFalloff);
        ts.brushSize = brushSize;
        ts.brushStrength = brushStrength;
        ts.brushRotation = brushRotation;
        ts.brushMaskPath = brushMaskPath;
        ts.flattenHeight = flattenHeight;
        ts.terraceSteps = terraceSteps;
        ts.noiseSize = noiseSize;
        ts.heightMapResolution = heightMapResolution;
        ts.blendMapResolution = blendMapResolution;
        ts.densityMapResolution = densityMapResolution;
        ts.normalizeBlendPaint = normalizeBlendPaint;
        ts.heightMapStartAtMiddle = heightMapStartAtMiddle;
        ts.flattenPickOnStroke = flattenPickOnStroke;
        ts.paintUseMask = paintUseMask;
        ts.filterAllTools = filterAllTools;
        ts.slopeFilterInvert = filterInvert;
        ts.heightFilterInvert = heightFilterInvert;
        ts.paintMinSlope = paintMinSlope;
        ts.paintMaxSlope = paintMaxSlope;
        ts.paintMinHeight = paintMinHeight;
        ts.paintMaxHeight = paintMaxHeight;
        ts.placeAssetPath = placeAssetPath;
        ts.placeInstanced = placeInstanced;
        ts.placeAsTree = foliagePanel->placesTrees();
        ts.placeSpacing = placeSpacing;
        ts.placeMinScale = placeMinScale;
        ts.placeMaxScale = placeMaxScale;
        ts.placeRotationJitter = placeRotationJitter;
        ts.placeAlignToNormal = placeAlignToNormal;
    }
    if (!windowOpen){
        setOpen(false);
    }
}

void editor::TerrainEditWindow::open(){
    setOpen(true);
    focusRequested = true;
}

void editor::TerrainEditWindow::setOpen(bool open){
    if (open){
        if (!windowOpen){
            focusRequested = true;
        }
        windowOpen = true;
        updateTargetFromSelection();
        updateFoliagePreview();
        return;
    }

    windowOpen = false;
    focusRequested = false;
    brushActive = false;
    endStroke();
    updateFoliagePreview();
}

void editor::TerrainEditWindow::openForEntity(Entity entity, uint32_t sceneId){
    loadSettings();
    open();
    selectedSceneId = sceneId;
    selectedEntity = entity;
    updateFoliagePreview();
}

void editor::TerrainEditWindow::loadSettings(){
    if (!project || loadedSettingsGeneration == project->getTerrainEditorSettingsGeneration()){
        return;
    }
    loadedSettingsGeneration = project->getTerrainEditorSettingsGeneration();
    const TerrainEditorSettings& ts = project->getTerrainEditorSettings();
    brushMode     = static_cast<TerrainBrushMode>(ts.brushMode);
    toolCategory  = toolCategoryFor(brushMode);
    brushShape    = static_cast<TerrainBrushShape>(ts.brushShape);
    brushFalloff  = static_cast<TerrainBrushFalloff>(ts.brushFalloff);
    brushSize     = std::clamp(ts.brushSize, MIN_BRUSH_SIZE, MAX_BRUSH_SIZE);
    brushStrength = std::clamp(ts.brushStrength, MIN_BRUSH_STRENGTH, MAX_BRUSH_STRENGTH);
    brushRotation = std::clamp(ts.brushRotation, 0.0f, 360.0f);
    setBrushMask(ts.brushMaskPath);
    flattenHeight = ts.flattenHeight;
    terraceSteps = std::clamp(ts.terraceSteps, MIN_TERRACE_STEPS, MAX_TERRACE_STEPS);
    noiseSize = std::max(0.1f, ts.noiseSize);
    heightMapResolution = ts.heightMapResolution;
    blendMapResolution  = ts.blendMapResolution;
    densityMapResolution = ts.densityMapResolution;
    normalizeBlendPaint = ts.normalizeBlendPaint;
    heightMapStartAtMiddle = ts.heightMapStartAtMiddle;
    flattenPickOnStroke = ts.flattenPickOnStroke;
    paintUseMask = ts.paintUseMask;
    filterAllTools = ts.filterAllTools;
    filterInvert = ts.slopeFilterInvert;
    heightFilterInvert = ts.heightFilterInvert;
    paintMinSlope = std::clamp(ts.paintMinSlope, 0.0f, 90.0f);
    paintMaxSlope = std::clamp(ts.paintMaxSlope, paintMinSlope, 90.0f);
    paintMinHeight = std::clamp(ts.paintMinHeight, 0.0f, 1.0f);
    paintMaxHeight = std::clamp(ts.paintMaxHeight, paintMinHeight, 1.0f);
    placeAssetPath = ts.placeAssetPath;
    placeInstanced = ts.placeInstanced;
    foliagePanel->setPlacesTrees(ts.placeAsTree);
    placeSpacing = std::max(MIN_PLACE_SPACING, ts.placeSpacing);
    placeMinScale = std::max(0.01f, ts.placeMinScale);
    placeMaxScale = std::max(placeMinScale, ts.placeMaxScale);
    placeRotationJitter = std::clamp(ts.placeRotationJitter, 0.0f, 1.0f);
    placeAlignToNormal = std::clamp(ts.placeAlignToNormal, 0.0f, 1.0f);
}

void editor::TerrainEditWindow::resetProjectState(){
    brushActive = false;
    endStroke();
    selectedSceneId = 0;
    selectedEntity = NULL_ENTITY;
    loadedSettingsGeneration = 0;
}

bool editor::TerrainEditWindow::isOpen() const{
    return windowOpen;
}

bool editor::TerrainEditWindow::isEditingScene(Scene* scene) const{
    if (!windowOpen || !brushActive || !scene){
        return false;
    }
    SceneProject* sceneProject = findSceneProject(scene);
    if (!sceneProject || sceneProject->id != selectedSceneId || !hasValidTarget(sceneProject)){
        return false;
    }

    TerrainComponent* terrain = sceneProject->scene->findComponent<TerrainComponent>(selectedEntity);
    if (!terrain){
        return false;
    }

    if (isPlacementBrush()){
        return foliagePanel->placesTrees() ? (brushMode == TerrainBrushMode::EraseObject || foliagePanel->isReady(*terrain)) : isPlacementReady();
    }
    // Stamp lays the mask down, so without one there is nothing for a stroke to write
    if (brushMode == TerrainBrushMode::Stamp && brushMaskPath.empty()){
        return false;
    }

    Texture* texture = TerrainMapUtils::findTexture(*terrain, getBrushMapRef());
    return texture && !texture->empty();
}

bool editor::TerrainEditWindow::beginStroke(Scene* scene, const Ray& ray){
    if (!isEditingScene(scene)){
        return false;
    }

    SceneProject* sceneProject = findSceneProject(scene);
    Entity entity = NULL_ENTITY;
    Vector3 localPoint;
    Vector3 worldPoint;
    float localHeight = 0.0f;
    if (!findTerrainHit(scene, ray, entity, localPoint, worldPoint, localHeight)){
        return false;
    }

    const ImGuiIO& io = ImGui::GetIO();

    // T1 R63: an armed eyedropper takes its value under the click and edits nothing; Alt+click with no drag
    // (Alt+drag orbits before this) picks without arming: the layer on Paint Texture, else the height
    TerrainPick pick = pickTarget;
    if (pick == TerrainPick::None && io.KeyAlt && !(isPlacementBrush() && foliagePanel->placesTrees())){
        pick = toolCategory == TerrainToolCategory::Texture ? TerrainPick::Layer : TerrainPick::Height;
    }
    if (pick != TerrainPick::None){
        const TerrainPickValues values = pickValues(scene, entity, localPoint.x, localPoint.z);
        switch (pick){
            case TerrainPick::Height:
                flattenHeight = values.height;
                flattenPickOnStroke = false;
                break;
            case TerrainPick::SlopeMin: paintMinSlope = std::min(values.slope, paintMaxSlope); break;
            case TerrainPick::SlopeMax: paintMaxSlope = std::max(values.slope, paintMinSlope); break;
            case TerrainPick::HeightMin: paintMinHeight = std::min(values.height, paintMaxHeight); break;
            case TerrainPick::HeightMax: paintMaxHeight = std::max(values.height, paintMinHeight); break;
            case TerrainPick::Layer:
                if (values.layer >= 0){
                    selectedTextureLayer = values.layer;
                    brushMode = TerrainBrushMode::PaintLayer;
                }else{
                    brushMode = TerrainBrushMode::PaintBase;
                }
                break;
            case TerrainPick::None: break;
        }
        pickTarget = TerrainPick::None;
        return false;
    }

    // T5 R46: Alt+click with no drag picks the prototype under the cursor (Alt+drag orbits before this)
    if (isPlacementBrush() && foliagePanel->placesTrees()){
        if (io.KeyAlt){
            foliagePanel->pickPrototype(sceneProject, entity, localPoint, std::max(brushSize, 1.0f));
            return false;
        }
        clearStroke();
        stroke.active = true;
        stroke.placement = true;
        stroke.treeStroke = true;
        stroke.sceneId = sceneProject->id;
        stroke.entity = entity;
        openStrokeEdit(sceneProject);
        return foliagePanel->beginStroke(sceneProject, entity, localPoint, brushSize, io.KeyShift, io.KeyCtrl || brushMode == TerrainBrushMode::EraseObject);
    }

    if (isPlacementBrush()){
        clearStroke();
        stroke.active = true;
        stroke.placement = true;
        stroke.sceneId = sceneProject->id;
        stroke.entity = entity;
        stroke.placementStrokeId = ++placementStrokeCounter;
        stroke.instanced = useInstancedPlacement();
        stroke.placedPoints = collectPlacedPoints(sceneProject, entity);
        openStrokeEdit(sceneProject);

        // Ctrl swaps place and erase for the stroke, matching the paint brushes.
        stroke.effectiveMode = brushMode;
        if (io.KeyCtrl){
            if (brushMode == TerrainBrushMode::PlaceObject){
                stroke.effectiveMode = TerrainBrushMode::EraseObject;
            }else if (!placeAssetPath.empty()){
                stroke.effectiveMode = TerrainBrushMode::PlaceObject;
            }
        }

        if (stroke.effectiveMode == TerrainBrushMode::EraseObject){
            return applyObjectErase(sceneProject, entity, localPoint);
        }
        return placeAlongStroke(sceneProject, entity, localPoint);
    }

    const TerrainMapRef ref = getBrushMapRef();
    Texture* texture = TerrainMapUtils::findTexture(scene->getComponent<TerrainComponent>(entity), ref);
    if (!texture){
        return false;
    }

    clearStroke();
    loadBrushMask();
    stroke.active = true;
    stroke.sceneId = sceneProject->id;
    stroke.entity = entity;
    stroke.ref = ref;
    openStrokeEdit(sceneProject);

    // Modifiers picked up at stroke start and held for the whole stroke
    stroke.effectiveMode = effectiveModeFor(io.KeyCtrl, io.KeyShift);

    // Flatten levels toward the height under the first click (slider value is used instead when
    // picking is disabled), and a ramp runs from here to wherever the drag ends
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    float pickedHeight = flattenHeight;
    if (std::abs(terrain.maxHeight) > std::numeric_limits<float>::epsilon()){
        pickedHeight = std::clamp(localHeight / terrain.maxHeight, 0.0f, 1.0f);
    }
    if (!openMapStroke(sceneProject, entity, ref, stroke.effectiveMode, localPoint, pickedHeight, false)){
        return false;
    }
    return paintMapStroke(sceneProject, localPoint);
}

bool editor::TerrainEditWindow::paintStroke(Scene* scene, const Ray& ray){
    if (!stroke.active || !isEditingScene(scene) || strokeCancelled()){
        return false;
    }

    SceneProject* sceneProject = findSceneProject(scene);
    Entity entity = NULL_ENTITY;
    Vector3 localPoint;
    Vector3 worldPoint;
    float localHeight = 0.0f;
    if (!findTerrainHit(scene, ray, entity, localPoint, worldPoint, localHeight, &stroke)){
        // The ray left the terrain: drop the path anchor so re-entering doesn't
        // interpolate a streak across the gap.
        if (stroke.session){
            stroke.session->liftAnchor();
        }
        return false;
    }
    if (entity != stroke.entity || sceneProject->id != stroke.sceneId){
        return false;
    }

    if (stroke.treeStroke){
        return foliagePanel->stamp(sceneProject, entity, localPoint, brushSize);
    }

    if (stroke.placement){
        if (stroke.effectiveMode == TerrainBrushMode::EraseObject){
            return applyObjectErase(sceneProject, entity, localPoint);
        }
        return placeAlongStroke(sceneProject, entity, localPoint);
    }

    return paintMapStroke(sceneProject, localPoint);
}

void editor::TerrainEditWindow::endStroke(){
    if (!stroke.active || strokeCancelled()){
        return;
    }

    // Placement already pushed its own commands; there is no map to diff.
    if (stroke.treeStroke){
        foliagePanel->endStroke();
    }
    if (stroke.placement){
        clearStroke();
        return;
    }

    SceneProject* sceneProject = project->getScene(stroke.sceneId);
    if (stroke.session && sceneProject && sceneProject->scene->isEntityCreated(stroke.entity) && strokeTerrainIsThere(sceneProject)){
        // what the frame budget left waiting lands before the step is cut
        stroke.session->run(0.0);
        stroke.session->flush();
        // R39: the stroke as the edit_terrain call that replays it, one line each
        if (const char* log = terrainStrokeLogPath()){
            const std::string line = stroke.session->replayCall(stroke.logSamples);
            if (!line.empty()){
                std::ofstream out(std::filesystem::u8path(log), std::ios::app);
                out << line << '\n';
            }
        }
        CommandHistory* history = CommandHandle::get(stroke.sceneId);
        if (removeFilledHoleMap(sceneProject)){
            clearStroke();
            return;
        }
        if (stroke.rebound){
            Texture* texture = TerrainMapUtils::findTexture(stroke.store->component(), stroke.ref);
            if (texture){
                TerrainMapSnapshot after = TerrainMapUtils::captureSnapshot(project, *texture, true);
                if (!TerrainMapUtils::snapshotsEqual(stroke.rebindSnapshot, after)){
                    history->addCommandNoMerge(new TerrainTextureEditCmd(project, stroke.sceneId, stroke.entity, stroke.ref, std::move(stroke.rebindSnapshot), std::move(after)));
                }
            }
        }else{
            auto delta = std::make_unique<TerrainMapDeltaCmd>(project, stroke.sceneId, stroke.entity, stroke.entity, stroke.session->overlay(), true);
            if (!delta->empty()){
                history->addCommandNoMerge(delta.release());
            }
        }
    }

    clearStroke();
}

void editor::TerrainEditWindow::drawHoleBrushInfo(TerrainComponent& terrain){
    const terrain::HoleMapView view = terrainSurfaceView(terrain).holes;
    const int samples = terrain.heightMap.hasData() ? std::clamp(std::min(terrain.heightMap.getData().getWidth(), terrain.heightMap.getData().getHeight()), 4, 1025) : 4;
    const float cell = terrain.terrainSize / static_cast<float>(samples - 1);
    const float texel = view.width > 0 ? terrain.terrainSize / static_cast<float>(view.width) : cell;
    ImGui::Text("Texel %.2f m, collision cell %.2f m", texel, cell);
    if (hoverValid){
        const terrain::SurfaceSample sample = querySurface(terrain, hoverLocal.x, hoverLocal.z);
        ImGui::Text("Under the cursor: %s", sample.hole ? "hole" : "solid");
    }
    if (view.texels){
        std::vector<uint8_t> open;
        std::vector<uint8_t> gaps;
        terrain::openSquares(view, samples, 1.0f, 1.0f, open);
        if (terrain::noCollisionSamples(open, samples, gaps) == 0 && terrain::countHoleTexels(view, 0, 0, view.width, view.height) > 0){
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "This hole opens no collision sample: a body stays on it. Cut at least 3 collision cells across.");
        }
    }
}

bool editor::TerrainEditWindow::removeFilledHoleMap(SceneProject* sceneProject){
    TerrainComponent& terrain = stroke.store->component();
    if (stroke.ref.target != TerrainMapTarget::HoleMap || terrain.holeMap.empty() || terrain.holeMapHolds > 0){
        return false;
    }
    const terrain::HoleMapView view = terrainSurfaceView(terrain).holes;
    if (!view.texels || terrain::countHoleTexels(view, 0, 0, view.width, view.height) > 0){
        return false;
    }
    // the map as the stroke found it; a map the stroke made has nothing to give back
    TerrainMapSnapshot before = std::move(stroke.rebindSnapshot);
    if (!stroke.rebound){
        stroke.session->cancel();
        before = TerrainMapUtils::captureSnapshot(project, terrain.holeMap, true);
    }
    if (before.empty){
        TerrainMapUtils::applySnapshotToTexture(project, terrain.holeMap, TerrainMapSnapshot());
        TerrainMapUtils::refresh(sceneProject, stroke.entity, stroke.ref);
        return true;
    }
    CommandHandle::get(stroke.sceneId)->addCommandNoMerge(new TerrainTextureEditCmd(project, stroke.sceneId, stroke.entity, stroke.ref, std::move(before), TerrainMapSnapshot()));
    return true;
}

bool editor::TerrainEditWindow::updateCursor(Scene* scene, const Ray& ray, TerrainBrushCursor& cursor) const{
    if (!isEditingScene(scene)){
        return false;
    }
    infoValid = false;
    // R55: no decal while the camera moves (right or middle drag, Alt+left orbit), and a fade once the pointer rests
    const ImGuiIO& pointer = ImGui::GetIO();
    if (!stroke.active && (pointer.MouseDown[ImGuiMouseButton_Right] || pointer.MouseDown[ImGuiMouseButton_Middle] ||
                           (pointer.KeyAlt && pointer.MouseDown[ImGuiMouseButton_Left]))){
        return false;
    }
    if (pointer.MousePos.x != decalPointerX || pointer.MousePos.y != decalPointerY || stroke.active){
        decalPointerX = pointer.MousePos.x;
        decalPointerY = pointer.MousePos.y;
        decalMovedAt = ImGui::GetTime();
    }
    const float opacity = decalOpacity(ImGui::GetTime() - decalMovedAt);
    if (opacity <= 0.0f){
        return false;
    }

    Entity entity = NULL_ENTITY;
    Vector3 localPoint;
    Vector3 worldPoint;
    float localHeight = 0.0f;
    if (!findTerrainHit(scene, ray, entity, localPoint, worldPoint, localHeight, stroke.active ? &stroke : nullptr)){
        return false;
    }

    hoverValid = true;
    hoverLocal = localPoint;
    Transform& transform = scene->getComponent<Transform>(entity);
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    infoValid = true;
    infoEntity = entity;
    infoLocal = localPoint;

    // Drape the cursor over the live heightmap so it follows the terrain's
    // topology (including edits still in progress).
    const unsigned char* heightPixels = nullptr;
    int mapWidth = 0, mapHeight = 0, mapChannels = 0, mapBytesPerChannel = 1;
    if (!terrain.heightMap.empty() && !terrain.heightMap.isFramebuffer() && TerrainMapUtils::hasLoadedData(terrain.heightMap)){
        TextureData& heightData = terrain.heightMap.getData();
        heightPixels = static_cast<const unsigned char*>(heightData.getData());
        mapWidth = heightData.getWidth();
        mapHeight = heightData.getHeight();
        mapChannels = heightData.getChannels();
        mapBytesPerChannel = TextureData::getBytesPerChannel(heightData.getColorFormat());
    }

    const float terrainSize = std::max(terrain.terrainSize, std::numeric_limits<float>::epsilon());
    const float halfSize = terrainSize * 0.5f;
    const float lift = std::max(0.03f, std::abs(terrain.maxHeight) * 0.01f);
    auto surfacePoint = [&](float localX, float localZ){
        const float clampedX = std::clamp(localX, -halfSize, halfSize);
        const float clampedZ = std::clamp(localZ, -halfSize, halfSize);
        float surfaceHeight = 0.0f;
        if (heightPixels){
            surfaceHeight = terrain::sampleHeightTexels(heightPixels, mapWidth, mapHeight, mapChannels, mapBytesPerChannel,
                                                 terrainTexel(clampedX, terrainSize, mapWidth), terrainTexel(clampedZ, terrainSize, mapHeight)) * terrain.maxHeight;
        }
        return transform.modelMatrix * Vector3(clampedX, surfaceHeight + lift, clampedZ);
    };

    auto buildLoop = [&](float radius, int segmentsPerQuarter, std::vector<Vector3>& points){
        points.clear();
        if (brushShape == TerrainBrushShape::Circle){
            const int segments = segmentsPerQuarter * 4;
            const float twoPi = 6.28318530718f;
            points.reserve(segments);
            for (int i = 0; i < segments; i++){
                const float angle = (twoPi * static_cast<float>(i)) / static_cast<float>(segments);
                points.push_back(surfacePoint(localPoint.x + radius * std::cos(angle), localPoint.z + radius * std::sin(angle)));
            }
        }else{
            const Vector2 corners[4] = {
                Vector2(localPoint.x - radius, localPoint.z - radius),
                Vector2(localPoint.x + radius, localPoint.z - radius),
                Vector2(localPoint.x + radius, localPoint.z + radius),
                Vector2(localPoint.x - radius, localPoint.z + radius)};
            points.reserve(segmentsPerQuarter * 4);
            for (int edge = 0; edge < 4; edge++){
                const Vector2& from = corners[edge];
                const Vector2& to = corners[(edge + 1) % 4];
                for (int i = 0; i < segmentsPerQuarter; i++){
                    const float t = static_cast<float>(i) / static_cast<float>(segmentsPerQuarter);
                    points.push_back(surfacePoint(from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t));
                }
            }
        }
    };

    // T3 R41: the hole brush's texels, when there are few enough to see
    if (isHoleBrush()){
        TerrainComponent& holeTerrain = scene->getComponent<TerrainComponent>(entity);
        const int side = holeTerrain.holeMap.hasData() ? static_cast<int>(holeTerrain.holeMap.getData().getWidth()) : std::max(2, heightMapResolution);
        const float texel = holeTerrain.terrainSize / static_cast<float>(side);
        if (brushSize / texel <= 12.0f){
            const float half = holeTerrain.terrainSize * 0.5f;
            const int cx = static_cast<int>(std::floor((localPoint.x + half) / texel));
            const int cz = static_cast<int>(std::floor((localPoint.z + half) / texel));
            const int reach = static_cast<int>(std::ceil(brushSize / texel));
            const auto corner = [&](int x, int z){
                const float lx = static_cast<float>(x) * texel - half;
                const float lz = static_cast<float>(z) * texel - half;
                return transform.modelMatrix * Vector3(lx, querySurface(holeTerrain, lx, lz).height + 0.02f, lz);
            };
            for (int z = cz - reach; z <= cz + reach; z++){
                for (int x = cx - reach; x <= cx + reach; x++){
                    const float dx = (static_cast<float>(x) + 0.5f) * texel - half - localPoint.x;
                    const float dz = (static_cast<float>(z) + 0.5f) * texel - half - localPoint.z;
                    if (dx * dx + dz * dz <= brushSize * brushSize && x >= 0 && z >= 0 && x < side && z < side){
                        cursor.texelLoops.push_back({corner(x, z), corner(x + 1, z), corner(x + 1, z + 1), corner(x, z + 1)});
                    }
                }
            }
        }
    }
    cursor.visible = true;
    // R55: the colour of what a click would do now, modifiers included; under one texel the warning
    const ImGuiIO& io = ImGui::GetIO();
    const TerrainBrushMode shown = stroke.active ? stroke.effectiveMode : effectiveModeFor(io.KeyCtrl, io.KeyShift);
    const float texel = mapWidth > 1 ? terrainSize / static_cast<float>(mapWidth - 1) : 0.0f;
    cursor.color = pickTarget != TerrainPick::None ? Vector4(1.0f, 1.0f, 1.0f, 1.0f)
                                   : (!isPlacementBrush() && brushSize < texel ? Vector4(1.0f, 0.0f, 1.0f, 1.0f)
                                                                                : decalColor(isAlternateOn() && shown != TerrainBrushMode::Smooth ? TerrainBrushMode::Flatten : shown));
    cursor.color.w *= opacity;
    buildLoop(brushSize, 16, cursor.outerPoints);
    // Placement has no falloff — the ring is the area objects can scatter into.
    if (brushFalloff != TerrainBrushFalloff::Constant && !isPlacementBrush()){
        // Half-strength contour: both smoothstep and linear falloff reach 0.5 at
        // half the brush radius.
        buildLoop(brushSize * 0.5f, 12, cursor.innerPoints);
    }else{
        cursor.innerPoints.clear();
    }
    cursor.capsuleLines.clear();
    if (isPlacementBrush() && foliagePanel->placesTrees()){
        const int selected = foliagePanel->selectedPrototype();
        if (selected >= 0 && selected < static_cast<int>(terrain.treePrototypes.size())
                && terrain.treePrototypes[selected].collision == TerrainTreeCollision::Capsule){
            const TerrainTreePrototype& prototype = terrain.treePrototypes[selected];
            const Vector3 base = surfacePoint(localPoint.x, localPoint.z);
            TerrainFoliagePanel::appendCapsuleLines(base, prototype.collisionRadius, prototype.collisionHeight, cursor.capsuleLines);
        }
    }
    return true;
}

void editor::TerrainEditWindow::armBrush(TerrainBrushMode mode){
    endStroke();
    brushMode = mode;
    toolCategory = toolCategoryFor(mode);
    brushActive = true;
    pickTarget = TerrainPick::None;
}

void editor::TerrainEditWindow::armPickHeight(){
    armBrush(TerrainBrushMode::Flatten);
    pickTarget = TerrainPick::Height;
}

bool editor::TerrainEditWindow::hasAlternate() const{
    return brushMode == TerrainBrushMode::Raise || brushMode == TerrainBrushMode::Lower || brushMode == TerrainBrushMode::Flatten;
}

bool editor::TerrainEditWindow::disarmBrush(){
    if (pickTarget != TerrainPick::None){
        pickTarget = TerrainPick::None;
        return true;
    }
    if (!brushActive){
        return false;
    }
    endStroke();
    brushActive = false;
    return true;
}

TerrainBrushMode editor::TerrainEditWindow::effectiveModeFor(bool ctrl, bool shift) const{
    if (isHeightBrush()){
        if (shift){
            return TerrainBrushMode::Smooth;
        }
        if (ctrl && brushMode == TerrainBrushMode::Raise){
            return TerrainBrushMode::Lower;
        }
        if (ctrl && brushMode == TerrainBrushMode::Lower){
            return TerrainBrushMode::Raise;
        }
    }else if (isHoleBrush() && ctrl){
        // T3 R41: Ctrl inverts (programme D16)
        return brushMode == TerrainBrushMode::CutHole ? TerrainBrushMode::FillHole : TerrainBrushMode::CutHole;
    }else if (isDensityBrush() && ctrl){
        return brushMode == TerrainBrushMode::PaintDensity ? TerrainBrushMode::EraseDensity : TerrainBrushMode::PaintDensity;
    }else if (isPlacementBrush() && ctrl){
        return brushMode == TerrainBrushMode::PlaceObject ? TerrainBrushMode::EraseObject : TerrainBrushMode::PlaceObject;
    }
    return brushMode;
}

editor::TerrainPickValues editor::TerrainEditWindow::pickValues(Scene* scene, Entity entity, float localX, float localZ){
    TerrainPickValues values;
    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    float height = 0.0f;
    Vector3 normal(0.0f, 1.0f, 0.0f);
    MeshSystem::sampleTerrainSurface(terrain, localX, localZ, height, normal);
    values.height = std::abs(terrain.maxHeight) > std::numeric_limits<float>::epsilon() ? std::clamp(height / terrain.maxHeight, 0.0f, 1.0f) : 0.0f;
    values.slope = Angle::radToDefault(std::acos(std::clamp(normal.y, -1.0f, 1.0f)));
    // the strongest layer as the blend maps weigh it, the base where they leave most
    TerrainComponentStore store(scene, entity, terrain);
    float best = 0.0f, total = 0.0f;
    std::vector<float> texel(4);
    for (int l = 0; l < static_cast<int>(terrain.surfaceLayers.size()); l++){
        const terrain::LayerChannel channel = terrain::terrainLayerChannel(l);
        const terrain::MapId blend{terrain::MapKind::Blend, channel.map};
        const terrain::MapFormat format = store.format(blend);
        if (!format.exists) continue;
        const Vector2 at = store.texelOfWorld(blend, localX, localZ);
        const int x = std::clamp(static_cast<int>(std::lround(at.x)), 0, format.bounds.w - 1);
        const int y = std::clamp(static_cast<int>(std::lround(at.y)), 0, format.bounds.h - 1);
        texel.assign(static_cast<size_t>(format.channels), 0.0f);
        if (!store.read(blend, terrain::TexelRect{x, y, 1, 1}, texel)) continue;
        const float weight = channel.inverted ? 1.0f - texel[channel.channel] : texel[channel.channel];
        total += weight;
        if (weight > best){
            best = weight;
            values.layer = l;
        }
    }
    if (1.0f - total > best) values.layer = -1;
    return values;
}

float editor::TerrainEditWindow::decalOpacity(double secondsSinceMoved){
    constexpr double HOLD = 0.5;
    constexpr double FADE = 0.25;
    if (secondsSinceMoved <= HOLD) return 1.0f;
    return static_cast<float>(std::max(0.0, 1.0 - (secondsSinceMoved - HOLD) / FADE));
}

Vector4 editor::TerrainEditWindow::decalColor(TerrainBrushMode mode){
    switch (mode){
        case TerrainBrushMode::Raise: return Vector4(1.0f, 1.0f, 1.0f, 1.0f);
        case TerrainBrushMode::Lower: return Vector4(0.2f, 0.2f, 0.2f, 1.0f);
        case TerrainBrushMode::Smooth: return Vector4(0.5f, 0.0f, 0.2f, 1.0f);
        case TerrainBrushMode::Sharpen: return Vector4(0.9f, 0.3f, 0.5f, 1.0f);
        case TerrainBrushMode::Flatten: return Vector4(0.0f, 0.6f, 1.0f, 1.0f);
        case TerrainBrushMode::Ramp: return Vector4(1.0f, 1.0f, 0.0f, 1.0f);
        case TerrainBrushMode::Noise: return Vector4(0.6f, 0.4f, 1.0f, 1.0f);
        case TerrainBrushMode::Terrace: return Vector4(0.0f, 0.7f, 0.6f, 1.0f);
        case TerrainBrushMode::Stamp: return Vector4(0.6f, 1.0f, 0.2f, 1.0f);
        case TerrainBrushMode::Erode: return Vector4(0.6f, 0.4f, 0.0f, 1.0f);
        case TerrainBrushMode::PaintLayer: return Vector4(0.0f, 0.5f, 0.0f, 1.0f);
        case TerrainBrushMode::PaintBase: return Vector4(0.4f, 0.8f, 0.4f, 1.0f);
        case TerrainBrushMode::PaintDensity: return Vector4(0.5f, 0.8f, 0.3f, 1.0f);
        case TerrainBrushMode::EraseDensity: return Vector4(0.5f, 0.2f, 0.5f, 1.0f);
        case TerrainBrushMode::PlaceObject: return Vector4(0.86f, 0.08f, 0.24f, 1.0f);
        case TerrainBrushMode::EraseObject: return Vector4(0.2f, 0.9f, 0.6f, 1.0f);
        // T3 R41: the hole brush's magenta, darker as it fills
        case TerrainBrushMode::CutHole: return Vector4(0.95f, 0.1f, 0.75f, 1.0f);
        case TerrainBrushMode::FillHole: return Vector4(0.55f, 0.1f, 0.45f, 1.0f);
    }
    return Vector4(1.0f, 0.78f, 0.22f, 1.0f);
}

void editor::TerrainEditWindow::brushSizeLimits(const TerrainComponent& terrain, float& minSize, float& maxSize){
    const int texels = terrain.heightMap.hasData() ? terrain.heightMap.getData().getWidth() : 0;
    const float texel = texels > 1 ? terrain.terrainSize / static_cast<float>(texels - 1) : 0.0f;
    minSize = std::max(MIN_BRUSH_SIZE, texel * 0.5f);
    maxSize = std::clamp(terrain.terrainSize, minSize, MAX_BRUSH_SIZE);
}

void editor::TerrainEditWindow::adjustBrushSize(float factor){
    float minSize = MIN_BRUSH_SIZE, maxSize = MAX_BRUSH_SIZE;
    SceneProject* sceneProject = getTargetSceneProject();
    if (hasValidTarget(sceneProject)){
        brushSizeLimits(sceneProject->scene->getComponent<TerrainComponent>(selectedEntity), minSize, maxSize);
    }
    brushSize = std::clamp(brushSize * factor, minSize, maxSize);
}

void editor::TerrainEditWindow::adjustBrushStrength(float factor){
    brushStrength = std::clamp(brushStrength * factor, MIN_BRUSH_STRENGTH, MAX_BRUSH_STRENGTH);
}

bool editor::TerrainEditWindow::scriptedStroke(uint32_t sceneId, Entity entity, const TerrainScriptedBrush& brush,
                                               const std::vector<Vector3>& localPoints, std::string& error, int* placed){
    // the saved resolutions and settings, as the window would have them
    loadSettings();
    if (placed){
        *placed = 0;
    }
    if (localPoints.empty()){
        error = "A stroke needs at least one point.";
        return false;
    }
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene){
        error = "Scene not found.";
        return false;
    }
    if (sceneProject->playState != ScenePlayState::STOPPED){
        error = "Terrain can't be edited while the scene is playing.";
        return false;
    }
    Scene* scene = sceneProject->scene;
    if (!scene->isEntityCreated(entity) || !scene->findComponent<TerrainComponent>(entity) || !scene->findComponent<Transform>(entity)){
        error = "The GameObject has no Terrain component.";
        return false;
    }

    // A mouse stroke in progress ends first, so its undo step stays its own
    endStroke();

    // The window's state, put back afterwards: a scripted stroke does not change the tool settings
    const uint32_t savedSceneId = selectedSceneId;
    const Entity savedEntity = selectedEntity;
    const TerrainBrushMode savedMode = brushMode;
    const TerrainBrushShape savedShape = brushShape;
    const TerrainBrushFalloff savedFalloff = brushFalloff;
    const float savedSize = brushSize, savedStrength = brushStrength, savedRotation = brushRotation;
    const float savedFlatten = flattenHeight, savedNoise = noiseSize;
    const int savedTerrace = terraceSteps, savedTextureLayer = selectedTextureLayer, savedFoliageLayer = selectedFoliageLayer;
    const bool savedNormalize = normalizeBlendPaint, savedPick = flattenPickOnStroke;
    const std::string savedMask = brushMaskPath;
    const int savedHeightRes = heightMapResolution, savedBlendRes = blendMapResolution, savedDensityRes = densityMapResolution;
    const std::string savedAsset = placeAssetPath;
    const bool savedInstanced = placeInstanced;
    const float savedSpacing = placeSpacing, savedMinScale = placeMinScale, savedMaxScale = placeMaxScale;
    const float savedJitter = placeRotationJitter, savedAlign = placeAlignToNormal;
    const bool savedUseMask = paintUseMask;

    auto restore = [&](){
        selectedSceneId = savedSceneId;
        selectedEntity = savedEntity;
        brushMode = savedMode;
        brushShape = savedShape;
        brushFalloff = savedFalloff;
        brushSize = savedSize;
        brushStrength = savedStrength;
        brushRotation = savedRotation;
        flattenHeight = savedFlatten;
        noiseSize = savedNoise;
        terraceSteps = savedTerrace;
        selectedTextureLayer = savedTextureLayer;
        selectedFoliageLayer = savedFoliageLayer;
        normalizeBlendPaint = savedNormalize;
        flattenPickOnStroke = savedPick;
        setBrushMask(savedMask);
        heightMapResolution = savedHeightRes;
        blendMapResolution = savedBlendRes;
        densityMapResolution = savedDensityRes;
        placeAssetPath = savedAsset;
        placeInstanced = savedInstanced;
        placeSpacing = savedSpacing;
        placeMinScale = savedMinScale;
        placeMaxScale = savedMaxScale;
        placeRotationJitter = savedJitter;
        placeAlignToNormal = savedAlign;
        paintUseMask = savedUseMask;
    };

    selectedSceneId = sceneId;
    selectedEntity = entity;
    brushMode = brush.mode;
    brushShape = brush.shape;
    brushFalloff = brush.falloff;
    float minSize = MIN_BRUSH_SIZE, maxSize = MAX_BRUSH_SIZE;
    brushSizeLimits(scene->getComponent<TerrainComponent>(entity), minSize, maxSize);
    brushSize = std::clamp(brush.size, minSize, maxSize);
    brushStrength = std::clamp(brush.strength, MIN_BRUSH_STRENGTH, MAX_BRUSH_STRENGTH);
    brushRotation = std::clamp(brush.rotation, 0.0f, 360.0f);
    flattenPickOnStroke = brush.flattenHeight < 0.0f;
    flattenHeight = std::clamp(brush.flattenHeight < 0.0f ? 0.5f : brush.flattenHeight, 0.0f, 1.0f);
    terraceSteps = std::clamp(brush.terraceSteps, MIN_TERRACE_STEPS, MAX_TERRACE_STEPS);
    noiseSize = std::max(0.1f, brush.noiseSize);
    selectedTextureLayer = std::clamp(brush.textureLayer, 0, MAX_TERRAIN_LAYERS - 1);
    selectedFoliageLayer = std::max(0, brush.foliageLayer);
    normalizeBlendPaint = brush.normalizeBlend;
    paintUseMask = false;
    setBrushMask(brush.maskPath);
    if (brush.mapResolution > 0){
        heightMapResolution = blendMapResolution = densityMapResolution = std::clamp(brush.mapResolution, 2, 4096);
    }
    placeAssetPath = brush.placeAssetPath;
    placeInstanced = brush.placeInstanced;
    placeSpacing = std::max(MIN_PLACE_SPACING, brush.placeSpacing);
    placeMinScale = std::max(0.01f, brush.placeMinScale);
    placeMaxScale = std::max(placeMinScale, brush.placeMaxScale);
    placeRotationJitter = std::clamp(brush.placeRotationJitter, 0.0f, 1.0f);
    placeAlignToNormal = std::clamp(brush.placeAlignToNormal, 0.0f, 1.0f);

    TerrainComponent& terrain = scene->getComponent<TerrainComponent>(entity);
    if (terrain.terrainSize <= std::numeric_limits<float>::epsilon()){
        restore();
        error = "The terrain has no size (terrainSize is 0).";
        return false;
    }

    if (isPlacementBrush()){
        if (!isPlacementReady()){
            restore();
            error = "place_object needs an asset (a model or prefab path).";
            return false;
        }
        clearStroke();
        stroke.active = true;
        stroke.placement = true;
        stroke.sceneId = sceneId;
        stroke.entity = entity;
        stroke.placementStrokeId = ++placementStrokeCounter;
        stroke.instanced = useInstancedPlacement();
        stroke.placedPoints = collectPlacedPoints(sceneProject, entity);
        stroke.effectiveMode = brushMode;
        int count = 0;
        for (const Vector3& point : localPoints){
            const bool done = brushMode == TerrainBrushMode::EraseObject
                ? applyObjectErase(sceneProject, entity, point)
                : applyPlacement(sceneProject, entity, point);
            if (done) count++;
        }
        clearStroke();
        restore();
        if (placed){
            *placed = count;
        }
        return true;
    }

    if (brushMode == TerrainBrushMode::Stamp && brushMaskPath.empty()){
        restore();
        error = "The stamp brush needs a mask texture (its relief).";
        return false;
    }

    const TerrainMapRef ref = getBrushMapRef();
    if (ref.target == TerrainMapTarget::DensityMap && ref.layer >= static_cast<int>(terrain.foliageLayers.size())){
        restore();
        error = "foliage_layer " + std::to_string(ref.layer) + " does not exist: the terrain has " +
                std::to_string(terrain.foliageLayers.size()) + " foliage layer(s).";
        return false;
    }

    // The map the brush writes is created first when missing, as the window's Create button does
    TerrainMapUtils::ensureMapSlot(terrain, ref);
    Texture* texture = TerrainMapUtils::findTexture(terrain, ref);
    if (!texture || texture->empty()){
        const int resolution = mapResolutionFor(ref.target);
        createMapForTarget(ref, resolution, resolution);
        texture = TerrainMapUtils::findTexture(scene->getComponent<TerrainComponent>(entity), ref);
    }
    if (!texture || texture->empty()){
        restore();
        error = "Could not create the terrain map the brush paints.";
        return false;
    }

    clearStroke();
    loadBrushMask();
    stroke.active = true;
    stroke.sceneId = sceneId;
    stroke.entity = entity;
    stroke.ref = ref;
    stroke.effectiveMode = brushMode;

    // The height under the first point: where a flatten without its own height levels to
    TerrainComponent& strokeTerrain = scene->getComponent<TerrainComponent>(entity);
    float surfaceHeight = 0.0f;
    Vector3 normal(0.0f, 1.0f, 0.0f);
    scene->getSystem<MeshSystem>()->sampleTerrainSurface(strokeTerrain, localPoints[0].x, localPoints[0].z, surfaceHeight, normal);
    float pickedHeight = flattenHeight;
    if (std::abs(strokeTerrain.maxHeight) > std::numeric_limits<float>::epsilon()){
        pickedHeight = std::clamp(surfaceHeight / strokeTerrain.maxHeight, 0.0f, 1.0f);
    }
    if (!openMapStroke(sceneProject, entity, ref, brushMode, localPoints[0], pickedHeight, true)){
        clearStroke();
        restore();
        error = "Could not open the terrain map the brush paints.";
        return false;
    }

    // T1 R18: each point is held for secondsPerPoint, in slices no longer than a mouse event's, so
    // existing calls give the heights they always did
    const float seconds = std::clamp(brush.secondsPerPoint, 0.0f, 10.0f);
    const int slices = std::max(1, static_cast<int>(std::ceil(seconds / BRUSH_MAX_STAMP_SECONDS)));
    for (const Vector3& point : localPoints){
        for (int i = 0; i < slices; i++){
            stroke.session->moveTo(point.x, point.z, seconds / static_cast<float>(slices));
        }
    }

    endStroke();
    restore();
    return true;
}
