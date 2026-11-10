// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "tilemap/TilesetEditorWindow.h"

#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "external/IconsFontAwesome6.h"
#include "pixelart/PixelImage.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"
#include "tilemap/TilePageTextures.h"
#include "tilemap/TilemapBrush.h"
#include "tilemap/TilesetEdit.h"
#include "util/FileUtils.h"
#include "util/UIProbe.h"
#include "util/Util.h"
#include "window/EditorModal.h"
#include "window/ProjectWindow.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::tilemap;

namespace fs = std::filesystem;

namespace jasperengine::editor::tileseteditor {

    constexpr float kTilePixelZoom = 2.0f;
    constexpr int kPainterZoom = 8;

    std::string readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

    ImU32 hexColor(const std::string& hex, int alpha = 255) {
        int r = 200, g = 200, b = 200;
        if (hex.size() == 7) std::sscanf(hex.c_str(), "#%02x%02x%02x", &r, &g, &b);
        return IM_COL32(r, g, b, alpha);
    }

    std::string hexOf(const float rgb[3]) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "#%02x%02x%02x", (int)std::lround(rgb[0] * 255.0f), (int)std::lround(rgb[1] * 255.0f), (int)std::lround(rgb[2] * 255.0f));
        return buffer;
    }

    // the 3 x 3 region of the square shape's peering slots (n ne e se s sw w nw) on a painter
    // laid out like the tile: rows top to bottom
    void slotCell(int slot, int& col, int& row) {
        static constexpr int kCols[8] = {1, 2, 2, 2, 1, 0, 0, 0};
        static constexpr int kRows[8] = {0, 0, 1, 2, 2, 2, 1, 0};
        col = kCols[slot];
        row = kRows[slot];
    }

    // a hex slot's direction from the tile's centre: sides then corners, counter-clockwise from
    // the first (east on pointy-top, north-east on flat-top); y up, as the screen's minus
    void hexSlotDirection(TileShape shape, int slot, float& dx, float& dy) {
        const float pi = 3.14159265f;
        float angle;
        if (slot < 6) {
            const float step = pi / 3.0f;
            angle = (shape == TileShape::HexPointy ? 0.0f : pi / 6.0f) + step * (float)slot;
        } else {
            const float step = pi / 3.0f;
            angle = (shape == TileShape::HexPointy ? pi / 6.0f : 0.0f) + step * (float)(slot - 6);
        }
        dx = std::cos(angle);
        dy = std::sin(angle);
    }

    std::string peeringLabel(const TilesetData& data, int set, int value) {
        if (value == PEERING_ANY) return "any";
        if (value == PEERING_NONE) return "none";
        if (set < 0 || (size_t)set >= data.terrainSets.size() || value < 0 || (size_t)value >= data.terrainSets[(size_t)set].terrains.size()) return "?";
        return data.terrainSets[(size_t)set].terrains[(size_t)value].name;
    }

}

using namespace jasperengine::editor::tileseteditor;

TilesetEditorWindow::TilesetEditorWindow(Project* project, ProjectWindow* projectWindow): project(project), projectWindow(projectWindow) {
}

void TilesetEditorWindow::setOpen(bool open) {
    if (open && !windowOpen) focusRequested = true;
    windowOpen = open;
}

void TilesetEditorWindow::resetForProjectSwitch() {
    windowOpen = false;
    file.clear();
    loaded = false;
    runtime.reset();
    selection.clear();
    notice.clear();
}

void TilesetEditorWindow::open(const fs::path& target) {
    fs::path absolute = target;
    if (absolute.is_relative()) absolute = project->getAssetsPath() / absolute;
    file = absolute;
    assetPath = project->normalizeToAssetsRelative(absolute).generic_string();
    selection.clear();
    rangeAnchor = NO_TILE;
    coverageValid = false;
    notice.clear();
    reload();
    setOpen(true);
    focusRequested = true;
}

std::filesystem::path TilesetEditorWindow::createEmpty(const fs::path& directory) {
    TilesetData data;
    data.name = "New Tileset";
    fs::path target = directory / "New Tileset.tileset";
    for (int n = 2; fs::exists(target); n++) target = directory / ("New Tileset " + std::to_string(n) + ".tileset");
    std::string error;
    if (!writeTilesetFile(project, target, data, error, "Create Tileset")) {
        Out::error("Create Tileset: %s", error.c_str());
        return {};
    }
    return target;
}

std::filesystem::path TilesetEditorWindow::createFromSource(const fs::path& source) {
    const std::string name = source.stem().string();
    const std::string relative = project->normalizeToAssetsRelative(source).generic_string();
    const AssetRef ref = project->makeAssetRef(relative, AssetPathBase::Assets);
    PixelProblem problem;
    TilesetData data;
    data.name = name;
    bool ok = false;
    int sourceId = 0;
    std::vector<std::string> warnings;
    if (SpriteSheetPool::isAsepritePath(relative)) {
        ok = addAsepriteSourceFromFile(data, ref, source, 0, warnings, sourceId, problem);
    } else if (SpriteSheetSources::isTexturePath(relative)) {
        TilesetPagePixels pixels;
        if (const PixelImage* image = pixels.get(relative)) {
            TextureSlicing slicing;
            ok = createFromTexture(name, ref, slicing, *image, data, problem);
        } else {
            problem.message = pixels.lastError();
        }
    } else {
        ok = addSheetSource(data, ref, {}, sourceId, problem);
    }
    if (!ok) {
        Out::error("Create Tileset: %s", problem.message.c_str());
        return {};
    }
    fs::path target = source.parent_path() / (name + ".tileset");
    for (int n = 2; fs::exists(target); n++) target = source.parent_path() / (name + " " + std::to_string(n) + ".tileset");
    std::string error;
    if (!writeTilesetFile(project, target, data, error, "Create Tileset")) {
        Out::error("Create Tileset: %s", error.c_str());
        return {};
    }
    open(target);
    return target;
}

void TilesetEditorWindow::setNotice(const std::string& text, bool error) {
    notice = text;
    noticeIsError = error;
    if (error) Out::warning("Tileset Editor: %s", text.c_str());
}

void TilesetEditorWindow::setDocument(const TilesetData& data) {
    document = data;
    runtime = TilesetPool::resolve(assetPath, std::make_shared<TilesetData>(document));
    coverageValid = false;
    selection.erase(std::remove_if(selection.begin(), selection.end(), [&](int32_t id) { return document.findTile(id) == nullptr; }), selection.end());
}

void TilesetEditorWindow::reload() {
    loadError.clear();
    loaded = false;
    runtime.reset();
    if (file.empty() || !fs::exists(file)) {
        loadError = "The file does not exist";
        return;
    }
    std::vector<TilesetProblem> problems;
    TilesetData parsed;
    if (!tileset::parse(readFile(file), parsed, problems)) {
        loadError = problems.empty() ? std::string("The file does not parse") : problems.front().message;
        return;
    }
    std::error_code error;
    fileTime = fs::last_write_time(file, error);
    loaded = true;
    setDocument(parsed);
}

void TilesetEditorWindow::pollDisk() {
    if (!windowOpen || file.empty()) return;
    const double now = ImGui::GetTime();
    if (now - lastDiskCheck < 0.5) return;
    lastDiskCheck = now;
    std::error_code error;
    const auto time = fs::last_write_time(file, error);
    if (!error && time != fileTime) reload();
}

bool TilesetEditorWindow::edit(const std::string& name, const std::function<void(TilesetData&)>& change) {
    if (!loaded) return false;
    TilesetData next = document;
    change(next);
    if (next == document) return true;
    std::vector<TilesetProblem> problems;
    tileset::validate(next, problems);
    if (!problems.empty()) {
        // the document stays as it was: a broken file is not written
        setNotice(problems.front().message + (problems.front().fix.empty() ? std::string() : " (" + problems.front().fix + ")"), true);
        return false;
    }
    std::string error;
    if (!writeTilesetFile(project, file, next, error, name)) {
        setNotice(error, true);
        return false;
    }
    setNotice(name, false);
    setDocument(next);
    std::error_code timeError;
    fileTime = fs::last_write_time(file, timeError);
    return true;
}

bool TilesetEditorWindow::isSelected(int32_t id) const {
    return std::binary_search(selection.begin(), selection.end(), id);
}

TileDef* TilesetEditorWindow::selectedFirst() {
    return selection.empty() ? nullptr : document.findTile(selection.front());
}

void TilesetEditorWindow::select(int32_t id, bool shift, bool ctrl) {
    if (shift && rangeAnchor != NO_TILE) {
        // the range in id order
        const int32_t lo = std::min(rangeAnchor, id), hi = std::max(rangeAnchor, id);
        if (!ctrl) selection.clear();
        for (const TileDef& tile : document.tiles) {
            if (tile.id >= lo && tile.id <= hi && !isSelected(tile.id)) selection.insert(std::upper_bound(selection.begin(), selection.end(), tile.id), tile.id);
        }
    } else if (ctrl) {
        auto it = std::lower_bound(selection.begin(), selection.end(), id);
        if (it != selection.end() && *it == id) selection.erase(it);
        else selection.insert(it, id);
        rangeAnchor = id;
    } else {
        selection.assign(1, id);
        rangeAnchor = id;
    }
    activePolygon = 0;
    dragPoint = -1;
}

void TilesetEditorWindow::forSelected(const std::string& name, const std::function<void(TileDef&)>& change) {
    forTiles(name, selection, change);
}

void TilesetEditorWindow::forTiles(const std::string& name, const std::vector<int32_t>& tiles, const std::function<void(TileDef&)>& change) {
    const std::vector<int32_t> ids = tiles;
    edit(name, [&](TilesetData& data) {
        for (int32_t id : ids) {
            if (TileDef* tile = data.findTile(id)) change(*tile);
        }
    });
}

template <typename T>
bool TilesetEditorWindow::heldField(const char* label, T* values, int count, const std::function<void()>& draw) {
    const ImGuiID id = ImGui::GetID(label);
    const int frame = ImGui::GetFrameCount();
    // a hold the field was not drawn for (its section closed) is over: the document is what it shows
    if (held.id == id && held.frame < frame - 1) held.id = 0;
    if (held.id == id) {
        for (int i = 0; i < count; i++) values[i] = static_cast<T>(held.values[i]);
    }
    draw();
    // the UI tests find the composite fields here (InputInt2, ColorEdit4: no item carries the label)
    if (UIProbe::enabled()) UIProbe::item(std::string("tilesetEditor.") + label);
    if (ImGui::IsItemActive()) {
        if (held.id != id) {
            held.id = id;
            held.tiles = selection;
        }
        held.frame = frame;
        for (int i = 0; i < count; i++) held.values[i] = static_cast<double>(values[i]);
        return false;
    }
    if (held.id != id) return false;
    held.id = 0;
    return ImGui::IsItemDeactivatedAfterEdit();
}

void TilesetEditorWindow::drawToolbar() {
    ImGui::SetNextItemWidth(Theme::dpi(180.0f));
    char nameBuffer[128];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", document.name.c_str());
    if (ImGui::InputText("Name##TilesetName", nameBuffer, sizeof(nameBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string newName = nameBuffer;
        edit("Rename Tileset", [&](TilesetData& data) { data.name = newName; });
    }
    ImGui::SameLine();
    int tileSize[2] = {document.tileWidth, document.tileHeight};
    ImGui::SetNextItemWidth(Theme::dpi(110.0f));
    if (heldField("Tile Size##TileSize", tileSize, 2, [&]() { ImGui::InputInt2("Tile Size##TileSize", tileSize); }) && tileSize[0] > 0 && tileSize[1] > 0) {
        const int w = tileSize[0], h = tileSize[1];
        edit("Set Tile Size", [&](TilesetData& data) { data.tileWidth = w; data.tileHeight = h; });
    }
    ImGui::SameLine();
    static const char* const shapeNames[] = {"Square", "Isometric", "Hexagon (Pointy Top)", "Hexagon (Flat Top)"};
    int shape = (int)document.shape;
    ImGui::SetNextItemWidth(Theme::dpi(150.0f));
    if (ImGui::Combo("Tile Shape##TileShape", &shape, shapeNames, IM_ARRAYSIZE(shapeNames))) {
        const TileShape chosen = (TileShape)shape;
        edit("Set Tile Shape", [&](TilesetData& data) { data.shape = chosen; });
    }
    ImGui::SameLine(0.0f, Theme::dpi(16.0f));
    ImGui::BeginDisabled(selection.empty() || document.terrainSets.empty());
    if (ImGui::Button("Apply Terrain Template...")) templateOpenRequested = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Assign names and peering values to the block of tiles whose top-left tile is the selected one");
    // the line is always there: a first notice must not move the tile grid under the pointer
    if (noticeIsError) ImGui::TextColored(Theme::Colors::WarningText, "%s", notice.c_str());
    else ImGui::TextDisabled("%s", notice.empty() ? " " : notice.c_str());
    ImGui::Separator();
}

void TilesetEditorWindow::drawSources() {
    if (!ImGui::CollapsingHeader("Sources", ImGuiTreeNodeFlags_DefaultOpen)) return;
    int removeSource = -1;
    for (const TilesetSource& source : document.sources) {
        ImGui::PushID(source.id);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  %s", tileset::sourceKindName(source.kind), source.asset.path.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK "##remove")) removeSource = source.id;
        ImGui::SetItemTooltip("Remove this source and its tiles");
        if (source.kind == TilesetSourceKind::Texture) {
            const bool expanded = sliceSource == source.id;
            ImGui::SameLine();
            if (ImGui::SmallButton(expanded ? "Close Slicing" : "Slicing...")) {
                sliceSource = expanded ? -1 : source.id;
                sliceWidth = source.tileWidth > 0 ? source.tileWidth : document.tileWidth;
                sliceHeight = source.tileHeight > 0 ? source.tileHeight : document.tileHeight;
                sliceMargin = source.margin;
                sliceSpacing = source.spacing;
                sliceSkipEmpty = source.skipEmpty;
            }
            if (sliceSource == source.id) {
                ImGui::SetNextItemWidth(Theme::dpi(100.0f));
                int size[2] = {sliceWidth, sliceHeight};
                if (ImGui::InputInt2("Size##slice", size)) { sliceWidth = std::max(1, size[0]); sliceHeight = std::max(1, size[1]); }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Theme::dpi(60.0f));
                ImGui::InputInt("Margin##slice", &sliceMargin, 0);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(Theme::dpi(60.0f));
                ImGui::InputInt("Spacing##slice", &sliceSpacing, 0);
                ImGui::SameLine();
                ImGui::Checkbox("Skip Empty##slice", &sliceSkipEmpty);
                ImGui::SameLine();
                if (ImGui::Button("Apply Slicing##slice")) {
                    TextureSlicing slicing;
                    slicing.tileWidth = sliceWidth;
                    slicing.tileHeight = sliceHeight;
                    slicing.margin = sliceMargin;
                    slicing.spacing = sliceSpacing;
                    slicing.skipEmpty = sliceSkipEmpty;
                    TilesetPagePixels pixels;
                    if (const PixelImage* image = pixels.get(source.asset.path)) {
                        ResliceReport report;
                        PixelProblem problem;
                        const int sourceId = source.id;
                        const bool applied = edit("Re-slice Source", [&](TilesetData& data) {
                            if (!resliceTexture(data, sourceId, slicing, *image, report, problem)) setNotice(problem.message, true);
                        });
                        if (applied) sliceSource = -1;
                    } else {
                        setNotice(pixels.lastError(), true);
                    }
                }
            }
        }
        ImGui::PopID();
    }
    if (removeSource >= 0) {
        edit("Remove Source", [&](TilesetData& data) {
            data.sources.erase(std::remove_if(data.sources.begin(), data.sources.end(), [&](const TilesetSource& s) { return s.id == removeSource; }), data.sources.end());
            std::vector<int32_t> ids;
            for (const TileDef& tile : data.tiles) if (tile.source == removeSource) ids.push_back(tile.id);
            for (int32_t id : ids) data.removeTile(id);
        });
    }

    // the add form: a path, or a file dragged from the Project window
    ImGui::SetNextItemWidth(Theme::dpi(220.0f));
    ImGui::InputTextWithHint("##SourcePath", "Texture, atlas or .aseprite path", sourcePath, sizeof(sourcePath));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            std::snprintf(sourcePath, sizeof(sourcePath), "%s", (const char*)payload->Data);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(sourcePath[0] == 0);
    if (ImGui::Button("Add Source")) {
        const std::string relative = project->normalizeToAssetsRelative(fs::path(sourcePath)).generic_string();
        const AssetRef ref = project->makeAssetRef(relative, AssetPathBase::Assets);
        const fs::path absolute = project->getAssetsPath() / relative;
        std::vector<std::string> warnings;
        PixelProblem problem;
        int sourceId = 0;
        bool ok = false;
        if (SpriteSheetPool::isAsepritePath(relative)) {
            edit("Add Aseprite Source", [&](TilesetData& data) { ok = addAsepriteSourceFromFile(data, ref, absolute, 0, warnings, sourceId, problem); });
        } else if (SpriteSheetSources::isTexturePath(relative)) {
            TilesetPagePixels pixels;
            if (const PixelImage* image = pixels.get(relative)) {
                TextureSlicing slicing;
                slicing.tileWidth = document.tileWidth;
                slicing.tileHeight = document.tileHeight;
                edit("Add Texture Source", [&](TilesetData& data) { ok = addTextureSource(data, ref, slicing, *image, sourceId, problem); });
            } else {
                problem.message = pixels.lastError();
            }
        } else {
            edit("Add Sheet Source", [&](TilesetData& data) { ok = addSheetSource(data, ref, {}, sourceId, problem); });
        }
        if (!ok && !problem.message.empty()) setNotice(problem.message, true);
        else sourcePath[0] = 0;
    }
    ImGui::EndDisabled();
}

void TilesetEditorWindow::drawTileGrids() {
    if (!runtime) return;
    const float zoom = kTilePixelZoom;
    const float cell = static_cast<float>(std::max(document.tileWidth, 1)) * zoom;
    const float cellH = static_cast<float>(std::max(document.tileHeight, 1)) * zoom;
    const float gap = 1.0f;
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    for (const TilesetSource& source : document.sources) {
        ImGui::PushID(source.id);
        ImGui::TextDisabled("%s", source.asset.path.c_str());
        std::vector<TileGridCell> cells;
        int columns = 0, rows = 0;
        tileSourceGrid(*runtime, source.id, cells, columns, rows);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size(static_cast<float>(columns) * (cell + gap), static_cast<float>(rows) * (cellH + gap));
        ImGui::InvisibleButton("##tiles", ImVec2(std::max(size.x, 1.0f), std::max(size.y, 1.0f)));
        const bool hovered = ImGui::IsItemHovered();
        list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(40, 42, 48, 255));
        for (const TileGridCell& at : cells) {
            const ImVec2 min(origin.x + static_cast<float>(at.col) * (cell + gap), origin.y + static_cast<float>(at.row) * (cellH + gap));
            drawTileImage(list, min, zoom, *runtime, at.id);
            if (isSelected(at.id)) {
                list->AddRect(min, ImVec2(min.x + cell, min.y + cellH), IM_COL32(255, 196, 48, 255), 0.0f, 0, Theme::dpi(2.0f));
            }
        }
        if (hovered) {
            int col = std::clamp(static_cast<int>((io.MousePos.x - origin.x) / (cell + gap)), 0, std::max(columns - 1, 0));
            int row = std::clamp(static_cast<int>((io.MousePos.y - origin.y) / (cellH + gap)), 0, std::max(rows - 1, 0));
            for (const TileGridCell& at : cells) {
                if (at.col == col && at.row == row) {
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) select(at.id, io.KeyShift, io.KeyCtrl || io.KeySuper);
                    showTileTooltip(*runtime, at.id);
                }
            }
        }
        ImGui::PopID();
    }
}

void TilesetEditorWindow::drawCollision(TileDef& first) {
    if (!ImGui::CollapsingHeader("Collision", ImGuiTreeNodeFlags_DefaultOpen)) return;
    static const char* const modeNames[] = {"None", "Full", "Alpha", "Custom"};
    int mode = (int)first.collision.mode;
    if (ImGui::Combo("Mode##collisionMode", &mode, modeNames, IM_ARRAYSIZE(modeNames))) {
        const auto chosen = (TileCollisionMode)mode;
        forSelected("Set Collision Mode", [&](TileDef& tile) { tile.collision.mode = chosen; });
    }
    static const char* const kindNames[] = {"Solid", "One Way", "Sensor"};
    int kind = (int)first.collision.kind;
    if (ImGui::Combo("Kind##collisionKind", &kind, kindNames, IM_ARRAYSIZE(kindNames))) {
        const auto chosen = (TileCollisionKind)kind;
        forSelected("Set Collision Kind", [&](TileDef& tile) { tile.collision.kind = chosen; });
    }
    if (first.collision.kind == TileCollisionKind::OneWay) {
        static const char* const directionNames[] = {"Up", "Down", "Left", "Right"};
        int direction = (int)first.collision.oneWay;
        if (ImGui::Combo("Direction##oneWay", &direction, directionNames, IM_ARRAYSIZE(directionNames))) {
            const auto chosen = (TileOneWayDirection)direction;
            forSelected("Set One-Way Direction", [&](TileDef& tile) { tile.collision.oneWay = chosen; });
        }
    }
    int threshold = first.collision.threshold;
    if (heldField("Alpha Threshold##threshold", &threshold, 1, [&]() { ImGui::SliderInt("Alpha Threshold##threshold", &threshold, 0, 255); })) {
        forTiles("Set Alpha Threshold", held.tiles, [&](TileDef& tile) { tile.collision.threshold = threshold; });
    }
    if (ImGui::Button("Auto from Alpha")) {
        // traces the outlines into the polygons, so the runtime never reads pixels (R15)
        const std::vector<int32_t> ids = selection;
        if (runtime) {
            TilesetPagePixels pixels;
            size_t filled = 0;
            PixelProblem problem;
            edit("Auto from Alpha", [&](TilesetData& data) {
                for (int32_t id : ids) if (TileDef* tile = data.findTile(id)) tile->collision.mode = TileCollisionMode::Alpha;
                if (!fillAlphaCollisions(data, *runtime, pixels, ids, filled, problem)) setNotice(problem.message, true);
            });
        }
    }
    ImGui::SetItemTooltip("Trace each selected tile's alpha outline into its collision polygons (pixel-exact)");
    if (first.collision.mode == TileCollisionMode::Custom) drawPolygonEditor(first);
}

void TilesetEditorWindow::drawPolygonEditor(TileDef& first) {
    if (!runtime) return;
    const TileRenderInfo* info = runtime->tile(first.id);
    if (!info) return;
    ImGui::TextDisabled("Click adds a point (snapped to pixels); drag moves it; right-click removes it");
    TileCollision& collision = first.collision;
    if (ImGui::SmallButton("Add Polygon")) {
        forSelected("Add Collision Polygon", [&](TileDef& tile) {
            tile.collision.polygons.push_back(TilePolygon{});
        });
        activePolygon = (int)first.collision.polygons.size();
        return;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Remove Polygon") && !collision.polygons.empty() && (size_t)activePolygon < collision.polygons.size()) {
        const int removeIndex = activePolygon;
        forSelected("Remove Collision Polygon", [&](TileDef& tile) {
            if ((size_t)removeIndex < tile.collision.polygons.size()) tile.collision.polygons.erase(tile.collision.polygons.begin() + removeIndex);
        });
        activePolygon = 0;
        return;
    }
    if (collision.polygons.empty()) return;
    activePolygon = std::clamp(activePolygon, 0, (int)collision.polygons.size() - 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Theme::dpi(80.0f));
    ImGui::SliderInt("Polygon##polygonIndex", &activePolygon, 0, (int)collision.polygons.size() - 1);

    TilePolygon& polygon = collision.polygons[(size_t)activePolygon];
    const float pixelZoom = static_cast<float>(kPainterZoom);
    const ImVec2 size(static_cast<float>(info->width) * pixelZoom, static_cast<float>(info->height) * pixelZoom);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##polygonCanvas", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(40, 42, 48, 255));
    drawTileImage(list, origin, pixelZoom, *runtime, first.id);
    const auto toScreen = [&](const std::array<int32_t, 2>& point) {
        return ImVec2(origin.x + (float)point[0] * pixelZoom, origin.y + size.y - (float)point[1] * pixelZoom);
    };
    const ImGuiIO& io = ImGui::GetIO();
    const auto toPixel = [&](ImVec2 mouse) {
        const int x = std::clamp((int)std::lround((mouse.x - origin.x) / pixelZoom), 0, info->width);
        const int y = std::clamp((int)std::lround((origin.y + size.y - mouse.y) / pixelZoom), 0, info->height);
        return std::array<int32_t, 2>{x, y};
    };
    // every polygon of the tile, the active one brighter
    for (size_t p = 0; p < collision.polygons.size(); p++) {
        const TilePolygon& shape = collision.polygons[p];
        std::vector<ImVec2> points;
        for (const auto& point : shape.points) points.push_back(toScreen(point));
        if (points.size() >= 2) list->AddPolyline(points.data(), (int)points.size(), (int)p == activePolygon ? IM_COL32(120, 255, 120, 255) : IM_COL32(120, 255, 120, 90), ImDrawFlags_Closed, 1.5f);
    }
    for (const auto& point : polygon.points) list->AddCircleFilled(toScreen(point), Theme::dpi(3.0f), IM_COL32(255, 255, 255, 255));

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !polygon.points.empty()) {
        // the nearest point goes
        int nearest = 0;
        float best = FLT_MAX;
        for (size_t i = 0; i < polygon.points.size(); i++) {
            const ImVec2 screen = toScreen(polygon.points[i]);
            const float distance = (screen.x - io.MousePos.x) * (screen.x - io.MousePos.x) + (screen.y - io.MousePos.y) * (screen.y - io.MousePos.y);
            if (distance < best) { best = distance; nearest = (int)i; }
        }
        polygon.points.erase(polygon.points.begin() + nearest);
        polygonDirty = true;
    } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // a press on a point drags it, else adds a point
        dragPoint = -1;
        for (size_t i = 0; i < polygon.points.size(); i++) {
            const ImVec2 screen = toScreen(polygon.points[i]);
            if (std::fabs(screen.x - io.MousePos.x) < Theme::dpi(6.0f) && std::fabs(screen.y - io.MousePos.y) < Theme::dpi(6.0f)) dragPoint = (int)i;
        }
        if (dragPoint < 0) {
            polygon.points.push_back(toPixel(io.MousePos));
            dragPoint = (int)polygon.points.size() - 1;
        }
        polygonDirty = true;
    }
    if (dragPoint >= 0 && (size_t)dragPoint < polygon.points.size() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        polygon.points[(size_t)dragPoint] = toPixel(io.MousePos);
        polygonDirty = true;
    }
    if (polygonDirty && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // one undo step for the gesture: the document carries the edit, written now
        dragPoint = -1;
        polygonDirty = false;
        const TileCollision edited = collision;
        // the shown tile was edited in place (edit() would see no change): write it, the other
        // selected tiles following
        TilesetData data = document;
        std::vector<TilesetProblem> problems;
        tileset::validate(data, problems);
        if (problems.empty()) {
            std::string error;
            for (int32_t id : selection) if (TileDef* other = data.findTile(id)) other->collision = edited;
            if (!writeTilesetFile(project, file, data, error, "Edit Collision Polygon")) setNotice(error, true);
            else setDocument(data);
        } else {
            setNotice(problems.front().message, true);
        }
    }
}

void TilesetEditorWindow::drawAnimation(TileDef& first) {
    if (!ImGui::CollapsingHeader("Animation")) return;
    ImGui::TextDisabled("Frames play on the game clock (the layer's Animation Speed)");
    bool randomStart = first.randomStart;
    if (ImGui::Checkbox("Random Start##randomStart", &randomStart)) {
        const bool value = randomStart;
        forSelected("Set Random Start", [&](TileDef& tile) { tile.randomStart = value; });
    }
    int removeFrame = -1;
    for (size_t i = 0; i < first.animation.size(); i++) {
        ImGui::PushID((int)i);
        TileAnimationFrame frame = first.animation[i];
        ImGui::SetNextItemWidth(Theme::dpi(60.0f));
        bool changed = heldField("Tile##frameTile", &frame.tile, 1, [&]() { ImGui::InputInt("Tile##frameTile", &frame.tile, 0); });
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(80.0f));
        changed = heldField("ms##frameMs", &frame.ms, 1, [&]() { ImGui::InputInt("ms##frameMs", &frame.ms, 0); }) || changed;
        if (runtime) {
            ImGui::SameLine();
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            drawTileImage(ImGui::GetWindowDrawList(), pos, 1.0f, *runtime, frame.tile);
            ImGui::Dummy(ImVec2((float)document.tileWidth, (float)document.tileHeight));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK "##removeFrame")) removeFrame = (int)i;
        if (changed && frame.ms > 0) {
            forTiles("Edit Animation Frame", held.tiles, [&](TileDef& tile) {
                if (i < tile.animation.size()) tile.animation[i] = frame;
            });
        }
        ImGui::PopID();
    }
    if (ImGui::SmallButton("Add Selected as Frames")) {
        const std::vector<int32_t> ids = selection;
        forSelected("Add Animation Frames", [&](TileDef& tile) {
            for (int32_t id : ids) tile.animation.push_back(TileAnimationFrame{id, 100});
        });
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear Frames")) {
        forSelected("Clear Animation", [&](TileDef& tile) { tile.animation.clear(); });
    }
    if (removeFrame >= 0) {
        const int index = removeFrame;
        forSelected("Remove Animation Frame", [&](TileDef& tile) {
            if ((size_t)index < tile.animation.size()) tile.animation.erase(tile.animation.begin() + index);
        });
    }
    if (runtime && first.animation.size() > 1) {
        const TileRenderInfo* info = runtime->tile(first.id);
        const int animation = info ? info->animation : -1;
        if (animation >= 0) {
            const TileAnimationRuntime& frames = runtime->animations[(size_t)animation];
            const int64_t now = static_cast<int64_t>(ImGui::GetTime() * 1000.0) % std::max(frames.totalMs, 1);
            ImGui::TextDisabled("Preview");
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            drawTileImage(ImGui::GetWindowDrawList(), pos, 3.0f, *runtime, runtime->animatedTile(first.id, now));
            ImGui::Dummy(ImVec2((float)document.tileWidth * 3.0f, (float)document.tileHeight * 3.0f));
        }
    }
}

void TilesetEditorWindow::drawPeeringPainter(TileDef& first, const TerrainSetDef& set) {
    if (!runtime) return;
    const TileRenderInfo* info = runtime->tile(first.id);
    if (!info) return;
    const std::vector<int> slots = tileset::peeringSlots(document.shape, set.mode);
    int setIndex = first.terrain.set;
    // the painted value: Any, None or a terrain of the set
    if (ImGui::RadioButton("Any##paintAny", paintValue == PEERING_ANY)) paintValue = PEERING_ANY;
    ImGui::SameLine();
    if (ImGui::RadioButton("None##paintNone", paintValue == PEERING_NONE)) paintValue = PEERING_NONE;
    for (size_t t = 0; t < set.terrains.size(); t++) {
        ImGui::SameLine();
        const std::string label = set.terrains[t].name + "##paint_" + std::to_string(t);
        if (ImGui::RadioButton(label.c_str(), paintValue == (int)t)) paintValue = (int)t;
    }

    const float pixelZoom = static_cast<float>(kPainterZoom);
    const ImVec2 tilePixels(static_cast<float>(info->width) * pixelZoom, static_cast<float>(info->height) * pixelZoom);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool hex = document.shape == TileShape::HexPointy || document.shape == TileShape::HexFlat;
    // room around a hex's ring of slots
    const float ringMargin = hex ? Theme::dpi(40.0f) : 0.0f;
    const ImVec2 canvasSize(tilePixels.x + ringMargin * 2.0f, tilePixels.y + ringMargin * 2.0f);
    ImGui::InvisibleButton("##peeringCanvas", canvasSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 tileOrigin(origin.x + ringMargin, origin.y + ringMargin);
    list->AddRectFilled(origin, ImVec2(origin.x + canvasSize.x, origin.y + canvasSize.y), IM_COL32(40, 42, 48, 255));
    drawTileImage(list, tileOrigin, pixelZoom, *runtime, first.id);

    // a region's fill: Any is hatched-free transparent (nothing to require), None dark, a terrain its colour
    const auto regionColor = [&](int value) -> ImU32 {
        if (value == PEERING_ANY) return IM_COL32(0, 0, 0, 0);
        if (value == PEERING_NONE) return IM_COL32(20, 20, 24, 170);
        if ((size_t)value < set.terrains.size()) return hexColor(set.terrains[(size_t)value].color, 170);
        return IM_COL32(255, 0, 255, 170);
    };
    int hoveredSlot = -2;   // -1: the centre
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    if (!hex) {
        const float third = tilePixels.x / 3.0f, thirdH = tilePixels.y / 3.0f;
        for (int slot : slots) {
            int col = 0, row = 0;
            slotCell(slot, col, row);
            const ImVec2 min(tileOrigin.x + (float)col * third, tileOrigin.y + (float)row * thirdH);
            const ImVec2 max(min.x + third, min.y + thirdH);
            if (mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y) hoveredSlot = slot;
            list->AddRectFilled(min, max, regionColor(first.terrain.peering[(size_t)slot]));
            list->AddRect(min, max, IM_COL32(255, 255, 255, 50));
        }
        const ImVec2 centerMin(tileOrigin.x + third, tileOrigin.y + thirdH);
        const ImVec2 centerMax(centerMin.x + third, centerMin.y + thirdH);
        if (mouse.x >= centerMin.x && mouse.x < centerMax.x && mouse.y >= centerMin.y && mouse.y < centerMax.y) hoveredSlot = -1;
        list->AddRectFilled(centerMin, centerMax, first.terrain.center >= 0 ? regionColor(first.terrain.center) : IM_COL32(0, 0, 0, 0));
        list->AddRect(centerMin, centerMax, IM_COL32(255, 255, 255, 120), 0.0f, 0, 1.5f);
    } else {
        // the ring: 6 sides, or 12 values in blob mode, around the tile's centre
        const ImVec2 center(tileOrigin.x + tilePixels.x * 0.5f, tileOrigin.y + tilePixels.y * 0.5f);
        const float radius = std::max(tilePixels.x, tilePixels.y) * 0.5f;
        const float square = Theme::dpi(14.0f);
        for (int slot : slots) {
            float dx = 0.0f, dy = 0.0f;
            hexSlotDirection(document.shape, slot, dx, dy);
            const ImVec2 point(center.x + dx * (radius + Theme::dpi(10.0f)), center.y - dy * (radius + Theme::dpi(10.0f)));
            const ImVec2 min(point.x - square, point.y - square), max(point.x + square, point.y + square);
            if (mouse.x >= min.x && mouse.x < max.x && mouse.y >= min.y && mouse.y < max.y) hoveredSlot = slot;
            list->AddRectFilled(min, max, regionColor(first.terrain.peering[(size_t)slot]));
            list->AddRect(min, max, IM_COL32(255, 255, 255, 80));
        }
        const float centerRadius = radius * 0.35f;
        if ((mouse.x - center.x) * (mouse.x - center.x) + (mouse.y - center.y) * (mouse.y - center.y) < centerRadius * centerRadius) hoveredSlot = -1;
        list->AddCircleFilled(center, centerRadius, first.terrain.center >= 0 ? regionColor(first.terrain.center) : IM_COL32(0, 0, 0, 90));
        list->AddCircle(center, centerRadius, IM_COL32(255, 255, 255, 120));
    }

    if (clicked && hoveredSlot != -2) {
        const int value = paintValue;
        if (hoveredSlot == -1) {
            // the centre takes a terrain, or Any/None clears it
            const int center = value >= 0 ? value : -1;
            forSelected("Paint Peering Center", [&](TileDef& tile) {
                if (tile.terrain.set < 0) tile.terrain.set = setIndex;
                tile.terrain.center = center;
            });
        } else {
            const int slot = hoveredSlot;
            forSelected("Paint Peering Value", [&](TileDef& tile) {
                if (tile.terrain.set < 0) tile.terrain.set = setIndex;
                tile.terrain.peering[(size_t)slot] = (int16_t)value;
            });
        }
    }
    if (hovered && hoveredSlot != -2) {
        if (hoveredSlot >= 0) {
            ImGui::SetTooltip("%s: %s", tileset::peeringKey(document.shape, hoveredSlot),
                              peeringLabel(document, setIndex, first.terrain.peering[(size_t)hoveredSlot]).c_str());
        } else {
            ImGui::SetTooltip("Centre: %s", first.terrain.center >= 0 ? peeringLabel(document, setIndex, first.terrain.center).c_str() : "none");
        }
    }
}

void TilesetEditorWindow::drawTerrain(TileDef& first) {
    if (!ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) return;
    std::vector<std::string> setNames = {"(none)"};
    for (const TerrainSetDef& set : document.terrainSets) setNames.push_back(set.name);
    int set = first.terrain.set + 1;
    ImGui::SetNextItemWidth(Theme::dpi(180.0f));
    if (ImGui::BeginCombo("Terrain Set##terrainSet", setNames[(size_t)std::clamp(set, 0, (int)setNames.size() - 1)].c_str())) {
        for (size_t i = 0; i < setNames.size(); i++) {
            if (ImGui::Selectable(setNames[i].c_str(), (int)i == set)) {
                const int value = (int)i - 1;
                forSelected("Set Terrain Set", [&](TileDef& tile) {
                    if (value < 0) tile.terrain = TileTerrain();
                    else tile.terrain.set = value;
                });
            }
        }
        ImGui::EndCombo();
    }
    if (first.terrain.set < 0 || (size_t)first.terrain.set >= document.terrainSets.size()) return;
    const TerrainSetDef& setDef = document.terrainSets[(size_t)first.terrain.set];
    std::vector<std::string> terrainNames = {"(none)"};
    for (const TerrainDef& terrain : setDef.terrains) terrainNames.push_back(terrain.name);
    int center = first.terrain.center + 1;
    ImGui::SetNextItemWidth(Theme::dpi(180.0f));
    if (ImGui::BeginCombo("Centre Terrain##terrainCenter", terrainNames[(size_t)std::clamp(center, 0, (int)terrainNames.size() - 1)].c_str())) {
        for (size_t i = 0; i < terrainNames.size(); i++) {
            if (ImGui::Selectable(terrainNames[i].c_str(), (int)i == center)) {
                const int value = (int)i - 1;
                forSelected("Set Centre Terrain", [&](TileDef& tile) { tile.terrain.center = value; });
            }
        }
        ImGui::EndCombo();
    }
    drawPeeringPainter(first, setDef);
}

void TilesetEditorWindow::drawProperties(TileDef& first) {
    if (document.propertySchema.empty() || !ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) return;
    for (const PropertySchemaEntry& entry : document.propertySchema) {
        ImGui::PushID(entry.name.c_str());
        auto found = first.properties.find(entry.name);
        const TilePropertyValue value = found == first.properties.end() ? entry.defaultValue : found->second;
        const std::string label = entry.name + "##prop";
        const auto set = [&](const TilePropertyValue& next, const std::vector<int32_t>& tiles) {
            const std::string name = entry.name;
            forTiles("Set Tile Property", tiles, [&](TileDef& tile) { tile.properties[name] = next; });
        };
        switch (entry.type) {
            case TilePropertyType::Bool: {
                bool v = value.b;
                if (ImGui::Checkbox(label.c_str(), &v)) { TilePropertyValue next = value; next.b = v; set(next, selection); }
                break;
            }
            case TilePropertyType::Int: {
                int v = (int)value.i;
                if (heldField(label.c_str(), &v, 1, [&]() { ImGui::InputInt(label.c_str(), &v); })) { TilePropertyValue next = value; next.i = v; set(next, held.tiles); }
                break;
            }
            case TilePropertyType::Float: {
                float v = (float)value.f;
                if (heldField(label.c_str(), &v, 1, [&]() { ImGui::DragFloat(label.c_str(), &v); })) { TilePropertyValue next = value; next.f = v; set(next, held.tiles); }
                break;
            }
            case TilePropertyType::String: {
                char buffer[256];
                std::snprintf(buffer, sizeof(buffer), "%s", value.s.c_str());
                if (ImGui::InputText(label.c_str(), buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)) { TilePropertyValue next = value; next.s = buffer; set(next, selection); }
                break;
            }
            case TilePropertyType::Color: {
                float v[4] = {value.color.x, value.color.y, value.color.z, value.color.w};
                if (heldField(label.c_str(), v, 4, [&]() { ImGui::ColorEdit4(label.c_str(), v); })) {
                    TilePropertyValue next = value;
                    next.color = Vector4(v[0], v[1], v[2], v[3]);
                    set(next, held.tiles);
                }
                break;
            }
        }
        ImGui::PopID();
    }
}

void TilesetEditorWindow::drawSelectedInspector() {
    TileDef* first = selectedFirst();
    if (!first) {
        ImGui::TextDisabled("Select tiles in the grid to edit them.");
        return;
    }
    ImGui::Text("%d tile(s) selected, first #%d", (int)selection.size(), first->id);

    char nameBuffer[128];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", first->name.c_str());
    ImGui::BeginDisabled(selection.size() > 1);
    if (ImGui::InputText("Name##tileName", nameBuffer, sizeof(nameBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
        const std::string name = nameBuffer;
        forSelected("Rename Tile", [&](TileDef& tile) { tile.name = name; });
    }
    ImGui::EndDisabled();

    float probability = first->probability;
    if (heldField("Probability##probability", &probability, 1, [&]() { ImGui::DragFloat("Probability##probability", &probability, 0.01f, 0.0f, 100.0f); })) {
        forTiles("Set Probability", held.tiles, [&](TileDef& tile) { tile.probability = probability; });
    }
    int offset[2] = {first->offsetX, first->offsetY};
    if (heldField("Offset##offset", offset, 2, [&]() { ImGui::InputInt2("Offset##offset", offset); })) {
        const int x = offset[0], y = offset[1];
        forTiles("Set Tile Offset", held.tiles, [&](TileDef& tile) { tile.offsetX = x; tile.offsetY = y; });
    }
    int navArea = first->navArea;
    if (heldField("Navigation Area##navArea", &navArea, 1, [&]() { ImGui::InputInt("Navigation Area##navArea", &navArea); })) {
        const int area = std::max(navArea, -1);
        forTiles("Set Navigation Area", held.tiles, [&](TileDef& tile) { tile.navArea = area; });
    }
    ImGui::SetItemTooltip("The NavGrid area this tile gives its cell (-1: none, S19's tile-name table decides)");

    drawCollision(*first);
    drawAnimation(*first);
    drawTerrain(*first);
    drawProperties(*first);

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Property Schema")) {
        ImGui::SetNextItemWidth(Theme::dpi(160.0f));
        ImGui::InputText("##schemaName", propertyName, sizeof(propertyName));
        ImGui::SameLine();
        static const char* const typeNames[] = {"Bool", "Int", "Float", "String", "Color"};
        ImGui::SetNextItemWidth(Theme::dpi(100.0f));
        ImGui::Combo("##schemaType", &propertyType, typeNames, IM_ARRAYSIZE(typeNames));
        ImGui::SameLine();
        ImGui::BeginDisabled(propertyName[0] == 0 || document.findProperty(propertyName) != nullptr);
        if (ImGui::Button("Add Property")) {
            const std::string name = propertyName;
            const auto type = (TilePropertyType)propertyType;
            edit("Add Property", [&](TilesetData& data) {
                PropertySchemaEntry entry;
                entry.name = name;
                entry.type = type;
                entry.defaultValue.type = type;
                data.propertySchema.push_back(entry);
            });
            propertyName[0] = 0;
        }
        ImGui::EndDisabled();
        int removeIndex = -1;
        for (size_t i = 0; i < document.propertySchema.size(); i++) {
            ImGui::PushID((int)i);
            ImGui::BulletText("%s (%s)", document.propertySchema[i].name.c_str(), tileset::propertyTypeName(document.propertySchema[i].type));
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_XMARK "##rm")) removeIndex = (int)i;
            ImGui::PopID();
        }
        if (removeIndex >= 0) {
            const std::string name = document.propertySchema[(size_t)removeIndex].name;
            edit("Remove Property", [&](TilesetData& data) {
                data.propertySchema.erase(data.propertySchema.begin() + removeIndex);
                for (TileDef& tile : data.tiles) tile.properties.erase(name);
            });
        }
    }
}

void TilesetEditorWindow::drawTerrainSets() {
    if (!ImGui::CollapsingHeader("Terrain Sets", ImGuiTreeNodeFlags_DefaultOpen)) return;
    for (size_t setIndex = 0; setIndex < document.terrainSets.size(); setIndex++) {
        const TerrainSetDef& set = document.terrainSets[setIndex];
        ImGui::PushID((int)setIndex);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s  (%s%s)", set.name.c_str(), tileset::terrainModeName(set.mode), set.twoSurface ? ", two surface" : "");
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK "##rmSet")) {
            edit("Remove Terrain Set", [&](TilesetData& data) {
                data.terrainSets.erase(data.terrainSets.begin() + (long)setIndex);
                for (TileDef& tile : data.tiles) {
                    if (tile.terrain.set == (int)setIndex) tile.terrain = TileTerrain();
                    else if (tile.terrain.set > (int)setIndex) tile.terrain.set--;
                }
            });
            ImGui::PopID();
            break;
        }
        for (size_t t = 0; t < set.terrains.size(); t++) {
            ImGui::PushID((int)t);
            const TerrainDef terrain = set.terrains[t];
            float color[3] = {1.0f, 1.0f, 1.0f};
            int r = 255, g = 255, b = 255;
            std::sscanf(terrain.color.c_str(), "#%02x%02x%02x", &r, &g, &b);
            color[0] = (float)r / 255.0f;
            color[1] = (float)g / 255.0f;
            color[2] = (float)b / 255.0f;
            ImGui::Indent();
            if (heldField("##terrainColor", color, 3, [&]() { ImGui::ColorEdit3("##terrainColor", color, ImGuiColorEditFlags_NoInputs); })) {
                const std::string hex = hexOf(color);
                edit("Set Terrain Color", [&](TilesetData& data) { data.terrainSets[setIndex].terrains[t].color = hex; });
            }
            ImGui::SameLine();
            char nameBuffer[64];
            std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", terrain.name.c_str());
            ImGui::SetNextItemWidth(Theme::dpi(120.0f));
            if (ImGui::InputText("##terrainName", nameBuffer, sizeof(nameBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
                const std::string name = nameBuffer;
                edit("Rename Terrain", [&](TilesetData& data) { data.terrainSets[setIndex].terrains[t].name = name; });
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_XMARK "##rmTerrain")) {
                const std::string name = terrain.name;
                edit("Remove Terrain", [&](TilesetData& data) {
                    data.terrainSets[setIndex].terrains.erase(data.terrainSets[setIndex].terrains.begin() + (long)t);
                });
            }
            ImGui::Unindent();
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(Theme::dpi(120.0f));
    ImGui::InputText("##newSetName", setName, sizeof(setName));
    ImGui::SameLine();
    static const char* const modeNames[] = {"Blob (Corners and Sides)", "Sides", "Dual Grid"};
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    ImGui::Combo("##newSetMode", &setMode, modeNames, IM_ARRAYSIZE(modeNames));
    ImGui::SameLine();
    ImGui::Checkbox("Two Surface##newTwoSurface", &setTwoSurface);
    ImGui::SameLine();
    ImGui::BeginDisabled(setName[0] == 0);
    if (ImGui::Button("Add Set")) {
        const std::string name = setName;
        const auto mode = (TerrainMode)setMode;
        const bool two = setTwoSurface;
        edit("Add Terrain Set", [&](TilesetData& data) {
            TerrainSetDef set;
            set.name = name;
            set.mode = mode;
            set.twoSurface = two;
            data.terrainSets.push_back(set);
        });
    }
    ImGui::EndDisabled();
    if (!document.terrainSets.empty()) {
        ImGui::SetNextItemWidth(Theme::dpi(120.0f));
        ImGui::InputText("##newTerrainName", terrainName, sizeof(terrainName));
        ImGui::SameLine();
        ImGui::ColorEdit3("##newTerrainColor", terrainColor, ImGuiColorEditFlags_NoInputs);
        ImGui::SameLine();
        ImGui::BeginDisabled(terrainName[0] == 0);
        if (ImGui::Button("Add Terrain")) {
            const std::string name = terrainName;
            const std::string hex = hexOf(terrainColor);
            const int target = std::clamp(coverageSet, 0, (int)document.terrainSets.size() - 1);
            edit("Add Terrain", [&](TilesetData& data) {
                TerrainDef terrain;
                terrain.name = name;
                terrain.color = hex;
                data.terrainSets[(size_t)target].terrains.push_back(terrain);
            });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("(into the coverage set below)");
    }
}

void TilesetEditorWindow::drawCoverage() {
    if (document.terrainSets.empty() || !ImGui::CollapsingHeader("Coverage")) return;
    std::vector<std::string> names;
    for (const TerrainSetDef& set : document.terrainSets) names.push_back(set.name);
    coverageSet = std::clamp(coverageSet, 0, (int)names.size() - 1);
    ImGui::SetNextItemWidth(Theme::dpi(160.0f));
    if (ImGui::BeginCombo("Terrain Set##coverageSet", names[(size_t)coverageSet].c_str())) {
        for (size_t i = 0; i < names.size(); i++) {
            if (ImGui::Selectable(names[i].c_str(), (int)i == coverageSet)) {
                coverageSet = (int)i;
                coverageValid = false;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Allow Transforms", &coverageTransforms)) coverageValid = false;
    if (!coverageValid) {
        coverage = terrain::coverage(document, coverageSet, coverageTransforms);
        coverageValid = true;
    }
    ImGui::Text("%d of %d masks covered", (int)coverage.required - (int)coverage.missing.size(), (int)coverage.required);
    if (!coverage.missing.empty()) {
        ImGui::TextColored(Theme::Colors::WarningText, "%d mask(s) have no tile", (int)coverage.missing.size());
        ImGui::BeginChild("##missing", ImVec2(0.0f, Theme::dpi(140.0f)), ImGuiChildFlags_Borders);
        const std::vector<PixelImage> images = coverageImages(document, coverageSet, coverage, 6);
        ImDrawList* list = ImGui::GetWindowDrawList();
        const float zoom = 2.0f;
        const float side = 6.0f * 3.0f * zoom;
        const float wrap = ImGui::GetContentRegionAvail().x;
        ImVec2 pos = ImGui::GetCursorScreenPos();
        const float startX = pos.x;
        for (size_t i = 0; i < images.size(); i++) {
            const PixelImage& image = images[i];
            // the mask drawn as its coloured cells, no texture needed (R20's small images)
            for (int y = 0; y < image.height(); y++) {
                for (int x = 0; x < image.width(); x++) {
                    const auto pixel = image.at(x, y);
                    if (pixel[3] == 0) continue;
                    const ImVec2 p0(pos.x + (float)x * zoom, pos.y + (float)y * zoom);
                    list->AddRectFilled(p0, ImVec2(p0.x + zoom, p0.y + zoom), IM_COL32(pixel[0], pixel[1], pixel[2], pixel[3]));
                }
            }
            pos.x += side + 6.0f;
            if (pos.x + side > startX + wrap) { pos.x = startX; pos.y += side + 6.0f; }
        }
        const float rows = std::ceil((float)images.size() / std::max(1.0f, std::floor(wrap / (side + 6.0f))));
        ImGui::Dummy(ImVec2(wrap, rows * (side + 6.0f)));
        ImGui::EndChild();
    }
}

void TilesetEditorWindow::drawTemplateModal() {
    if (templateOpenRequested) {
        ImGui::OpenPopup("Apply Terrain Template###TilesetTemplate");
        templateOpenRequested = false;
        templateError.clear();
        templateTerrain = 0;
        templateSecond = -1;
        coverageSet = std::clamp(coverageSet, 0, (int)document.terrainSets.size() - 1);
    }
    if (!EditorModal::begin("Apply Terrain Template###TilesetTemplate", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    static const std::vector<std::string> templateLabels = {"Blob 47 (cr31 7x7)", "Blob 47 (Godot 12x4)", "Sides 16 (4x4)", "Dual Grid (4x4)"};
    std::vector<std::string> setNames;
    for (const TerrainSetDef& set : document.terrainSets) setNames.push_back(set.name);
    EditorModal::message("Assign names and peering values to the block of tiles whose top-left tile is the selected tile.");
    EditorModal::combo("Template", "template", &templateChoice, templateLabels);
    if (!setNames.empty()) {
        coverageSet = std::clamp(coverageSet, 0, (int)setNames.size() - 1);
        EditorModal::combo("Terrain Set", "set", &coverageSet, setNames);
        const TerrainSetDef& set = document.terrainSets[(size_t)coverageSet];
        std::vector<std::string> terrains;
        for (const TerrainDef& terrain : set.terrains) terrains.push_back(terrain.name);
        if (!terrains.empty()) {
            templateTerrain = std::clamp(templateTerrain, 0, (int)terrains.size() - 1);
            EditorModal::combo("Terrain", "terrain", &templateTerrain, terrains);
            if (set.twoSurface && terrains.size() > 1) {
                std::vector<std::string> seconds = {"(auto)"};
                for (const std::string& name : terrains) seconds.push_back(name);
                int second = templateSecond + 1;
                EditorModal::combo("Second Terrain", "second_terrain", &second, seconds);
                templateSecond = second - 1;
            }
        }
    }
    static const TerrainTemplate kTemplates[] = {TerrainTemplate::Blob47, TerrainTemplate::Blob47Godot, TerrainTemplate::Sides16, TerrainTemplate::DualGrid4x4};
    const bool canApply = !selection.empty() && !document.terrainSets.empty() && !document.terrainSets[(size_t)coverageSet].terrains.empty();
    if (!canApply) EditorModal::error("Select the block's top-left tile and a terrain first");
    else if (!templateError.empty()) EditorModal::error(templateError);
    if (!templateError.empty()) ImGui::TextColored(Theme::Colors::WarningText, "%s", templateError.c_str());
    ImGui::BeginDisabled(!canApply);
    if (EditorModal::button("Apply", "ok")) {
        const TerrainTemplate chosen = kTemplates[std::clamp(templateChoice, 0, 3)];
        const int setIndex = coverageSet;
        const int32_t origin = selection.front();
        TerrainTemplateOptions options;
        options.terrain = templateTerrain;
        options.secondTerrain = templateSecond;
        std::vector<TilesetProblem> problems;
        const bool applied = edit("Apply Terrain Template", [&](TilesetData& data) {
            terrain::applyTemplate(data, setIndex, chosen, origin, options, problems);
        });
        // a refusal is said in the dialog: the notice line sits behind the modal
        if (applied && problems.empty()) {
            templateError.clear();
            ImGui::CloseCurrentPopup();
        } else {
            templateError = !problems.empty() ? problems.front().message : notice;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (EditorModal::button("Cancel", "cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void TilesetEditorWindow::show() {
    if (!windowOpen) return;
    pollDisk();
    if (focusRequested) {
        ImGui::SetNextWindowFocus();
        focusRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(900.0f), Theme::dpi(600.0f)), ImGuiCond_FirstUseEver);
    bool keepOpen = true;
    if (ImGui::Begin(WINDOW_NAME, &keepOpen)) {
        if (!loaded) {
            if (!loadError.empty()) ImGui::TextColored(Theme::Colors::WarningText, "%s", loadError.c_str());
            else ImGui::TextDisabled("Open a .tileset from the Project window.");
        } else {
            drawToolbar();
            ImGui::BeginChild("##TilesetLeft", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
            drawSources();
            drawTileGrids();
            drawTerrainSets();
            drawCoverage();
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##TilesetRight", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
            drawSelectedInspector();
            ImGui::EndChild();
            drawTemplateModal();
        }
    }
    ImGui::End();
    if (!keepOpen) windowOpen = false;
}
