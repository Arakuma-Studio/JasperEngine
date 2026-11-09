// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "NoiseGraphDocument.h"

#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/GraphEditCmd.h"
#include "graph/GraphRegistry.h"
#include "noise/NoiseGraphRuntime.h"
#include "noise/NoiseNodes.h"
#include "util/ProjectChecks.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <iterator>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    // A canvas id of a graph's id: 0 is the canvas's "none"
    uint64_t idOf(const std::string& kind, const std::string& a, const std::string& b = std::string()) {
        uint64_t hash = 0xcbf29ce484222325ull;
        const auto mix = [&hash](const std::string& text) {
            for (unsigned char c : text) {
                hash ^= c;
                hash *= 0x100000001b3ull;
            }
            hash ^= 0xff;
            hash *= 0x100000001b3ull;
        };
        mix(kind);
        mix(a);
        mix(b);
        return hash == 0 ? 1 : hash;
    }

    std::string lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    std::string number(float value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(value));
        return text;
    }

    std::shared_ptr<const graph::GraphKindDesc> noiseKind() {
        noise::registerNoiseKind();
        return graph::GraphRegistry::get().kind(noise::kNoiseKindId);
    }

    graph::Json vec2Json(const graph::Vec2& v) {
        return graph::Json::array({v.x, v.y});
    }

}

NoiseGraphDocument::NoiseGraphDocument(Project* project) : project(project) {
}

bool NoiseGraphDocument::open(const fs::path& file) {
    close();
    filePath = file;
    stamp();
    std::string read;
    if (!ProjectChecks::readFile(file, read)) {
        fileError = "Could not read " + file.filename().string() + ".";
        markGraphChanged();
        return false;
    }
    adopt(read);
    return fileError.empty();
}

void NoiseGraphDocument::close() {
    service.cancel();
    filePath.clear();
    text.clear();
    working = graph::Graph();
    fileError.clear();
    lastStatus.clear();
    drag.reset();
    nodeSelection.clear();
    wireSelection.clear();
    previews.clear();
    lastImage = NoisePreviewImage();
    pendingUploads.clear();
    previewed = 0;
    requested = 0;
    markGraphChanged();
}

std::string NoiseGraphDocument::displayName() const {
    return filePath.stem().string();
}

void NoiseGraphDocument::stamp() {
    std::error_code error;
    stampExists = fs::exists(filePath, error);
    if (!stampExists) return;
    stampTime = fs::last_write_time(filePath, error);
    stampSize = fs::file_size(filePath, error);
}

void NoiseGraphDocument::adopt(const std::string& newText) {
    text = newText;
    graph::LoadReport report;
    graph::Graph loaded = graph::load(text, report);
    if (!report.ok) {
        fileError = filePath.filename().string() + " does not parse: " + report.error;
    } else if (loaded.kind != noise::kNoiseKindId) {
        fileError = filePath.filename().string() + " is a '" + loaded.kind + "' graph, not a Noise Graph.";
    } else {
        fileError.clear();
        working = std::move(loaded);
        pruneSelection();
    }
    drag.reset();
    markGraphChanged();
}

bool NoiseGraphDocument::refresh() {
    if (!isOpen()) return false;
    std::error_code error;
    const bool exists = fs::exists(filePath, error);
    if (!exists) {
        if (!stampExists) return false;
        stampExists = false;
        fileError = filePath.filename().string() + " is gone.";
        drag.reset();
        markGraphChanged();
        return true;
    }
    const fs::file_time_type time = fs::last_write_time(filePath, error);
    const uintmax_t size = fs::file_size(filePath, error);
    if (stampExists && time == stampTime && size == stampSize) return false;
    stampExists = true;
    stampTime = time;
    stampSize = size;
    std::string read;
    if (!ProjectChecks::readFile(filePath, read)) return false;
    if (read == text && fileError.empty()) return false;
    adopt(read);
    return true;
}

void NoiseGraphDocument::markGraphChanged() {
    currentRevision++;
    viewsDirty = true;
    recomputeIssues();
    rebuildPins();
}

void NoiseGraphDocument::recomputeIssues() {
    currentIssues.clear();
    deadNodes.clear();
    foldedNodes.clear();
    if (!isOpen() || !fileError.empty()) return;
    noise::CompileOptions options;
    options.maxHeight = maxHeight();
    const noise::CompileResult compiled = noise::compile(working, options);
    currentIssues = compiled.issues;
    deadNodes.insert(compiled.deadNodes.begin(), compiled.deadNodes.end());
    foldedNodes.insert(compiled.foldedNodes.begin(), compiled.foldedNodes.end());
}

void NoiseGraphDocument::rebuildPins() {
    pins.clear();
    nodeIds.clear();
    edgeIds.clear();
    for (const graph::Node& node : working.nodes) {
        nodeIds[idOf("node", node.id)] = node.id;
        for (const graph::Port& port : graph::portsOf(working, node)) {
            PinRef ref;
            ref.node = node.id;
            ref.port = port.id;
            ref.direction = port.direction;
            ref.type = port.type;
            ref.multi = port.multi;
            pins[idOf("pin", node.id, port.id)] = std::move(ref);
        }
    }
    for (const graph::Edge& edge : working.edges) edgeIds[idOf("edge", edge.id)] = edge.id;
}

void NoiseGraphDocument::pruneSelection() {
    for (auto it = nodeSelection.begin(); it != nodeSelection.end();) {
        it = working.findNode(*it) ? std::next(it) : nodeSelection.erase(it);
    }
    for (auto it = wireSelection.begin(); it != wireSelection.end();) {
        it = working.findEdge(*it) ? std::next(it) : wireSelection.erase(it);
    }
}

bool NoiseGraphDocument::refuse(const std::string& why) {
    lastStatus = why;
    return false;
}

bool NoiseGraphDocument::ensureFileUnchanged() {
    if (!canEdit()) return refuse(isOpen() ? fileError : "No graph is open.");
    std::string onDisk;
    if (!ProjectChecks::readFile(filePath, onDisk)) return refuse("Could not read " + filePath.filename().string() + ".");
    if (onDisk != text) {
        stamp();
        adopt(onDisk);
        return refuse(filePath.filename().string() + " changed on disk: it was reloaded, make the edit again.");
    }
    return true;
}

bool NoiseGraphDocument::commit(graph::Graph after, const std::string& name) {
    if (!ensureFileUnchanged()) return false;
    std::string afterText = graph::save(after);
    if (afterText == text) {
        lastStatus.clear();
        return true;
    }
    if (!project->getProjectCommandHistory()->addCommandNoMerge(new GraphEditCmd(filePath, text, afterText, name))) {
        return refuse("Could not write " + filePath.filename().string() + ".");
    }
    text = std::move(afterText);
    working = std::move(after);
    stamp();
    pruneSelection();
    lastStatus.clear();
    markGraphChanged();
    return true;
}

std::string NoiseGraphDocument::addNode(const std::string& typeId, const graph::Vec2& position) {
    return addFromSearch(typeId, position);
}

std::string NoiseGraphDocument::addFromSearch(const std::string& typeId, const graph::Vec2& position, uint64_t connectPin) {
    if (!canEdit()) {
        refuse(isOpen() ? fileError : "No graph is open.");
        return std::string();
    }
    graph::Graph after = working;
    // no id: the op allocates one and fills in the type's parameter defaults
    const graph::GraphOp add{"addNode", {{"node", {{"type", typeId}, {"position", vec2Json(position)}}}}};
    const std::string name = graph::describe(after, add);
    std::string id;
    try {
        id = graph::apply(after, add).args.value("id", std::string());
    } catch (const std::exception& e) {
        refuse(e.what());
        return std::string();
    }
    if (connectPin != 0) {
        const std::optional<PinRef> pin = pinRef(connectPin);
        if (pin) {
            // R59: the first compatible port of the new node, on the side the wire was dragged toward
            std::shared_ptr<const graph::GraphKindDesc> kind = noiseKind();
            const graph::Node* added = after.findNode(id);
            for (const graph::Port& port : graph::portsOf(after, *added)) {
                if (port.direction == pin->direction) continue;
                const bool fits = pin->direction == graph::PortDirection::Out ? graph::GraphRegistry::canConnect(kind.get(), pin->type, port.type)
                                                                              : graph::GraphRegistry::canConnect(kind.get(), port.type, pin->type);
                if (!fits) continue;
                PinRef other;
                other.node = id;
                other.port = port.id;
                other.direction = port.direction;
                other.type = port.type;
                other.multi = port.multi;
                if (pin->direction == graph::PortDirection::Out) connectInto(after, *pin, other);
                else connectInto(after, other, *pin);
                break;
            }
        }
    }
    if (!commit(std::move(after), name)) return std::string();
    return id;
}

std::optional<NoiseGraphDocument::PinRef> NoiseGraphDocument::pinRef(uint64_t pin) const {
    auto it = pins.find(pin);
    if (it == pins.end()) return std::nullopt;
    return it->second;
}

bool NoiseGraphDocument::wouldCycle(const std::string& fromNode, const std::string& toNode) const {
    // walking upstream from the output's node: reaching the input's node closes a loop
    std::vector<std::string> stack{fromNode};
    std::set<std::string> seen;
    while (!stack.empty()) {
        const std::string current = std::move(stack.back());
        stack.pop_back();
        if (current == toNode) return true;
        if (!seen.insert(current).second) continue;
        for (const graph::Edge& edge : working.edges) {
            if (edge.to.node == current) stack.push_back(edge.from.node);
        }
    }
    return false;
}

bool NoiseGraphDocument::canConnectPins(uint64_t outputPin, uint64_t inputPin) const {
    const std::optional<PinRef> output = pinRef(outputPin);
    const std::optional<PinRef> input = pinRef(inputPin);
    if (!output || !input) return false;
    if (output->direction != graph::PortDirection::Out || input->direction != graph::PortDirection::In) return false;
    if (output->node == input->node) return false;
    std::shared_ptr<const graph::GraphKindDesc> kind = noiseKind();
    if (!graph::GraphRegistry::canConnect(kind.get(), output->type, input->type)) return false;
    return !wouldCycle(output->node, input->node);
}

void NoiseGraphDocument::connectInto(graph::Graph& target, const PinRef& output, const PinRef& input) const {
    if (!input.multi) {
        std::vector<std::string> replaced;
        for (const graph::Edge& edge : target.edges) {
            if (edge.to.node == input.node && edge.to.port == input.port) replaced.push_back(edge.id);
        }
        for (const std::string& id : replaced) graph::apply(target, graph::GraphOp{"disconnect", {{"id", id}}});
    }
    graph::apply(target, graph::GraphOp{"connect", {{"from", {{"node", output.node}, {"port", output.port}}},
                                                    {"to", {{"node", input.node}, {"port", input.port}}}}});
}

bool NoiseGraphDocument::connectPins(uint64_t outputPin, uint64_t inputPin) {
    if (!canEdit()) return refuse(isOpen() ? fileError : "No graph is open.");
    if (!canConnectPins(outputPin, inputPin)) return refuse("Those pins cannot connect.");
    const PinRef output = pins.at(outputPin);
    const PinRef input = pins.at(inputPin);
    graph::Graph after = working;
    // R57's "Connect Perlin → Remap": graph::describe names the source by its port, and here nearly
    // every port is called Out
    std::shared_ptr<const graph::GraphKindDesc> kind = noiseKind();
    const auto title = [&](const std::string& nodeId) {
        const graph::Node* node = working.findNode(nodeId);
        const graph::NodeTypeDesc* type = node ? graph::GraphRegistry::nodeType(*kind, node->type) : nullptr;
        return type && !type->displayName.empty() ? type->displayName : nodeId;
    };
    const std::string name = "Connect " + title(output.node) + " \xE2\x86\x92 " + title(input.node);
    try {
        connectInto(after, output, input);
    } catch (const std::exception& e) {
        return refuse(e.what());
    }
    return commit(std::move(after), name);
}

bool NoiseGraphDocument::removeWire(const std::string& edgeId) {
    if (!canEdit()) return refuse(isOpen() ? fileError : "No graph is open.");
    graph::Graph after = working;
    const graph::GraphOp op{"disconnect", {{"id", edgeId}}};
    const std::string name = graph::describe(after, op);
    try {
        graph::apply(after, op);
    } catch (const std::exception& e) {
        return refuse(e.what());
    }
    return commit(std::move(after), name);
}

bool NoiseGraphDocument::moveNodes(const std::vector<std::pair<std::string, graph::Vec2>>& positions) {
    if (!canEdit()) return refuse(isOpen() ? fileError : "No graph is open.");
    if (positions.empty()) return true;
    graph::Json moved = graph::Json::object();
    for (const auto& [id, position] : positions) moved[id] = vec2Json(position);
    graph::Graph after = working;
    const graph::GraphOp op{"moveNodes", {{"positions", moved}}};
    const std::string name = graph::describe(after, op);
    try {
        graph::apply(after, op);
    } catch (const std::exception& e) {
        return refuse(e.what());
    }
    return commit(std::move(after), name);
}

bool NoiseGraphDocument::setParam(const std::string& nodeId, const std::string& name, const graph::Json& value) {
    if (!canEdit()) return refuse(isOpen() ? fileError : "No graph is open.");
    graph::Graph after = working;
    const graph::GraphOp op{"setParam", {{"node", nodeId}, {"name", name}, {"value", value}}};
    const std::string undoName = graph::describe(after, op);
    try {
        graph::apply(after, op);
    } catch (const std::exception& e) {
        return refuse(e.what());
    }
    return commit(std::move(after), undoName);
}

bool NoiseGraphDocument::isWorldSpace() const {
    auto meta = working.metadata.find("noise");
    return meta != working.metadata.end() && meta->is_object() && meta->value("space", std::string("terrain")) == "world";
}

bool NoiseGraphDocument::setSpace(bool world) {
    if (!canEdit()) return refuse(isOpen() ? fileError : "No graph is open.");
    graph::Graph after = working;
    if (!after.metadata.is_object()) after.metadata = graph::Json::object();
    if (!after.metadata.contains("noise") || !after.metadata["noise"].is_object()) after.metadata["noise"] = graph::Json::object();
    after.metadata["noise"]["space"] = world ? "world" : "terrain";
    return commit(std::move(after), world ? "Set Space to World Units" : "Set Space to Terrain Size");
}

void NoiseGraphDocument::beginParamDrag(const std::string& nodeId, const std::string& name) {
    if (!canEdit() || drag) return;
    const graph::Node* node = working.findNode(nodeId);
    if (!node) return;
    ParamDrag next;
    next.node = nodeId;
    next.name = name;
    auto current = node->params.find(name);
    next.hadValue = current != node->params.end();
    if (next.hadValue) next.original = *current;
    drag = std::move(next);
}

void NoiseGraphDocument::updateParamDrag(const graph::Json& value) {
    if (!drag) return;
    graph::Node* node = working.findNode(drag->node);
    if (!node) return;
    node->params[drag->name] = value;
    drag->changed = true;
    // the file has not moved: only the previews and the view see this
    currentRevision++;
    viewsDirty = true;
    recomputeIssues();
}

bool NoiseGraphDocument::commitParamDrag() {
    if (!drag) return false;
    const ParamDrag finished = std::move(*drag);
    drag.reset();
    if (!finished.changed) return true;
    const graph::Node* node = working.findNode(finished.node);
    if (!node) return refuse("That node is gone.");
    const graph::Json value = node->params.value(finished.name, graph::Json());
    // the step is the whole drag: from the value before it
    graph::Node* edited = working.findNode(finished.node);
    if (finished.hadValue) edited->params[finished.name] = finished.original;
    else edited->params.erase(finished.name);
    return setParam(finished.node, finished.name, value);
}

void NoiseGraphDocument::cancelParamDrag() {
    if (!drag) return;
    const ParamDrag cancelled = std::move(*drag);
    drag.reset();
    if (!cancelled.changed) return;
    if (graph::Node* node = working.findNode(cancelled.node)) {
        if (cancelled.hadValue) node->params[cancelled.name] = cancelled.original;
        else node->params.erase(cancelled.name);
    }
    currentRevision++;
    viewsDirty = true;
    recomputeIssues();
}

void NoiseGraphDocument::setSelection(std::set<std::string> newNodes, std::set<std::string> newWires) {
    if (newNodes == nodeSelection && newWires == wireSelection) return;
    nodeSelection = std::move(newNodes);
    wireSelection = std::move(newWires);
    viewsDirty = true;
}

bool NoiseGraphDocument::deleteSelection() {
    if (!canDelete()) return refuse(canEdit() ? "Nothing is selected." : fileError);
    graph::Graph after = working;
    std::vector<graph::GraphOp> ops;
    // wires first: a node's removal takes its own with it
    for (const std::string& id : wireSelection) {
        const graph::Edge* edge = after.findEdge(id);
        if (edge && !nodeSelection.count(edge->from.node) && !nodeSelection.count(edge->to.node)) ops.push_back({"disconnect", {{"id", id}}});
    }
    size_t removedNodes = 0;
    for (const std::string& id : nodeSelection) {
        ops.push_back({"removeNode", {{"id", id}}});
        removedNodes++;
    }
    if (ops.empty()) return true;
    std::string name = graph::describe(after, ops.front());
    if (ops.size() > 1) {
        name = removedNodes == ops.size() ? "Delete " + std::to_string(removedNodes) + " Nodes"
             : removedNodes == 0 ? "Delete " + std::to_string(ops.size()) + " Wires"
             : "Delete " + std::to_string(ops.size()) + " Items";
    }
    try {
        for (const graph::GraphOp& op : ops) graph::apply(after, op);
    } catch (const std::exception& e) {
        return refuse(e.what());
    }
    nodeSelection.clear();
    wireSelection.clear();
    return commit(std::move(after), name);
}

bool NoiseGraphDocument::copySelection() {
    if (!canCopy()) return false;
    clipboardNodes.clear();
    clipboardEdges.clear();
    for (const graph::Node& node : working.nodes) {
        if (nodeSelection.count(node.id)) clipboardNodes.push_back(node);
    }
    for (const graph::Edge& edge : working.edges) {
        if (nodeSelection.count(edge.from.node) && nodeSelection.count(edge.to.node)) clipboardEdges.push_back(edge);
    }
    return true;
}

std::string NoiseGraphDocument::pasteInto(graph::Graph& target, const graph::Vec2& offset, std::set<std::string>* added) const {
    std::map<std::string, std::string> renamed;
    for (const graph::Node& source : clipboardNodes) {
        graph::Node node = source;
        node.id = target.allocateId("n");
        node.position.x += offset.x;
        node.position.y += offset.y;
        renamed[source.id] = node.id;
        graph::apply(target, graph::GraphOp{"addNode", {{"node", graph::toJson(node)}}});
        if (added) added->insert(node.id);
    }
    for (const graph::Edge& source : clipboardEdges) {
        graph::apply(target, graph::GraphOp{"connect", {{"from", {{"node", renamed[source.from.node]}, {"port", source.from.port}}},
                                                        {"to", {{"node", renamed[source.to.node]}, {"port", source.to.port}}}}});
    }
    return std::to_string(clipboardNodes.size());
}

bool NoiseGraphDocument::paste(const graph::Vec2& position) {
    if (!canPaste()) return refuse(canEdit() ? "Nothing to paste." : fileError);
    graph::Vec2 corner = clipboardNodes.front().position;
    for (const graph::Node& node : clipboardNodes) {
        corner.x = std::min(corner.x, node.position.x);
        corner.y = std::min(corner.y, node.position.y);
    }
    graph::Graph after = working;
    std::set<std::string> added;
    std::string count;
    try {
        count = pasteInto(after, graph::Vec2{position.x - corner.x, position.y - corner.y}, &added);
    } catch (const std::exception& e) {
        return refuse(e.what());
    }
    const bool ok = commit(std::move(after), clipboardNodes.size() == 1 ? "Paste Node" : "Paste " + count + " Nodes");
    if (ok) setSelection(std::move(added), {});
    return ok;
}

bool NoiseGraphDocument::duplicateSelection() {
    if (!canEdit() || nodeSelection.empty()) return refuse(canEdit() ? "Nothing is selected." : fileError);
    const std::vector<graph::Node> keepNodes = clipboardNodes;
    const std::vector<graph::Edge> keepEdges = clipboardEdges;
    copySelection();
    graph::Graph after = working;
    std::set<std::string> added;
    std::string count;
    bool ok = false;
    try {
        count = pasteInto(after, graph::Vec2{40.0, 40.0}, &added);
        ok = commit(std::move(after), clipboardNodes.size() == 1 ? "Duplicate Node" : "Duplicate " + count + " Nodes");
    } catch (const std::exception& e) {
        refuse(e.what());
    }
    // Duplicate does not disturb what Copy took
    clipboardNodes = keepNodes;
    clipboardEdges = keepEdges;
    if (ok) setSelection(std::move(added), {});
    return ok;
}

std::string NoiseGraphDocument::nodeError(const std::string& nodeId) const {
    const graph::Issue* first = nullptr;
    for (const graph::Issue& issue : currentIssues) {
        if (issue.nodeId != nodeId || issue.severity == graph::Severity::Info) continue;
        if (issue.severity == graph::Severity::Error) return issue.message;
        if (!first) first = &issue;
    }
    return first ? first->message : std::string();
}

std::vector<graph::Issue> NoiseGraphDocument::issuesWithoutNode() const {
    std::vector<graph::Issue> out;
    for (const graph::Issue& issue : currentIssues) {
        if (issue.nodeId.empty()) out.push_back(issue);
    }
    return out;
}

bool NoiseGraphDocument::hasErrors() const {
    if (!fileError.empty()) return true;
    for (const graph::Issue& issue : currentIssues) {
        if (issue.severity == graph::Severity::Error) return true;
    }
    return false;
}

std::string NoiseGraphDocument::firstError() const {
    if (!fileError.empty()) return fileError;
    for (const graph::Issue& issue : currentIssues) {
        if (issue.severity == graph::Severity::Error) return issue.message;
    }
    return std::string();
}

std::vector<NoiseGraphSearchEntry> NoiseGraphDocument::search(const std::string& query, uint64_t connectPin) const {
    std::vector<NoiseGraphSearchEntry> out;
    std::shared_ptr<const graph::GraphKindDesc> kind = noiseKind();
    const std::optional<PinRef> pin = pinRef(connectPin);
    const std::string needle = lower(query);
    for (const graph::NodeTypeDesc& type : kind->nodeTypes) {
        if (!needle.empty() && lower(type.displayName).find(needle) == std::string::npos && lower(type.category).find(needle) == std::string::npos &&
            lower(type.id).find(needle) == std::string::npos) {
            continue;
        }
        NoiseGraphSearchEntry entry;
        entry.typeId = type.id;
        entry.name = type.displayName.empty() ? type.id : type.displayName;
        entry.category = type.category;
        if (pin) {
            for (const graph::Port& port : type.ports) {
                if (port.direction == pin->direction) continue;
                const bool fits = pin->direction == graph::PortDirection::Out ? graph::GraphRegistry::canConnect(kind.get(), pin->type, port.type)
                                                                              : graph::GraphRegistry::canConnect(kind.get(), port.type, pin->type);
                if (fits) {
                    entry.port = port.id;
                    break;
                }
            }
            if (entry.port.empty()) continue;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

void NoiseGraphDocument::rebuildViews() {
    viewsDirty = false;
    nodes.clear();
    wires.clear();
    std::shared_ptr<const graph::GraphKindDesc> kind = noiseKind();
    const auto colorOf = [&kind](const std::string& type) {
        const graph::PortTypeDesc* desc = graph::GraphRegistry::portType(*kind, type);
        return desc ? desc->color : 0xffffffffu;
    };
    std::set<std::string> connected;
    for (const graph::Edge& edge : working.edges) {
        connected.insert(edge.from.node + "\n" + edge.from.port);
        connected.insert(edge.to.node + "\n" + edge.to.port);
    }
    for (const graph::Node& node : working.nodes) {
        NoiseGraphNodeView view;
        view.id = idOf("node", node.id);
        view.nodeId = node.id;
        view.type = node.type;
        const graph::NodeTypeDesc* type = graph::GraphRegistry::nodeType(*kind, node.type);
        view.title = type && !type->displayName.empty() ? type->displayName : node.type;
        view.position = node.position;
        view.selected = nodeSelection.count(node.id) > 0;
        view.dead = isDead(node.id);
        view.folded = isFolded(node.id);
        view.errorText = nodeError(node.id);
        for (const graph::Port& port : graph::portsOf(working, node)) {
            NoiseGraphPinView pin;
            pin.id = idOf("pin", node.id, port.id);
            pin.port = port.id;
            pin.name = port.name.empty() ? port.id : port.name;
            pin.type = port.type;
            pin.color = colorOf(port.type);
            pin.connected = connected.count(node.id + "\n" + port.id) > 0;
            for (const graph::Issue& issue : currentIssues) {
                if (issue.nodeId == node.id && issue.portId == port.id && issue.severity == graph::Severity::Error) pin.error = true;
            }
            (port.direction == graph::PortDirection::In ? view.inputs : view.outputs).push_back(std::move(pin));
        }
        if (const NoiseNodePreview* preview = nodePreview(node.id)) {
            view.footer.push_back(rangeText(preview->observed, preview->analytic));
            char time[32];
            std::snprintf(time, sizeof(time), "%.2f ms", preview->ms);
            view.cost = time;
        }
        nodes.push_back(std::move(view));
    }
    for (const graph::Edge& edge : working.edges) {
        NoiseGraphWireView wire;
        wire.id = idOf("edge", edge.id);
        wire.edgeId = edge.id;
        wire.fromNode = idOf("node", edge.from.node);
        wire.toNode = idOf("node", edge.to.node);
        wire.fromPin = idOf("pin", edge.from.node, edge.from.port);
        wire.toPin = idOf("pin", edge.to.node, edge.to.port);
        wire.fromPort = edge.from.port;
        wire.toPort = edge.to.port;
        auto from = pins.find(wire.fromPin);
        wire.color = from != pins.end() ? colorOf(from->second.type) : 0xc8c8c8ffu;
        wire.selected = wireSelection.count(edge.id) > 0;
        wires.push_back(std::move(wire));
    }
}

const std::vector<NoiseGraphNodeView>& NoiseGraphDocument::nodeViews() {
    if (viewsDirty) rebuildViews();
    return nodes;
}

const std::vector<NoiseGraphWireView>& NoiseGraphDocument::wireViews() {
    if (viewsDirty) rebuildViews();
    return wires;
}

std::string NoiseGraphDocument::nodeOfId(uint64_t id) const {
    auto it = nodeIds.find(id);
    return it != nodeIds.end() ? it->second : std::string();
}

std::string NoiseGraphDocument::edgeOfId(uint64_t id) const {
    auto it = edgeIds.find(id);
    return it != edgeIds.end() ? it->second : std::string();
}

uint64_t NoiseGraphDocument::pinOf(const std::string& nodeId, const std::string& port) const {
    const uint64_t id = idOf("pin", nodeId, port);
    return pins.count(id) ? id : 0;
}

float NoiseGraphDocument::terrainSize() const {
    auto preset = working.metadata.find("preset");
    return preset != working.metadata.end() && preset->is_object() ? preset->value("terrain_size", 1000.0f) : 1000.0f;
}

float NoiseGraphDocument::maxHeight() const {
    auto preset = working.metadata.find("preset");
    return preset != working.metadata.end() && preset->is_object() ? preset->value("max_height", 100.0f) : 100.0f;
}

void NoiseGraphDocument::setPreviewSettings(const NoisePreviewSettings& next) {
    if (next == settings) return;
    settings = next;
    currentRevision++;
}

bool NoiseGraphDocument::update(bool windowOpen) {
    if (!windowOpen || !isOpen() || !fileError.empty()) {
        if (requested != 0 || service.busy()) service.cancel();
        requested = 0;
        if (!windowOpen && (!previews.empty() || lastImage.size != 0)) {
            previews.clear();
            lastImage = NoisePreviewImage();
            pendingUploads.clear();
            previewed = 0;
            viewsDirty = true;
        }
        return false;
    }
    bool fresh = false;
    NoisePreviewResult result;
    if (service.poll(result)) fresh = acceptPreview(std::move(result));
    if (requested != currentRevision) {
        NoisePreviewRequest request;
        request.graph = working;
        request.revision = currentRevision;
        request.seed = settings.seed;
        request.thumbnailSize = thumbnailSize;
        request.imageSize = settings.imageSize;
        request.terrainSize = terrainSize();
        request.maxHeight = maxHeight();
        request.variables = settings.variables;
        requested = currentRevision;
        service.request(std::move(request));
    }
    return fresh;
}

bool NoiseGraphDocument::acceptPreview(NoisePreviewResult&& result) {
    if (result.revision != currentRevision) {
        dropped++;
        return false;
    }
    previewed = result.revision;
    if (!result.ok) return false;
    pendingUploads.clear();
    previews.clear();
    for (NoiseNodePreview& node : result.nodes) {
        pendingUploads.push_back(node.nodeId);
        std::string id = node.nodeId;
        previews[std::move(id)] = std::move(node);
    }
    thumbnailSize = result.thumbnailSize;
    lastImage = std::move(result.image);
    viewsDirty = true;
    return true;
}

const NoiseNodePreview* NoiseGraphDocument::nodePreview(const std::string& nodeId) const {
    auto it = previews.find(nodeId);
    return it != previews.end() ? &it->second : nullptr;
}

std::vector<std::string> NoiseGraphDocument::takeThumbnailUploads(size_t limit) {
    const size_t count = std::min(limit, pendingUploads.size());
    std::vector<std::string> out(pendingUploads.begin(), pendingUploads.begin() + static_cast<std::ptrdiff_t>(count));
    pendingUploads.erase(pendingUploads.begin(), pendingUploads.begin() + static_cast<std::ptrdiff_t>(count));
    return out;
}

std::string NoiseGraphDocument::rangeText(const noise::Interval& observed, const noise::Interval& analytic) {
    // symmetric bounds read as one number, the way R56 shows them
    const bool symmetric = analytic.min == -analytic.max;
    const std::string bound = symmetric ? "\xE2\x89\xA4 \xC2\xB1" + number(analytic.max) : "in " + number(analytic.min) + " ... " + number(analytic.max);
    return number(observed.min) + " ... " + number(observed.max) + "  " + bound;
}
