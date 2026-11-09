// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// All of the Noise Graph window's logic, with no ImGui, so it unit-tests headless (T2 R55-R59, N10). The file
// is what is true: every gesture writes it through one GraphEditCmd of the project history, named by
// graph::describe, and the document follows the file back (an undo, MCP, a text editor). A file that stops
// parsing shows its error and refuses edits rather than overwriting it. A parameter drag edits a working copy
// the previews read, and writes one step on release.
//
// Pin, node and wire ids are hashes of the graph's own ids, so they stay the same across a reload while a
// canvas gesture lasts.
//
// Thread-safety: main thread.

#include "graph/Graph.h"
#include "noise/NoisePreviewService.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct NoiseGraphPinView {
        uint64_t id = 0;
        std::string port;
        std::string name;
        std::string type;
        uint32_t color = 0xffffffffu;   // RGBA, the port type's
        bool connected = false;
        bool error = false;
    };

    struct NoiseGraphNodeView {
        uint64_t id = 0;
        std::string nodeId;
        std::string type;
        std::string title;
        graph::Vec2 position;
        std::vector<NoiseGraphPinView> inputs;
        std::vector<NoiseGraphPinView> outputs;
        std::vector<std::string> footer;    // R56: the range readout
        std::string cost;                   // R56: the time ("0.31 ms"), the canvas header's corner
        std::string errorText;
        bool selected = false;
        bool dead = false;
        bool folded = false;
    };

    struct NoiseGraphWireView {
        uint64_t id = 0;
        std::string edgeId;
        uint64_t fromNode = 0, toNode = 0;
        uint64_t fromPin = 0, toPin = 0;
        std::string fromPort, toPort;
        uint32_t color = 0xffffffffu;
        bool selected = false;
    };

    struct NoiseGraphSearchEntry {
        std::string typeId;
        std::string name;
        std::string category;
        std::string port;               // with a pin to connect: the first compatible port
    };

    // What the toolbar and the right panel set; a change is a new preview revision
    struct NoisePreviewSettings {
        uint32_t seed = 1;
        int imageSize = 128;
        std::vector<std::pair<std::string, float>> variables;
        bool operator==(const NoisePreviewSettings&) const = default;
    };

    class NoiseGraphDocument {
    public:
        explicit NoiseGraphDocument(Project* project);

        // An absolute path. False with loadError() when it cannot be read or parsed: the document stays on
        // that file, so a repaired file comes back through refresh().
        bool open(const std::filesystem::path& file);
        void close();
        bool isOpen() const { return !filePath.empty(); }
        const std::filesystem::path& path() const { return filePath; }
        const graph::Graph& graph() const { return working; }
        std::string displayName() const;
        // Bumps on every change the previews must see: an edit, a working value, a reload, the settings
        uint64_t revision() const { return currentRevision; }
        // Why the file cannot be edited now; empty when it can
        const std::string& loadError() const { return fileError; }
        // Why the last gesture was refused; empty after one that went through
        const std::string& status() const { return lastStatus; }

        // Follows the file: a cheap stat, then a read when it moved. True when the view changed.
        bool refresh();

        // ---- gestures: one undo step each. False (with status()) when refused; nothing is written.
        bool canEdit() const { return isOpen() && fileError.empty(); }
        // Adds a node, returns its id ("" when refused)
        std::string addNode(const std::string& typeId, const graph::Vec2& position);
        // R59: a node from the search popup; with a pin, connected to its first compatible port
        std::string addFromSearch(const std::string& typeId, const graph::Vec2& position, uint64_t connectPin = 0);
        // An output pin to an input pin. A single input already fed is rewired in the same step.
        bool connectPins(uint64_t outputPin, uint64_t inputPin);
        bool canConnectPins(uint64_t outputPin, uint64_t inputPin) const;
        bool removeWire(const std::string& edgeId);
        bool moveNodes(const std::vector<std::pair<std::string, graph::Vec2>>& positions);
        bool setParam(const std::string& nodeId, const std::string& name, const graph::Json& value);
        bool setSpace(bool world);
        bool isWorldSpace() const;

        // ---- a parameter drag: one step, committed on release (R57)
        void beginParamDrag(const std::string& nodeId, const std::string& name);
        void updateParamDrag(const graph::Json& value);
        bool commitParamDrag();
        void cancelParamDrag();
        bool isParamDragging() const { return drag.has_value(); }

        // ---- selection and the edit commands (the shortcut catalog's edit.*, through the window's claim)
        const std::set<std::string>& selectedNodes() const { return nodeSelection; }
        const std::set<std::string>& selectedWires() const { return wireSelection; }
        void setSelection(std::set<std::string> nodes, std::set<std::string> wires);
        bool canDelete() const { return canEdit() && (!nodeSelection.empty() || !wireSelection.empty()); }
        bool canCopy() const { return isOpen() && !nodeSelection.empty(); }
        bool canPaste() const { return canEdit() && !clipboardNodes.empty(); }
        bool deleteSelection();
        bool copySelection();
        bool paste(const graph::Vec2& position);
        bool duplicateSelection();

        // ---- issues (R58), of the working graph
        const std::vector<graph::Issue>& issues() const { return currentIssues; }
        // The first message on that node, errors before warnings
        std::string nodeError(const std::string& nodeId) const;
        std::vector<graph::Issue> issuesWithoutNode() const;
        bool hasErrors() const;
        // Apply's tooltip while it is disabled: the first error's message
        std::string firstError() const;
        bool isDead(const std::string& nodeId) const { return deadNodes.count(nodeId) > 0; }
        bool isFolded(const std::string& nodeId) const { return foldedNodes.count(nodeId) > 0; }

        // ---- node search (R59): by name and category, filtered by the pin's type if any
        std::vector<NoiseGraphSearchEntry> search(const std::string& query, uint64_t connectPin = 0) const;

        // ---- what the canvas draws
        const std::vector<NoiseGraphNodeView>& nodeViews();
        const std::vector<NoiseGraphWireView>& wireViews();
        // The graph's node of a canvas id ("" when it has none)
        std::string nodeOfId(uint64_t id) const;
        std::string edgeOfId(uint64_t id) const;
        uint64_t pinOf(const std::string& nodeId, const std::string& port) const;

        // ---- previews
        // Once a frame. Closed: no job and nothing kept (R50). Open: a job for a new revision, its result
        // taken when it is ready. True when a new result came in.
        bool update(bool windowOpen);
        void setPreviewSettings(const NoisePreviewSettings& settings);
        const NoisePreviewSettings& previewSettings() const { return settings; }
        NoisePreviewService& previewService() { return service; }
        // A finished job's result: dropped unless it is of the current revision (R56)
        bool acceptPreview(NoisePreviewResult&& result);
        const NoiseNodePreview* nodePreview(const std::string& nodeId) const;
        const NoisePreviewImage& previewImage() const { return lastImage; }
        int previewThumbnailSize() const { return thumbnailSize; }
        uint64_t previewedRevision() const { return previewed; }
        uint64_t droppedPreviews() const { return dropped; }
        // The nodes whose thumbnail changed, at most `limit` of them (R50: 8 uploads a frame), oldest first
        std::vector<std::string> takeThumbnailUploads(size_t limit);

        // "-0.62 ... 0.71  ≤ ±1": observed, then analytic (R56)
        static std::string rangeText(const noise::Interval& observed, const noise::Interval& analytic);

        // The toolbar's world-space width and the output's height scale: the graph's preset metadata, else defaults
        float terrainSize() const;
        float maxHeight() const;

    private:
        struct PinRef {
            std::string node;
            std::string port;
            graph::PortDirection direction = graph::PortDirection::In;
            std::string type;
            bool multi = false;
        };
        struct ParamDrag {
            std::string node;
            std::string name;
            graph::Json original;
            bool hadValue = false;
            bool changed = false;
        };

        void adopt(const std::string& text);
        void markGraphChanged();
        void recomputeIssues();
        void rebuildPins();
        void rebuildViews();
        void pruneSelection();
        void stamp();
        // Writes `after` as one step, if the file still is what the document read
        bool commit(graph::Graph after, const std::string& name);
        bool refuse(const std::string& why);
        bool ensureFileUnchanged();
        std::optional<PinRef> pinRef(uint64_t pin) const;
        // True when the edge output -> input would close a loop
        bool wouldCycle(const std::string& fromNode, const std::string& toNode) const;
        void connectInto(graph::Graph& target, const PinRef& output, const PinRef& input) const;
        std::string pasteInto(graph::Graph& target, const graph::Vec2& offset, std::set<std::string>* added) const;

        Project* project = nullptr;
        std::filesystem::path filePath;
        std::string text;           // what the file held when the document last read or wrote it
        graph::Graph working;       // the file's graph, and a drag's working value
        std::string fileError;
        std::string lastStatus;
        uint64_t currentRevision = 1;
        bool viewsDirty = true;

        // what refresh() compares
        std::filesystem::file_time_type stampTime{};
        uintmax_t stampSize = 0;
        bool stampExists = false;

        std::optional<ParamDrag> drag;
        std::set<std::string> nodeSelection;
        std::set<std::string> wireSelection;
        std::vector<graph::Node> clipboardNodes;
        std::vector<graph::Edge> clipboardEdges;

        std::vector<graph::Issue> currentIssues;
        std::set<std::string> deadNodes;
        std::set<std::string> foldedNodes;

        std::unordered_map<uint64_t, PinRef> pins;
        std::vector<NoiseGraphNodeView> nodes;
        std::vector<NoiseGraphWireView> wires;
        std::unordered_map<uint64_t, std::string> nodeIds;
        std::unordered_map<uint64_t, std::string> edgeIds;

        NoisePreviewService service;
        NoisePreviewSettings settings;
        uint64_t requested = 0;
        uint64_t previewed = 0;
        uint64_t dropped = 0;
        int thumbnailSize = 64;
        std::map<std::string, NoiseNodePreview> previews;
        NoisePreviewImage lastImage;
        std::vector<std::string> pendingUploads;
    };

}
