// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilemapSceneTools.h"

#include "Engine.h"
#include "Out.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "component/GridComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "math/Matrix4.h"
#include "render/SceneRender.h"
#include "shortcuts/CommandCatalog.h"
#include "subsystem/TilemapSystem.h"
#include "tilemap/TilePageTextures.h"
#include "tilemap/TilemapCellsCmd.h"
#include "util/UIProbe.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::tilemap;

// file-local, in a namespace of its own: unity builds merge the editor's sources
namespace jasperengine::editor::tilemapscenetools {

    const char* commandIdOf(TileTool tool) {
        switch (tool) {
            case TileTool::Select: return "tilePalette.select";
            case TileTool::Paint: return "tilePalette.paint";
            case TileTool::Box: return "tilePalette.box";
            case TileTool::Line: return "tilePalette.line";
            case TileTool::Flood: return "tilePalette.flood";
            case TileTool::Eraser: return "tilePalette.eraser";
            case TileTool::Picker: return "tilePalette.picker";
        }
        return "tilePalette.paint";
    }

    const char* idOf(TileTool tool) {
        switch (tool) {
            case TileTool::Select: return "tilemap.select";
            case TileTool::Paint: return "tilemap.paint";
            case TileTool::Box: return "tilemap.box";
            case TileTool::Line: return "tilemap.line";
            case TileTool::Flood: return "tilemap.flood";
            case TileTool::Eraser: return "tilemap.eraser";
            case TileTool::Picker: return "tilemap.picker";
        }
        return "tilemap.paint";
    }

    const char* toolName(TileTool tool) {
        switch (tool) {
            case TileTool::Select: return "Select";
            case TileTool::Paint: return "Paint";
            case TileTool::Box: return "Box";
            case TileTool::Line: return "Line";
            case TileTool::Flood: return "Flood";
            case TileTool::Eraser: return "Eraser";
            case TileTool::Picker: return "Picker";
        }
        return "Paint";
    }

    // a stamp without holes covers its bounding box; this gives its width and height
    void stampSize(const BrushStamp& stamp, int& width, int& height) {
        width = stamp.width;
        height = stamp.height;
        if (width > 0 && height > 0) return;
        int maxX = 0, maxY = 0;
        for (const BrushCell& cell : stamp.cells) {
            maxX = std::max(maxX, cell.x);
            maxY = std::max(maxY, cell.y);
        }
        width = maxX + 1;
        height = maxY + 1;
    }

    // the image-space rotation of a tile's transform bits (Tiled's order: transpose first); a
    // 2 x 2 matrix over the bits, composed with the rotation, read back as bits
    struct Orientation { int a, b, c, d; };

    Orientation orientationOf(uint32_t flags) {
        const int sx = (flags & TILE_FLIP_X) ? -1 : 1;
        const int sy = (flags & TILE_FLIP_Y) ? -1 : 1;
        if (flags & TILE_TRANSPOSE) return Orientation{0, sx, sy, 0};
        return Orientation{sx, 0, 0, sy};
    }

    uint32_t flagsOf(const Orientation& o) {
        uint32_t flags = 0;
        if (o.a != 0) {
            if (o.a < 0) flags |= TILE_FLIP_X;
            if (o.d < 0) flags |= TILE_FLIP_Y;
        } else {
            flags |= TILE_TRANSPOSE;
            if (o.b < 0) flags |= TILE_FLIP_X;
            if (o.c < 0) flags |= TILE_FLIP_Y;
        }
        return flags;
    }

    // rotates a tile's bits a quarter turn clockwise in image space
    uint32_t rotateFlags(uint32_t flags, int direction) {
        const Orientation o = orientationOf(flags);
        // R = [[0, -1], [1, 0]] clockwise in image coordinates (y down)
        const Orientation rotated = direction > 0
            ? Orientation{-o.c, -o.d, o.a, o.b}
            : Orientation{o.c, o.d, -o.a, -o.b};
        return flagsOf(rotated);
    }

    Vector3 cellLocalToWorld(const TileLayerContext& context, TilePoint point) {
        float local[3] = {0.0f, 0.0f, 0.0f};
        tilegrid::planeToLocal(context.spec, point, local);
        return context.transform->modelMatrix * Vector3(local[0], local[1], local[2]);
    }

    // the ray through the layer's GameObject transform inverse, then onto the Grid's plane: false
    // when it runs parallel to the plane or away from it
    bool rayToPlane(const Ray& ray, const TileLayerContext& context, TilePoint& out) {
        const Matrix4 inverse = context.transform->modelMatrix.inverse();
        const Vector3 origin = inverse * ray.getOrigin();
        const Vector3 end = inverse * (ray.getOrigin() + ray.getDirection());
        const Vector3 direction = end - origin;
        const bool xz = context.spec.plane == TileGridPlane::XZ;
        const float planeValue = xz ? origin.y : origin.z;
        const float delta = xz ? direction.y : direction.z;
        if (std::fabs(delta) < 1e-9f) return false;
        const float t = -planeValue / delta;
        if (t < 0.0f) return false;
        const float local[3] = {origin.x + direction.x * t, origin.y + direction.y * t, origin.z + direction.z * t};
        out = tilegrid::localToPlane(context.spec, local);
        return true;
    }

    // the layer the context names is still the one a stroke began on
    bool sameLayer(const TileLayerContext& context, Entity layer) {
        return context.layer && context.entity == layer;
    }

}

using namespace jasperengine::editor::tilemapscenetools;

// ---- TilemapTools

TilemapTools& TilemapTools::get() {
    static TilemapTools instance;
    return instance;
}

void TilemapTools::setProject(Project* project) {
    if (projectPtr != project) {
        // a project switch: nothing of the old one survives
        layerEntity = NULL_ENTITY;
        selectionValid = false;
    }
    projectPtr = project;
}

void TilemapTools::setActiveLayer(uint32_t sceneId, Entity layer) {
    if (layerScene == sceneId && layerEntity == layer) return;
    if (activeTool) activeTool->cancel();
    layerScene = sceneId;
    layerEntity = layer;
    selectionValid = false;
}

TileLayerContext TilemapTools::resolve() const {
    TileLayerContext context;
    if (!projectPtr || layerEntity == NULL_ENTITY) return context;
    SceneProject* sceneProject = projectPtr->getScene(layerScene);
    if (!sceneProject || !sceneProject->scene || !sceneProject->scene->isEntityCreated(layerEntity)) return context;
    Scene* scene = sceneProject->scene;
    TilemapLayerComponent* layer = scene->findComponent<TilemapLayerComponent>(layerEntity);
    Transform* transform = scene->findComponent<Transform>(layerEntity);
    if (!layer || !transform) return context;
    context.sceneProject = sceneProject;
    context.scene = scene;
    context.entity = layerEntity;
    context.layer = layer;
    context.transform = transform;
    if (auto system = scene->getSystem<TilemapSystem>()) {
        context.grid = system->findGrid(layerEntity);
        context.spec = system->layerSpec(layerEntity, *layer);
    }
    if (context.grid != NULL_ENTITY) {
        if (const GridComponent* grid = scene->findComponent<GridComponent>(context.grid)) context.linked = grid->linked;
    }
    if (!layer->tileset.path.empty()) context.tileset = TilesetPool::get(layer->tileset.path);
    return context;
}

void TilemapTools::activate(TileTool tool) {
    if (!projectPtr) return;
    const uint32_t sceneId = projectPtr->getSelectedSceneId();
    toolMode = tool;
    SceneTools::activate(sceneId, std::make_unique<TilemapSceneTool>(sceneId, tool));
}

void TilemapTools::deactivate() {
    if (projectPtr) SceneTools::deactivate(projectPtr->getSelectedSceneId());
}

bool TilemapTools::isToolActive() const {
    return activeTool != nullptr;
}

void TilemapTools::setFocus(TileFocusMode mode) {
    focusMode = mode;
    if (!projectPtr) return;
    if (SceneProject* sceneProject = projectPtr->getScene(layerScene)) {
        if (sceneProject->scene) {
            // with a tool up its overlay dims again next frame; without one nothing stays dimmed
            if (mode == TileFocusMode::None || !activeTool) clearFocusDim(sceneProject->scene);
            sceneProject->needUpdateRender = true;
        }
    }
}

bool TilemapTools::applyFocusDim(Scene* scene) const {
    if (!scene) return false;
    const TileLayerContext context = resolve();
    // no focused layer here (Focus On None, or the active layer deleted or in another scene): no
    // layer of this scene stays dimmed
    if (focusMode == TileFocusMode::None || !context.layer || context.scene != scene) return clearFocusDim(scene);
    auto system = scene->getSystem<TilemapSystem>();
    auto layers = scene->getComponentArray<TilemapLayerComponent>();
    bool changed = false;
    for (size_t i = 0; i < layers->size(); i++) {
        TilemapLayerComponent& other = layers->getComponentFromIndex(i);
        const Entity entity = layers->getEntity(i);
        bool focused = entity == context.entity;
        if (focusMode == TileFocusMode::Grid && context.grid != NULL_ENTITY && system) focused = system->findGrid(entity) == context.grid;
        const float dim = focused ? 1.0f : 0.35f;
        changed = changed || other.focusDim != dim;
        other.focusDim = dim;
    }
    return changed;
}

bool TilemapTools::clearFocusDim(Scene* scene) const {
    if (!scene) return false;
    auto layers = scene->getComponentArray<TilemapLayerComponent>();
    bool changed = false;
    for (size_t i = 0; i < layers->size(); i++) {
        TilemapLayerComponent& layer = layers->getComponentFromIndex(i);
        changed = changed || layer.focusDim != 1.0f;
        layer.focusDim = 1.0f;
    }
    return changed;
}

void TilemapTools::rotateBrush(int direction) {
    TileBrushState& brush = brushState;
    if (brush.stamp.empty()) return;
    const TileLayerContext context = resolve();
    if (context.layer && context.spec.layout == TileGridLayout::Hexagon) {
        statusText = "Hexagon cells take flips only: rotating needs a transpose";
        return;
    }
    int width = 0, height = 0;
    stampSize(brush.stamp, width, height);
    for (BrushCell& cell : brush.stamp.cells) {
        const int x = cell.x, y = cell.y;
        // the cell offsets are y up: a quarter turn clockwise maps (x, y) to (y, w - 1 - x)
        if (direction > 0) {
            cell.x = y;
            cell.y = width - 1 - x;
        } else {
            cell.x = height - 1 - y;
            cell.y = x;
        }
        cell.flags = rotateFlags(cell.flags, direction);
    }
    std::swap(brush.stamp.width, brush.stamp.height);
}

void TilemapTools::flipBrush(bool vertical) {
    TileBrushState& brush = brushState;
    if (brush.stamp.empty()) return;
    int width = 0, height = 0;
    stampSize(brush.stamp, width, height);
    for (BrushCell& cell : brush.stamp.cells) {
        if (vertical) {
            cell.y = height - 1 - cell.y;
            cell.flags ^= TILE_FLIP_Y;
        } else {
            cell.x = width - 1 - cell.x;
            cell.flags ^= TILE_FLIP_X;
        }
    }
}

void TilemapTools::copySelection() {
    if (!selectionValid) return;
    const TileLayerContext context = resolve();
    if (!context.layer) return;
    BrushTarget target{context.layer->cells.edit(), context.tileset ? context.tileset->data.get() : nullptr, context.spec, context.layer->dualGrid, context.layer->seed};
    BrushStamp stamp;
    PixelProblem error;
    if (!copyRect(target, selectionRect, stamp, error)) {
        statusText = error.message;
        return;
    }
    brushState.stamp = std::move(stamp);
    brushState.terrainOn = false;
    brushState.randomOn = false;
    activate(TileTool::Paint);
}

void TilemapTools::deleteSelection() {
    if (!selectionValid) return;
    const TileLayerContext context = resolve();
    if (!context.layer) return;
    if (context.linked) {
        statusText = lockedLinkedError().message;
        return;
    }
    BrushTarget target{context.layer->cells.edit(), context.tileset ? context.tileset->data.get() : nullptr, context.spec, context.layer->dualGrid, context.layer->seed};
    BrushEdit edit;
    if (!deleteRect(target, selectionRect, edit)) {
        statusText = edit.error.message;
        return;
    }
    const std::vector<TileCellChange> changes = edit.patch.changes();
    if (!changes.empty()) {
        CommandHandle::get(context.sceneProject->id)->addCommandNoMerge(new TilemapCellsCmd(projectPtr, context.sceneProject->id, context.entity, changes, "Erase Tiles"));
    }
    selectionValid = false;
    context.sceneProject->needUpdateRender = true;
}

void TilemapTools::clearSelection() {
    selectionValid = false;
}

bool TilemapTools::onEscape() {
    if (activeTool && activeTool->stroke != TilemapSceneTool::StrokeShape::None) {
        activeTool->cancel();
        return true;
    }
    if (selectionValid) {
        selectionValid = false;
        return true;
    }
    return false;
}

void TilemapTools::cancelStroke() {
    if (activeTool) activeTool->cancel();
}

// ---- the tool

TilemapSceneTool::TilemapSceneTool(uint32_t sceneId, TileTool mode): sceneId(sceneId), mode(mode) {
    TilemapTools::get().activeTool = this;
    labelText = CommandCatalog::editor().tooltip(commandIdOf(mode), toolName(mode));
}

TilemapSceneTool::~TilemapSceneTool() {
    if (TilemapTools::get().activeTool == this) TilemapTools::get().activeTool = nullptr;
}

const char* TilemapSceneTool::id() const {
    return idOf(mode);
}

const char* TilemapSceneTool::label() const {
    return labelText.c_str();
}

bool TilemapSceneTool::available(const SceneProject& sceneProject) const {
    // Esc with nothing to cancel and no selection leaves tool mode (TilemapTools::onEscape), and
    // edits in Play would not be saved
    return !exitRequested && sceneProject.playState == ScenePlayState::STOPPED;
}

bool TilemapSceneTool::targetFor(TileLayerContext& context, bool refuseLinked) {
    context = TilemapTools::get().resolve();
    if (!context.layer) {
        statusText = "Choose an Active Tilemap in the Tile Palette";
        return false;
    }
    // a stroke goes to this view's undo history: a layer of another scene would be edited there
    if (context.sceneProject->id != sceneId) {
        statusText = "The Active Tilemap is in another scene: choose one of this scene in the Tile Palette";
        return false;
    }
    if (refuseLinked && context.linked) {
        const PixelProblem error = lockedLinkedError();
        // R30: refused in the status and the log, nothing edited
        statusText = error.kind + ": " + error.fix;
        Out::warning("%s", error.message.c_str());
        return false;
    }
    return true;
}

bool TilemapSceneTool::cellsAt(const SceneToolEvent& event, const TileLayerContext& context, CellCoord& logical, CellCoord& display) const {
    TilePoint point;
    if (!rayToPlane(event.ray, context, point)) return false;
    logical = tilegrid::localToCell(context.spec, point);
    display = logical;
    // a dual-grid tile sits half a cell down-left of its logical corner (R27)
    if (context.layer->dualGrid) display = tilegrid::shownCellAt(context.spec, point, tilegrid::dualGridShift(context.spec));
    return true;
}

void TilemapSceneTool::refreshStatus(const TileLayerContext& context) {
    TilemapTools& tools = TilemapTools::get();
    if (!hoverValid) {
        statusText = context.linked ? std::string("locked_linked: the Grid is linked to its source map") : std::string();
        return;
    }
    const TilemapLayerComponent& layer = *context.layer;
    char coords[64];
    std::snprintf(coords, sizeof(coords), "Cell (%d, %d)", hoverCell.x, hoverCell.y);
    statusText = coords;
    const TilesetData* data = context.tileset ? context.tileset->data.get() : nullptr;
    const int32_t tile = layer.cells.read().getTile(hoverDisplay);
    if (tile != NO_TILE) {
        statusText += " \xC2\xB7 ";
        if (const TileDef* def = data ? data->findTile(tile) : nullptr) statusText += data->tileName(*def);
        else statusText += "#" + std::to_string(tile);
    }
    const int terrain = layer.cells.read().getTerrain(hoverCell);
    if (terrain != NO_TERRAIN && data) {
        if (const TerrainDef* def = data->terrain(terrain)) statusText += " \xC2\xB7 " + def->name;
    }
    if (context.linked) statusText += " \xC2\xB7 locked_linked";
    else if (tools.brush().terrainOn && data) {
        if (const TerrainDef* def = data->terrain(tools.brush().terrain)) statusText += " \xC2\xB7 paint: " + def->name;
    }
}

void TilemapSceneTool::hover(const SceneToolEvent& event) {
    TileLayerContext context;
    hoverValid = false;
    if (!targetFor(context, false)) return;
    hoverValid = cellsAt(event, context, hoverCell, hoverDisplay);
    refreshStatus(context);
}

void TilemapSceneTool::beginStroke(StrokeShape shape, bool erase, Entity layer) {
    stroke = shape;
    strokeErase = erase;
    edit = BrushEdit();
    strokeLayer = layer;
}

// one brush application over a run of cells (the path of a Paint stroke, the cells of a Box or a
// Line): the brush's terrain, random entries or stamp, or an erase
void TilemapSceneTool::applyAll(const BrushTarget& target, const TileBrushState& brush, std::span<const CellCoord> cells) {
    PaintOptions options;
    options.keepTerrain = brush.keepTerrain;
    bool applied = true;
    if (brush.terrainOn) {
        applied = paintTerrain(target, cells, strokeErase ? NO_TERRAIN : brush.terrain, edit);
    } else if (strokeErase) {
        for (const CellCoord& cell : cells) applied = eraseCell(target, cell, edit) && applied;
    } else if (brush.randomOn && !brush.random.empty()) {
        applied = paintRandom(target, brush.random, strokeSeed, cells, options, edit);
    } else if (!brush.stamp.empty()) {
        for (const CellCoord& cell : cells) applied = paintStamp(target, brush.stamp, cell, options, edit) && applied;
    } else {
        statusText = "Choose a tile in the Tile Palette";
        return;
    }
    if (!applied) statusText = edit.error.message;
}

void TilemapSceneTool::paintPoint(const TileLayerContext& context, CellCoord from, CellCoord to) {
    TileBrushState& brush = TilemapTools::get().brush();
    BrushTarget target{context.layer->cells.edit(), context.tileset ? context.tileset->data.get() : nullptr, context.spec, context.layer->dualGrid, context.layer->seed};
    const std::vector<CellCoord> path = tilegrid::line(context.spec, from, to);
    applyAll(target, brush, path);
    context.sceneProject->needUpdateRender = true;
}

void TilemapSceneTool::paintShape(const TileLayerContext& context, CellCoord from, CellCoord to) {
    TileBrushState& brush = TilemapTools::get().brush();
    TileCells& cells = context.layer->cells.edit();
    // a Box and a Line preview by rewriting the store (R65): reverted, then applied at the new end
    edit.patch.revert(cells);
    edit.patch.clear();
    edit.terrain = TerrainReport();
    const BrushTarget target{cells, context.tileset ? context.tileset->data.get() : nullptr, context.spec, context.layer->dualGrid, context.layer->seed};
    PixelProblem cellsError;
    std::vector<CellCoord> covered;
    PaintOptions options;
    options.keepTerrain = brush.keepTerrain;
    bool applied = true;
    if (stroke == StrokeShape::Box) {
        const CellRect rect = CellRect::of(from, to);
        if (!brush.terrainOn && strokeErase) {
            applied = eraseBox(target, rect, edit);
        } else if (!brush.terrainOn && !(brush.randomOn && !brush.random.empty()) && !brush.stamp.empty()) {
            applied = boxFill(target, brush.stamp, rect, options, edit);
        } else {
            if (boxCells(rect, covered, cellsError)) applyAll(target, brush, covered);
            else { statusText = cellsError.message; context.sceneProject->needUpdateRender = true; return; }
        }
    } else {
        covered = tilegrid::line(context.spec, from, to);
        if (!brush.terrainOn && strokeErase) {
            applied = eraseLine(target, from, to, edit);
        } else if (!brush.terrainOn && !(brush.randomOn && !brush.random.empty()) && !brush.stamp.empty()) {
            applied = paintLine(target, brush.stamp, from, to, options, edit);
        } else {
            applyAll(target, brush, covered);
        }
    }
    if (!applied) statusText = edit.error.message;
    context.sceneProject->needUpdateRender = true;
}

void TilemapSceneTool::applyFlood(const TileLayerContext& context, CellCoord cell) {
    TileBrushState& brush = TilemapTools::get().brush();
    const BrushTarget target{context.layer->cells.edit(), context.tileset ? context.tileset->data.get() : nullptr, context.spec, context.layer->dualGrid, context.layer->seed};
    PaintOptions options;
    options.keepTerrain = brush.keepTerrain;
    bool applied = true;
    if (brush.terrainOn || (brush.randomOn && !brush.random.empty())) {
        std::vector<CellCoord> cells;
        PixelProblem error;
        applied = floodCells(target, cell, FloodScope::Contiguous, cells, error);
        if (applied) applyAll(target, brush, cells);
        else statusText = error.message;
    } else if (strokeErase) {
        applied = eraseFlood(target, cell, FloodScope::Contiguous, edit);
    } else if (!brush.stamp.empty()) {
        applied = floodFill(target, brush.stamp, cell, FloodScope::Contiguous, options, edit);
    } else {
        statusText = "Choose a tile in the Tile Palette";
    }
    if (!applied && edit.error.kind.size() > 0) statusText = edit.error.message;
    context.sceneProject->needUpdateRender = true;
}

const char* TilemapSceneTool::undoName() const {
    const TileBrushState& brush = TilemapTools::get().brush();
    if (stroke == StrokeShape::Move) return "Move Tiles";
    if (brush.terrainOn) return "Paint Terrain";
    if (strokeErase) return "Erase Tiles";
    if (stroke == StrokeShape::Box) return "Box Fill";
    if (stroke == StrokeShape::Flood) return "Flood Fill";
    return "Paint Tiles";
}

void TilemapSceneTool::finishStroke() {
    Project* project = TilemapTools::get().project();
    const std::vector<TileCellChange> changes = edit.patch.changes();
    if (project && !changes.empty()) {
        CommandHandle::get(sceneId)->addCommandNoMerge(new TilemapCellsCmd(project, sceneId, strokeLayer, changes, undoName()));
    }
    stroke = StrokeShape::None;
    edit = BrushEdit();
}

SceneToolPress TilemapSceneTool::press(const SceneToolEvent& event) {
    TileLayerContext context;
    TilemapTools& tools = TilemapTools::get();
    if (!targetFor(context, true)) return SceneToolPress::Captured;
    CellCoord logical, display;
    if (!cellsAt(event, context, logical, display)) return SceneToolPress::Captured;
    hoverValid = true;
    hoverCell = logical;
    hoverDisplay = display;

    const TileBrushState& brush = tools.brush();
    const bool terrainBrush = brush.terrainOn;
    const CellCoord cell = terrainBrush ? logical : display;

    // Ctrl picks (R65), with every tool
    if (event.ctrl) {
        pickAt(context, logical, display);
        return SceneToolPress::Captured;
    }
    if (mode == TileTool::Picker) {
        pickAt(context, logical, display);
        // the picker hands over to Paint in place: this tool is still the view's while its press runs
        mode = TileTool::Paint;
        labelText = CommandCatalog::editor().tooltip(commandIdOf(mode), toolName(mode));
        tools.toolMode = TileTool::Paint;
        return SceneToolPress::Captured;
    }
    if (mode == TileTool::Select) {
        // a press inside the selection moves it, elsewhere a new rectangle
        if (tools.hasSelection() && tools.selection().contains(display)) {
            beginStroke(StrokeShape::Move, false, context.entity);
            moveSource = tools.selection();
            strokeStart = strokeLast = display;
        } else {
            beginStroke(StrokeShape::Select, false, context.entity);
            strokeStart = strokeLast = display;
            tools.selectionRect = CellRect::of(display, display);
            tools.selectionValid = true;
        }
        return SceneToolPress::Captured;
    }

    const bool erase = mode == TileTool::Eraser || event.shift;
    strokeSeed = static_cast<uint32_t>(++strokeCounter);
    switch (mode) {
        case TileTool::Box:
            beginStroke(StrokeShape::Box, erase, context.entity);
            strokeStart = strokeLast = cell;
            paintShape(context, cell, cell);
            break;
        case TileTool::Line:
            beginStroke(StrokeShape::Line, erase, context.entity);
            strokeStart = strokeLast = cell;
            paintShape(context, cell, cell);
            break;
        case TileTool::Flood:
            beginStroke(StrokeShape::Flood, erase, context.entity);
            applyFlood(context, cell);
            strokeStart = strokeLast = cell;
            break;
        default:
            beginStroke(StrokeShape::Paint, erase, context.entity);
            strokeStart = strokeLast = cell;
            paintPoint(context, cell, cell);
            break;
    }
    return SceneToolPress::Captured;
}

void TilemapSceneTool::pickAt(const TileLayerContext& context, CellCoord logical, CellCoord display) {
    TilemapTools& tools = TilemapTools::get();
    TileBrushState& brush = tools.brush();
    const BrushTarget target{context.layer->cells.edit(), context.tileset ? context.tileset->data.get() : nullptr, context.spec, context.layer->dualGrid, context.layer->seed};
    const BrushStamp stamp = pick(target, display);
    const int terrain = context.layer->cells.read().getTerrain(logical);
    if (!stamp.empty()) {
        brush.stamp = stamp;
        brush.randomOn = false;
        brush.terrainOn = false;
    } else if (terrain != NO_TERRAIN) {
        // an empty cell with a terrain gives the terrain to paint
        brush.terrainOn = true;
        brush.terrain = terrain;
    }
    if (terrain != NO_TERRAIN && !stamp.empty()) {
        // a terrain cell keeps its tile as the stamp and its terrain for the next terrain pick
        brush.terrain = terrain;
    }
}

void TilemapSceneTool::drag(const SceneToolEvent& event) {
    if (stroke == StrokeShape::None) return;
    TileLayerContext context;
    TilemapTools& tools = TilemapTools::get();
    if (!targetFor(context, false) || !sameLayer(context, strokeLayer)) {
        cancel();
        return;
    }
    CellCoord logical, display;
    if (!cellsAt(event, context, logical, display)) return;
    hoverValid = true;
    hoverCell = logical;
    hoverDisplay = display;
    const TileBrushState& brush = tools.brush();
    const CellCoord cell = brush.terrainOn ? logical : display;

    if (stroke == StrokeShape::Select) {
        if (cell != strokeLast) {
            strokeLast = cell;
            tools.selectionRect = CellRect::of(strokeStart, strokeLast);
            context.sceneProject->needUpdateRender = true;
        }
    } else if (stroke == StrokeShape::Move) {
        const CellCoord delta{display.x - strokeStart.x, display.y - strokeStart.y};
        if (display != strokeLast) {
            strokeLast = display;
            TileCells& cells = context.layer->cells.edit();
            edit.patch.revert(cells);
            edit.patch.clear();
            const BrushTarget target{cells, context.tileset ? context.tileset->data.get() : nullptr, context.spec, context.layer->dualGrid, context.layer->seed};
            if (!moveRect(target, moveSource, delta, edit)) statusText = edit.error.message;
            context.sceneProject->needUpdateRender = true;
        }
    } else if (stroke == StrokeShape::Paint) {
        if (cell != strokeLast) {
            paintPoint(context, strokeLast, cell);
            strokeLast = cell;
        }
    } else if ((stroke == StrokeShape::Box || stroke == StrokeShape::Line) && cell != strokeLast) {
        strokeLast = cell;
        paintShape(context, strokeStart, strokeLast);
    }
    refreshStatus(context);
}

void TilemapSceneTool::release(const SceneToolEvent&) {
    TilemapTools& tools = TilemapTools::get();
    if (stroke == StrokeShape::Select) {
        // the rectangle stays as the selection; Ctrl+C copies, Delete deletes
        stroke = StrokeShape::None;
        return;
    }
    if (stroke == StrokeShape::Move) {
        // the selection follows the moved cells
        if (const auto context = tools.resolve(); context.layer) {
            const CellCoord delta{strokeLast.x - strokeStart.x, strokeLast.y - strokeStart.y};
            tools.selectionRect = CellRect{moveSource.minX + delta.x, moveSource.minY + delta.y, moveSource.maxX + delta.x, moveSource.maxY + delta.y};
        }
    }
    if (stroke != StrokeShape::None) finishStroke();
}

bool TilemapSceneTool::key(ImGuiKey key, const SceneToolEvent&) {
    if (key != ImGuiKey_Escape) return false;
    // Esc: the stroke first, then the selection, then the tool itself (R65, R66)
    if (TilemapTools::get().onEscape()) return true;
    exitRequested = true;
    return true;
}

void TilemapSceneTool::cancel() {
    TilemapTools& tools = TilemapTools::get();
    const TileLayerContext context = tools.resolve();
    if (context.layer && context.entity == strokeLayer && stroke != StrokeShape::None && !edit.patch.empty()) {
        edit.patch.revert(context.layer->cells.edit());
        context.sceneProject->needUpdateRender = true;
    }
    if (stroke == StrokeShape::Select) {
        tools.clearSelection();
    }
    stroke = StrokeShape::None;
    edit = BrushEdit();
    // the dim is the tool's while it is up (drawOverlay): leaving it undims this view's scene, also
    // when the active layer is gone or in another scene
    if (Project* project = tools.project()) {
        if (SceneProject* sceneProject = project->getScene(sceneId); sceneProject && sceneProject->scene && tools.clearFocusDim(sceneProject->scene)) {
            sceneProject->needUpdateRender = true;
        }
    }
}

// ---- overlay (R65)

void TilemapSceneTool::drawCellOutline(const TileLayerContext& context, SceneRender& render, ImDrawList& list, CellCoord cell, ImU32 color, float thickness, bool display) {
    const TileCellPolygon polygon = tilegrid::cellPolygon(context.spec, cell);
    const TilePoint shift = display && context.layer->dualGrid ? tilegrid::dualGridShift(context.spec) : TilePoint{0.0, 0.0};
    ImVec2 points[6];
    for (int i = 0; i < polygon.count; i++) {
        const Vector3 world = cellLocalToWorld(context, TilePoint{polygon.points[i].x + shift.x, polygon.points[i].y + shift.y});
        if (!SceneTools::worldToScreen(sceneId, render, world, points[i])) return;
    }
    list.AddPolyline(points, polygon.count, color, ImDrawFlags_Closed, thickness);
}

void TilemapSceneTool::drawGrid(const TileLayerContext& context, SceneRender& render, ImDrawList& list) {
    // the cells the view shows: the plane under the canvas corners, capped (a far zoom)
    const int viewWidth = render.getViewWidth() > 0 ? render.getViewWidth() : Engine::getCanvasWidth();
    const int viewHeight = render.getViewHeight() > 0 ? render.getViewHeight() : Engine::getCanvasHeight();
    const float cornerXs[4] = {0.0f, (float)viewWidth, (float)viewWidth, 0.0f};
    const float cornerYs[4] = {0.0f, 0.0f, (float)viewHeight, (float)viewHeight};
    CellRect rect;
    bool any = false;
    for (int i = 0; i < 4; i++) {
        TilePoint point;
        if (!rayToPlane(render.screenToRay(cornerXs[i], cornerYs[i]), context, point)) continue;
        rect.merge(tilegrid::localToCell(context.spec, point));
        any = true;
    }
    if (!any) {
        if (!hoverValid) return;
        rect.merge(hoverCell);
    }
    // a corner hit lands in the corner cell; one ring more shows the staggered and iso neighbours
    rect.minX -= 1; rect.minY -= 1; rect.maxX += 1; rect.maxY += 1;

    const ImU32 lineColor = IM_COL32(255, 255, 255, 40);
    const int64_t maxLines = 600;
    if (context.spec.layout == TileGridLayout::Rectangle) {
        // straight lines: the same picture for any zoom, cheap at any count
        rect.maxX = (int32_t)std::min<int64_t>(rect.maxX, rect.minX + maxLines);
        rect.maxY = (int32_t)std::min<int64_t>(rect.maxY, rect.minY + maxLines);
        const auto segment = [&](TilePoint a, TilePoint b) {
            ImVec2 p0, p1;
            if (SceneTools::worldToScreen(sceneId, render, cellLocalToWorld(context, a), p0) &&
                SceneTools::worldToScreen(sceneId, render, cellLocalToWorld(context, b), p1)) {
                list.AddLine(p0, p1, lineColor);
            }
        };
        const double pitchX = tilegrid::pitchX(context.spec), pitchY = tilegrid::pitchY(context.spec);
        const double width = context.spec.cellWidth, height = context.spec.cellHeight;
        const double y0 = rect.minY * pitchY, y1 = (double)rect.maxY * pitchY + height;
        const double x0 = rect.minX * pitchX, x1 = (double)rect.maxX * pitchX + width;
        for (int64_t x = rect.minX; x <= rect.maxX + 1; x++) {
            const double px = (double)x * pitchX;
            segment(TilePoint{px, y0}, TilePoint{px, y1});
            if (pitchX != width) segment(TilePoint{px - pitchX + width, y0}, TilePoint{px - pitchX + width, y1});
        }
        for (int64_t y = rect.minY; y <= rect.maxY + 1; y++) {
            const double py = (double)y * pitchY;
            segment(TilePoint{x0, py}, TilePoint{x1, py});
            if (pitchY != height) segment(TilePoint{x0, py - pitchY + height}, TilePoint{x1, py - pitchY + height});
        }
        return;
    }
    // diamonds and hexes: each cell's outline; capped on a far zoom, around the pointer, else around
    // the view's centre (a pointer off the view must not draw every cell it covers)
    if (rect.area() > 6000) {
        CellCoord around = hoverCell;
        if (!hoverValid) {
            TilePoint point;
            if (rayToPlane(render.screenToRay((float)viewWidth * 0.5f, (float)viewHeight * 0.5f), context, point)) around = tilegrid::localToCell(context.spec, point);
            else around = CellCoord{(int32_t)((rect.minX + rect.maxX) / 2), (int32_t)((rect.minY + rect.maxY) / 2)};
        }
        rect.minX = around.x - 40; rect.maxX = around.x + 40;
        rect.minY = around.y - 40; rect.maxY = around.y + 40;
    }
    int64_t outlines = 0;
    for (int64_t y = rect.minY; y <= rect.maxY; y++) {
        for (int64_t x = rect.minX; x <= rect.maxX; x++) {
            drawCellOutline(context, render, list, CellCoord{(int32_t)x, (int32_t)y}, lineColor, 1.0f, false);
            outlines++;
        }
    }
    if (UIProbe::enabled()) {
        // the UI tests read how many cells the overlay outlined (a name, as the probe holds rects)
        const ImVec2 corner = list.GetClipRectMin();
        UIProbe::rect("tilemap.overlay.cells." + std::to_string(outlines), corner.x + 1.0f, corner.y + 1.0f, corner.x + 2.0f, corner.y + 2.0f);
    }
}

void TilemapSceneTool::drawTileGhost(const TileLayerContext& context, SceneRender& render, ImDrawList& list, CellCoord cell, int32_t tile, uint32_t flags) {
    const TileRenderInfo* info = context.tileset ? context.tileset->tile(tile) : nullptr;
    if (!info) return;
    ImTextureID texture = TilePageTextures::get().page(context.tileset->pages[info->page], *context.tileset);
    if (!texture) return;
    const double ppu = std::max(1, Engine::getPixelsPerUnit());
    const double tileW = (double)info->width / ppu;
    // R8: the image's bottom-left (rectangle) or bottom-centre (other layouts) at the cell's anchor
    TilePoint anchor = tilegrid::cellToLocal(context.spec, cell);
    double originX = context.spec.layout == TileGridLayout::Rectangle ? anchor.x : anchor.x - tileW * 0.5;
    double originY = anchor.y;
    const TilePoint shift = tilegrid::tileShift(context.spec, context.layer->tileAnchor.x, context.layer->tileAnchor.y, context.layer->dualGrid);
    originX += (double)info->offsetX / ppu + shift.x;
    originY += (double)info->offsetY / ppu + shift.y;
    const double x0 = originX + info->x0 / ppu, x1 = originX + info->x1 / ppu;
    const double y0 = originY + info->y0 / ppu, y1 = originY + info->y1 / ppu;
    // the quad's corners on the plane, then the tile's flips and transpose in the texture's corners
    const TilePoint quad[4] = {{x0, y1}, {x1, y1}, {x1, y0}, {x0, y0}};
    ImVec2 screen[4];
    for (int i = 0; i < 4; i++) {
        if (!SceneTools::worldToScreen(sceneId, render, cellLocalToWorld(context, quad[i]), screen[i])) return;
    }
    ImVec2 uv[4] = {ImVec2(info->u0, info->v0), ImVec2(info->u1, info->v0), ImVec2(info->u1, info->v1), ImVec2(info->u0, info->v1)};
    if (flags & TILE_TRANSPOSE) std::swap(uv[1], uv[3]);
    if (flags & TILE_FLIP_X) {
        std::swap(uv[0], uv[1]);
        std::swap(uv[3], uv[2]);
    }
    if (flags & TILE_FLIP_Y) {
        std::swap(uv[0], uv[3]);
        std::swap(uv[1], uv[2]);
    }
    list.AddImageQuad(texture, screen[0], screen[1], screen[2], screen[3], uv[0], uv[1], uv[2], uv[3], IM_COL32(255, 255, 255, 128));
}

void TilemapSceneTool::drawGhost(const TileLayerContext& context, SceneRender& render, ImDrawList& list) {
    if (!hoverValid || !context.tileset) return;
    const TilesetData* data = context.tileset->data.get();
    TilemapTools& tools = TilemapTools::get();
    const TileBrushState& brush = tools.brush();
    if (brush.terrainOn) {
        // the terrain's colour over the logical cell under the pointer
        if (const TerrainDef* terrain = data->terrain(brush.terrain)) {
            int r = 255, g = 255, b = 255;
            std::sscanf(terrain->color.c_str(), "#%02x%02x%02x", &r, &g, &b);
            const TileCellPolygon polygon = tilegrid::cellPolygon(context.spec, hoverCell);
            ImVec2 points[6];
            bool visible = true;
            for (int i = 0; i < polygon.count && visible; i++) {
                visible = SceneTools::worldToScreen(sceneId, render, cellLocalToWorld(context, polygon.points[i]), points[i]);
            }
            if (visible) {
                list.AddConvexPolyFilled(points, polygon.count, IM_COL32(r, g, b, 128));
            }
        }
        return;
    }
    if (brush.randomOn && !brush.random.empty()) {
        drawTileGhost(context, render, list, hoverDisplay, brush.random.front().tile, brush.random.front().flags);
        return;
    }
    // the brush stamp, 50 % over its cells (R65)
    for (const BrushCell& cell : brush.stamp.cells) {
        drawTileGhost(context, render, list, CellCoord{hoverDisplay.x + cell.x, hoverDisplay.y + cell.y}, cell.tile, cell.flags);
    }
}

void TilemapSceneTool::drawOverlay(SceneRender& render, ImDrawList& list) {
    TilemapTools& tools = TilemapTools::get();
    TileLayerContext context = tools.resolve();
    if (!context.layer || context.sceneProject->id != sceneId) {
        // the active layer deleted or in another scene: what it dimmed here is undimmed
        Project* project = tools.project();
        SceneProject* view = project ? project->getScene(sceneId) : nullptr;
        if (view && view->scene && tools.clearFocusDim(view->scene)) view->needUpdateRender = true;
        return;
    }
    // the overlay is ImGui's, drawn every frame: the scene redraws only when the dim changes
    if (tools.applyFocusDim(context.scene)) context.sceneProject->needUpdateRender = true;
    drawGrid(context, render, list);

    const TileBrushState& brush = tools.brush();
    const bool display = !brush.terrainOn;
    if (hoverValid && stroke == StrokeShape::None && (mode == TileTool::Paint || mode == TileTool::Box || mode == TileTool::Line || mode == TileTool::Flood)) {
        drawGhost(context, render, list);
    }

    const ImU32 accent = IM_COL32(255, 196, 48, 230);
    // the box and line previews: the outlines of the cells the stroke is rewriting
    if (stroke == StrokeShape::Box) {
        const CellRect rect = CellRect::of(strokeStart, strokeLast);
        for (int64_t y = rect.minY; y <= rect.maxY; y++) {
            for (int64_t x = rect.minX; x <= rect.maxX; x++) {
                if (x == rect.minX || x == rect.maxX || y == rect.minY || y == rect.maxY)
                    drawCellOutline(context, render, list, CellCoord{(int32_t)x, (int32_t)y}, accent, 1.5f, display);
            }
        }
    } else if (stroke == StrokeShape::Line) {
        for (const CellCoord& cell : tilegrid::line(context.spec, strokeStart, strokeLast))
            drawCellOutline(context, render, list, cell, accent, 1.5f, display);
    }

    // the selection, marching dashes aside
    if (tools.hasSelection()) {
        const ImU32 selectionColor = IM_COL32(120, 200, 255, 230);
        const CellRect rect = tools.selection();
        for (int64_t y = rect.minY; y <= rect.maxY; y++) {
            for (int64_t x = rect.minX; x <= rect.maxX; x++) {
                if (x == rect.minX || x == rect.maxX || y == rect.minY || y == rect.maxY)
                    drawCellOutline(context, render, list, CellCoord{(int32_t)x, (int32_t)y}, selectionColor, 1.5f, true);
            }
        }
    }

    // the hovered cell
    if (hoverValid) {
        drawCellOutline(context, render, list, display ? hoverDisplay : hoverCell, accent, 2.0f, display);
    }
}
