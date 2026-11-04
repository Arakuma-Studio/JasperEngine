// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NavigationEditor.h"

#include "Backend.h"
#include "App.h"
#include "Out.h"
#include "Project.h"
#include "component/NavGridComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "io/AtomicFile.h"
#include "navigation/NavGridMap.h"
#include "navigation/NavGridCellsCmd.h"
#include "navigation/NavGridBrush.h"
#include "navigation/NavigationOverlay.h"
#include "render/SceneRender.h"
#include "component/Transform.h"
#include "component/NavMeshLinkComponent.h"
#include "command/type/PropertyCmd.h"
#include "command/CommandHandle.h"

#include <algorithm>
#include <chrono>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    double nowSeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // a file name from an entity name: letters, digits, '-', '_' and '.' kept, the rest '_'
    std::string fileSafe(const std::string& name) {
        std::string out;
        for (char c : name) {
            const unsigned char u = (unsigned char)c;
            out += (std::isalnum(u) || c == '-' || c == '_' || c == '.') ? c : '_';
        }
        if (out.empty() || out == "." || out == "..") out = "Entity";
        return out;
    }

    bool inside(const fs::path& file, const fs::path& folder) {
        const fs::path relative = file.lexically_normal().lexically_relative(folder.lexically_normal());
        return !relative.empty() && *relative.begin() != "..";
    }

}

NavigationEditor& NavigationEditor::get() {
    static NavigationEditor instance;
    return instance;
}

NavigationSystem* NavigationEditor::systemOf(SceneProject& sceneProject) {
    if (!sceneProject.scene) return nullptr;
    auto system = sceneProject.scene->getSystem<NavigationSystem>();
    if (!system) return nullptr;
    // Play keeps what a game does: tile records only when a script rebakes
    if (sceneProject.playState == ScenePlayState::STOPPED) {
        system->setKeepBakeData(true);
        system->setBakeInBackground(true);
    }
    return system.get();
}

fs::path NavigationEditor::defaultAssetPath(Project* project, const SceneProject& sceneProject, Entity entity,
                                            const std::string& prefix, const std::string& extension) {
    fs::path sceneDir;
    std::string sceneName = sceneProject.name;
    if (!sceneProject.filepath.empty()) {
        fs::path scenePath = sceneProject.filepath;
        if (scenePath.is_absolute()) {
            std::error_code ec;
            scenePath = fs::relative(scenePath, project->getProjectPath(), ec);
        }
        sceneDir = scenePath.parent_path();
        sceneName = scenePath.stem().string();
        // a scene inside the assets folder keeps its own folder (assets/levels/Level1/...)
        const fs::path assetsRelative = project->getAssetsPath().lexically_normal().lexically_relative(project->getProjectPath().lexically_normal());
        if (!assetsRelative.empty() && inside(project->getProjectPath() / sceneDir, project->getAssetsPath())) {
            sceneDir = sceneDir.lexically_relative(assetsRelative);
            if (sceneDir == ".") sceneDir.clear();
        }
    }
    const std::string entityName = sceneProject.scene ? sceneProject.scene->getEntityName(entity) : std::string();
    const std::string name = prefix + "-" + fileSafe(entityName.empty() ? "Entity" + std::to_string(entity) : entityName) + "." + extension;
    return (project->getAssetsPath() / sceneDir / fileSafe(sceneName) / name).lexically_normal();
}

fs::path NavigationEditor::navMeshFileOf(Project* project, const SceneProject& sceneProject, Entity surface) {
    if (sceneProject.scene) {
        if (const NavMeshSurfaceComponent* component = sceneProject.scene->findComponent<NavMeshSurfaceComponent>(surface)) {
            if (!component->navMeshData.path.empty()) {
                const fs::path file = (project->getAssetsPath() / component->navMeshData.path).lexically_normal();
                if (inside(file, project->getAssetsPath())) return file;
            }
        }
    }
    return defaultAssetPath(project, sceneProject, surface, "NavMesh", "navmesh");
}

bool NavigationEditor::writeAsset(Project* project, const fs::path& file, const std::vector<uint8_t>& bytes, AssetRef& ref,
                                  std::string& error) {
    if (!inside(file, project->getAssetsPath())) {
        error = "'" + file.generic_string() + "' is outside the assets folder";
        return false;
    }
    io::AtomicWriteOptions options;
    options.skipIfUnchanged = true;
    const io::WriteResult written = io::writeFileAtomic(file, std::string_view((const char*)bytes.data(), bytes.size()), options);
    if (!written.ok()) {
        error = written.message;
        return false;
    }
    AssetDatabase& database = project->getAssetDatabase();
    Guid guid = database.getAssetGuid(file);
    if (guid.isNull()) {
        project->onAssetFilesChanged(database.importAssets(file));
        guid = database.getAssetGuid(file);
    }
    const std::string path = file.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
    ref.set(path, guid);
    return true;
}

bool NavigationEditor::bake(Project* project, uint32_t sceneId, Entity surface, NavBakeMode mode, std::string& error,
                            BakeCallback onDone) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    NavigationSystem* system = sceneProject ? systemOf(*sceneProject) : nullptr;
    if (!system) {
        error = "no scene " + std::to_string(sceneId);
        return false;
    }
    if (!sceneProject->scene->findComponent<NavMeshSurfaceComponent>(surface)) {
        error = "entity " + std::to_string(surface) + " has no NavMeshSurface";
        return false;
    }
    // a second request for a running bake waits for it
    for (Pending& entry : pending) {
        if (entry.sceneId == sceneId && entry.surface == surface) {
            if (onDone) entry.callbacks.push_back(std::move(onDone));
            return true;
        }
    }
    if (mode == NavBakeMode::CLEAR) {
        const bool ok = clear(project, sceneId, surface, error);
        if (onDone) {
            NavBakeResult result;
            result.ok = ok;
            result.error = error;
            onDone(result);
        }
        return ok;
    }
    if (!system->bake(surface, mode, error)) return false;
    Pending entry;
    entry.sceneId = sceneId;
    entry.surface = surface;
    entry.mode = mode;
    if (onDone) entry.callbacks.push_back(std::move(onDone));
    pending.push_back(std::move(entry));
    Backend::getApp().requestFrame();
    return true;
}

int NavigationEditor::bakeAll(Project* project, uint32_t sceneId, NavBakeMode mode, std::string& error) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    if (!sceneProject || !sceneProject->scene) {
        error = "no scene " + std::to_string(sceneId);
        return 0;
    }
    int started = 0;
    std::vector<Entity> surfaces;
    auto array = sceneProject->scene->getComponentArray<NavMeshSurfaceComponent>();
    for (size_t i = 0; i < array->size(); i++) surfaces.push_back(array->getEntity(i));
    std::sort(surfaces.begin(), surfaces.end());
    for (Entity surface : surfaces) {
        std::string why;
        if (bake(project, sceneId, surface, mode, why)) {
            started++;
        } else if (error.empty()) {
            error = why;
        }
    }
    return started;
}

bool NavigationEditor::cancel(Project* project, uint32_t sceneId, Entity surface) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    NavigationSystem* system = sceneProject ? systemOf(*sceneProject) : nullptr;
    return system && system->cancelBake(surface);
}

bool NavigationEditor::clear(Project* project, uint32_t sceneId, Entity surface, std::string& error) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    NavigationSystem* system = sceneProject ? systemOf(*sceneProject) : nullptr;
    NavMeshSurfaceComponent* component = system ? sceneProject->scene->findComponent<NavMeshSurfaceComponent>(surface) : nullptr;
    if (!component) {
        error = "entity " + std::to_string(surface) + " has no NavMeshSurface";
        return false;
    }
    system->cancelBake(surface);
    NavBakeResult result = system->bakeNow(surface, NavBakeMode::CLEAR);
    if (!result.ok && !result.error.empty()) {
        error = result.error;
        return false;
    }
    if (!component->navMeshData.path.empty()) {
        const fs::path file = (project->getAssetsPath() / component->navMeshData.path).lexically_normal();
        std::error_code ec;
        if (inside(file, project->getAssetsPath()) && fs::is_regular_file(file, ec)) {
            fs::remove(file, ec);
            fs::path meta = file;
            meta += ".meta";
            fs::remove(meta, ec);
            project->onAssetFilesChanged(project->getAssetDatabase().removeAssets(file));
        }
        component->navMeshData.clear();
        sceneProject->isModified = true;
    }
    sceneProject->needUpdateRender = true;
    return true;
}

bool NavigationEditor::isBaking(uint32_t sceneId, Entity surface) const {
    for (const Pending& entry : pending) {
        if (entry.sceneId == sceneId && entry.surface == surface) return true;
    }
    return false;
}

void NavigationEditor::update(Project* project) {
    if (!project) return;
    // Play lands its own bakes in fixedUpdate; the stopped scenes' bakes land here
    for (SceneProject& sceneProject : project->getScenes()) {
        if (!sceneProject.scene || sceneProject.playState != ScenePlayState::STOPPED) continue;
        auto system = sceneProject.scene->getSystem<NavigationSystem>();
        if (system && system->isAnyBaking()) {
            system->processBakes(nav::getSettings().webBakeBudgetMs);
            sceneProject.needUpdateRender = true;
        }
        // grids with Auto Rebuild follow their tilemap or terrain while editing too (R36)
        if (system && sceneProject.scene->getComponentArray<NavGridComponent>()->size() > 0) {
            system->updateGridSources();
        }
        // runtime area volumes and link Activated show in edit-mode queries and overlays (S55 D9)
        if (system && system->syncAreaOverrides()) sceneProject.needUpdateRender = true;
    }
    for (size_t i = 0; i < pending.size();) {
        Pending& entry = pending[i];
        SceneProject* sceneProject = project->getScene(entry.sceneId);
        auto system = sceneProject && sceneProject->scene ? sceneProject->scene->getSystem<NavigationSystem>() : nullptr;
        if (!system) {
            NavBakeResult gone;
            gone.error = "the scene closed";
            Pending done = std::move(entry);
            pending.erase(pending.begin() + (long)i);
            for (BakeCallback& callback : done.callbacks) callback(gone);
            continue;
        }
        if (system->isBaking(entry.surface)) {
            i++;
            continue;
        }
        const NavBakeResult* last = system->getLastBakeResult(entry.surface);
        NavBakeResult result = last ? *last : NavBakeResult();
        if (!last) result.error = "the bake did not finish";
        Pending done = std::move(entry);
        pending.erase(pending.begin() + (long)i);
        finish(project, done, result);
    }
    if (!pending.empty()) Backend::getApp().requestFrame();
    // a kept input can be MBs: only while the slope overlay or the Path Tester asks for it; an
    // entry no one read for a minute (a closed scene, a removed surface) goes
    const double now = nowSeconds();
    for (InputCache& cache : inputCache) {
        if (cache.report.input && now - cache.inputAskedAt > 2.0) cache.report.input.reset();
    }
    inputCache.erase(std::remove_if(inputCache.begin(), inputCache.end(), [&](const InputCache& cache) { return now - cache.checkedAt > 60.0; }),
                     inputCache.end());
}

void NavigationEditor::finish(Project* project, Pending& entry, const NavBakeResult& bakeResult) {
    NavBakeResult result = bakeResult;
    SceneProject* sceneProject = project->getScene(entry.sceneId);
    NavigationSystem* system = sceneProject ? systemOf(*sceneProject) : nullptr;
    NavMeshSurfaceComponent* component = system ? sceneProject->scene->findComponent<NavMeshSurfaceComponent>(entry.surface) : nullptr;
    if (result.ok && component) {
        const std::vector<uint8_t> bytes = system->getNavMeshFile(entry.surface);
        if (bytes.empty()) {
            result.ok = false;
            result.error = "the bake produced no navmesh";
            result.errorCode = nav::NavBakeError::NavMeshFailed;
            result.hint = nav::bakeErrorHint(result.errorCode);
        } else {
            AssetRef ref;
            std::string error;
            if (writeAsset(project, navMeshFileOf(project, *sceneProject, entry.surface), bytes, ref, error)) {
                if (component->navMeshData != ref) {
                    component->navMeshData = ref;
                    sceneProject->isModified = true;
                }
                system->adoptNavMeshAsset(entry.surface);
            } else {
                result.ok = false;
                result.error = "the navmesh was baked but not saved: " + error;
                result.errorCode = nav::NavBakeError::SaveFailed;
                result.hint = nav::bakeErrorHint(result.errorCode);
                Out::error("NavMeshSurface '%s': %s", sceneProject->scene->getEntityName(entry.surface).c_str(), result.error.c_str());
            }
        }
        inputCache.erase(std::remove_if(inputCache.begin(), inputCache.end(), [&](const InputCache& cache) {
            return cache.sceneId == entry.sceneId && cache.surface == entry.surface;
        }), inputCache.end());
        sceneProject->needUpdateRender = true;
    }
    if (sceneProject && sceneProject->scene && !result.cancelled) {
        const std::string name = sceneProject->scene->getEntityName(entry.surface);
        if (result.ok) {
            Out::info("NavMeshSurface '%s' baked: %d of %d tiles built, %d polys, %.0f ms", name.c_str(), result.tilesBuilt,
                      result.tilesTotal, result.polys, result.milliseconds);
        }
        for (const std::string& warning : result.warnings) Out::warning("NavMeshSurface '%s': %s", name.c_str(), warning.c_str());
    }
    for (BakeCallback& callback : entry.callbacks) callback(result);
}

void NavigationEditor::saveSceneAssets(Project* project, SceneProject& sceneProject) {
    if (!sceneProject.scene) return;
    auto array = sceneProject.scene->getComponentArray<NavGridComponent>();
    for (size_t i = 0; i < array->size(); i++) {
        NavGridComponent& grid = array->getComponentFromIndex(i);
        const Entity entity = array->getEntity(i);
        if (!grid.map || !grid.mapDirty) continue;
        fs::path file;
        if (!grid.gridData.path.empty()) file = (project->getAssetsPath() / grid.gridData.path).lexically_normal();
        if (file.empty() || !inside(file, project->getAssetsPath()))
            file = defaultAssetPath(project, sceneProject, entity, "NavGrid", "navgrid");
        AssetRef ref;
        std::string error;
        if (writeAsset(project, file, grid.map->write(), ref, error)) {
            grid.gridData = ref;
            grid.mapDirty = false;
        } else {
            Out::error("NavGrid '%s' not saved: %s", sceneProject.scene->getEntityName(entity).c_str(), error.c_str());
        }
    }
}

namespace {

    bool snapshotGrid(Project* project, uint32_t sceneId, Entity grid, NavigationSystem*& system, nav::NavGridMap*& map,
                      std::vector<uint8_t>& bytes, std::string* error) {
        SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
        system = sceneProject ? NavigationEditor::systemOf(*sceneProject) : nullptr;
        map = system ? system->getGridMap(grid) : nullptr;
        if (!map) {
            if (error) *error = "entity " + std::to_string(grid) + " has no NavGrid";
            return false;
        }
        bytes = map->write();
        return true;
    }

}

bool NavigationEditor::rebuildGrid(Project* project, uint32_t sceneId, Entity grid, std::string* error) {
    NavigationSystem* system = nullptr;
    nav::NavGridMap* map = nullptr;
    std::vector<uint8_t> before;
    if (!snapshotGrid(project, sceneId, grid, system, map, before, error)) return false;
    std::string why;
    if (!system->rebuildGrid(grid, why)) {
        if (error) *error = why;
        Out::error("NavGrid rebuild: %s", why.c_str());
        return false;
    }
    std::vector<uint8_t> after = map->write();
    if (after != before) {
        // already applied: the command's execute() writes the same cells again
        CommandHandle::get(sceneId)->addCommand(new NavGridCellsCmd(project, sceneId, grid, std::move(before), std::move(after),
                                                                    "Rebuild Nav Grid"));
    }
    return true;
}

bool NavigationEditor::resetGridOverrides(Project* project, uint32_t sceneId, Entity grid, std::string* error) {
    NavigationSystem* system = nullptr;
    nav::NavGridMap* map = nullptr;
    std::vector<uint8_t> before;
    if (!snapshotGrid(project, sceneId, grid, system, map, before, error)) return false;
    map->clearOverrides();
    std::string why;
    SceneProject* sceneProject = project->getScene(sceneId);
    const NavGridComponent* component = sceneProject->scene->findComponent<NavGridComponent>(grid);
    if (component && component->source != NavGridSource::NONE && !system->rebuildGrid(grid, why)) {
        Out::warning("NavGrid rebuild: %s", why.c_str());
    }
    std::vector<uint8_t> after = map->write();
    if (after != before) {
        CommandHandle::get(sceneId)->addCommand(new NavGridCellsCmd(project, sceneId, grid, std::move(before), std::move(after),
                                                                    "Reset Nav Grid Overrides"));
    }
    return true;
}

int NavigationEditor::dirtyTiles(Project* project, uint32_t sceneId, Entity surface) {
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    const NavigationSystem::InputReport* report = sceneProject ? inputReport(*sceneProject, surface) : nullptr;
    return report ? report->dirtyTiles : -1;
}

const NavigationSystem::InputReport* NavigationEditor::inputReport(SceneProject& sceneProject, Entity surface, bool keepInput) {
    NavigationSystem* system = systemOf(sceneProject);
    // a game's surfaces keep no tile records to compare with
    if (!system || sceneProject.playState != ScenePlayState::STOPPED || isBaking(sceneProject.id, surface)) return nullptr;
    const double now = nowSeconds();
    auto it = std::find_if(inputCache.begin(), inputCache.end(),
                           [&](const InputCache& cache) {
                               return cache.sceneId == sceneProject.id && cache.scene == sceneProject.scene && cache.surface == surface;
                           });
    if (it == inputCache.end()) {
        InputCache cache;
        cache.sceneId = sceneProject.id;
        cache.scene = sceneProject.scene;
        cache.surface = surface;
        inputCache.push_back(std::move(cache));
        it = inputCache.end() - 1;
    } else if (now - it->checkedAt < 1.0 && (!keepInput || it->report.input)) {
        // collecting reads the whole scene: once a second at most
        if (keepInput) it->inputAskedAt = now;
        return &it->report;
    }
    nav::NavCollectOptions options;
    options.recordSources = keepInput;
    it->report = system->inspectInputs(surface, options, keepInput);
    it->checkedAt = now;
    if (keepInput) it->inputAskedAt = now;
    return &it->report;
}

std::shared_ptr<const nav::NavBakeInput> NavigationEditor::keptInput(SceneProject& sceneProject, Entity surface) {
    const NavigationSystem::InputReport* report = inputReport(sceneProject, surface, true);
    return report ? report->input : nullptr;
}

// ---------------------------------------------------------------- the overlay's query

void NavigationEditor::setQueryDraw(uint32_t sceneId, NavQueryDraw draw) {
    queryDraws[sceneId] = std::move(draw);
}

void NavigationEditor::clearQueryDraw(uint32_t sceneId) {
    queryDraws.erase(sceneId);
}

const NavQueryDraw* NavigationEditor::getQueryDraw(uint32_t sceneId) const {
    auto it = queryDraws.find(sceneId);
    return it != queryDraws.end() ? &it->second : nullptr;
}

NavigationOverlay* NavigationEditor::overlayOf(uint32_t sceneId) {
    Project* project = Backend::getApp().getProject();
    SceneProject* sceneProject = project ? project->getScene(sceneId) : nullptr;
    return sceneProject && sceneProject->sceneRender ? sceneProject->sceneRender->getNavigationOverlay() : nullptr;
}

// ---------------------------------------------------------------- the nav-grid brush

struct NavigationEditor::BrushStroke {
    Scene* scene = nullptr;
    uint32_t sceneId = 0;
    Entity grid = NULL_ENTITY;
    NavGridBrush::Stroke edits;
    nav::GridCell last;
    // where the stroke began (a Shift-drag's line starts there)
    nav::GridCell anchor;
    bool started = false;
};

bool NavigationEditor::isBrushPainting(Scene* scene) const {
    if (brushGrid == NULL_ENTITY || !scene) return false;
    Project* project = Backend::getApp().getProject();
    SceneProject* sceneProject = project ? project->getScene(brushScene) : nullptr;
    return sceneProject && sceneProject->scene == scene && sceneProject->playState == ScenePlayState::STOPPED &&
           scene->findComponent<NavGridComponent>(brushGrid) != nullptr;
}

bool NavigationEditor::cellUnderRay(Scene* scene, Entity grid, const Ray& ray, nav::GridCell& cell) {
    const NavGridComponent* component = scene->findComponent<NavGridComponent>(grid);
    const Transform* transform = scene->findComponent<Transform>(grid);
    auto system = scene->getSystem<NavigationSystem>();
    if (!component || !transform || !system) return false;
    const Vector3 normal = component->plane == nav::NavPlane::XY ? Vector3(0, 0, 1) : transform->worldRotation * Vector3(0, 1, 0);
    const RayReturn hit = ray.intersects(Plane(normal, transform->worldPosition));
    if (!hit.hit) return false;
    cell = system->gridWorldToCell(grid, hit.point);
    return true;
}

void NavigationEditor::strokeTo(Scene* scene, nav::GridCell cell) {
    auto system = scene->getSystem<NavigationSystem>();
    nav::NavGridMap* map = system ? system->getGridMap(stroke->grid) : nullptr;
    if (!map) return;
    const uint8_t area = brush.mode == BrushMode::Erase ? nav::AREA_WALKABLE : (uint8_t)std::clamp(brush.area, 0, nav::AREA_COUNT - 1);
    if (brush.mode == BrushMode::Fill) {
        if (!stroke->started) stroke->edits.apply(*map, NavGridBrush::flood(*map, cell), area);
    } else {
        const NavGridBrush::Shape shape = brush.shape == BrushShape::Square ? NavGridBrush::Shape::Square : NavGridBrush::Shape::Circle;
        const std::vector<nav::GridCell> along = stroke->started ? NavGridBrush::line(*map, stroke->last, cell) : std::vector<nav::GridCell>{cell};
        for (const nav::GridCell& c : along) stroke->edits.apply(*map, NavGridBrush::stamp(*map, c, brush.size, shape), area);
    }
    stroke->last = cell;
    stroke->started = true;
    if (NavGridComponent* component = scene->findComponent<NavGridComponent>(stroke->grid)) component->mapDirty = true;
}

void NavigationEditor::beginBrushStroke(Project* project, Scene* scene, const Ray& ray, bool shift) {
    if (!isBrushPainting(scene)) return;
    nav::GridCell cell;
    if (!cellUnderRay(scene, brushGrid, ray, cell)) return;
    stroke = std::make_shared<BrushStroke>();
    stroke->scene = scene;
    stroke->sceneId = brushScene;
    stroke->grid = brushGrid;
    (void)project;
    // Shift: a straight line from where the last stroke ended
    stroke->anchor = cell;
    if (shift && hasLastStrokeEnd && brush.mode != BrushMode::Fill) {
        stroke->last = lastStrokeEnd;
        stroke->anchor = lastStrokeEnd;
        stroke->started = true;
    }
    strokeTo(scene, cell);
}

void NavigationEditor::paintBrushStroke(Scene* scene, const Ray& ray, bool shift) {
    if (!stroke || stroke->scene != scene || brush.mode == BrushMode::Fill) return;
    nav::GridCell cell;
    if (!cellUnderRay(scene, stroke->grid, ray, cell) || cell == stroke->last) return;
    if (shift) {
        // a straight line from the anchor: what the drag painted so far goes back first
        auto system = scene->getSystem<NavigationSystem>();
        if (nav::NavGridMap* map = system ? system->getGridMap(stroke->grid) : nullptr) {
            stroke->edits.revert(*map);
            stroke->edits = NavGridBrush::Stroke();
            stroke->last = stroke->anchor;
            stroke->started = false;
            strokeTo(scene, stroke->anchor);
        }
    }
    strokeTo(scene, cell);
}

void NavigationEditor::endBrushStroke(Project* project) {
    if (!stroke) return;
    std::shared_ptr<BrushStroke> done = std::move(stroke);
    stroke.reset();
    lastStrokeEnd = done->last;
    hasLastStrokeEnd = done->started;
    const std::vector<NavGridCellsCmd::Cell> cells = done->edits.cells();
    if (cells.empty() || !project) return;
    // the cells are painted already: the command's execute() writes the same values
    CommandHandle::get(done->sceneId)->addCommand(new NavGridCellsCmd(project, done->sceneId, done->grid, cells, "Paint Nav Grid"));
    if (SceneProject* sceneProject = project->getScene(done->sceneId)) sceneProject->needUpdateRender = true;
}

// ---------------------------------------------------------------- link handles

bool NavigationEditor::beginLinkHandleDrag(Project* project, uint32_t sceneId, Scene* scene, const Ray& ray,
                                           const std::vector<Entity>& selected) {
    (void)project;
    linkDrag = LinkDrag();
    float bestDistance = std::numeric_limits<float>::max();
    for (Entity entity : selected) {
        const NavMeshLinkComponent* link = scene->findComponent<NavMeshLinkComponent>(entity);
        const Transform* transform = scene->findComponent<Transform>(entity);
        if (!link || !transform) continue;
        for (int end = 0; end < 2; end++) {
            const Vector3 world = transform->modelMatrix * (end ? link->endPoint : link->startPoint);
            // how far the ray passes from the handle, and the grab radius there (it grows with the distance)
            const Vector3 toPoint = world - ray.getOrigin();
            const Vector3 direction = ray.getDirection().normalized();
            const float along = toPoint.dotProduct(direction);
            if (along <= 0.0f) continue;
            const float miss = (toPoint - direction * along).length();
            const float grab = std::max(0.15f, std::max(link->width * 0.5f, along * 0.02f));
            if (miss <= grab && along < bestDistance) {
                bestDistance = along;
                linkDrag.active = true;
                linkDrag.sceneId = sceneId;
                linkDrag.link = entity;
                linkDrag.end = end == 1;
                linkDrag.original = end ? link->endPoint : link->startPoint;
                linkDrag.planePoint = world;
                linkDrag.planeNormal = transform->worldRotation * Vector3(0, 1, 0);
            }
        }
    }
    return linkDrag.active;
}

void NavigationEditor::dragLinkHandle(Scene* scene, const Ray& ray) {
    if (!linkDrag.active) return;
    NavMeshLinkComponent* link = scene->findComponent<NavMeshLinkComponent>(linkDrag.link);
    const Transform* transform = scene->findComponent<Transform>(linkDrag.link);
    if (!link || !transform) return;
    const RayReturn hit = ray.intersects(Plane(linkDrag.planeNormal, linkDrag.planePoint));
    if (!hit.hit) return;
    // the GameObject's space (the handle moves on the plane it was grabbed on)
    const Vector3 local = transform->modelMatrix.inverse() * hit.point;
    (linkDrag.end ? link->endPoint : link->startPoint) = local;
}

void NavigationEditor::endLinkHandleDrag(Project* project) {
    if (!linkDrag.active) return;
    const LinkDrag drag = linkDrag;
    linkDrag = LinkDrag();
    SceneProject* sceneProject = project ? project->getScene(drag.sceneId) : nullptr;
    NavMeshLinkComponent* link = sceneProject && sceneProject->scene ? sceneProject->scene->findComponent<NavMeshLinkComponent>(drag.link) : nullptr;
    if (!link) return;
    Vector3& field = drag.end ? link->endPoint : link->startPoint;
    const Vector3 moved = field;
    if (moved == drag.original) return;
    // the command records the value before the drag and sets the dragged one (one undo step)
    field = drag.original;
    Command* cmd = new PropertyCmd<Vector3>(project, drag.sceneId, drag.link, ComponentType::NavMeshLinkComponent,
                                            drag.end ? "endPoint" : "startPoint", moved);
    CommandHandle::get(drag.sceneId)->addCommand(cmd);
    cmd->setNoMerge();
    sceneProject->needUpdateRender = true;
}

