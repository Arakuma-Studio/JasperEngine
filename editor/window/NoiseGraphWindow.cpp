// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NoiseGraphWindow.h"

#include "Backend.h"
#include "Project.h"
#include "component/TerrainComponent.h"
#include "external/IconsFontAwesome6.h"
#include "graph/GraphEditorHooks.h"
#include "graph/GraphRegistry.h"
#include "noise/NoiseGraphRuntime.h"
#include "memory/MemoryStats.h"
#include "noise/NoiseNodes.h"
#include "util/ProjectChecks.h"
#include "util/UIProbe.h"
#include "util/Util.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    const size_t kThumbnailUploadsPerFrame = 8;     // R50
    const ImU32 kNodeFill = IM_COL32(58, 58, 64, 255);
    const ImU32 kDeadFill = IM_COL32(44, 44, 48, 200);
    const ImU32 kNodeText = IM_COL32(230, 230, 230, 255);
    const ImU32 kDeadText = IM_COL32(140, 140, 140, 255);
    const int kPreviewSizes[] = {64, 128, 256, 512};
    const float kThumbnailMinZoom = 0.5f;

    ImU32 colorOf(uint32_t rgba) {
        return IM_COL32((rgba >> 24) & 0xFF, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
    }

    uint32_t diceSeed() {
        static std::random_device device;
        return std::uniform_int_distribution<uint32_t>(1, 1000000)(device);
    }

}

NoiseGraphWindow::NoiseGraphWindow(Project* project) : project(project), document(project) {
    canvas.canConnect = [this](const GraphCanvas::PortRef& output, const GraphCanvas::PortRef& input) {
        return document.canConnectPins(pinOf(output), pinOf(input));
    };
    // R56: a node's image, at zoom 0.5 or above
    canvas.drawThumbnail = [this](ImDrawList* list, uint64_t id, ImVec2 min, ImVec2 max) {
        if (canvas.getZoom() < kThumbnailMinZoom) return;
        auto thumbnail = thumbnails.find(document.nodeOfId(id));
        if (thumbnail == thumbnails.end() || thumbnail->second.empty()) return;
        TextureRender* render = thumbnail->second.getRender();
        if (render && render->isCreated()) list->AddImage(ImTextureRef(Backend::getImGuiTexture(render)), min, max);
    };
    // a .noisegraph opens here wherever a graph file is opened (the Graph Editor's routes)
    graphEditorHooks().kindWindows[noise::kNoiseKindId] = {WINDOW_NAME, [this](const std::string& path) {
        const fs::path given(path);
        std::error_code ec;
        fs::path file = given.is_absolute() ? given : this->project->getProjectPath() / given;
        if (!given.is_absolute() && !fs::exists(file, ec)) file = this->project->resolveAssetPath(given);
        if (file.empty() || !fs::exists(file, ec)) return false;
        openGraph(file);
        return true;
    }};
}

uint64_t NoiseGraphWindow::pinOf(const GraphCanvas::PortRef& port) const {
    return document.pinOf(document.nodeOfId(port.node), port.port);
}

NoiseGraphWindow::~NoiseGraphWindow() {
    graphEditorHooks().kindWindows.erase(noise::kNoiseKindId);
}

void NoiseGraphWindow::setOpen(bool open) {
    windowOpen = open;
    if (open) refreshGraphList();
}

void NoiseGraphWindow::requestFocus() {
    focusRequest = true;
}

void NoiseGraphWindow::openGraph(const fs::path& file) {
    const fs::path absolute = file.is_absolute() ? file : project->getAssetsPath() / file;
    document.open(absolute);
    settings = document.previewSettings();
    releaseTextures();
    frameRequest = true;
    setOpen(true);
    requestFocus();
}

void NoiseGraphWindow::prepareForProjectSwitch() {
    if (applyRun) applyRun->cancel();
    applyRun.reset();
    document.close();
    releaseTextures();
    releaseRetired();
    windowOpen = false;
}

void NoiseGraphWindow::paste() {
    document.paste(graph::Vec2{canvas.toCanvas(ImGui::GetMousePos()).x, canvas.toCanvas(ImGui::GetMousePos()).y});
}

void NoiseGraphWindow::refreshGraphList() {
    graphFiles.clear();
    project->getAssetDatabase().forEachAsset([this](const AssetDatabase::AssetInfo& asset) {
        if (asset.type == AssetType::NoiseGraph) graphFiles.push_back(project->getProjectPath() / asset.path);
    });
    std::sort(graphFiles.begin(), graphFiles.end());
}

namespace {

    // What the window's thumbnails and Preview image hold on the GPU, in get_memory_stats: closed, it is 0 (R50)
    memory::TrackedBytes& noiseGraphTextureBytes() {
        static auto* tracked = new memory::TrackedBytes("Texture.NoiseGraph", memory::Residency::Gpu);
        return *tracked;
    }

}

void NoiseGraphWindow::releaseRetired() {
    for (Texture& texture : retired) {
        if (auto it = textureBytes.find(texture.getId()); it != textureBytes.end()) {
            noiseGraphTextureBytes().subtract(it->second);
            textureBytes.erase(it);
        }
        texture.destroy();
    }
    retired.clear();
}

void NoiseGraphWindow::releaseTextures() {
    for (auto& [node, texture] : thumbnails) retired.push_back(texture);
    thumbnails.clear();
    if (!previewTexture.empty()) retired.push_back(previewTexture);
    previewTexture = Texture();
    previewTextureRevision = 0;
}

ImTextureID NoiseGraphWindow::makeTexture(Texture& texture, const std::string& id, const std::vector<uint8_t>& rgba, int size) {
    if (!texture.empty()) retired.push_back(texture);
    texture = Texture();
    // TextureData frees what it is given (stbi_image_free = free)
    void* pixels = std::malloc(rgba.size());
    if (!pixels) return ImTextureID{};
    std::memcpy(pixels, rgba.data(), rgba.size());
    TextureData data(size, size, static_cast<unsigned int>(rgba.size()), ColorFormat::RGBA, 4, pixels);
    texture.setData(id, data);
    texture.setMinFilter(TextureFilter::LINEAR);
    texture.setMagFilter(TextureFilter::LINEAR);
    TextureRender* render = texture.getRender();
    texture.releaseData();
    if (!render || !render->isCreated()) {
        retired.push_back(texture);
        texture = Texture();
        return ImTextureID{};
    }
    const uint64_t bytes = static_cast<uint64_t>(size) * static_cast<uint64_t>(size) * 4u;
    textureBytes[id] = bytes;
    noiseGraphTextureBytes().add(bytes);
    return Backend::getImGuiTexture(render);
}

void NoiseGraphWindow::uploadThumbnails() {
    for (const std::string& node : document.takeThumbnailUploads(kThumbnailUploadsPerFrame)) {
        const NoiseNodePreview* preview = document.nodePreview(node);
        if (!preview) continue;
        makeTexture(thumbnails[node], "editor:noise-graph:" + std::to_string(++textureCounter), preview->rgba, document.previewThumbnailSize());
    }
    // nodes that went (a delete, an undo) leave no texture behind
    for (auto it = thumbnails.begin(); it != thumbnails.end();) {
        if (document.graph().findNode(it->first)) {
            ++it;
        } else {
            retired.push_back(it->second);
            it = thumbnails.erase(it);
        }
    }
}

void NoiseGraphWindow::uploadPreviewImage() {
    if (previewTextureRevision == document.previewedRevision()) return;
    previewTextureRevision = document.previewedRevision();
    const NoisePreviewImage& image = document.previewImage();
    if (image.size <= 0) return;
    makeTexture(previewTexture, "editor:noise-graph:" + std::to_string(++textureCounter), image.rgba, image.size);
}

void NoiseGraphWindow::show() {
    releaseRetired();
    if (!windowOpen) {
        // the first frame after closing: the job and the textures go; every later one, nothing happens here
        if (windowFocused || !thumbnails.empty() || !previewTexture.empty() || document.previewService().busy()) {
            document.update(false);
            releaseTextures();
            windowFocused = false;
        }
        return;
    }
    if (focusRequest) {
        ImGui::SetNextWindowFocus();
        focusRequest = false;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 70.0f, ImGui::GetFontSize() * 40.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(WINDOW_NAME, &windowOpen)) {
        // on a hidden tab: as closed
        windowFocused = false;
        document.update(false);
        releaseTextures();
        ImGui::End();
        return;
    }
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    document.refresh();
    document.setPreviewSettings(settings);
    document.update(true);
    uploadThumbnails();
    uploadPreviewImage();

    drawToolbar();
    if (!document.isOpen()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Pick a Noise Graph above, double-click a .noisegraph in the Project window, or use Create > Noise Graph.");
        ImGui::End();
        return;
    }
    drawIssueBar();
    const float rightWidth = ImGui::GetFontSize() * 20.0f;
    ImGui::BeginChild("##noisegraph_canvas", ImVec2(std::max(100.0f, ImGui::GetContentRegionAvail().x - rightWidth), 0.0f));
    drawCanvas();
    drawSearchPopup();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##noisegraph_panel", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    drawRightPanel();
    ImGui::EndChild();
    ImGui::End();
}

void NoiseGraphWindow::drawToolbar() {
    // Graph: the project's .noisegraph files
    const std::string current = document.isOpen() ? document.path().filename().string() : std::string("(none)");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    if (ImGui::BeginCombo("Graph", current.c_str())) {
        if (ImGui::IsWindowAppearing()) refreshGraphList();
        for (const fs::path& file : graphFiles) {
            const bool selected = document.isOpen() && document.path() == file;
            if (ImGui::Selectable(file.filename().string().c_str(), selected)) openGraph(file);
        }
        if (graphFiles.empty()) ImGui::TextDisabled("No .noisegraph in the project");
        ImGui::EndCombo();
    }
    UIProbe::item("noisegraph.graph");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("resource_files")) {
            const std::vector<std::string> dropped = Util::getStringsFromPayload(payload);
            if (!dropped.empty() && fs::path(dropped[0]).extension() == ".noisegraph") openGraph(dropped[0]);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    uint32_t seed = settings.seed;
    if (ImGui::InputScalar("Seed", ImGuiDataType_U32, &seed)) settings.seed = seed;
    UIProbe::item("noisegraph.seed");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_DICE "##noisegraph_dice")) settings.seed = diceSeed();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A new random seed; the graph's result depends only on it");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
    char sizeLabel[16];
    std::snprintf(sizeLabel, sizeof(sizeLabel), "%d", settings.imageSize);
    if (ImGui::BeginCombo("Preview Size", sizeLabel)) {
        for (int size : kPreviewSizes) {
            std::snprintf(sizeLabel, sizeof(sizeLabel), "%d", size);
            if (ImGui::Selectable(sizeLabel, size == settings.imageSize)) settings.imageSize = size;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!document.canEdit());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    const bool world = document.isWorldSpace();
    if (ImGui::BeginCombo("Space", world ? "World Units" : "Terrain Size")) {
        if (ImGui::Selectable("Terrain Size", !world) && world) document.setSpace(false);
        if (ImGui::Selectable("World Units", world) && !world) document.setSpace(true);
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Frame")) frameRequest = true;
    UIProbe::item("noisegraph.frame");
    ImGui::SameLine();
    // Previews run by themselves; this shows the graph's own in the panel, whatever was selected
    if (ImGui::Button("Preview")) document.setSelection({}, {});
    UIProbe::item("noisegraph.preview");
    ImGui::SameLine();
    const bool hasTerrain = [this]() {
        const uint32_t sceneId = project->getSelectedSceneId();
        const SceneProject* scene = project->getScene(sceneId);
        if (!scene || !scene->scene) return false;
        for (Entity entity : project->getSelectedEntities(sceneId)) {
            if (scene->scene->findComponent<TerrainComponent>(entity)) return true;
        }
        return false;
    }();
    const bool canApply = document.isOpen() && !document.hasErrors() && hasTerrain && !applyRun;
    ImGui::BeginDisabled(!canApply);
    if (ImGui::Button("Apply")) applyToSelectedTerrain();
    ImGui::EndDisabled();
    UIProbe::item("noisegraph.apply");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        if (document.hasErrors()) ImGui::SetTooltip("%s", document.firstError().c_str());
        else if (!hasTerrain) ImGui::SetTooltip("Select a Terrain in the Hierarchy to apply this graph to it");
    }
    if (applyRun) {
        ImGui::SameLine();
        ImGui::ProgressBar(applyRun->progress(), ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f));
        ImGui::SameLine();
        if (ImGui::Button("Cancel##noisegraph_apply")) applyRun->cancel();
    }
    if (!applyStatus.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", applyStatus.c_str());
    }
}

void NoiseGraphWindow::applyToSelectedTerrain() {
    const uint32_t sceneId = project->getSelectedSceneId();
    const SceneProject* scene = project->getScene(sceneId);
    if (!scene || !scene->scene) return;
    Entity target = NULL_ENTITY;
    for (Entity entity : project->getSelectedEntities(sceneId)) {
        if (scene->scene->findComponent<TerrainComponent>(entity)) {
            target = entity;
            break;
        }
    }
    if (target == NULL_ENTITY) return;
    GenerationRequest request;
    request.graph = document.graph();
    request.displayName = document.displayName();
    request.seed = settings.seed;
    request.variables = settings.variables;
    applyStatus.clear();
    applyRun = TerrainGeneration::start(project, sceneId, target, std::move(request), [this](const GenerationResult& result) {
        applyRun.reset();
        applyStatus = result.ok ? result.undoName : result.cancelled ? std::string("Cancelled") : result.error;
    });
}

void NoiseGraphWindow::drawIssueBar() {
    if (!document.loadError().empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  %s", document.loadError().c_str());
        ImGui::PopStyleColor();
        UIProbe::item("noisegraph.load_error: " + document.loadError());
        return;
    }
    for (const graph::Issue& issue : document.issuesWithoutNode()) {
        const bool error = issue.severity == graph::Severity::Error;
        ImGui::PushStyleColor(ImGuiCol_Text, error ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f) : ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
        ImGui::TextWrapped(ICON_FA_TRIANGLE_EXCLAMATION "  %s", issue.message.c_str());
        ImGui::PopStyleColor();
        UIProbe::item("noisegraph.issue: " + issue.message);
    }
    if (!document.status().empty()) {
        ImGui::TextDisabled("%s", document.status().c_str());
    }
}

void NoiseGraphWindow::drawCanvas() {
    std::vector<GraphCanvas::Node> nodes;
    std::vector<GraphCanvas::Link> links;
    const std::vector<NoiseGraphNodeView>& views = document.nodeViews();
    nodes.reserve(views.size());
    for (const NoiseGraphNodeView& view : views) {
        GraphCanvas::Node node;
        node.id = view.id;
        node.position = ImVec2(static_cast<float>(view.position.x), static_cast<float>(view.position.y));
        node.size = ImVec2(GraphCanvas::MIN_WIDTH, 0.0f);
        node.label = view.title;
        node.fill = view.dead ? kDeadFill : kNodeFill;
        node.text = view.dead ? kDeadText : kNodeText;
        node.selected = view.selected;
        for (const NoiseGraphPinView& pin : view.inputs) node.inputs.push_back({pin.port, pin.name, colorOf(pin.color), pin.connected, pin.error});
        for (const NoiseGraphPinView& pin : view.outputs) node.outputs.push_back({pin.port, pin.name, colorOf(pin.color), pin.connected, pin.error});
        // the slot keeps its place while the texture is not there yet
        node.thumbnail = true;
        node.cost = view.cost;
        // a dataflow node's header holds its label and cost: the state is its first dim line
        if (view.folded || view.dead) node.footer.push_back(view.folded ? "folded" : "unused");
        node.footer.insert(node.footer.end(), view.footer.begin(), view.footer.end());
        node.error = !view.errorText.empty();
        node.errorText = view.errorText;
        nodes.push_back(std::move(node));
    }
    for (const NoiseGraphWireView& wire : document.wireViews()) {
        GraphCanvas::Link link;
        link.id = wire.id;
        link.from = wire.fromNode;
        link.to = wire.toNode;
        link.fromPort = wire.fromPort;
        link.toPort = wire.toPort;
        link.color = colorOf(wire.color);
        link.selected = wire.selected;
        links.push_back(link);
    }

    if (frameRequest) {
        frameRequest = false;
        canvas.frame({});
    }

    const GraphCanvas::Result result = canvas.draw("noisegraph.canvas", nodes, links);
    UIProbe::item("noisegraph.canvas");

    // What the driven tests read: an error's message and each node's place
    if (UIProbe::enabled()) {
        for (size_t i = 0; i < nodes.size(); i++) {
            const ImVec2 a = canvas.toScreen(nodes[i].position);
            const ImVec2 b = canvas.toScreen(ImVec2(nodes[i].position.x + nodes[i].size.x, nodes[i].position.y + nodes[i].size.y));
            UIProbe::rect("noisegraph.node." + views[i].nodeId, a.x, a.y, b.x, b.y);
            if (!views[i].errorText.empty()) UIProbe::rect("noisegraph.error." + views[i].nodeId + ": " + views[i].errorText, a.x, a.y, b.x, b.y);
            const auto probePins = [&](const std::vector<NoiseGraphPinView>& pins, bool output) {
                for (const NoiseGraphPinView& pin : pins) {
                    const ImVec2 at = canvas.toScreen(GraphCanvas::portPosition(nodes[i], pin.port, output));
                    UIProbe::rect("noisegraph.pin." + views[i].nodeId + "." + pin.port, at.x - 4.0f, at.y - 4.0f, at.x + 4.0f, at.y + 4.0f);
                }
            };
            probePins(views[i].inputs, false);
            probePins(views[i].outputs, true);
        }
    }

    // ---- the gestures
    if (result.selectionChanged) {
        std::set<std::string> nodeSelection;
        std::set<std::string> wireSelection;
        for (const GraphCanvas::Node& node : nodes) {
            if (node.selected) nodeSelection.insert(document.nodeOfId(node.id));
        }
        for (const GraphCanvas::Link& link : links) {
            if (link.selected) wireSelection.insert(document.edgeOfId(link.id));
        }
        document.setSelection(std::move(nodeSelection), std::move(wireSelection));
    }
    if (!result.moved.empty()) {
        std::vector<std::pair<std::string, graph::Vec2>> positions;
        for (const auto& [id, position] : result.moved) positions.push_back({document.nodeOfId(id), graph::Vec2{position.x, position.y}});
        document.moveNodes(positions);
    }
    if (result.connectFrom.node != 0 && result.connectTo.node != 0) document.connectPins(pinOf(result.connectFrom), pinOf(result.connectTo));
    if (result.wireDropped) {
        searchRequest = true;
        searchPosition = result.wireDropCanvas;
        searchPin = pinOf(result.wireSource);
    }
    // R59: right click and Tab open the same popup
    if (result.contextRequested && result.contextNode == 0 && result.contextLink == 0) {
        searchRequest = true;
        searchPosition = result.contextPosition;
        searchPin = 0;
    }
    if (result.hovered && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        searchRequest = true;
        searchPosition = result.mouseCanvas;
        searchPin = 0;
    }
    // on a wire, it selects it: Delete takes it
    if (result.contextLink != 0) document.setSelection({}, {document.edgeOfId(result.contextLink)});
}

void NoiseGraphWindow::drawSearchPopup() {
    if (searchRequest) {
        searchRequest = false;
        searchText[0] = '\0';
        searchCursor = 0;
        ImGui::OpenPopup("##noisegraph_search");
    }
    if (!ImGui::BeginPopup("##noisegraph_search")) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    const bool edited = ImGui::InputTextWithHint("##noisegraph_search_text", "Search nodes", searchText, sizeof(searchText));
    UIProbe::item("noisegraph.search");
    if (edited) searchCursor = 0;
    const std::vector<NoiseGraphSearchEntry> entries = document.search(searchText, searchPin);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !entries.empty()) searchCursor = (searchCursor + 1) % static_cast<int>(entries.size());
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && !entries.empty()) searchCursor = (searchCursor + static_cast<int>(entries.size()) - 1) % static_cast<int>(entries.size());
    bool add = !entries.empty() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
    int chosen = searchCursor;
    std::string category;
    // The popup opens at the mouse, which may be near the screen's bottom: the list takes what fits there, so no
    // row is drawn off-screen (it scrolls instead)
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float roomBelow = viewport->WorkPos.y + viewport->WorkSize.y - ImGui::GetCursorScreenPos().y - ImGui::GetStyle().WindowPadding.y * 2.0f;
    const float listHeight = std::max(ImGui::GetFontSize() * 4.0f, std::min(ImGui::GetFontSize() * 18.0f, roomBelow));
    ImGui::BeginChild("##noisegraph_search_list", ImVec2(ImGui::GetFontSize() * 16.0f, listHeight));
    for (int i = 0; i < static_cast<int>(entries.size()); i++) {
        if (entries[i].category != category) {
            category = entries[i].category;
            ImGui::TextDisabled("%s", category.c_str());
        }
        ImGui::PushID(i);
        if (ImGui::Selectable(entries[i].name.c_str(), i == searchCursor)) {
            add = true;
            chosen = i;
        }
        UIProbe::item("noisegraph.search." + entries[i].typeId);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (add && chosen < static_cast<int>(entries.size())) {
        document.addFromSearch(entries[chosen].typeId, graph::Vec2{searchPosition.x, searchPosition.y}, searchPin);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void NoiseGraphWindow::drawRightPanel() {
    if (document.selectedNodes().size() == 1) drawNodeParams(*document.selectedNodes().begin());
    else drawVariablesAndPreview();
}

void NoiseGraphWindow::drawNodeParams(const std::string& nodeId) {
    const graph::Node* node = document.graph().findNode(nodeId);
    if (!node) return;
    noise::registerNoiseKind();
    std::shared_ptr<const graph::GraphKindDesc> kind = graph::GraphRegistry::get().kind(noise::kNoiseKindId);
    const graph::NodeTypeDesc* type = kind ? graph::GraphRegistry::nodeType(*kind, node->type) : nullptr;
    ImGui::TextUnformatted(type && !type->displayName.empty() ? type->displayName.c_str() : node->type.c_str());
    ImGui::TextDisabled("%s", nodeId.c_str());
    if (!type) {
        ImGui::TextWrapped("A node type this editor does not know: its parameters are kept as they are.");
        return;
    }
    ImGui::Separator();
    ImGui::BeginDisabled(!document.canEdit());
    for (const graph::ParamDesc& param : type->params) {
        // an edit below replaces the document's graph: never keep a node across a row
        node = document.graph().findNode(nodeId);
        if (!node) break;
        ImGui::PushID(param.name.c_str());
        auto stored = node->params.find(param.name);
        const graph::Json value = stored != node->params.end() ? *stored : param.defaultValue;
        const std::string label = param.name;
        // A drag: the working value while it lasts, one undo step on release (R57)
        const auto dragState = [&](bool changed, const graph::Json& next) {
            if (ImGui::IsItemActivated()) document.beginParamDrag(nodeId, param.name);
            if (changed) document.updateParamDrag(next);
            if (ImGui::IsItemDeactivated()) document.commitParamDrag();
        };
        if (param.kind == "float" && value.is_number()) {
            float number = value.get<float>();
            const float low = param.min ? static_cast<float>(*param.min) : 0.0f;
            const float high = param.max ? static_cast<float>(*param.max) : 0.0f;
            const float speed = param.min && param.max ? (high - low) / 200.0f : std::max(0.01f, std::fabs(number) * 0.01f);
            const bool changed = ImGui::DragFloat(label.c_str(), &number, speed, low, high, "%.3f");
            dragState(changed, number);
        } else if (param.kind == "int" && value.is_number()) {
            int number = value.get<int>();
            const int low = param.min ? static_cast<int>(*param.min) : 0;
            const int high = param.max ? static_cast<int>(*param.max) : 0;
            const bool changed = ImGui::DragInt(label.c_str(), &number, 0.1f, low, high);
            dragState(changed, number);
        } else if (param.kind == "bool" && value.is_boolean()) {
            bool flag = value.get<bool>();
            if (ImGui::Checkbox(label.c_str(), &flag)) document.setParam(nodeId, param.name, flag);
        } else if (param.kind == "enum" && value.is_string()) {
            const std::string current = value.get<std::string>();
            if (ImGui::BeginCombo(label.c_str(), current.c_str())) {
                for (const std::string& option : param.enumValues) {
                    if (ImGui::Selectable(option.c_str(), option == current) && option != current) document.setParam(nodeId, param.name, option);
                }
                ImGui::EndCombo();
            }
        } else if (param.kind == "string" && (value.is_string() || value.is_null())) {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "%s", value.is_string() ? value.get<std::string>().c_str() : "");
            ImGui::InputText(label.c_str(), buffer, sizeof(buffer));
            if (ImGui::IsItemDeactivatedAfterEdit()) document.setParam(nodeId, param.name, std::string(buffer));
        } else {
            // curves and the like: shown; edit_graph and the file edit them
            const std::string dump = value.dump();
            ImGui::TextDisabled("%s", label.c_str());
            ImGui::SameLine();
            ImGui::TextWrapped("%s", dump.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (const NoiseNodePreview* preview = document.nodePreview(nodeId)) {
        ImGui::Separator();
        ImGui::TextUnformatted(NoiseGraphDocument::rangeText(preview->observed, preview->analytic).c_str());
    }
}

void NoiseGraphWindow::drawVariablesAndPreview() {
    bool anyVariable = false;
    for (const graph::Variable& variable : document.graph().variables) {
        if (!variable.exposed) continue;
        if (!anyVariable) {
            ImGui::TextUnformatted("Variables");
            anyVariable = true;
        }
        auto found = std::find_if(settings.variables.begin(), settings.variables.end(), [&](const auto& entry) { return entry.first == variable.name; });
        float value = found != settings.variables.end() ? found->second
                    : variable.defaultValue.is_number() ? variable.defaultValue.get<float>() : 0.0f;
        if (ImGui::DragFloat(variable.name.c_str(), &value, 0.01f)) {
            if (found != settings.variables.end()) found->second = value;
            else settings.variables.push_back({variable.name, value});
        }
    }
    if (anyVariable) ImGui::Separator();
    ImGui::TextUnformatted("Preview");
    const NoisePreviewImage& image = document.previewImage();
    if (!previewTexture.empty() && image.size > 0) {
        if (TextureRender* render = previewTexture.getRender(); render && render->isCreated()) {
            const float side = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 18.0f);
            ImGui::Image(ImTextureRef(Backend::getImGuiTexture(render)), ImVec2(side, side));
            UIProbe::item("noisegraph.preview_image");
        }
        ImGui::Text("%s  %d x %d", image.outputName.c_str(), image.size, image.size);
        ImGui::Text("Min %.3f   Max %.3f   Mean %.3f", static_cast<double>(image.min), static_cast<double>(image.max), static_cast<double>(image.mean));
    } else {
        ImGui::TextDisabled(document.hasErrors() ? "Fix the errors to preview." : "Working...");
    }
}
