// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphEditorWindow.h"

#include "Project.h"
#include "Theme.h"
#include "command/CommandHistory.h"
#include "external/IconsFontAwesome6.h"
#include "graph/GraphEditorHooks.h"
#include "graph/GraphParamValues.h"
#include "search/SearchScorer.h"
#include "util/FileDialogs.h"
#include "util/UIProbe.h"
#include "widget/ColorField.h"
#include "widget/CurveField.h"
#include "widget/GradientField.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;
// (the editor has its own Json, nlohmann::json; graph files are ordered)
using GraphJson = graph::Json;

namespace{

    // canvas ids of the wires (nodes are their index + 1)
    constexpr uint64_t EDGE_BASE = 1ull << 40;
    constexpr const char* CLIPBOARD_KEY = "graph_clipboard";

    ImU32 portColor(const graph::GraphKindDesc* kind, const std::string& type){
        const graph::PortTypeDesc* desc = kind ? graph::GraphRegistry::portType(*kind, type) : nullptr;
        if (!desc) return IM_COL32(200, 200, 200, 255);
        const uint32_t c = desc->color;   // RGBA
        return IM_COL32((c >> 24) & 0xFF, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }

    std::string typeLabel(const graph::NodeTypeDesc* type, const std::string& id){
        return type && !type->displayName.empty() ? type->displayName : id;
    }

    // the ports of a node of this type with these parameters (a dynamic type's from them)
    std::vector<graph::Port> typePorts(const graph::NodeTypeDesc& type, const GraphJson& params){
        return type.dynamicPorts ? type.dynamicPorts(params) : type.ports;
    }

    GraphJson defaultParams(const graph::NodeTypeDesc& type){
        GraphJson params = GraphJson::object();
        for (const graph::ParamDesc& param : type.params){
            if (!param.defaultValue.is_null()) params[param.name] = param.defaultValue;
        }
        return params;
    }

    GraphJson positionJson(const ImVec2& p){
        return GraphJson::array({std::round((double)p.x), std::round((double)p.y)});
    }

    // "Base Color" from "base_color"
    std::string paramLabel(const std::string& name){
        std::string out;
        bool upper = true;
        for (char c : name){
            if (c == '_' || c == '-'){
                out += ' ';
                upper = true;
                continue;
            }
            out += upper ? (char)std::toupper((unsigned char)c) : c;
            upper = false;
        }
        return out;
    }

}

std::vector<GraphEditorWindow*>& GraphEditorWindow::instances(){
    static std::vector<GraphEditorWindow*> all;
    return all;
}

GraphEditorWindow::GraphEditorWindow(Project* project, std::string windowName, std::string kindId)
    : project(project), name(std::move(windowName)), kindFilter(std::move(kindId)){
    instances().push_back(this);
    graphEditorHooks().drawSelection = [](){ return drawClaimedInspector(); };
    // a kind's own window first, the window of any kind after it
    graphEditorHooks().openGraph = [](const std::string& path){
        auto kind = graph::GraphRegistry::get().kindForFile(path);
        if (!kind) return false;
        GraphEditorWindow* target = nullptr;
        for (GraphEditorWindow* window : instances()){
            if (window->kindFilter == kind->id) target = window;
        }
        for (GraphEditorWindow* window : instances()){
            if (!target && window->kindFilter.empty()) target = window;
        }
        if (!target || !target->openGraph(path)) return false;
        target->setOpen(true);
        target->requestFocus();
        return true;
    };
    canvas.canConnect = [this](const GraphCanvas::PortRef& output, const GraphCanvas::PortRef& input){
        return canConnect(nodeIdOf(output.node), output.port, nodeIdOf(input.node), input.port);
    };
    canvas.drawThumbnail = [this](ImDrawList* list, uint64_t id, ImVec2 min, ImVec2 max){
        if (!document) return;
        if (const graph::Node* node = document->graph().findNode(nodeIdOf(id))) drawNodeThumbnail(*node, list, min, max);
    };
}

GraphEditorWindow::~GraphEditorWindow(){
    setDocument(nullptr);
    auto& all = instances();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
    if (all.empty()){
        graphEditorHooks().drawSelection = nullptr;
        graphEditorHooks().openGraph = nullptr;
    }
}

void GraphEditorWindow::prepareForProjectSwitch(){
    closeSearch();
    setDocument(nullptr);
    selection.clear();
    edgeSelection.clear();
    inspectorClaim = false;
    status.clear();
    // the documents belong to the project that closes
    GraphDocument::closeAll();
}

// ---------------------------------------------------------------- document

bool GraphEditorWindow::openGraph(const std::string& path){
    if (!kindFilter.empty()){
        auto kind = graph::GraphRegistry::get().kindForFile(path);
        if (!kind || kind->id != kindFilter){
            status = "'" + path + "' is not a " + kindFilter + " graph";
            return false;
        }
    }
    std::string error;
    std::shared_ptr<GraphDocument> next = GraphDocument::open(project, path, &error);
    if (!next){
        status = error;
        return false;
    }
    if (next != document){
        selection.clear();
        edgeSelection.clear();
        inspectorClaim = false;
        closeSearch();
        frameRequest = true;
    }
    setDocument(next);
    status.clear();
    return true;
}

void GraphEditorWindow::setDocument(std::shared_ptr<GraphDocument> next){
    if (document && listener) document->removeListener(listener);
    listener = 0;
    document = std::move(next);
    seenRevision = 0;
    if (document) listener = document->addListener([this](){ syncToDocument(); });
    syncToDocument();
}

void GraphEditorWindow::syncToDocument(){
    if (!document || document->revision() == seenRevision) return;
    seenRevision = document->revision();
    // a selection whose node or wire is gone (an undo, MCP, a file) is dropped
    const graph::Graph& g = document->graph();
    std::vector<std::string> kept;
    for (const std::string& id : selection){
        if (g.findNode(id)) kept.push_back(id);
    }
    if (kept != selection){
        selection = std::move(kept);
        selectionChanged();
    }
    if (!edgeSelection.empty() && !g.findEdge(edgeSelection)) edgeSelection.clear();
    documentChanged();
}

bool GraphEditorWindow::apply(const std::vector<graph::GraphOp>& ops, const std::string& undoName, std::vector<graph::GraphOp>* inverses){
    if (!document){
        status = "no graph is open";
        return false;
    }
    std::string error;
    if (!document->apply(ops, undoName, &error, inverses)){
        status = error;
        return false;
    }
    status.clear();
    syncToDocument();
    return true;
}

// ---------------------------------------------------------------- selection and claim

void GraphEditorWindow::setSelection(std::vector<std::string> ids, bool claim){
    const bool changed = ids != selection;
    selection = std::move(ids);
    if (!selection.empty()) edgeSelection.clear();
    if (claim && !selection.empty()) claimInspector();
    if (selection.empty()) inspectorClaim = false;
    if (changed) selectionChanged();
}

void GraphEditorWindow::selectNodes(const std::vector<std::string>& ids){
    std::vector<std::string> existing;
    for (const std::string& id : ids){
        if (document && document->graph().findNode(id)) existing.push_back(id);
    }
    setSelection(existing, true);
}

void GraphEditorWindow::claimInspector(){
    inspectorClaim = true;
    claimScene = project->getSelectedSceneId();
    claimEntities = project->getSelectedEntities(claimScene);
    claimAsset = project->getInspectedAsset().string();
    // the last window clicked owns the Inspector
    for (GraphEditorWindow* other : instances()){
        if (other != this) other->inspectorClaim = false;
    }
}

bool GraphEditorWindow::claimHolds(){
    if (!inspectorClaim) return false;
    const uint32_t scene = project->getSelectedSceneId();
    if (scene != claimScene || project->getSelectedEntities(scene) != claimEntities || project->getInspectedAsset().string() != claimAsset){
        inspectorClaim = false;
    }
    return inspectorClaim;
}

bool GraphEditorWindow::inspectorShowsSelection() const{
    return inspectorFrame >= ImGui::GetFrameCount() - 1;
}

bool GraphEditorWindow::drawClaimedInspector(){
    for (GraphEditorWindow* window : instances()){
        if (window->drawSelectionInspector()) return true;
    }
    return false;
}

std::string GraphEditorWindow::nodeIdOf(uint64_t canvasId) const{
    return canvasId >= 1 && canvasId <= nodeIds.size() ? nodeIds[canvasId - 1] : std::string();
}

uint64_t GraphEditorWindow::canvasIdOf(const std::string& nodeId) const{
    for (size_t i = 0; i < nodeIds.size(); i++){
        if (nodeIds[i] == nodeId) return i + 1;
    }
    return 0;
}

bool GraphEditorWindow::nodeRect(const std::string& nodeId, ImVec2& min, ImVec2& max) const{
    const uint64_t id = canvasIdOf(nodeId);
    if (id == 0 || id > lastNodes.size()) return false;
    const GraphCanvas::Node& node = lastNodes[id - 1];
    min = canvas.toScreen(node.position);
    max = canvas.toScreen(ImVec2(node.position.x + node.size.x, node.position.y + node.size.y));
    return true;
}

bool GraphEditorWindow::portScreen(const std::string& nodeId, const std::string& port, bool output, ImVec2& out) const{
    const uint64_t id = canvasIdOf(nodeId);
    if (id == 0 || id > lastNodes.size()) return false;
    out = canvas.toScreen(GraphCanvas::portPosition(lastNodes[id - 1], port, output));
    return true;
}

// ---------------------------------------------------------------- the canvas' nodes

const graph::Port* GraphEditorWindow::portOf(const graph::Node& node, const std::string& port, bool output, std::vector<graph::Port>& storage) const{
    storage = graph::portsOf(document->graph(), node);
    for (const graph::Port& p : storage){
        if (p.id == port && (p.direction == graph::PortDirection::Out) == output) return &p;
    }
    return nullptr;
}

GraphCanvas::Node GraphEditorWindow::canvasNodeOfType(const std::string& type, const GraphJson& params) const{
    GraphCanvas::Node out;
    const graph::GraphKindDesc* kind = document && document->kind() ? document->kind().get() : nullptr;
    const graph::NodeTypeDesc* desc = kind ? graph::GraphRegistry::nodeType(*kind, type) : nullptr;
    out.label = typeLabel(desc, type);
    if (!desc) return out;
    for (const graph::Port& port : typePorts(*desc, params)){
        GraphCanvas::Port p{port.id, port.name.empty() ? port.id : port.name, portColor(kind, port.type), false};
        (port.direction == graph::PortDirection::Out ? out.outputs : out.inputs).push_back(p);
    }
    return out;
}

GraphCanvas::Node GraphEditorWindow::canvasNode(const graph::Node& node) const{
    GraphCanvas::Node out;
    const graph::Graph& g = document->graph();
    const graph::GraphKindDesc* kind = document->kind().get();
    const graph::NodeTypeDesc* desc = document->nodeType(node.type);
    out.label = typeLabel(desc, node.type);
    const graph::Vec2 at = document->displayPosition(node);
    out.position = ImVec2((float)at.x, (float)at.y);
    if (node.size) out.size = ImVec2((float)node.size->x, out.size.y);
    if (!desc){
        // a type this build does not know: kept as it is, shown as a box
        out.label += " (unknown)";
        out.fill = IM_COL32(110, 50, 50, 255);
        out.size = ImVec2(180, 40);
        return out;
    }
    std::set<std::string> connected;
    for (const graph::Edge& edge : g.edges){
        if (edge.from.node == node.id) connected.insert("o:" + edge.from.port);
        if (edge.to.node == node.id) connected.insert("i:" + edge.to.port);
    }
    for (const graph::Port& port : graph::portsOf(g, node)){
        const bool output = port.direction == graph::PortDirection::Out;
        GraphCanvas::Port p{port.id, port.name.empty() ? port.id : port.name, portColor(kind, port.type),
                            connected.count((output ? "o:" : "i:") + port.id) > 0};
        (output ? out.outputs : out.inputs).push_back(p);
    }
    if (out.inputs.empty() && out.outputs.empty()){
        // a dataflow graph's node without ports still gets a header, so it reads as one
        out.size = ImVec2(GraphCanvas::MIN_WIDTH, GraphCanvas::HEADER_HEIGHT + GraphCanvas::PADDING);
    }
    for (const graph::Issue& issue : document->issues()){
        if (issue.severity == graph::Severity::Error && issue.nodeId == node.id) out.error = true;
    }
    out.thumbnail = nodeHasThumbnail(node);
    out.cost = nodeCost(node);
    out.selected = std::find(selection.begin(), selection.end(), node.id) != selection.end();
    return out;
}

// ---------------------------------------------------------------- edits

bool GraphEditorWindow::canConnect(const std::string& fromNode, const std::string& fromPort, const std::string& toNode,
                                   const std::string& toPort) const{
    if (!document || !document->editable() || fromNode == toNode) return false;
    const graph::Graph& g = document->graph();
    const graph::Node* from = g.findNode(fromNode);
    const graph::Node* to = g.findNode(toNode);
    if (!from || !to) return false;
    std::vector<graph::Port> fromPorts, toPorts;
    const graph::Port* out = portOf(*from, fromPort, true, fromPorts);
    const graph::Port* in = portOf(*to, toPort, false, toPorts);
    if (!out || !in || !graph::GraphRegistry::canConnect(document->kind().get(), out->type, in->type)) return false;
    if (!document->kind()->acyclic) return true;
    // a wire that would close a loop: `from` reached from `to`
    std::vector<std::string> pending = {toNode};
    std::set<std::string> seen;
    while (!pending.empty()){
        const std::string current = pending.back();
        pending.pop_back();
        if (current == fromNode) return false;
        if (!seen.insert(current).second) continue;
        for (const graph::Edge& edge : g.edges){
            if (edge.from.node == current) pending.push_back(edge.to.node);
        }
    }
    return true;
}

namespace{

    // the ops that join an output to an input: the wire an input of one wire had goes first
    std::vector<graph::GraphOp> connectOps(const graph::Graph& g, const std::string& fromNode, const std::string& fromPort, const std::string& toNode,
                                           const std::string& toPort, bool multi){
        std::vector<graph::GraphOp> ops;
        for (const graph::Edge& edge : g.edges){
            if (multi || edge.to.node != toNode || edge.to.port != toPort) continue;
            ops.push_back({"disconnect", {{"id", edge.id}}});
        }
        ops.push_back({"connect", {{"from", {{"node", fromNode}, {"port", fromPort}}}, {"to", {{"node", toNode}, {"port", toPort}}}}});
        return ops;
    }

}

bool GraphEditorWindow::connect(const std::string& fromNode, const std::string& fromPort, const std::string& toNode, const std::string& toPort){
    if (!canConnect(fromNode, fromPort, toNode, toPort)){
        status = "those ports cannot be joined (their types, their direction, or a loop)";
        return false;
    }
    const graph::Graph& g = document->graph();
    std::vector<graph::Port> ports;
    const graph::Port* in = portOf(*g.findNode(toNode), toPort, false, ports);
    const std::vector<graph::GraphOp> ops = connectOps(g, fromNode, fromPort, toNode, toPort, in && in->multi);
    return apply(ops, graph::describe(g, ops.back()));
}

std::string GraphEditorWindow::addNode(const std::string& type, const ImVec2& position, const GraphJson& params){
    if (!document || !document->nodeType(type)){
        status = "no node type '" + type + "' in this graph's kind";
        return std::string();
    }
    graph::Graph copy = document->graph();
    const std::string id = copy.allocateId("n");
    GraphJson node = {{"id", id}, {"type", type}, {"position", positionJson(position)}};
    if (params.is_object() && !params.empty()) node["params"] = params;
    const graph::GraphOp op{"addNode", {{"node", node}}};
    if (!apply({op}, graph::describe(document->graph(), op))) return std::string();
    setSelection({id}, true);
    return id;
}

bool GraphEditorWindow::setParam(const std::string& nodeId, const std::string& param, const GraphJson& value){
    if (!document || !document->graph().findNode(nodeId)) return false;
    const GraphJson* current = nullptr;
    const graph::Node* node = document->graph().findNode(nodeId);
    if (auto it = node->params.find(param); it != node->params.end()) current = &*it;
    if (current && *current == value) return true;
    return apply({{"setParam", {{"node", nodeId}, {"name", param}, {"value", value}}}}, "Set " + paramLabel(param));
}

bool GraphEditorWindow::moveNodes(const std::vector<std::pair<std::string, ImVec2>>& positions){
    if (positions.empty()) return true;
    GraphJson moved = GraphJson::object();
    for (const auto& [id, at] : positions) moved[id] = positionJson(at);
    const graph::GraphOp op{"moveNodes", {{"positions", moved}}};
    return apply({op}, graph::describe(document->graph(), op));
}

bool GraphEditorWindow::canDelete() const{
    return windowFocused && document && document->editable() && (!selection.empty() || !edgeSelection.empty());
}

bool GraphEditorWindow::removeSelection(){
    if (!document) return false;
    std::vector<graph::GraphOp> ops;
    if (!selection.empty()){
        for (const std::string& id : selection) ops.push_back({"removeNode", {{"id", id}}});
    }else if (!edgeSelection.empty()){
        ops.push_back({"disconnect", {{"id", edgeSelection}}});
    }
    if (ops.empty()) return false;
    const std::string undo = ops.size() == 1 ? graph::describe(document->graph(), ops[0]) : "Delete " + std::to_string(ops.size()) + " Nodes";
    if (!apply(ops, undo)) return false;
    setSelection({}, false);
    edgeSelection.clear();
    return true;
}

bool GraphEditorWindow::arrange(){
    if (!document || document->graph().nodes.empty()) return false;
    GraphJson ids = GraphJson::array();
    for (const graph::Node& node : document->graph().nodes) ids.push_back(node.id);
    if (!apply({{"layout", {{"ids", ids}}}}, "Arrange Nodes")) return false;
    frameRequest = true;
    return true;
}

void GraphEditorWindow::frameSelected(){
    frameRequest = true;
}

// ---------------------------------------------------------------- copy and paste

std::string GraphEditorWindow::copyText() const{
    if (!document || selection.empty()) return std::string();
    const graph::Graph& g = document->graph();
    GraphJson nodes = GraphJson::array();
    std::set<std::string> picked(selection.begin(), selection.end());
    for (const graph::Node& node : g.nodes){
        if (!picked.count(node.id)) continue;
        graph::Node placed = node;
        placed.position = document->displayPosition(node);
        placed.placed = true;
        nodes.push_back(graph::toJson(placed));
    }
    GraphJson edges = GraphJson::array();
    for (const graph::Edge& edge : g.edges){
        if (picked.count(edge.from.node) && picked.count(edge.to.node)) edges.push_back(graph::toJson(edge));
    }
    return GraphJson{{CLIPBOARD_KEY, g.kind}, {"nodes", nodes}, {"edges", edges}}.dump(2);
}

bool GraphEditorWindow::copySelection(){
    const std::string text = copyText();
    if (text.empty()) return false;
    ImGui::SetClipboardText(text.c_str());
    return true;
}

bool GraphEditorWindow::canPaste() const{
    if (!document || !document->editable()) return false;
    const char* text = ImGui::GetClipboardText();
    return text && std::string(text).find(CLIPBOARD_KEY) != std::string::npos;
}

bool GraphEditorWindow::paste(){
    const char* text = ImGui::GetClipboardText();
    if (!text) return false;
    return pasteText(text, canvasHovered ? lastMouseCanvas : canvas.toCanvas(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f)));
}

bool GraphEditorWindow::duplicateSelection(){
    const std::string text = copyText();
    if (text.empty()) return false;
    ImVec2 min(FLT_MAX, FLT_MAX);
    for (const std::string& id : selection){
        if (const graph::Node* node = document->graph().findNode(id)){
            const graph::Vec2 at = document->displayPosition(*node);
            min = ImVec2(std::min(min.x, (float)at.x), std::min(min.y, (float)at.y));
        }
    }
    return pasteText(text, ImVec2(min.x + 40.0f, min.y + 40.0f));
}

bool GraphEditorWindow::pasteText(const std::string& text, const ImVec2& at){
    if (!document || !document->editable()) return false;
    const GraphJson clip = GraphJson::parse(text, nullptr, false);
    if (clip.is_discarded() || !clip.is_object() || !clip.contains(CLIPBOARD_KEY) || !clip["nodes"].is_array() || clip["nodes"].empty()){
        status = "the clipboard holds no graph nodes";
        return false;
    }
    if (clip.value(CLIPBOARD_KEY, std::string()) != document->graph().kind){
        status = "the clipboard's nodes are of a " + clip.value(CLIPBOARD_KEY, std::string()) + " graph";
        return false;
    }
    graph::Graph copy = document->graph();
    ImVec2 min(FLT_MAX, FLT_MAX);
    for (const GraphJson& node : clip["nodes"]){
        const graph::Node parsed = graph::nodeFromJson(node);
        min = ImVec2(std::min(min.x, (float)parsed.position.x), std::min(min.y, (float)parsed.position.y));
    }
    std::map<std::string, std::string> ids;
    std::vector<graph::GraphOp> ops;
    std::vector<std::string> pasted;
    for (const GraphJson& node : clip["nodes"]){
        graph::Node parsed = graph::nodeFromJson(node);
        if (parsed.type.empty() || ids.count(parsed.id)) continue;
        const std::string fresh = copy.allocateId("n");
        ids[parsed.id] = fresh;
        parsed.id = fresh;
        parsed.position = {parsed.position.x - min.x + at.x, parsed.position.y - min.y + at.y};
        parsed.placed = true;
        ops.push_back({"addNode", {{"node", graph::toJson(parsed)}}});
        pasted.push_back(fresh);
    }
    if (clip.contains("edges") && clip["edges"].is_array()){
        for (const GraphJson& item : clip["edges"]){
            const graph::Edge edge = graph::edgeFromJson(item);
            if (!ids.count(edge.from.node) || !ids.count(edge.to.node)) continue;
            ops.push_back({"connect", {{"from", {{"node", ids[edge.from.node]}, {"port", edge.from.port}}}, {"to", {{"node", ids[edge.to.node]}, {"port", edge.to.port}}}}});
        }
    }
    if (pasted.empty()) return false;
    const std::string firstType = graph::nodeFromJson(clip["nodes"][0]).type;
    const std::string undo = pasted.size() == 1 ? "Paste " + typeLabel(document->nodeType(firstType), firstType) : "Paste " + std::to_string(pasted.size()) + " Nodes";
    if (!apply(ops, undo)) return false;
    setSelection(pasted, true);
    return true;
}

std::string GraphEditorWindow::copyValueText(const std::string& nodeId, const std::string& param) const{
    const graph::Node* node = document ? document->graph().findNode(nodeId) : nullptr;
    if (!node) return std::string();
    auto it = node->params.find(param);
    return it == node->params.end() ? std::string() : it->dump();
}

bool GraphEditorWindow::pasteValueText(const std::string& nodeId, const std::string& param, const std::string& text){
    const GraphJson value = GraphJson::parse(text, nullptr, false);
    if (value.is_discarded()){
        status = "the clipboard holds no JSON value";
        return false;
    }
    return setParam(nodeId, param, value);
}

// ---------------------------------------------------------------- node search

std::vector<GraphEditorWindow::SearchEntry> GraphEditorWindow::searchNodes(const std::string& text, const GraphCanvas::PortRef* source) const{
    std::vector<SearchEntry> out;
    if (!document || !document->kind()) return out;
    const graph::GraphKindDesc* kind = document->kind().get();
    // the dragged port's type
    std::string sourceType;
    if (source){
        const graph::Node* node = document->graph().findNode(nodeIdOf(source->node));
        std::vector<graph::Port> ports;
        const graph::Port* port = node ? portOf(*node, source->port, source->output, ports) : nullptr;
        if (!port) return out;
        sourceType = port->type;
    }
    const std::string q = search::normalize(text);
    for (const graph::NodeTypeDesc& type : kind->nodeTypes){
        SearchEntry base;
        base.type = type.id;
        base.label = typeLabel(&type, type.id);
        base.category = type.category;
        if (source){
            // the first port of the new node that can join the dragged one
            bool found = false;
            for (const graph::Port& port : typePorts(type, defaultParams(type))){
                const bool output = port.direction == graph::PortDirection::Out;
                if (output == source->output) continue;
                const bool fits = source->output ? graph::GraphRegistry::canConnect(kind, sourceType, port.type)
                                                 : graph::GraphRegistry::canConnect(kind, port.type, sourceType);
                if (!fits) continue;
                base.port = port.id;
                base.output = output;
                found = true;
                break;
            }
            if (!found) continue;
        }
        auto scored = [&](SearchEntry entry, const std::vector<std::string>& extra) -> bool {
            if (q.empty()){
                entry.score = 0;
                out.push_back(std::move(entry));
                return true;
            }
            int best = search::fuzzyScore(q, search::normalize(entry.label));
            for (const std::string& alias : extra) best = std::max(best, search::fuzzyScore(q, search::normalize(alias)) - 10);
            if (!entry.category.empty()) best = std::max(best, search::fuzzyScore(q, search::normalize(entry.category + " " + entry.label)) - 20);
            if (best < 0) return false;
            entry.score = best;
            out.push_back(std::move(entry));
            return true;
        };
        scored(base, type.aliases);
        // each value of an enum parameter is an entry once the user types (R26)
        if (q.empty()) continue;
        for (const graph::ParamDesc& param : type.params){
            if (param.kind != "enum") continue;
            for (const std::string& value : param.enumValues){
                SearchEntry entry = base;
                entry.label = base.label + ": " + paramLabel(value);
                entry.param = param.name;
                entry.value = value;
                std::vector<std::string> aliases;
                for (const std::string& alias : type.aliases) aliases.push_back(alias + " " + value);
                aliases.push_back(value);
                scored(entry, aliases);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const SearchEntry& a, const SearchEntry& b){
        if (a.score != b.score) return a.score > b.score;
        if (a.param.empty() != b.param.empty()) return a.param.empty();
        if (a.category != b.category) return a.category < b.category;
        return a.label < b.label;
    });
    return out;
}

void GraphEditorWindow::openSearch(const ImVec2& at, const std::optional<GraphCanvas::PortRef>& source){
    if (!document || !document->editable()) return;
    searchOpen = true;
    searchFocus = true;
    searchAt = at;
    searchSource = source;
    searchSourceNode = source ? nodeIdOf(source->node) : std::string();
    searchCursor = 0;
    query.clear();
}

void GraphEditorWindow::closeSearch(){
    searchOpen = false;
    searchSource.reset();
    searchSourceNode.clear();
    query.clear();
}

std::string GraphEditorWindow::pickSearchEntry(const SearchEntry& entry){
    const graph::NodeTypeDesc* type = document ? document->nodeType(entry.type) : nullptr;
    if (!type){
        closeSearch();
        return std::string();
    }
    GraphJson params = GraphJson::object();
    if (!entry.param.empty()) params[entry.param] = entry.value;
    GraphJson full = defaultParams(*type);
    for (auto it = params.begin(); it != params.end(); ++it) full[it.key()] = it.value();
    graph::Graph copy = document->graph();
    const std::string id = copy.allocateId("n");
    const bool joins = searchSource && !entry.port.empty() && copy.findNode(searchSourceNode);
    ImVec2 at = searchAt;
    if (joins){
        // the joined port lands under the cursor
        GraphCanvas::Node shape = canvasNodeOfType(entry.type, full);
        shape.position = ImVec2(0, 0);
        const ImVec2 offset = GraphCanvas::portPosition(shape, entry.port, entry.output);
        at = ImVec2(searchAt.x - offset.x, searchAt.y - offset.y);
    }
    GraphJson node = {{"id", id}, {"type", entry.type}, {"position", positionJson(at)}};
    if (!params.empty()) node["params"] = params;
    std::vector<graph::GraphOp> ops = {{"addNode", {{"node", node}}}};
    const std::string undo = graph::describe(document->graph(), ops.back());
    if (joins){
        // the wire in the same undo step
        const std::string from = entry.output ? id : searchSourceNode;
        const std::string fromPort = entry.output ? entry.port : searchSource->port;
        const std::string to = entry.output ? searchSourceNode : id;
        const std::string toPort = entry.output ? searchSource->port : entry.port;
        bool multi = false;
        if (entry.output){
            std::vector<graph::Port> ports;
            const graph::Port* in = portOf(*copy.findNode(searchSourceNode), toPort, false, ports);
            multi = in && in->multi;
        }else{
            for (const graph::Port& port : typePorts(*type, full)) if (port.id == toPort) multi = port.multi;
        }
        const std::vector<graph::GraphOp> join = connectOps(copy, from, fromPort, to, toPort, multi);
        ops.insert(ops.end(), join.begin(), join.end());
    }
    closeSearch();
    if (!apply(ops, undo)) return std::string();
    setSelection({id}, true);
    return id;
}

// ---------------------------------------------------------------- problems

std::vector<GraphEditorWindow::Problem> GraphEditorWindow::problems() const{
    std::vector<Problem> out;
    if (!document) return out;
    if (!document->loadError().empty()) out.push_back({graph::Severity::Error, "load", document->loadError(), "", {}, ""});
    if (!document->kind()){
        out.push_back({graph::Severity::Warning, "unknown-kind", "The kind '" + document->graph().kind + "' is not registered: the graph is read-only.", "", {}, ""});
    }
    for (const graph::Issue& issue : document->issues()){
        Problem problem{issue.severity, issue.code, issue.message, issue.nodeId, {}, ""};
        if (!issue.suggestion.empty()) problem.message += " (" + issue.suggestion + ")";
        // a wire to a node or port that is gone: removing it is the one edit that fixes it
        if (issue.code == "dangling-edge" && !issue.edgeId.empty()){
            problem.fix = {{"disconnect", {{"id", issue.edgeId}}}};
            problem.fixLabel = "Remove Dangling Wire";
        }
        out.push_back(std::move(problem));
    }
    collectProblems(out);
    return out;
}

bool GraphEditorWindow::inspectProblem(const Problem& problem){
    if (!document || problem.nodeId.empty() || !document->graph().findNode(problem.nodeId)) return false;
    setSelection({problem.nodeId}, true);
    frameRequest = true;
    return true;
}

bool GraphEditorWindow::fixProblem(const Problem& problem){
    if (problem.fix.empty()) return false;
    return apply(problem.fix, problem.fixLabel);
}

// ---------------------------------------------------------------- drawing

void GraphEditorWindow::show(){
    if (!windowOpen) return;
    if (focusRequest){
        ImGui::SetNextWindowFocus();
        focusRequest = false;
    }
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(900.0f), Theme::dpi(600.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(name.c_str(), &windowOpen)){
        windowFocused = false;
        ImGui::End();
        return;
    }
    windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    syncToDocument();
    if (document) frameStarted();
    drawToolbar();
    if (!document){
        ImGui::Spacing();
        ImGui::TextDisabled("Open a graph file (double-click it in the Project window, or Open...).");
        ImGui::End();
        return;
    }
    drawProblems();
    const float previewWidth = hasPreviewPane() ? Theme::dpi(280.0f) : 0.0f;
    ImGui::BeginChild("##graph_canvas_area", ImVec2(previewWidth > 0.0f ? -previewWidth : 0.0f, 0), ImGuiChildFlags_None);
    drawCanvas();
    drawSearch();
    ImGui::EndChild();
    if (previewWidth > 0.0f){
        ImGui::SameLine();
        ImGui::BeginChild("##graph_preview", ImVec2(0, 0), ImGuiChildFlags_Borders);
        drawPreviewPane();
        ImGui::EndChild();
    }
    ImGui::End();
}

void GraphEditorWindow::drawToolbar(){
    const std::string file = document ? fs::path(document->projectPath()).filename().string() : std::string("(no graph)");
    ImGui::TextUnformatted(ICON_FA_DIAGRAM_PROJECT);
    ImGui::SameLine();
    ImGui::TextUnformatted(file.c_str());
    UIProbe::item("graph.path");
    if (document) ImGui::SetItemTooltip("%s", document->projectPath().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Open...")){
        std::string extension;
        if (auto kind = graph::GraphRegistry::get().kind(kindFilter)) extension = kind->fileExtension;
        const std::string path = extension.empty() ? FileDialogs::openFileDialog(project->getAssetsPath().string())
                                                   : FileDialogs::openFileDialogFiltered(project->getAssetsPath().string(), "Graphs", extension.substr(1).c_str());
        if (!path.empty()) openGraph(path);
    }
    UIProbe::item("graph.open");
    if (document){
        ImGui::SameLine();
        ImGui::BeginDisabled(!document->editable() || document->graph().nodes.empty());
        if (ImGui::Button(ICON_FA_TABLE_CELLS " Arrange")) arrange();
        ImGui::EndDisabled();
        UIProbe::item("graph.arrange");
        ImGui::SetItemTooltip("%s", document->editable() ? "Lay every node out left to right, by what feeds what (one undo step)"
                                                         : "The graph is read-only: its kind is not registered");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_EXPAND)) frameSelected();
        ImGui::SetItemTooltip("Frame Selected (F); all nodes when none is selected");
        UIProbe::item("graph.frame");
        drawToolbarExtras();
    }
    if (!status.empty()){
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "%s", status.c_str());
        UIProbe::item("graph.status");
    }
    ImGui::Separator();
}

void GraphEditorWindow::drawProblems(){
    const std::vector<Problem> rows = problems();
    if (rows.empty()) return;
    const std::string header = std::string(ICON_FA_TRIANGLE_EXCLAMATION " ") + std::to_string(rows.size()) + " problem(s)###graph_problems";
    if (ImGui::CollapsingHeader(header.c_str())){
        UIProbe::item("graph.problems");
        ImGui::BeginChild("##graph_problem_list", ImVec2(0, std::min(6.0f, (float)rows.size()) * ImGui::GetFrameHeightWithSpacing() + 4.0f));
        for (size_t i = 0; i < rows.size(); i++){
            const Problem& problem = rows[i];
            ImGui::PushID((int)i);
            if (!problem.fix.empty()){
                if (ImGui::SmallButton("Fix")) fixProblem(problem);
                ImGui::SetItemTooltip("%s", problem.fixLabel.c_str());
                UIProbe::item("graph.problem." + std::to_string(i) + ".fix");
                ImGui::SameLine();
            }
            const char* icon = problem.severity == graph::Severity::Error ? ICON_FA_CIRCLE_XMARK : problem.severity == graph::Severity::Warning ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_CIRCLE_INFO;
            const std::string line = std::string(icon) + " " + (problem.nodeId.empty() ? std::string() : problem.nodeId + ": ") + problem.message;
            if (ImGui::Selectable(line.c_str(), false)) inspectProblem(problem);
            UIProbe::item("graph.problem." + std::to_string(i));
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
}

void GraphEditorWindow::drawCanvas(){
    const graph::Graph& g = document->graph();
    nodeIds.clear();
    std::vector<GraphCanvas::Node> nodes;
    for (size_t i = 0; i < g.nodes.size(); i++){
        GraphCanvas::Node node = canvasNode(g.nodes[i]);
        node.id = i + 1;
        nodeIds.push_back(g.nodes[i].id);
        nodes.push_back(std::move(node));
    }
    std::vector<GraphCanvas::Link> links;
    for (size_t i = 0; i < g.edges.size(); i++){
        const graph::Edge& edge = g.edges[i];
        GraphCanvas::Link link;
        link.id = EDGE_BASE + i;
        link.from = canvasIdOf(edge.from.node);
        link.to = canvasIdOf(edge.to.node);
        if (link.from == 0 || link.to == 0) continue;
        link.fromPort = edge.from.port;
        link.toPort = edge.to.port;
        link.selected = edge.id == edgeSelection;
        for (const GraphCanvas::Port& port : nodes[link.from - 1].outputs){
            if (port.id == edge.from.port) link.color = port.color;
        }
        links.push_back(link);
    }
    std::vector<GraphCanvas::Group> groups;
    for (size_t i = 0; i < g.groups.size(); i++){
        GraphCanvas::Group group;
        group.id = i + 1;
        group.title = g.groups[i].title;
        group.position = ImVec2((float)g.groups[i].rect.x, (float)g.groups[i].rect.y);
        group.size = ImVec2((float)g.groups[i].rect.width, (float)g.groups[i].rect.height);
        for (const std::string& member : g.groups[i].nodeIds){
            if (const uint64_t id = canvasIdOf(member)) group.nodes.push_back(id);
        }
        groups.push_back(std::move(group));
    }
    if (frameRequest){
        for (GraphCanvas::Node& node : nodes) node.size = GraphCanvas::nodeSize(node);
        canvas.frameSelected(nodes);
        frameRequest = false;
    }
    const std::vector<std::string> before = selection;
    GraphCanvas::Result result = canvas.draw("##graph_canvas", nodes, links, groups);
    UIProbe::item("graph.canvas");
    lastNodes = nodes;
    lastMouseCanvas = result.mouseCanvas;
    canvasHovered = result.hovered;
    // the nodes' and ports' screen rects (UI tests drag between them)
    for (const GraphCanvas::Node& node : nodes){
        const std::string& id = nodeIds[node.id - 1];
        const ImVec2 a = canvas.toScreen(node.position);
        const ImVec2 b = canvas.toScreen(ImVec2(node.position.x + node.size.x, node.position.y + node.size.y));
        UIProbe::rect("graph.node." + id, a.x, a.y, b.x, b.y);
        for (int side = 0; side < 2; side++){
            for (const GraphCanvas::Port& port : side ? node.outputs : node.inputs){
                const ImVec2 p = canvas.toScreen(GraphCanvas::portPosition(node, port.id, side == 1));
                UIProbe::rect("graph.port." + id + (side ? ".out." : ".in.") + port.id, p.x - 4, p.y - 4, p.x + 4, p.y + 4);
            }
        }
    }

    if (result.clickedNode || result.clickedBackground || result.clickedGroup) selectionAtPress = before;
    if (result.cancelled){
        setSelection(selectionAtPress, false);
    }else if (result.selectionChanged && !result.clickedLink){
        std::vector<std::string> picked;
        for (uint64_t id : result.selectedNodes) picked.push_back(nodeIdOf(id));
        // the node clicked first, so the Inspector shows it
        const std::string clicked = nodeIdOf(result.clickedNode);
        auto at = std::find(picked.begin(), picked.end(), clicked);
        if (!clicked.empty() && at != picked.end()) std::rotate(picked.begin(), at, at + 1);
        setSelection(picked, true);
    }
    if (result.clickedLink >= EDGE_BASE && result.clickedLink - EDGE_BASE < g.edges.size()){
        setSelection({}, false);
        edgeSelection = g.edges[result.clickedLink - EDGE_BASE].id;
    }
    if (!result.moved.empty() && document->editable()){
        std::vector<std::pair<std::string, ImVec2>> positions;
        for (const auto& [id, position] : result.moved) positions.emplace_back(nodeIdOf(id), position);
        moveNodes(positions);
    }
    if (result.connectTo.node != 0){
        connect(nodeIdOf(result.connectFrom.node), result.connectFrom.port, nodeIdOf(result.connectTo.node), result.connectTo.port);
    }
    if (result.wireDropped) openSearch(result.wireDropCanvas, result.wireSource);
    if (result.contextRequested && document->editable()){
        contextAt = result.contextPosition;
        if (result.contextNode){
            ImGui::OpenPopup("##graph_node_menu");
        }else if (result.contextLink >= EDGE_BASE && result.contextLink - EDGE_BASE < g.edges.size()){
            edgeSelection = g.edges[result.contextLink - EDGE_BASE].id;
            ImGui::OpenPopup("##graph_link_menu");
        }else{
            openSearch(result.contextPosition, std::nullopt);
        }
    }
    if (ImGui::BeginPopup("##graph_node_menu")){
        if (ImGui::MenuItem("Copy")) copySelection();
        if (ImGui::MenuItem("Duplicate")) duplicateSelection();
        if (ImGui::MenuItem("Delete")) removeSelection();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##graph_link_menu")){
        if (ImGui::MenuItem("Delete Wire")) removeSelection();
        ImGui::EndPopup();
    }
}

void GraphEditorWindow::drawSearch(){
    if (!searchOpen) return;
    if (searchFocus) ImGui::OpenPopup("##graph_search");
    ImGui::SetNextWindowPos(canvas.toScreen(searchAt), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(Theme::dpi(300.0f), Theme::dpi(340.0f)), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("##graph_search")){
        // closed by a click outside it
        closeSearch();
        return;
    }
    if (searchFocus){
        ImGui::SetKeyboardFocusHere();
        searchFocus = false;
    }
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s", query.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##graph_search_query", searchSource ? "Search nodes that fit this port" : "Search nodes", buffer, sizeof(buffer))){
        query = buffer;
        searchCursor = 0;
    }
    UIProbe::item("graph.search.query");
    const GraphCanvas::PortRef* source = searchSource ? &*searchSource : nullptr;
    const std::vector<SearchEntry> entries = searchNodes(query, source);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) searchCursor = std::min(searchCursor + 1, (int)entries.size() - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) searchCursor = std::max(searchCursor - 1, 0);
    const SearchEntry* picked = nullptr;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && searchCursor >= 0 && (size_t)searchCursor < entries.size()) picked = &entries[(size_t)searchCursor];
    const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    ImGui::BeginChild("##graph_search_list");
    const size_t shown = std::min<size_t>(entries.size(), 80);
    std::string category;
    for (size_t i = 0; i < shown; i++){
        const SearchEntry& entry = entries[i];
        if (query.empty() && entry.category != category){
            category = entry.category;
            ImGui::TextDisabled("%s", category.empty() ? "Other" : category.c_str());
        }
        ImGui::PushID((int)i);
        if (ImGui::Selectable(entry.label.c_str(), (int)i == searchCursor)) picked = &entry;
        UIProbe::item("graph.search.entry." + std::to_string(i));
        if ((int)i == searchCursor && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ImGui::SetScrollHereY();
        ImGui::PopID();
    }
    if (entries.empty()) ImGui::TextDisabled("%s", source ? "No node has a port that fits." : "No node matches.");
    ImGui::EndChild();
    if (picked){
        const SearchEntry entry = *picked;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        pickSearchEntry(entry);
        return;
    }
    if (escape){
        ImGui::CloseCurrentPopup();
        closeSearch();
    }
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- Inspector

bool GraphEditorWindow::drawSelectionInspector(){
    if (!windowOpen || !document || selection.size() != 1 || !claimHolds()) return false;
    const graph::Node* node = document->graph().findNode(selection[0]);
    if (!node) return false;
    inspectorFrame = ImGui::GetFrameCount();
    ImGui::PushID("graph_inspector");
    drawNodeInspector(*node);
    ImGui::PopID();
    return true;
}

void GraphEditorWindow::drawNodeInspector(const graph::Node& node){
    const graph::NodeTypeDesc* type = document->nodeType(node.type);
    ImGui::Text(ICON_FA_DIAGRAM_PROJECT " %s", typeLabel(type, node.type).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", node.id.c_str());
    if (type && !type->category.empty()) ImGui::TextDisabled("%s", type->category.c_str());
    ImGui::Separator();
    const std::string nodeId = node.id;
    const bool editable = document->editable();
    std::vector<graph::ParamDesc> params = type ? type->params : std::vector<graph::ParamDesc>();
    // values the type does not declare (a newer build's, a typo) still show, as JSON
    for (auto it = node.params.begin(); it != node.params.end(); ++it){
        const bool declared = std::any_of(params.begin(), params.end(), [&](const graph::ParamDesc& p){ return p.name == it.key(); });
        if (!declared) params.push_back({it.key(), "any", GraphJson(), std::nullopt, std::nullopt, {}});
    }
    if (params.empty()){
        ImGui::TextDisabled("This node has no parameters.");
        return;
    }
    CommandHistory* history = project->getProjectCommandHistory();
    if (!ImGui::BeginTable("##graph_params", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.4f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.6f);
    ImGui::BeginDisabled(!editable);
    for (const graph::ParamDesc& param : params){
        const std::string label = paramLabel(param.name);
        GraphJson value = param.defaultValue;
        if (auto it = node.params.find(param.name); it != node.params.end()) value = *it;
        ImGui::PushID(param.name.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        UIProbe::item("graph.inspector." + param.name + ".label");
        if (ImGui::BeginPopupContextItem("##value_menu")){
            if (ImGui::MenuItem("Copy Value")) ImGui::SetClipboardText(value.dump().c_str());
            if (ImGui::MenuItem("Paste Value", nullptr, false, editable && ImGui::GetClipboardText())) pasteValueText(nodeId, param.name, ImGui::GetClipboardText());
            ImGui::EndPopup();
        }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const std::string kind = param.kind;
        // a field window's phase (FieldEdit.h): its session in the project's history
        auto phase = [&](ui::EditPhase p, const GraphJson& next){
            if (p == ui::EditPhase::Begin) ui::beginFieldEdit(history, label.c_str());
            if (p == ui::EditPhase::Changing || p == ui::EditPhase::Applied) setParam(nodeId, param.name, next);
        };
        if (kind == "float" || kind == "int"){
            const bool whole = kind == "int";
            double number = value.is_number() ? value.get<double>() : 0.0;
            const double lo = param.min.value_or(-DBL_MAX), hi = param.max.value_or(DBL_MAX);
            const float speed = param.min && param.max ? (float)((hi - lo) / 200.0) : 0.01f;
            bool changed;
            if (whole){
                int v = (int)number;
                changed = ImGui::DragInt("##v", &v, std::max(0.1f, speed), param.min ? (int)lo : 0, param.max ? (int)hi : 0, "%d", ImGuiSliderFlags_AlwaysClamp);
                number = v;
            }else{
                float v = (float)number;
                changed = ImGui::DragFloat("##v", &v, speed, param.min ? (float)lo : 0.0f, param.max ? (float)hi : 0.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
                number = v;
            }
            UIProbe::item("graph.inspector." + param.name);
            if (changed) setParam(nodeId, param.name, whole ? GraphJson((int)number) : GraphJson(std::round(number * 1e6) / 1e6));
        }else if (kind == "bool"){
            bool v = value.is_boolean() ? value.get<bool>() : (value.is_number() && value.get<double>() != 0.0);
            if (ImGui::Checkbox("##v", &v)) setParam(nodeId, param.name, v);
            UIProbe::item("graph.inspector." + param.name);
        }else if (kind == "enum"){
            const std::string current = value.is_string() ? value.get<std::string>() : std::string();
            if (ImGui::BeginCombo("##v", paramLabel(current).c_str())){
                for (const std::string& option : param.enumValues){
                    if (ImGui::Selectable(paramLabel(option).c_str(), option == current)) setParam(nodeId, param.name, option);
                }
                ImGui::EndCombo();
            }
            UIProbe::item("graph.inspector." + param.name);
        }else if (kind == "vector2" || kind == "vector3"){
            const int count = kind == "vector2" ? 2 : 3;
            float v[3] = {0.0f, 0.0f, 0.0f};
            for (int i = 0; i < count && value.is_array() && i < (int)value.size(); i++) v[i] = value[(size_t)i].is_number() ? value[(size_t)i].get<float>() : 0.0f;
            const bool changed = count == 2 ? ImGui::DragFloat2("##v", v, 0.01f) : ImGui::DragFloat3("##v", v, 0.01f);
            if (changed){
                GraphJson next = GraphJson::array();
                for (int i = 0; i < count; i++) next.push_back(std::round((double)v[i] * 1e6) / 1e6);
                setParam(nodeId, param.name, next);
            }
        }else if (kind == "color"){
            Vector4 color(1.0f, 1.0f, 1.0f, 1.0f);
            graphvalues::colorFrom(value, color);
            ui::ColorFieldOptions options;
            options.label = label.c_str();
            options.linearStorage = false;   // stored as shown (GraphRegistry.h)
            const ui::EditPhase p = ui::colorField("##v", color, options);
            UIProbe::item("graph.inspector." + param.name);
            phase(p, graphvalues::colorJson(color, value));
        }else if (kind == "gradient"){
            Gradient gradient = Gradient::twoColors(Vector3(0, 0, 0), Vector3(1, 1, 1));
            graphvalues::gradientFrom(value, gradient);
            ui::GradientFieldOptions options;
            options.label = label.c_str();
            options.maxKeys = graphvalues::RAMP_KEYS_MAX;
            const ui::EditPhase p = ui::gradientField("##v", gradient, options);
            UIProbe::item("graph.inspector." + param.name);
            if (p != ui::EditPhase::None){
                GraphJson next;
                std::string error;
                if (graphvalues::gradientJson(gradient, value, next, error)) phase(p, next);
                else status = error;
            }
        }else if (kind == "curve"){
            Curve curve;
            graphvalues::curveFrom(value, curve);
            ui::CurveFieldOptions options;
            options.label = label.c_str();
            for (const CurveKey& key : curve.keys){
                options.range.v0 = std::min(options.range.v0, key.value);
                options.range.v1 = std::max(options.range.v1, key.value);
            }
            const ui::EditPhase p = ui::curveField("##v", curve, options);
            UIProbe::item("graph.inspector." + param.name);
            if (p != ui::EditPhase::None){
                GraphJson next;
                std::string error;
                if (graphvalues::curveJson(curve, value, next, error)) phase(p, next);
                else status = error;
            }
        }else{
            // string, asset and any: the text (a string), or the JSON (the rest), applied on Enter
            const bool text = kind == "string" && (value.is_string() || value.is_null());
            const std::string shown = text ? (value.is_string() ? value.get<std::string>() : std::string()) : kind == "asset" && value.is_object() ? value.value("path", std::string()) : value.dump();
            char buffer[1024];
            std::snprintf(buffer, sizeof(buffer), "%s", shown.c_str());
            if (ImGui::InputText("##v", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)){
                if (text){
                    setParam(nodeId, param.name, std::string(buffer));
                }else if (kind == "asset"){
                    GraphJson next = value.is_object() ? value : GraphJson::object();
                    next["path"] = std::string(buffer);
                    setParam(nodeId, param.name, next);
                }else{
                    pasteValueText(nodeId, param.name, buffer);
                }
            }
            UIProbe::item("graph.inspector." + param.name);
            if (!text && kind != "asset") ImGui::SetItemTooltip("JSON; Enter applies it");
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::EndTable();
}
