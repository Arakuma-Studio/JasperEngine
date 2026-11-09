// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The Grid and Tilemap Layer Inspectors, and the legacy Free Tilemap's conversion (docs/specs/
// S50-grid-tilemap.md R68, R69, R72). Their component fields are drawn from their reflection
// tables through propertyRow (undo, prefab overrides, multi-edit come with it); the layer list,
// the import rows and the buttons are single-selection.

#include "InspectorWindow.h"

#include "Catalog.h"
#include "Project.h"
#include "Out.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/DeleteEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/GridComponent.h"
#include "component/MeshComponent.h"
#include "component/TilemapComponent.h"
#include "component/TilemapLayerComponent.h"
#include "external/IconsFontAwesome6.h"
#include "reflect/Enums.h"
#include "reflect/Reflect.h"
#include "subsystem/TilemapSystem.h"
#include "tilemap/TilemapBrush.h"
#include "tilemap/TilemapCellsCmd.h"
#include "tilemap/TilemapConvert.h"
#include "tilemap/TilesetPool.h"
#include "tilemap/import/TilemapImportApply.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::inspectortilemap {

    // the Grid's layers in draw order: Order in Layer, then the hierarchy's
    std::vector<Entity> gridLayersByOrder(Scene* scene, Entity grid) {
        std::vector<Entity> layers;
        auto system = scene->getSystem<TilemapSystem>();
        auto components = scene->getComponentArray<TilemapLayerComponent>();
        for (size_t i = 0; i < components->size(); i++) {
            if (system && system->findGrid(components->getEntity(i)) == grid) layers.push_back(components->getEntity(i));
        }
        std::stable_sort(layers.begin(), layers.end(), [&](Entity a, Entity b) {
            const MeshComponent* meshA = scene->findComponent<MeshComponent>(a);
            const MeshComponent* meshB = scene->findComponent<MeshComponent>(b);
            return (meshA ? meshA->sortingOrder : 0) < (meshB ? meshB->sortingOrder : 0);
        });
        return layers;
    }

    constexpr const char* kLayerRowPayload = "TILEMAP_LAYER_ROW";

}

using namespace jasperengine::editor::inspectortilemap;

void editor::InspectorWindow::drawGridComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    if (entities.empty()) return;
    Scene* scene = sceneProject->scene;
    const reflect::ComponentDesc* desc = reflect::find(cpType);
    RowSettings base;
    if (desc && desc->edited) {
        auto edited = desc->edited;
        base.onValueChanged = [scene, entities, edited]() {
            for (Entity entity : entities) edited(scene, entity);
        };
    }
    RowSettings layout = base;
    layout.enumEntries = enumEntries(reflect::enums::kTileGridLayout);
    RowSettings plane = base;
    plane.enumEntries = enumEntries(reflect::enums::kTileGridPlane);
    plane.help = "XY for 2D, XZ for 3D boards";
    RowSettings axis = base;
    axis.enumEntries = enumEntries(reflect::enums::kStaggerAxis);
    RowSettings index = base;
    index.enumEntries = enumEntries(reflect::enums::kStaggerIndex);
    RowSettings side = base;
    side.help = "The hexagon's side; 0: a regular hexagon in the cell";

    const GridComponent& grid = scene->getComponent<GridComponent>(entities[0]);
    beginTable(cpType, getLabelSize("Hex Side Length"));
    propertyRow(RowPropertyType::Enum, cpType, "layout", "Layout", sceneProject, entities, layout);
    propertyRow(RowPropertyType::Vector2, cpType, "cellSize", "Cell Size", sceneProject, entities, base);
    propertyRow(RowPropertyType::Vector2, cpType, "cellGap", "Cell Gap", sceneProject, entities, base);
    propertyRow(RowPropertyType::Enum, cpType, "plane", "Plane", sceneProject, entities, plane);
    if (grid.layout == TileGridLayout::IsometricStaggered || grid.layout == TileGridLayout::Hexagon) {
        propertyRow(RowPropertyType::Enum, cpType, "staggerAxis", "Stagger Axis", sceneProject, entities, axis);
        propertyRow(RowPropertyType::Enum, cpType, "staggerIndex", "Stagger Index", sceneProject, entities, index);
    }
    if (grid.layout == TileGridLayout::Hexagon) {
        propertyRow(RowPropertyType::FloatPositive, cpType, "hexSideLength", "Hex Side Length", sceneProject, entities, side);
    }
    endTable();

    if (entities.size() != 1) return;
    const Entity gridEntity = entities[0];
    const uint32_t sceneId = sceneProject->id;
    CommandHistory* history = CommandHandle::get(sceneId);

    // the layer list (R68): add, remove, drag to reorder = Order in Layer
    separatorText("Tilemap Layers");
    std::vector<Entity> layers = gridLayersByOrder(scene, gridEntity);
    Entity reorderSource = NULL_ENTITY, reorderTarget = NULL_ENTITY;
    Entity remove = NULL_ENTITY;
    for (size_t i = 0; i < layers.size(); i++) {
        const Entity layer = layers[i];
        ImGui::PushID((int)layer);
        const std::string name = scene->getEntityName(layer);
        const MeshComponent* mesh = scene->findComponent<MeshComponent>(layer);
        const std::string label = std::string(ICON_FA_GRIP_LINES " ") + name + "  (Order " + std::to_string(mesh ? mesh->sortingOrder : 0) + ")";
        ImGui::Selectable(label.c_str());
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(kLayerRowPayload, &layer, sizeof(layer));
            ImGui::TextUnformatted(name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kLayerRowPayload)) {
                reorderSource = *static_cast<const Entity*>(payload->Data);
                reorderTarget = layer;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight());
        if (ImGui::SmallButton(ICON_FA_XMARK "##removeLayer")) remove = layer;
        ImGui::SetItemTooltip("Delete this layer's GameObject");
        ImGui::PopID();
    }
    if (ImGui::Button(ICON_FA_PLUS " Add Layer")) {
        const size_t mark = history->mark();
        auto* create = new CreateEntityCmd(project, sceneId, "Tilemap", EntityCreationType::TILEMAP_LAYER, gridEntity, false);
        history->addCommandNoMerge(create);
        const Entity entity = create->getEntity();
        auto* properties = new MultiPropertyCmd();
        bool any = false;
        if (!layers.empty()) {
            // the next Order in Layer (R3), the same tileset as the top layer
            const MeshComponent* top = scene->findComponent<MeshComponent>(layers.back());
            if (top) {
                properties->addPropertyCmd<int>(project, sceneId, entity, ComponentType::MeshComponent, "sortingOrder", top->sortingOrder + 1);
                any = true;
            }
            if (const TilemapLayerComponent* sibling = scene->findComponent<TilemapLayerComponent>(layers.back()); sibling && !sibling->tileset.path.empty()) {
                properties->addPropertyCmd<AssetRef>(project, sceneId, entity, ComponentType::TilemapLayerComponent, "tileset", sibling->tileset);
                any = true;
            }
        }
        if (any) history->addCommandNoMerge(properties);
        else delete properties;
        history->groupSince(mark, "Add Tilemap Layer");
        project->setSelectedEntity(sceneId, entity);
    }
    if (remove != NULL_ENTITY) {
        history->addCommandNoMerge(new DeleteEntityCmd(project, sceneId, remove));
    }
    if (reorderSource != NULL_ENTITY && reorderTarget != NULL_ENTITY && reorderSource != reorderTarget) {
        // the dragged layer takes the target's place; orders are renumbered in steps from the lowest
        auto from = std::find(layers.begin(), layers.end(), reorderSource);
        auto to = std::find(layers.begin(), layers.end(), reorderTarget);
        if (from != layers.end() && to != layers.end()) {
            const size_t fromIndex = (size_t)(from - layers.begin()), toIndex = (size_t)(to - layers.begin());
            const Entity moved = layers[fromIndex];
            layers.erase(layers.begin() + (long)fromIndex);
            layers.insert(layers.begin() + (long)toIndex, moved);
            auto* properties = new MultiPropertyCmd();
            bool any = false;
            int order = scene->getComponent<MeshComponent>(layers[0]).sortingOrder;
            for (size_t i = 0; i < layers.size(); i++, order++) {
                const MeshComponent& mesh = scene->getComponent<MeshComponent>(layers[i]);
                if (mesh.sortingOrder != order) {
                    properties->addPropertyCmd<int>(project, sceneId, layers[i], ComponentType::MeshComponent, "sortingOrder", order);
                    any = true;
                }
            }
            if (any) {
                properties->setNoMerge();
                history->addCommandNoMerge(properties);
            } else {
                delete properties;
            }
        }
    }

    // Import Source, Linked, Reimport (R47, R68)
    separatorText("Import");
    RowSettings linked = base;
    linked.help = "The layers are read-only and re-import when the source map changes";
    beginTable(cpType, getLabelSize("Import Source"));
    propertyRow(RowPropertyType::Asset, cpType, "importSource", "Import Source", sceneProject, entities, base);
    propertyRow(RowPropertyType::Bool, cpType, "linked", "Linked", sceneProject, entities, linked);
    endTable();
    ImGui::BeginDisabled(grid.importSource.empty());
    if (ImGui::Button("Reimport")) {
        tilemapimport::ImportResult result = tilemapimport::reimportLinked(project, sceneId, gridEntity);
        if (!result.ok) Out::error("Reimport: %s", result.problem.message.c_str());
    }
    ImGui::EndDisabled();
    if (grid.importSource.empty()) ImGui::SetItemTooltip("This Grid was not imported from a Tiled or LDtk map");
}

void editor::InspectorWindow::drawTilemapLayerComponent(ComponentType cpType, SceneProject* sceneProject, std::vector<Entity> entities) {
    if (entities.empty()) return;
    Scene* scene = sceneProject->scene;
    const reflect::ComponentDesc* desc = reflect::find(cpType);
    RowSettings base;
    if (desc && desc->edited) {
        auto edited = desc->edited;
        base.onValueChanged = [scene, entities, edited]() {
            for (Entity entity : entities) edited(scene, entity);
        };
    }
    RowSettings mode = base;
    mode.enumEntries = enumEntries(reflect::enums::kTilemapRenderMode);
    mode.help = "Individual sorts each tile on its own (characters walk behind tall tiles)";
    RowSettings tileset = base;
    tileset.help = "A .tileset asset";
    RowSettings chunkSize = base;
    chunkSize.help = "8, 16, 32 or 64 cells a side";
    RowSettings dual = base;
    dual.help = "Tiles drawn half a cell down-left, between four logical cells (rectangle and isometric Grids)";

    beginTable(cpType, getLabelSize("Animation Speed"));
    propertyRow(RowPropertyType::Asset, cpType, "tileset", "Tileset", sceneProject, entities, tileset);
    propertyRow(RowPropertyType::Color4L, cpType, "tint", "Tint", sceneProject, entities, base);
    propertyRow(RowPropertyType::Material, ComponentType::MeshComponent, "submeshes[0].material", "Material", sceneProject, entities, base);
    drawSortingRows(ComponentType::MeshComponent, sceneProject, entities);
    propertyRow(RowPropertyType::Enum, cpType, "mode", "Mode", sceneProject, entities, mode);
    propertyRow(RowPropertyType::Int, cpType, "chunkSize", "Chunk Size", sceneProject, entities, chunkSize);
    propertyRow(RowPropertyType::Vector2, cpType, "tileAnchor", "Tile Anchor", sceneProject, entities, base);
    propertyRow(RowPropertyType::FloatPositive, cpType, "animationSpeed", "Animation Speed", sceneProject, entities, base);
    propertyRow(RowPropertyType::Bool, cpType, "dualGrid", "Dual Grid", sceneProject, entities, dual);
    propertyRow(RowPropertyType::UInt, cpType, "seed", "Seed", sceneProject, entities, base);
    endTable();

    if (entities.size() != 1) return;
    const Entity layerEntity = entities[0];
    TilemapLayerComponent& layer = scene->getComponent<TilemapLayerComponent>(layerEntity);
    const TileCells& cells = layer.cells.read();
    const CellRect used = cells.usedRect();
    char rect[96] = "(empty)";
    if (!used.isEmpty()) std::snprintf(rect, sizeof(rect), "(%d, %d) to (%d, %d)", used.minX, used.minY, used.maxX, used.maxY);
    separatorText("Cells");
    ImGui::TextDisabled("Used Rect  %s", rect);
    ImGui::TextDisabled("Cells %u   Chunks %u", layer.cellCount, layer.chunkCount);

    const uint32_t sceneId = sceneProject->id;
    auto system = scene->getSystem<TilemapSystem>();
    const Entity gridEntity = system ? system->findGrid(layerEntity) : NULL_ENTITY;
    const GridComponent* grid = gridEntity != NULL_ENTITY ? scene->findComponent<GridComponent>(gridEntity) : nullptr;
    const bool linked = grid && grid->linked;
    if (linked) ImGui::TextColored(Theme::Colors::WarningText, "locked_linked: unlink the Grid or edit the source map");

    ImGui::BeginDisabled(linked);
    if (ImGui::Button("Clear")) {
        TileCells& store = layer.cells.edit();
        std::vector<CellCoord> used_cells;
        store.forEachUsed([&](CellCoord cell, uint32_t, uint8_t) { used_cells.push_back(cell); });
        tilemap::BrushEdit edit;
        for (const CellCoord& cell : used_cells) {
            edit.patch.setValue(store, cell, 0);
            edit.patch.setTerrainValue(store, cell, TERRAIN_NONE_VALUE);
        }
        CommandHandle::get(sceneId)->addCommandNoMerge(new tilemap::TilemapCellsCmd(project, sceneId, layerEntity, edit.patch.changes(), "Erase Tiles"));
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh Autotile")) {
        std::shared_ptr<const TilesetRuntime> runtime = layer.tileset.path.empty() ? nullptr : TilesetPool::get(layer.tileset.path);
        if (runtime && system) {
            const TileGridSpec spec = system->layerSpec(layerEntity, layer);
            tilemap::BrushTarget target{layer.cells.edit(), runtime->data.get(), spec, layer.dualGrid, layer.seed};
            tilemap::BrushEdit edit;
            if (tilemap::refreshTerrain(target, layer.cells.read().usedRectWithTerrain(), edit)) {
                CommandHandle::get(sceneId)->addCommandNoMerge(new tilemap::TilemapCellsCmd(project, sceneId, layerEntity, edit.patch.changes(), "Refresh Autotile"));
            } else {
                Out::warning("Refresh Autotile: %s", edit.error.message.c_str());
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Compress Bounds")) {
        // the cell store is sparse and drops empty chunks, so its bounds are always the tight ones
        Out::info("Compress Bounds: the layer's bounds are already tight (%s)", rect);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The store keeps only chunks with cells, so the used rectangle is always tight");
}

// the legacy Free Tilemap's "Convert to Grid Tilemap" (R50, R72), with check()'s result first
void editor::InspectorWindow::drawTilemapConvertSection(SceneProject* sceneProject, Entity entity) {
    const TilemapComponent* tilemap = sceneProject->scene->findComponent<TilemapComponent>(entity);
    const MeshComponent* mesh = sceneProject->scene->findComponent<MeshComponent>(entity);
    if (!tilemap || !mesh) return;
    separatorText("Grid Tilemap");
    const tilemap::ConvertCheck check = tilemap::check(*tilemap, *mesh);
    if (check.ok) {
        ImGui::TextDisabled("Convertible: %d x %d tiles, %d rect(s)", check.tileWidth, check.tileHeight, (int)check.rects.size());
    } else {
        ImGui::TextColored(Theme::Colors::WarningText, "%s", check.problem.message.c_str());
        for (const tilemap::ConvertOffender& offender : check.offenders) {
            ImGui::BulletText("%s: %s", offender.name.c_str(), offender.reason.c_str());
        }
    }
    for (const std::string& warning : check.warnings) ImGui::TextDisabled("%s", warning.c_str());
    ImGui::BeginDisabled(!check.ok);
    if (ImGui::Button("Convert to Grid Tilemap")) {
        const tilemap::ConvertResult result = tilemap::convert(project, sceneProject->id, entity);
        if (!result.ok) Out::error("Convert to Grid Tilemap: %s", result.problem.message.c_str());
        for (const std::string& warning : result.warnings) Out::warning("Convert to Grid Tilemap: %s", warning.c_str());
    }
    ImGui::EndDisabled();
    if (!check.ok) ImGui::SetItemTooltip("%s", check.problem.fix.c_str());
}
