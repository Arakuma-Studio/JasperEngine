// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilePaletteWindow.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHandle.h"
#include "command/CommandHistory.h"
#include "command/type/CreateEntityCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "component/GridComponent.h"
#include "component/MeshComponent.h"
#include "component/TilemapLayerComponent.h"
#include "component/Transform.h"
#include "external/IconsFontAwesome6.h"
#include "shortcuts/CommandCatalog.h"
#include "subsystem/TilemapSystem.h"
#include "tilemap/TilePageTextures.h"
#include "tilemap/TilemapSceneTools.h"
#include "tilemap/TilesetEdit.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <utility>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::tilemap;

namespace fs = std::filesystem;

namespace jasperengine::editor::tilepalette {

    struct ToolButton {
        TileTool tool;
        const char* icon;
        const char* name;
        const char* command;
    };

    constexpr ToolButton kToolButtons[] = {
        {TileTool::Select, ICON_FA_VECTOR_SQUARE, "Select", "tilePalette.select"},
        {TileTool::Paint, ICON_FA_PAINTBRUSH, "Paint", "tilePalette.paint"},
        {TileTool::Box, ICON_FA_SQUARE, "Box", "tilePalette.box"},
        {TileTool::Line, ICON_FA_PEN, "Line", "tilePalette.line"},
        {TileTool::Flood, ICON_FA_FILL_DRIP, "Flood", "tilePalette.flood"},
        {TileTool::Eraser, ICON_FA_ERASER, "Eraser", "tilePalette.eraser"},
        {TileTool::Picker, ICON_FA_EYE_DROPPER, "Picker", "tilePalette.picker"},
    };

    int nextOrderInLayer(Scene* scene, const std::vector<Entity>& layers) {
        int order = 0;
        bool any = false;
        for (Entity layer : layers) {
            const MeshComponent* mesh = scene->findComponent<MeshComponent>(layer);
            if (!mesh) continue;
            order = any ? std::max(order, (int)mesh->sortingOrder) : (int)mesh->sortingOrder;
            any = true;
        }
        return any ? order + 1 : 0;
    }

    ImU32 colorFromHex(const std::string& hex) {
        int r = 255, g = 255, b = 255;
        if (hex.size() == 7) std::sscanf(hex.c_str(), "#%02x%02x%02x", &r, &g, &b);
        return IM_COL32(r, g, b, 255);
    }

}

using namespace jasperengine::editor::tilepalette;

TilePaletteWindow::TilePaletteWindow(Project* project): project(project) {
    TilemapTools::get().setProject(project);
}

void TilePaletteWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

bool TilePaletteWindow::takesKeys() const {
    return windowOpen && focused && !ImGui::GetIO().WantTextInput;
}

void TilePaletteWindow::resetForProjectSwitch() {
    TilemapTools::get().setProject(project);
    windowOpen = false;
    selectionSource = -1;
    draggingTiles = false;
    TilePageTextures::get().clear();
}

void TilePaletteWindow::bindCommands(CommandCatalog& catalog) {
    auto bind = [&catalog, this](const char* id, std::function<void()> action, std::function<bool()> enabled) {
        const CommandDef* existing = catalog.find(id);
        if (!existing) return;
        CommandDef def = *existing;
        def.run = [action](const CommandArgs&) { action(); };
        def.enabled = std::move(enabled);
        // while the window has the keyboard, or a Scene tool is up: the Scene view's keys it shares
        // (L, [, ], Delete) belong to the tile tools then
        def.keyGate = [this]() { return !ImGui::IsAnyItemActive(); };
        catalog.add(std::move(def));
    };
    const auto always = []() { return true; };
    for (const ToolButton& button : kToolButtons) {
        const TileTool tool = button.tool;
        bind(button.command, [tool]() { TilemapTools::get().activate(tool); }, always);
    }
    bind("tilePalette.rotateCW", []() { TilemapTools::get().rotateBrush(1); }, always);
    bind("tilePalette.rotateCCW", []() { TilemapTools::get().rotateBrush(-1); }, always);
    bind("tilePalette.flipX", []() { TilemapTools::get().flipBrush(false); }, always);
    bind("tilePalette.flipY", []() { TilemapTools::get().flipBrush(true); }, always);
    bind("tilePalette.copySelection", []() { TilemapTools::get().copySelection(); }, []() { return TilemapTools::get().hasSelection(); });
    bind("tilePalette.deleteSelection", []() { TilemapTools::get().deleteSelection(); }, []() { return TilemapTools::get().hasSelection(); });
}

void TilePaletteWindow::gridLayers(SceneProject*& sceneProject, Entity& grid, std::vector<Entity>& layers) const {
    sceneProject = project->getSelectedScene();
    grid = NULL_ENTITY;
    layers.clear();
    if (!sceneProject || !sceneProject->scene) return;
    Scene* scene = sceneProject->scene;
    auto system = scene->getSystem<TilemapSystem>();
    const std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    if (selected.empty()) {
        // no selection: the Grid of the active layer keeps the list
        const Entity active = TilemapTools::get().activeLayer();
        if (active != NULL_ENTITY && scene->isEntityCreated(active) && system) grid = system->findGrid(active);
    } else if (scene->findComponent<GridComponent>(selected[0])) {
        grid = selected[0];
    } else if (system) {
        grid = system->findGrid(selected[0]);
    }
    auto components = scene->getComponentArray<TilemapLayerComponent>();
    std::vector<std::pair<std::string, Entity>> named;
    for (size_t i = 0; i < components->size(); i++) {
        const Entity entity = components->getEntity(i);
        if (system && system->findGrid(entity) != grid) continue;
        named.emplace_back(scene->getEntityName(entity), entity);
    }
    std::sort(named.begin(), named.end());
    for (auto& [name, entity] : named) layers.push_back(entity);
}

void TilePaletteWindow::syncActiveLayerFromSelection() {
    SceneProject* sceneProject = nullptr;
    Entity grid = NULL_ENTITY;
    std::vector<Entity> layers;
    gridLayers(sceneProject, grid, layers);
    if (!sceneProject) return;
    TilemapTools& tools = TilemapTools::get();
    // a selected layer becomes the active one; while the active one still belongs to the list it stays
    const std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
    for (Entity entity : selected) {
        if (sceneProject->scene->findComponent<TilemapLayerComponent>(entity)) {
            if (std::find(layers.begin(), layers.end(), entity) != layers.end() || layers.empty()) {
                tools.setActiveLayer(sceneProject->id, entity);
                return;
            }
        }
    }
    const Entity active = tools.activeLayer();
    const bool listed = std::find(layers.begin(), layers.end(), active) != layers.end() && tools.activeScene() == sceneProject->id;
    if (!listed) {
        tools.setActiveLayer(sceneProject->id, layers.empty() ? NULL_ENTITY : layers.front());
    }
}

void TilePaletteWindow::createLayer(SceneProject* sceneProject, Entity grid) {
    if (!sceneProject || grid == NULL_ENTITY) return;
    Scene* scene = sceneProject->scene;
    std::vector<Entity> layers;
    auto system = scene->getSystem<TilemapSystem>();
    auto components = scene->getComponentArray<TilemapLayerComponent>();
    for (size_t i = 0; i < components->size(); i++) {
        if (system && system->findGrid(components->getEntity(i)) == grid) layers.push_back(components->getEntity(i));
    }
    CommandHistory* history = CommandHandle::get(sceneProject->id);
    const size_t mark = history->mark();
    auto* create = new CreateEntityCmd(project, sceneProject->id, "Tilemap", EntityCreationType::TILEMAP_LAYER, grid, false);
    history->addCommandNoMerge(create);
    const Entity entity = create->getEntity();
    auto* properties = new MultiPropertyCmd();
    bool any = false;
    const int order = nextOrderInLayer(scene, layers);
    if (order != 0) {
        properties->addPropertyCmd<int>(project, sceneProject->id, entity, ComponentType::MeshComponent, "sortingOrder", order);
        any = true;
    }
    // the same tileset as the layer it stands beside, so it can paint at once
    if (!layers.empty()) {
        if (const TilemapLayerComponent* sibling = scene->findComponent<TilemapLayerComponent>(layers.front()); sibling && !sibling->tileset.path.empty()) {
            properties->addPropertyCmd<AssetRef>(project, sceneProject->id, entity, ComponentType::TilemapLayerComponent, "tileset", sibling->tileset);
            any = true;
        }
    }
    if (any) history->addCommandNoMerge(properties);
    else delete properties;
    history->groupSince(mark, "Create Layer");
    project->setSelectedEntity(sceneProject->id, entity);
    TilemapTools::get().setActiveLayer(sceneProject->id, entity);
}

void TilePaletteWindow::drawActiveTilemap() {
    SceneProject* sceneProject = nullptr;
    Entity grid = NULL_ENTITY;
    std::vector<Entity> layers;
    gridLayers(sceneProject, grid, layers);
    TilemapTools& tools = TilemapTools::get();

    std::string preview = "(none)";
    if (sceneProject && tools.activeLayer() != NULL_ENTITY && tools.activeScene() == sceneProject->id &&
        sceneProject->scene->isEntityCreated(tools.activeLayer())) {
        preview = sceneProject->scene->getEntityName(tools.activeLayer());
    }
    ImGui::TextUnformatted("Active Tilemap");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(180.0f));
    if (ImGui::BeginCombo("##ActiveTilemap", preview.c_str())) {
        for (Entity entity : layers) {
            const std::string name = sceneProject->scene->getEntityName(entity);
            ImGui::PushID((int)entity);
            const bool selected = entity == tools.activeLayer();
            if (ImGui::Selectable(name.c_str(), selected)) tools.setActiveLayer(sceneProject->id, entity);
            ImGui::PopID();
        }
        if (grid != NULL_ENTITY) {
            if (!layers.empty()) ImGui::Separator();
            if (ImGui::Selectable("Create Layer...")) createLayer(sceneProject, grid);
        } else if (layers.empty()) {
            ImGui::TextDisabled("Select a Grid in the Hierarchy");
        }
        ImGui::EndCombo();
    }
}

void TilePaletteWindow::drawToolbar() {
    TilemapTools& tools = TilemapTools::get();
    const CommandCatalog& catalog = CommandCatalog::editor();
    bool activeInView = tools.isToolActive();
    for (const ToolButton& button : kToolButtons) {
        ImGui::PushID(button.name);
        const bool selected = activeInView && tools.currentTool() == button.tool;
        if (selected) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
        const std::string label = std::string(button.icon) + "###tool_" + button.name;
        if (ImGui::Button(label.c_str())) tools.activate(button.tool);
        if (selected) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", catalog.tooltip(button.command, button.name).c_str());
        ImGui::PopID();
        ImGui::SameLine();
    }
    ImGui::SameLine(0.0f, Theme::dpi(16.0f));
    drawActiveTilemap();

    // rotate and flip of the brush (R64), Random and Keep Terrain
    TileBrushState& brush = tools.brush();
    ImGui::SameLine(0.0f, Theme::dpi(16.0f));
    if (ImGui::Button(ICON_FA_ROTATE_LEFT "##rotate_ccw")) tools.rotateBrush(-1);
    ImGui::SetItemTooltip("%s", catalog.tooltip("tilePalette.rotateCCW", "Rotate Brush Counter-Clockwise").c_str());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ROTATE_RIGHT "##rotate_cw")) tools.rotateBrush(1);
    ImGui::SetItemTooltip("%s", catalog.tooltip("tilePalette.rotateCW", "Rotate Brush Clockwise").c_str());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_LEFT_RIGHT "##flip_x")) tools.flipBrush(false);
    ImGui::SetItemTooltip("%s", catalog.tooltip("tilePalette.flipX", "Flip Brush Horizontally").c_str());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_UP_DOWN "##flip_y")) tools.flipBrush(true);
    ImGui::SetItemTooltip("%s", catalog.tooltip("tilePalette.flipY", "Flip Brush Vertically").c_str());
    ImGui::SameLine();
    ImGui::Checkbox("Random", &brush.randomOn);
    ImGui::SameLine();
    ImGui::Checkbox("Keep Terrain", &brush.keepTerrain);

    ImGui::SameLine(0.0f, Theme::dpi(16.0f));
    static const char* const focusNames[] = {"None", "Layer", "Grid"};
    int focus = static_cast<int>(tools.focus());
    ImGui::TextUnformatted("Focus On");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(80.0f));
    if (ImGui::Combo("##FocusOn", &focus, focusNames, IM_ARRAYSIZE(focusNames))) tools.setFocus(static_cast<TileFocusMode>(focus));
}

std::vector<int32_t> TilePaletteWindow::selectedTiles(const TilesetRuntime& tileset) const {
    std::vector<int32_t> out;
    if (selectionSource < 0 || selectionMaxCol < selectionMinCol) return out;
    std::vector<TileGridCell> cells;
    int columns = 0, rows = 0;
    tileSourceGrid(tileset, selectionSource, cells, columns, rows);
    for (const TileGridCell& cell : cells) {
        if (cell.col >= selectionMinCol && cell.col <= selectionMaxCol && cell.row >= selectionMinRow && cell.row <= selectionMaxRow) out.push_back(cell.id);
    }
    return out;
}

void TilePaletteWindow::setStampFromSelection(const TilesetRuntime& tileset) {
    TilemapTools& tools = TilemapTools::get();
    TileBrushState& brush = tools.brush();
    std::vector<TileGridCell> cells;
    int columns = 0, rows = 0;
    tileSourceGrid(tileset, selectionSource, cells, columns, rows);
    BrushStamp stamp;
    stamp.width = selectionMaxCol - selectionMinCol + 1;
    stamp.height = selectionMaxRow - selectionMinRow + 1;
    for (const TileGridCell& cell : cells) {
        if (cell.col < selectionMinCol || cell.col > selectionMaxCol || cell.row < selectionMinRow || cell.row > selectionMaxRow) continue;
        BrushCell brushCell;
        brushCell.x = cell.col - selectionMinCol;
        // the image's rows run down, the stamp's offsets up (the selection's top row is its highest)
        brushCell.y = selectionMaxRow - cell.row;
        brushCell.tile = cell.id;
        stamp.cells.push_back(brushCell);
    }
    if (stamp.empty()) return;
    brush.stamp = std::move(stamp);
    brush.terrainOn = false;
    // a choice in the palette arms the Scene view (Paint, or the shape tool in use)
    if (!tools.isToolActive() || tools.currentTool() == TileTool::Select || tools.currentTool() == TileTool::Picker || tools.currentTool() == TileTool::Eraser) {
        tools.activate(TileTool::Paint);
    }
}

void TilePaletteWindow::drawTiles(const std::shared_ptr<const TilesetRuntime>& tileset) {
    const TilesetData* data = tileset->data.get();
    ImGui::SetNextItemWidth(Theme::dpi(60.0f));
    static const char* const zoomNames[] = {"1x", "2x", "3x", "4x"};
    int zoomIndex = zoom - 1;
    if (ImGui::Combo("Zoom##TileZoom", &zoomIndex, zoomNames, IM_ARRAYSIZE(zoomNames))) zoom = zoomIndex + 1;

    ImGui::BeginChild("##TilesScroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const float cell = static_cast<float>(std::max(data->tileWidth, 1)) * static_cast<float>(zoom);
    const float cellH = static_cast<float>(std::max(data->tileHeight, 1)) * static_cast<float>(zoom);
    const float gap = Theme::dpi(1.0f);
    const ImGuiIO& io = ImGui::GetIO();

    for (const TilesetSource& source : data->sources) {
        ImGui::PushID(source.id);
        ImGui::TextDisabled("%s", source.asset.path.c_str());
        std::vector<TileGridCell> cells;
        int columns = 0, rows = 0;
        tileSourceGrid(*tileset, source.id, cells, columns, rows);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size(static_cast<float>(columns) * (cell + gap), static_cast<float>(rows) * (cellH + gap));
        ImGui::InvisibleButton("##grid", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)));
        const bool hovered = ImGui::IsItemHovered();
        list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(40, 42, 48, 255));
        for (const TileGridCell& at : cells) {
            const ImVec2 min(origin.x + static_cast<float>(at.col) * (cell + gap), origin.y + static_cast<float>(at.row) * (cellH + gap));
            drawTileImage(list, min, static_cast<float>(zoom), *tileset, at.id);
        }
        // pointer to a cell of this source's grid
        const auto cellAt = [&](ImVec2 mouse, int& col, int& row) {
            col = std::clamp(static_cast<int>((mouse.x - origin.x) / (cell + gap)), 0, std::max(columns - 1, 0));
            row = std::clamp(static_cast<int>((mouse.y - origin.y) / (cellH + gap)), 0, std::max(rows - 1, 0));
        };
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            draggingTiles = true;
            dragSource = source.id;
            cellAt(io.MousePos, dragCol0, dragRow0);
            dragCol1 = dragCol0;
            dragRow1 = dragRow0;
        }
        if (draggingTiles && dragSource == source.id) {
            cellAt(io.MousePos, dragCol1, dragRow1);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                draggingTiles = false;
                selectionSource = source.id;
                selectionMinCol = std::min(dragCol0, dragCol1);
                selectionMaxCol = std::max(dragCol0, dragCol1);
                selectionMinRow = std::min(dragRow0, dragRow1);
                selectionMaxRow = std::max(dragRow0, dragRow1);
                setStampFromSelection(*tileset);
            }
        }
        // the selection rectangle (or the drag in progress)
        const bool dragging = draggingTiles && dragSource == source.id;
        if (dragging || selectionSource == source.id) {
            const int minCol = dragging ? std::min(dragCol0, dragCol1) : selectionMinCol;
            const int maxCol = dragging ? std::max(dragCol0, dragCol1) : selectionMaxCol;
            const int minRow = dragging ? std::min(dragRow0, dragRow1) : selectionMinRow;
            const int maxRow = dragging ? std::max(dragRow0, dragRow1) : selectionMaxRow;
            if (maxCol >= minCol && maxRow >= minRow) {
                const ImVec2 min(origin.x + static_cast<float>(minCol) * (cell + gap), origin.y + static_cast<float>(minRow) * (cellH + gap));
                const ImVec2 max(origin.x + static_cast<float>(maxCol + 1) * (cell + gap) - gap, origin.y + static_cast<float>(maxRow + 1) * (cellH + gap) - gap);
                list->AddRect(min, max, IM_COL32(255, 196, 48, 255), 0.0f, 0, Theme::dpi(2.0f));
            }
        }
        if (hovered && !draggingTiles) {
            int col = 0, row = 0;
            cellAt(io.MousePos, col, row);
            for (const TileGridCell& at : cells) {
                if (at.col == col && at.row == row) showTileTooltip(*tileset, at.id);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void TilePaletteWindow::drawTerrains(const std::shared_ptr<const TilesetRuntime>& tileset) {
    const TilesetData* data = tileset->data.get();
    TilemapTools& tools = TilemapTools::get();
    TileBrushState& brush = tools.brush();
    if (data->terrainSets.empty()) {
        ImGui::TextDisabled("This Tileset has no terrain sets: add them in the Tileset Editor.");
        return;
    }
    if (ImGui::Selectable("None", !brush.terrainOn)) {
        brush.terrainOn = false;
    }
    ImGuiStyle& style = ImGui::GetStyle();
    const float chip = ImGui::GetFontSize();
    for (size_t setIndex = 0; setIndex < data->terrainSets.size(); setIndex++) {
        const TerrainSetDef& set = data->terrainSets[setIndex];
        ImGui::SeparatorText((set.name + " (" + tileset::terrainModeName(set.mode) + ")").c_str());
        for (size_t local = 0; local < set.terrains.size(); local++) {
            const int global = data->terrainIndex((int)setIndex, (int)local);
            ImGui::PushID(global);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(("    " + set.terrains[local].name).c_str(), brush.terrainOn && brush.terrain == global)) {
                brush.terrainOn = true;
                brush.terrain = global;
                brush.randomOn = false;
                if (!tools.isToolActive() || tools.currentTool() == TileTool::Select || tools.currentTool() == TileTool::Picker) tools.activate(TileTool::Paint);
            }
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x + style.ItemInnerSpacing.x, pos.y + 1.0f), ImVec2(pos.x + chip, pos.y + chip - 1.0f), colorFromHex(set.terrains[local].color));
            ImGui::PopID();
        }
    }
}

void TilePaletteWindow::drawRandom(const std::shared_ptr<const TilesetRuntime>& tileset) {
    TileBrushState& brush = TilemapTools::get().brush();
    if (ImGui::Button("Add Selection")) {
        for (int32_t tile : selectedTiles(*tileset)) {
            bool present = false;
            for (const RandomEntry& entry : brush.random) present = present || entry.tile == tile;
            if (!present) brush.random.push_back(RandomEntry{tile, -1.0f, 0});
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) brush.random.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("Weights default to each tile's Probability");

    int removeIndex = -1;
    const float pixelZoom = 2.0f;
    const TilesetData* data = tileset->data.get();
    for (size_t i = 0; i < brush.random.size(); i++) {
        RandomEntry& entry = brush.random[i];
        ImGui::PushID((int)i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        drawTileImage(ImGui::GetWindowDrawList(), pos, pixelZoom, *tileset, entry.tile);
        ImGui::Dummy(ImVec2(static_cast<float>(data->tileWidth) * pixelZoom, static_cast<float>(data->tileHeight) * pixelZoom));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        const TileDef* def = data->findTile(entry.tile);
        ImGui::TextUnformatted(def ? data->tileName(*def).c_str() : "?");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(80.0f));
        ImGui::DragFloat("Weight##weight", &entry.weight, 0.05f, -1.0f, 100.0f, entry.weight < 0.0f ? "probability" : "%.2f");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_XMARK "##remove")) removeIndex = (int)i;
        ImGui::PopID();
    }
    if (removeIndex >= 0) brush.random.erase(brush.random.begin() + removeIndex);
}

void TilePaletteWindow::drawStamps(const std::shared_ptr<const TilesetRuntime>& tileset) {
    TilemapTools& tools = TilemapTools::get();
    TileBrushState& brush = tools.brush();
    const TilesetData* data = tileset->data.get();
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::InputText("##StampName", stampName, sizeof(stampName));
    ImGui::SameLine();
    ImGui::BeginDisabled(brush.stamp.empty() || stampName[0] == 0);
    if (ImGui::Button("Save Stamp")) {
        TilesetData next = *data;
        TileStamp stamp;
        stamp.name = stampName;
        int width = brush.stamp.width, height = brush.stamp.height;
        int maxX = 0, maxY = 0;
        for (const BrushCell& cell : brush.stamp.cells) {
            stamp.cells.push_back(TileStampCell{cell.x, cell.y, cell.tile, cell.flags});
            maxX = std::max(maxX, cell.x);
            maxY = std::max(maxY, cell.y);
        }
        stamp.width = width > 0 ? width : maxX + 1;
        stamp.height = height > 0 ? height : maxY + 1;
        next.stamps.push_back(std::move(stamp));
        writeTileset(next, "Save Stamp");
    }
    ImGui::EndDisabled();
    int removeIndex = -1;
    for (size_t i = 0; i < data->stamps.size(); i++) {
        const TileStamp& stamp = data->stamps[i];
        ImGui::PushID((int)i);
        const std::string label = stamp.name + " (" + std::to_string(stamp.width) + "x" + std::to_string(stamp.height) + ")";
        if (ImGui::Selectable(label.c_str())) {
            BrushStamp chosen;
            chosen.width = stamp.width;
            chosen.height = stamp.height;
            for (const TileStampCell& cell : stamp.cells) chosen.cells.push_back(BrushCell{cell.x, cell.y, cell.tile, cell.flags});
            brush.stamp = std::move(chosen);
            brush.terrainOn = false;
            if (!tools.isToolActive() || tools.currentTool() == TileTool::Select) tools.activate(TileTool::Paint);
        }
        if (ImGui::BeginPopupContextItem("##StampMenu")) {
            if (ImGui::MenuItem("Delete")) removeIndex = (int)i;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (removeIndex >= 0) {
        TilesetData next = *data;
        next.stamps.erase(next.stamps.begin() + removeIndex);
        writeTileset(next, "Delete Stamp");
    }
}

bool TilePaletteWindow::writeTileset(const TilesetData& data, const std::string& stepName) {
    const TileLayerContext context = TilemapTools::get().resolve();
    if (!context.layer || !context.tileset) return false;
    std::string error;
    const fs::path absolute = project->getAssetsPath() / context.layer->tileset.path;
    if (!writeTilesetFile(project, absolute, data, error, stepName)) {
        Out::error("Tile Palette: %s", error.c_str());
        return false;
    }
    return true;
}

void TilePaletteWindow::show() {
    TilePageTextures::get().beginFrame();
    if (!windowOpen) {
        focused = false;
        return;
    }
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    // first opened off the Scene view it paints: over the panels at the work area's bottom right
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 firstSize(Theme::dpi(640.0f), Theme::dpi(420.0f));
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - firstSize.x - Theme::dpi(8.0f),
                                   viewport->WorkPos.y + viewport->WorkSize.y - firstSize.y - Theme::dpi(8.0f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(firstSize, ImGuiCond_FirstUseEver);
    bool keepOpen = true;
    if (ImGui::Begin(WINDOW_NAME, &keepOpen)) {
        focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        syncActiveLayerFromSelection();
        drawToolbar();
        ImGui::Separator();
        TilemapTools& tools = TilemapTools::get();
        const TileLayerContext context = tools.resolve();
        if (!context.layer) {
            ImGui::TextDisabled("Select a Grid or a Tilemap Layer in the Hierarchy to choose an Active Tilemap.");
        } else if (!context.tileset) {
            // a set path that does not load says why, rather than "no Tileset"
            const std::string& path = context.layer->tileset.path;
            if (path.empty()) ImGui::TextDisabled("This Tilemap Layer has no Tileset: assign one in its Inspector.");
            else ImGui::TextColored(Theme::Colors::WarningText, "%s: %s", path.c_str(), TilesetPool::lastError(path).c_str());
        } else {
            if (context.linked) ImGui::TextColored(Theme::Colors::WarningText, "locked_linked: the Grid is linked to its source map (unlink it or edit the map)");
            if (!tools.statusLine().empty()) ImGui::TextDisabled("%s", tools.statusLine().c_str());
            if (ImGui::BeginTabBar("##TilePaletteTabs")) {
                if (ImGui::BeginTabItem("Tiles")) {
                    drawTiles(context.tileset);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Terrains")) {
                    drawTerrains(context.tileset);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Random")) {
                    drawRandom(context.tileset);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Stamps")) {
                    drawStamps(context.tileset);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
    } else {
        focused = false;
    }
    ImGui::End();
    if (!keepOpen) windowOpen = false;
}
