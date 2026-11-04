// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphCanvas.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace{

    ImVec2 add(ImVec2 a, ImVec2 b){ return ImVec2(a.x + b.x, a.y + b.y); }
    ImVec2 sub(ImVec2 a, ImVec2 b){ return ImVec2(a.x - b.x, a.y - b.y); }
    ImVec2 mul(ImVec2 a, float s){ return ImVec2(a.x * s, a.y * s); }
    float length(ImVec2 a){ return std::sqrt(a.x * a.x + a.y * a.y); }

    // where the segment from the box centre toward `toward` leaves the box
    ImVec2 boxEdge(ImVec2 min, ImVec2 max, ImVec2 toward){
        const ImVec2 centre = mul(add(min, max), 0.5f);
        const ImVec2 d = sub(toward, centre);
        const float hx = (max.x - min.x) * 0.5f;
        const float hy = (max.y - min.y) * 0.5f;
        if (std::fabs(d.x) < 1e-4f && std::fabs(d.y) < 1e-4f) return centre;
        const float tx = std::fabs(d.x) > 1e-4f ? hx / std::fabs(d.x) : 1e9f;
        const float ty = std::fabs(d.y) > 1e-4f ? hy / std::fabs(d.y) : 1e9f;
        const float t = std::min(tx, ty);
        return add(centre, mul(d, std::min(t, 1.0f)));
    }

    float distanceToSegment(ImVec2 p, ImVec2 a, ImVec2 b){
        const ImVec2 ab = sub(b, a);
        const float len2 = ab.x * ab.x + ab.y * ab.y;
        float t = len2 > 0.0f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2 : 0.0f;
        t = std::min(1.0f, std::max(0.0f, t));
        return length(sub(p, add(a, mul(ab, t))));
    }

    void arrowHead(ImDrawList* list, ImVec2 tip, ImVec2 direction, float size, ImU32 color){
        const float len = length(direction);
        if (len < 1e-4f) return;
        const ImVec2 d = mul(direction, 1.0f / len);
        const ImVec2 n(-d.y, d.x);
        list->AddTriangleFilled(tip, add(sub(tip, mul(d, size)), mul(n, size * 0.55f)), sub(sub(tip, mul(d, size)), mul(n, size * 0.55f)), color);
    }

    // ---- a dataflow node's box (T2 R53), in canvas units so it scales with the zoom
    constexpr float kHeaderHeight = 24.0f;
    constexpr float kPinRowHeight = 18.0f;
    constexpr float kPadding = 6.0f;
    constexpr float kFooterLine = 15.0f;
    constexpr float kPinRadius = 5.0f;
    constexpr float kPinHitRadius = 8.0f;
    constexpr float kThumbnailMinZoom = 0.5f;   // R56
    constexpr int kWireHitSegments = 24;

    bool usesPinLayout(const GraphCanvas::Node& node){
        return !node.inputs.empty() || !node.outputs.empty() || node.thumbnail != ImTextureID{} || node.thumbnailSize.y > 0.0f ||
               !node.footer.empty() || !node.errorText.empty();
    }

    float widthOf(ImFont* font, float size, const std::string& text){
        return font->CalcTextSizeA(size, std::numeric_limits<float>::max(), 0.0f, text.c_str()).x;
    }

    // What the box needs at zoom 1. The error wraps to the width the rest settled on.
    void fitPinLayout(GraphCanvas::Node& node, ImFont* font, float fontSize){
        float width = std::max(node.size.x, widthOf(font, fontSize, node.label) + (node.subtitle.empty() ? 0.0f : widthOf(font, fontSize * 0.85f, node.subtitle) + kPadding) + 2 * kPadding);
        const size_t rows = std::max(node.inputs.size(), node.outputs.size());
        for (size_t i = 0; i < rows; i++){
            float row = 2 * kPinRadius + 2 * kPadding;
            if (i < node.inputs.size()) row += widthOf(font, fontSize, node.inputs[i].label);
            if (i < node.outputs.size()) row += widthOf(font, fontSize, node.outputs[i].label) + kPadding;
            width = std::max(width, row);
        }
        width = std::max(width, node.thumbnailSize.x + 2 * kPadding);
        for (const std::string& line : node.footer) width = std::max(width, widthOf(font, fontSize * 0.85f, line) + 2 * kPadding);
        float height = kHeaderHeight + kPinRowHeight * static_cast<float>(rows);
        if (node.thumbnailSize.y > 0.0f) height += node.thumbnailSize.y + kPadding;
        height += kFooterLine * static_cast<float>(node.footer.size());
        if (!node.errorText.empty()){
            // one more line than measured: a smaller zoom's glyph sizes round differently
            const float wrapped = font->CalcTextSizeA(fontSize * 0.85f, std::numeric_limits<float>::max(), width - 2 * kPadding, node.errorText.c_str()).y;
            height += wrapped + fontSize + kPadding;
        }
        node.size = ImVec2(width, std::max(node.size.y, height + kPadding));
    }

    ImVec2 pinPosition(const GraphCanvas::Node& node, size_t index, bool input){
        return ImVec2(node.position.x + (input ? 0.0f : node.size.x),
                      node.position.y + kHeaderHeight + kPinRowHeight * (static_cast<float>(index) + 0.5f));
    }

    // A wire leaves an output to the right and enters an input from the left
    void wireControls(ImVec2 from, ImVec2 to, float zoom, ImVec2& c1, ImVec2& c2){
        const float reach = std::max(40.0f * zoom, std::fabs(to.x - from.x) * 0.5f);
        c1 = ImVec2(from.x + reach, from.y);
        c2 = ImVec2(to.x - reach, to.y);
    }

    ImVec2 cubicAt(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, float t){
        const float u = 1.0f - t;
        const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        return ImVec2(w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x, w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y);
    }

    float distanceToCubic(ImVec2 p, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d){
        float best = std::numeric_limits<float>::max();
        ImVec2 previous = a;
        for (int i = 1; i <= kWireHitSegments; i++){
            const ImVec2 next = cubicAt(a, b, c, d, static_cast<float>(i) / static_cast<float>(kWireHitSegments));
            best = std::min(best, distanceToSegment(p, previous, next));
            previous = next;
        }
        return best;
    }

    ImU32 dimmed(ImU32 color, float factor){
        const ImU32 alpha = static_cast<ImU32>(static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * factor);
        return (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
    }

}

ImVec2 GraphCanvas::toCanvas(const ImVec2& screen) const{
    return add(mul(sub(screen, origin), 1.0f / zoom), pan);
}

ImVec2 GraphCanvas::toScreen(const ImVec2& canvas) const{
    return add(mul(sub(canvas, pan), zoom), origin);
}

void GraphCanvas::beginLink(uint64_t from){
    linkSource = from;
}

void GraphCanvas::frame(const std::vector<Node>& nodes){
    if (nodes.empty() || regionSize.x <= 0.0f || regionSize.y <= 0.0f) {
        framed = false;
        return;
    }
    ImVec2 min(1e9f, 1e9f), max(-1e9f, -1e9f);
    for (const Node& node : nodes){
        min.x = std::min(min.x, node.position.x);
        min.y = std::min(min.y, node.position.y);
        max.x = std::max(max.x, node.position.x + node.size.x);
        max.y = std::max(max.y, node.position.y + node.size.y);
    }
    const float margin = 40.0f;
    const float zx = regionSize.x / std::max(1.0f, max.x - min.x + 2 * margin);
    const float zy = regionSize.y / std::max(1.0f, max.y - min.y + 2 * margin);
    zoom = std::min(1.0f, std::max(0.2f, std::min(zx, zy)));
    const ImVec2 centre = mul(add(min, max), 0.5f);
    pan = sub(centre, mul(regionSize, 0.5f / zoom));
    framed = true;
}

GraphCanvas::Result GraphCanvas::draw(const char* id, std::vector<Node>& nodes, std::vector<Link>& links){
    Result result;
    for (Node& node : nodes){
        if (usesPinLayout(node)) fitPinLayout(node, ImGui::GetFont(), ImGui::GetFontSize());
    }
    origin = ImGui::GetCursorScreenPos();
    regionSize = ImGui::GetContentRegionAvail();
    regionSize.x = std::max(regionSize.x, 50.0f);
    regionSize.y = std::max(regionSize.y, 50.0f);
    if (!framed) frame(nodes);

    ImGui::InvisibleButton(id, regionSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    result.hovered = hovered;
    result.mouseCanvas = toCanvas(mouse);

    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 regionMax = add(origin, regionSize);
    list->PushClipRect(origin, regionMax, true);
    list->AddRectFilled(origin, regionMax, IM_COL32(38, 38, 42, 255));

    // view: pan and zoom
    if (hovered && io.MouseWheel != 0.0f){
        const ImVec2 before = toCanvas(mouse);
        zoom = std::min(2.5f, std::max(0.2f, zoom * std::pow(1.15f, io.MouseWheel)));
        const ImVec2 after = toCanvas(mouse);
        pan = add(pan, sub(before, after));
    }
    const bool panning = (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) && (hovered || active)) ||
                         (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && active);
    if (panning){
        pan = sub(pan, mul(io.MouseDelta, 1.0f / zoom));
    }

    // grid
    const float step = gridSize * zoom;
    if (step > 6.0f){
        const ImVec2 first = toScreen(ImVec2(std::floor(pan.x / gridSize) * gridSize, std::floor(pan.y / gridSize) * gridSize));
        for (float x = first.x; x < regionMax.x; x += step) list->AddLine(ImVec2(x, origin.y), ImVec2(x, regionMax.y), IM_COL32(50, 50, 55, 255));
        for (float y = first.y; y < regionMax.y; y += step) list->AddLine(ImVec2(origin.x, y), ImVec2(regionMax.x, y), IM_COL32(50, 50, 55, 255));
    }

    // hit testing
    auto nodeAt = [&](ImVec2 screen) -> Node* {
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it){
            const ImVec2 a = toScreen(it->position);
            const ImVec2 b = toScreen(add(it->position, it->size));
            if (screen.x >= a.x && screen.x <= b.x && screen.y >= a.y && screen.y <= b.y) return &*it;
        }
        return nullptr;
    };
    std::map<uint64_t, const Node*> byId;
    for (const Node& node : nodes) byId[node.id] = &node;

    // the pins (none: a graph of boxes and arrows, which pays nothing here)
    struct PinSpot{
        const Pin* pin;
        ImVec2 position;    // screen
        bool input;
    };
    std::map<uint64_t, PinSpot> pinSpots;
    for (const Node& node : nodes){
        for (size_t i = 0; i < node.inputs.size(); i++) pinSpots[node.inputs[i].id] = {&node.inputs[i], toScreen(pinPosition(node, i, true)), true};
        for (size_t i = 0; i < node.outputs.size(); i++) pinSpots[node.outputs[i].id] = {&node.outputs[i], toScreen(pinPosition(node, i, false)), false};
    }
    auto pinAt = [&](ImVec2 screen) -> uint64_t {
        uint64_t found = 0;
        float best = kPinHitRadius * std::max(1.0f, zoom);
        for (const auto& [id, spot] : pinSpots){
            const float distance = length(sub(screen, spot.position));
            if (distance <= best){
                best = distance;
                found = id;
            }
        }
        return found;
    };
    // an output and an input, in that order, that may connect
    auto pinsConnect = [&](uint64_t a, uint64_t b) -> bool {
        auto first = pinSpots.find(a);
        auto second = pinSpots.find(b);
        if (first == pinSpots.end() || second == pinSpots.end() || first->second.input == second->second.input) return false;
        const uint64_t output = first->second.input ? b : a;
        const uint64_t input = first->second.input ? a : b;
        return !canConnect || canConnect(output, input);
    };
    const uint64_t pinUnderMouse = hovered || active ? pinAt(mouse) : 0;
    result.hoveredPin = pinUnderMouse;

    // link geometry: paired directions offset to the side
    struct Segment{
        ImVec2 a, b;
        size_t link;
    };
    std::vector<Segment> segments;
    struct Wire{
        ImVec2 a, c1, c2, b;
        size_t link;
    };
    std::vector<Wire> wires;
    for (size_t i = 0; i < links.size(); i++){
        const Link& link = links[i];
        if (link.fromPin != 0 && link.toPin != 0){
            auto fromPin = pinSpots.find(link.fromPin);
            auto toPin = pinSpots.find(link.toPin);
            if (fromPin == pinSpots.end() || toPin == pinSpots.end()) continue;
            Wire wire;
            wire.a = fromPin->second.position;
            wire.b = toPin->second.position;
            wire.link = i;
            wireControls(wire.a, wire.b, zoom, wire.c1, wire.c2);
            wires.push_back(wire);
            continue;
        }
        auto from = byId.find(link.from);
        auto to = byId.find(link.to);
        if (from == byId.end() || to == byId.end()) continue;
        const ImVec2 fa = toScreen(from->second->position), fb = toScreen(add(from->second->position, from->second->size));
        const ImVec2 ta = toScreen(to->second->position), tb = toScreen(add(to->second->position, to->second->size));
        if (link.from == link.to){
            // a loop over the top edge
            const ImVec2 top((fa.x + fb.x) * 0.5f, fa.y);
            segments.push_back({add(top, ImVec2(-12 * zoom, 0)), add(top, ImVec2(12 * zoom, 0)), i});
            continue;
        }
        const ImVec2 fc = mul(add(fa, fb), 0.5f), tc = mul(add(ta, tb), 0.5f);
        ImVec2 dir = sub(tc, fc);
        const float len = std::max(1e-3f, length(dir));
        dir = mul(dir, 1.0f / len);
        bool paired = false;
        for (const Link& other : links) paired = paired || (other.from == link.to && other.to == link.from);
        const ImVec2 side = paired ? mul(ImVec2(-dir.y, dir.x), 6.0f * zoom) : ImVec2(0, 0);
        const ImVec2 a = add(boxEdge(fa, fb, tc), side);
        const ImVec2 b = add(boxEdge(ta, tb, fc), side);
        segments.push_back({a, b, i});
    }
    auto linkAt = [&](ImVec2 screen) -> Link* {
        for (const Segment& s : segments){
            if (distanceToSegment(screen, s.a, s.b) < 5.0f) return &links[s.link];
        }
        for (const Wire& w : wires){
            if (distanceToCubic(screen, w.a, w.c1, w.c2, w.b) < 5.0f) return &links[w.link];
        }
        return nullptr;
    };

    // input
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && pinUnderMouse != 0 && linkSource == 0){
        // a press on a pin starts a wire, and neither a node drag nor a box select
        wireSource = pinUnderMouse;
    }else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt){
        Node* node = nodeAt(mouse);
        if (linkSource != 0){
            if (node){
                result.linkFrom = linkSource;
                result.linkTo = node->id;
            }
            linkSource = 0;
        }else if (node){
            result.clickedNode = node->id;
            if (io.KeyCtrl){
                node->selected = !node->selected;
            }else if (io.KeyShift){
                node->selected = true;
            }else if (!node->selected){
                for (Node& n : nodes) n.selected = false;
                for (Link& l : links) l.selected = false;
                node->selected = true;
            }
            result.selectionChanged = true;
            draggingNodes = true;
            dragStartMouse = mouse;
            dragStart.clear();
            for (const Node& n : nodes) if (n.selected) dragStart.push_back({n.id, n.position});
        }else if (Link* link = linkAt(mouse)){
            result.clickedLink = link->id;
            for (Node& n : nodes) n.selected = false;
            for (Link& l : links) l.selected = false;
            link->selected = true;
            result.selectionChanged = true;
        }else{
            result.clickedBackground = true;
            if (!io.KeyShift && !io.KeyCtrl){
                for (Node& n : nodes) n.selected = false;
                for (Link& l : links) l.selected = false;
                result.selectionChanged = true;
            }
            boxSelecting = true;
            boxStart = mouse;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
        if (Node* node = nodeAt(mouse)) result.doubleClickedNode = node->id;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && linkSource != 0) linkSource = 0;
    if (wireSource != 0 && (ImGui::IsKeyPressed(ImGuiKey_Escape) || !pinSpots.count(wireSource))) wireSource = 0;
    if (wireSource != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)){
        const uint64_t source = wireSource;
        wireSource = 0;
        if (pinUnderMouse != 0){
            if (pinUnderMouse != source && pinsConnect(source, pinUnderMouse)){
                const bool sourceIsInput = pinSpots.at(source).input;
                result.wireFrom = sourceIsInput ? pinUnderMouse : source;
                result.wireTo = sourceIsInput ? source : pinUnderMouse;
            }
        }else if (!nodeAt(mouse) && mouse.x >= origin.x && mouse.x <= regionMax.x && mouse.y >= origin.y && mouse.y <= regionMax.y){
            result.wireDroppedFrom = source;
            result.wireDropPosition = toCanvas(mouse);
        }
    }

    if (draggingNodes){
        const ImVec2 delta = mul(sub(mouse, dragStartMouse), 1.0f / zoom);
        for (const auto& start : dragStart){
            for (Node& n : nodes){
                if (n.id != start.first) continue;
                ImVec2 p = add(start.second, delta);
                if (snap && gridSize > 0.0f){
                    p.x = std::round(p.x / gridSize) * gridSize;
                    p.y = std::round(p.y / gridSize) * gridSize;
                }
                n.position = p;
            }
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            draggingNodes = false;
            bool movedAny = false;
            for (const auto& start : dragStart){
                for (const Node& n : nodes){
                    if (n.id == start.first && (n.position.x != start.second.x || n.position.y != start.second.y)) movedAny = true;
                }
            }
            if (movedAny){
                for (const auto& start : dragStart){
                    for (const Node& n : nodes) if (n.id == start.first) result.moved.push_back({n.id, n.position});
                }
            }
        }
    }
    if (boxSelecting){
        const ImVec2 a(std::min(boxStart.x, mouse.x), std::min(boxStart.y, mouse.y));
        const ImVec2 b(std::max(boxStart.x, mouse.x), std::max(boxStart.y, mouse.y));
        list->AddRectFilled(a, b, IM_COL32(90, 140, 220, 40));
        list->AddRect(a, b, IM_COL32(90, 140, 220, 200));
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            boxSelecting = false;
            if (b.x - a.x > 3.0f || b.y - a.y > 3.0f){
                for (Node& n : nodes){
                    const ImVec2 na = toScreen(n.position), nb = toScreen(add(n.position, n.size));
                    if (na.x <= b.x && nb.x >= a.x && na.y <= b.y && nb.y >= a.y) n.selected = true;
                }
                result.selectionChanged = true;
            }
        }
    }
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right)){
        result.contextRequested = true;
        result.contextPosition = toCanvas(mouse);
        if (Node* node = nodeAt(mouse)){
            result.contextNode = node->id;
            if (!node->selected){
                for (Node& n : nodes) n.selected = false;
                node->selected = true;
                result.selectionChanged = true;
            }
        }else if (Link* link = linkAt(mouse)){
            result.contextLink = link->id;
        }
    }
    if (result.selectionChanged){
        for (const Node& n : nodes) if (n.selected) result.selectedNodes.push_back(n.id);
    }

    // links
    for (const Segment& s : segments){
        const Link& link = links[s.link];
        const ImU32 color = link.selected ? IM_COL32(110, 170, 255, 255) : link.color;
        const float thickness = (link.selected ? 3.0f : 2.0f) * std::max(0.6f, zoom);
        if (link.from == link.to){
            list->AddBezierCubic(s.a, add(s.a, ImVec2(-10 * zoom, -28 * zoom)), add(s.b, ImVec2(10 * zoom, -28 * zoom)), s.b, color, thickness);
            arrowHead(list, s.b, ImVec2(0, 1), 9.0f * zoom, color);
            continue;
        }
        list->AddLine(s.a, s.b, color, thickness);
        const ImVec2 mid = mul(add(s.a, s.b), 0.5f);
        arrowHead(list, add(mid, mul(sub(s.b, s.a), 0.06f)), sub(s.b, s.a), 10.0f * zoom, color);
        if (link.count > 1){
            const ImVec2 dir = mul(sub(s.b, s.a), 1.0f / std::max(1e-3f, length(sub(s.b, s.a))));
            arrowHead(list, sub(mid, mul(dir, 10.0f * zoom)), sub(s.b, s.a), 10.0f * zoom, color);
        }
        if (link.progress >= 0.0f){
            const ImVec2 p = add(s.a, mul(sub(s.b, s.a), std::min(1.0f, link.progress)));
            list->AddLine(s.a, p, IM_COL32(90, 200, 255, 255), thickness + 1.5f);
        }
    }
    for (const Wire& w : wires){
        const Link& link = links[w.link];
        const ImU32 color = link.selected ? IM_COL32(110, 170, 255, 255) : link.color;
        list->AddBezierCubic(w.a, w.c1, w.c2, w.b, color, (link.selected ? 3.0f : 2.0f) * std::max(0.6f, zoom));
    }
    // a link being made
    if (linkSource != 0){
        auto from = byId.find(linkSource);
        if (from != byId.end()){
            const ImVec2 fa = toScreen(from->second->position), fb = toScreen(add(from->second->position, from->second->size));
            const ImVec2 a = boxEdge(fa, fb, mouse);
            list->AddLine(a, mouse, IM_COL32(255, 255, 255, 200), 2.0f);
            arrowHead(list, mouse, sub(mouse, a), 10.0f, IM_COL32(255, 255, 255, 200));
        }
    }

    // nodes (text scaled with the zoom)
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * zoom;
    for (const Node& node : nodes){
        const ImVec2 a = toScreen(node.position);
        const ImVec2 b = toScreen(add(node.position, node.size));
        if (b.x < origin.x || a.x > regionMax.x || b.y < origin.y || a.y > regionMax.y) continue;
        if (usesPinLayout(node)){
            const float rounding = node.rounded ? 6.0f * zoom : 0.0f;
            list->AddRectFilled(a, b, node.fill, rounding);
            if (node.highlighted) list->AddRect(a, b, IM_COL32(90, 200, 255, 255), rounding, 0, 2.5f);
            if (!node.errorText.empty()) list->AddRect(a, b, IM_COL32(235, 70, 70, 255), rounding, 0, 2.0f);
            if (node.selected) list->AddRect(add(a, ImVec2(-2, -2)), add(b, ImVec2(2, 2)), IM_COL32(110, 170, 255, 255), rounding, 0, 2.0f);
            const bool text = fontSize >= 5.0f;
            const float pad = kPadding * zoom;
            if (text){
                const float y = a.y + (kHeaderHeight * zoom - fontSize) * 0.5f;
                list->AddText(font, fontSize, ImVec2(a.x + pad, y), node.text, node.label.c_str());
                if (!node.subtitle.empty()){
                    const float subSize = fontSize * 0.85f;
                    const float subWidth = font->CalcTextSizeA(subSize, std::numeric_limits<float>::max(), 0.0f, node.subtitle.c_str()).x;
                    list->AddText(font, subSize, ImVec2(b.x - pad - subWidth, y), IM_COL32(170, 170, 170, 255), node.subtitle.c_str());
                }
            }
            auto drawPins = [&](const std::vector<Pin>& pins, bool input){
                for (size_t i = 0; i < pins.size(); i++){
                    const Pin& pin = pins[i];
                    const ImVec2 at = toScreen(pinPosition(node, i, input));
                    // during a wire drag, what cannot take it dims (R53)
                    const bool refuses = wireSource != 0 && pin.id != wireSource && !pinsConnect(wireSource, pin.id);
                    const float dim = refuses ? 0.25f : 1.0f;
                    const float radius = kPinRadius * zoom;
                    const ImU32 color = dimmed(pin.color, dim);
                    if (pin.connected) list->AddCircleFilled(at, radius, color);
                    else list->AddCircle(at, radius, color, 0, 1.5f * std::max(0.6f, zoom));
                    if (pin.error) list->AddCircle(at, radius + 2.5f * zoom, IM_COL32(235, 70, 70, 255), 0, 2.0f);
                    if (wireSource != 0 && pin.id == pinUnderMouse && !refuses) list->AddCircle(at, radius + 2.5f * zoom, IM_COL32(255, 255, 255, 220), 0, 1.5f);
                    if (text && !pin.label.empty()){
                        const float labelWidth = font->CalcTextSizeA(fontSize, std::numeric_limits<float>::max(), 0.0f, pin.label.c_str()).x;
                        const ImVec2 labelAt(input ? at.x + radius + pad * 0.5f : at.x - radius - pad * 0.5f - labelWidth, at.y - fontSize * 0.5f);
                        list->AddText(font, fontSize, labelAt, dimmed(pin.error ? IM_COL32(255, 120, 120, 255) : node.text, dim), pin.label.c_str());
                    }
                }
            };
            drawPins(node.inputs, true);
            drawPins(node.outputs, false);
            float y = node.position.y + kHeaderHeight + kPinRowHeight * static_cast<float>(std::max(node.inputs.size(), node.outputs.size()));
            if (node.thumbnailSize.y > 0.0f){
                if (node.thumbnail != ImTextureID{} && zoom >= kThumbnailMinZoom){
                    const float x = node.position.x + (node.size.x - node.thumbnailSize.x) * 0.5f;
                    list->AddImage(ImTextureRef(node.thumbnail), toScreen(ImVec2(x, y)), toScreen(ImVec2(x + node.thumbnailSize.x, y + node.thumbnailSize.y)));
                }
                y += node.thumbnailSize.y + kPadding;
            }
            if (text){
                const float smallSize = fontSize * 0.85f;
                for (const std::string& line : node.footer){
                    list->AddText(font, smallSize, toScreen(ImVec2(node.position.x + kPadding, y)), IM_COL32(170, 170, 170, 255), line.c_str());
                    y += kFooterLine;
                }
                if (!node.errorText.empty()){
                    list->AddText(font, smallSize, toScreen(ImVec2(node.position.x + kPadding, y)), IM_COL32(255, 120, 120, 255), node.errorText.c_str(),
                                  nullptr, (node.size.x - 2 * kPadding) * zoom);
                }
            }
            continue;
        }
        const float rounding = node.rounded ? 6.0f * zoom : 0.0f;
        list->AddRectFilled(a, b, node.fill, rounding);
        if (node.highlighted) list->AddRect(a, b, IM_COL32(90, 200, 255, 255), rounding, 0, 2.5f);
        if (node.selected) list->AddRect(add(a, ImVec2(-2, -2)), add(b, ImVec2(2, 2)), IM_COL32(110, 170, 255, 255), rounding, 0, 2.0f);
        if (node.progress >= 0.0f){
            const float h = 4.0f * zoom;
            list->AddRectFilled(ImVec2(a.x, b.y - h), ImVec2(a.x + (b.x - a.x) * std::min(1.0f, node.progress), b.y), IM_COL32(90, 200, 255, 255));
        }
        if (fontSize >= 5.0f){
            const ImVec2 labelSize = font->CalcTextSizeA(fontSize, 1e9f, 0.0f, node.label.c_str());
            const float lines = node.subtitle.empty() ? 1.0f : 2.0f;
            ImVec2 p((a.x + b.x - labelSize.x) * 0.5f, (a.y + b.y - labelSize.y * lines) * 0.5f);
            list->AddText(font, fontSize, p, node.text, node.label.c_str());
            if (!node.subtitle.empty()){
                const float subSize = fontSize * 0.85f;
                const ImVec2 subtitleSize = font->CalcTextSizeA(subSize, 1e9f, 0.0f, node.subtitle.c_str());
                list->AddText(font, subSize, ImVec2((a.x + b.x - subtitleSize.x) * 0.5f, p.y + labelSize.y), IM_COL32(170, 170, 170, 255),
                              node.subtitle.c_str());
            }
        }
    }
    // a wire being dragged, over the boxes: it snaps to a pin that takes it
    if (wireSource != 0){
        auto source = pinSpots.find(wireSource);
        if (source != pinSpots.end()){
            ImVec2 tip = mouse;
            if (pinUnderMouse != 0 && pinUnderMouse != wireSource && pinsConnect(wireSource, pinUnderMouse)) tip = pinSpots.at(pinUnderMouse).position;
            const ImVec2 from = source->second.input ? tip : source->second.position;
            const ImVec2 to = source->second.input ? source->second.position : tip;
            ImVec2 c1, c2;
            wireControls(from, to, zoom, c1, c2);
            list->AddBezierCubic(from, c1, c2, to, source->second.pin->color, 2.0f * std::max(0.6f, zoom));
        }
    }
    list->PopClipRect();
    return result;
}
