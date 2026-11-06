// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphCanvas.h"

#include <algorithm>
#include <cmath>
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

    // link geometry: paired directions offset to the side
    struct Segment{
        ImVec2 a, b;
        size_t link;
    };
    std::vector<Segment> segments;
    for (size_t i = 0; i < links.size(); i++){
        const Link& link = links[i];
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
        return nullptr;
    };

    // input
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt){
        Node* node = nodeAt(mouse);
        selectionAtPress.clear();
        for (const Node& n : nodes) if (n.selected) selectionAtPress.push_back(n.id);
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
            }else{
                collapseTo = node->id;
            }
            result.selectionChanged = true;
            draggingNodes = true;
            dragMoved = false;
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
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)){
        linkSource = 0;
        if (draggingNodes || boxSelecting){
            for (Node& n : nodes){
                for (const auto& start : dragStart) if (draggingNodes && n.id == start.first) n.position = start.second;
                n.selected = std::find(selectionAtPress.begin(), selectionAtPress.end(), n.id) != selectionAtPress.end();
            }
            draggingNodes = false;
            boxSelecting = false;
            collapseTo = 0;
            result.cancelled = true;
            result.selectionChanged = true;
        }
    }

    if (draggingNodes){
        dragMoved = dragMoved || length(sub(mouse, dragStartMouse)) >= io.MouseDragThreshold;
        const ImVec2 delta = mul(sub(mouse, dragStartMouse), 1.0f / zoom);
        // (a snap would move an off-grid node on the press itself)
        if (dragMoved) for (const auto& start : dragStart){
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
            }else if (collapseTo != 0 && dragStart.size() > 1){
                for (Node& n : nodes) n.selected = n.id == collapseTo;
                result.selectionChanged = true;
            }
            collapseTo = 0;
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
    list->PopClipRect();
    return result;
}
