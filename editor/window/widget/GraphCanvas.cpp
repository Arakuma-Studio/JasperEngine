// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "GraphCanvas.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

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

    // a wire's control points: out of the output to the right, into the input from the left
    void wireCurve(ImVec2 a, ImVec2 b, float zoom, ImVec2 out[4]){
        const float dx = std::max(40.0f * zoom, std::fabs(b.x - a.x) * 0.5f);
        out[0] = a;
        out[1] = ImVec2(a.x + dx, a.y);
        out[2] = ImVec2(b.x - dx, b.y);
        out[3] = b;
    }

    ImVec2 bezierAt(const ImVec2 p[4], float t){
        const float u = 1.0f - t;
        const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        return ImVec2(w0 * p[0].x + w1 * p[1].x + w2 * p[2].x + w3 * p[3].x, w0 * p[0].y + w1 * p[1].y + w2 * p[2].y + w3 * p[3].y);
    }

    float distanceToWire(ImVec2 point, const ImVec2 p[4]){
        float best = 1e9f;
        ImVec2 previous = p[0];
        for (int i = 1; i <= 16; i++){
            const ImVec2 next = bezierAt(p, (float)i / 16.0f);
            best = std::min(best, distanceToSegment(point, previous, next));
            previous = next;
        }
        return best;
    }

    // the bounds of points, to cull what lies outside the view
    bool outside(const ImVec2* points, int count, ImVec2 min, ImVec2 max){
        ImVec2 lo = points[0], hi = points[0];
        for (int i = 1; i < count; i++){
            lo = ImVec2(std::min(lo.x, points[i].x), std::min(lo.y, points[i].y));
            hi = ImVec2(std::max(hi.x, points[i].x), std::max(hi.y, points[i].y));
        }
        return hi.x < min.x || lo.x > max.x || hi.y < min.y || lo.y > max.y;
    }

    // the colour's alpha scaled (above 1: more opaque, up to opaque)
    ImU32 withAlpha(ImU32 color, float alpha){
        const ImU32 a = (ImU32)std::min(255.0f, (float)((color >> IM_COL32_A_SHIFT) & 0xFF) * std::max(0.0f, alpha));
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }

    ImU32 lighter(ImU32 color, float amount){
        auto channel = [&](int shift){
            const float c = (float)((color >> shift) & 0xFF);
            return (ImU32)std::min(255.0f, c + (255.0f - c) * amount) << shift;
        };
        return channel(IM_COL32_R_SHIFT) | channel(IM_COL32_G_SHIFT) | channel(IM_COL32_B_SHIFT) | (color & IM_COL32_A_MASK);
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
        const ImVec2 size = nodeSize(node);
        min.x = std::min(min.x, node.position.x);
        min.y = std::min(min.y, node.position.y);
        max.x = std::max(max.x, node.position.x + size.x);
        max.y = std::max(max.y, node.position.y + size.y);
    }
    const float margin = 40.0f;
    const float zx = regionSize.x / std::max(1.0f, max.x - min.x + 2 * margin);
    const float zy = regionSize.y / std::max(1.0f, max.y - min.y + 2 * margin);
    zoom = std::min(1.0f, std::max(0.2f, std::min(zx, zy)));
    const ImVec2 centre = mul(add(min, max), 0.5f);
    pan = sub(centre, mul(regionSize, 0.5f / zoom));
    framed = true;
}

void GraphCanvas::frameSelected(const std::vector<Node>& nodes){
    std::vector<Node> selected;
    for (const Node& node : nodes) if (node.selected) selected.push_back(node);
    frame(selected.empty() ? nodes : selected);
}

float GraphCanvas::controlsAlpha(float zoom){
    return std::min(1.0f, std::max(0.0f, (zoom - FADE_END) / (FADE_START - FADE_END)));
}

ImVec2 GraphCanvas::nodeSize(const Node& node){
    if (!isDataflow(node)) return node.size;
    const float rows = (float)std::max(node.inputs.size(), node.outputs.size());
    float height = HEADER_HEIGHT + rows * PORT_ROW + PADDING;
    if (node.thumbnail) height += THUMBNAIL_SLOT + PADDING;
    return ImVec2(std::max(MIN_WIDTH, node.size.x), height);
}

ImVec2 GraphCanvas::portPosition(const Node& node, const std::string& port, bool output){
    const std::vector<Port>& ports = output ? node.outputs : node.inputs;
    for (size_t i = 0; i < ports.size(); i++){
        if (ports[i].id != port) continue;
        const float x = output ? node.position.x + nodeSize(node).x : node.position.x;
        return ImVec2(x, node.position.y + HEADER_HEIGHT + PORT_ROW * ((float)i + 0.5f));
    }
    return node.position;
}

void GraphCanvas::thumbnailRect(const Node& node, ImVec2& min, ImVec2& max){
    const ImVec2 size = nodeSize(node);
    const float rows = (float)std::max(node.inputs.size(), node.outputs.size());
    min = ImVec2(node.position.x + (size.x - THUMBNAIL_SLOT) * 0.5f, node.position.y + HEADER_HEIGHT + rows * PORT_ROW + PADDING);
    max = ImVec2(min.x + THUMBNAIL_SLOT, min.y + THUMBNAIL_SLOT);
}

bool GraphCanvas::accepts(const PortRef& a, const PortRef& b) const{
    if (a.node == b.node || a.output == b.output) return false;
    const PortRef& output = a.output ? a : b;
    const PortRef& input = a.output ? b : a;
    return !canConnect || canConnect(output, input);
}

GraphCanvas::Result GraphCanvas::draw(const char* id, std::vector<Node>& nodes, std::vector<Link>& links){
    static const std::vector<Group> none;
    return draw(id, nodes, links, none);
}

GraphCanvas::Result GraphCanvas::draw(const char* id, std::vector<Node>& nodes, std::vector<Link>& links, const std::vector<Group>& groups){
    Result result;
    for (Node& node : nodes) if (isDataflow(node)) node.size = nodeSize(node);
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
    std::unordered_map<uint64_t, const Node*> byId;
    for (const Node& node : nodes) byId[node.id] = &node;
    const float alpha = controlsAlpha(zoom);
    // the port under a screen point (ports take no press once faded out)
    auto portAt = [&](ImVec2 screen, PortRef& out) -> bool {
        if (alpha <= 0.0f) return false;
        const float reach = std::max(7.0f, PORT_ROW * 0.5f * zoom);
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it){
            for (int side = 0; side < 2; side++){
                const std::vector<Port>& ports = side ? it->outputs : it->inputs;
                for (const Port& port : ports){
                    const ImVec2 p = toScreen(portPosition(*it, port.id, side == 1));
                    if (std::fabs(screen.x - p.x) <= std::max(8.0f, 10.0f * zoom) && std::fabs(screen.y - p.y) <= reach){
                        out = {it->id, port.id, side == 1};
                        return true;
                    }
                }
            }
        }
        return false;
    };
    // group rects (screen) and their title bars
    struct GroupBox{
        ImVec2 a, b;
        float title;
        const Group* group;
    };
    std::vector<GroupBox> groupBoxes;
    for (const Group& group : groups){
        ImVec2 a = group.position, b = ImVec2(group.position.x + group.size.x, group.position.y + group.size.y);
        bool any = false;
        for (uint64_t member : group.nodes){
            auto found = byId.find(member);
            if (found == byId.end()) continue;
            const Node& node = *found->second;
            const ImVec2 lo(node.position.x - 16.0f, node.position.y - 16.0f - HEADER_HEIGHT);
            const ImVec2 hi(node.position.x + node.size.x + 16.0f, node.position.y + node.size.y + 16.0f);
            a = any ? ImVec2(std::min(a.x, lo.x), std::min(a.y, lo.y)) : lo;
            b = any ? ImVec2(std::max(b.x, hi.x), std::max(b.y, hi.y)) : hi;
            any = true;
        }
        groupBoxes.push_back({toScreen(a), toScreen(b), HEADER_HEIGHT * zoom, &group});
    }

    // link geometry: paired directions offset to the side; wires as curves; what lies outside the
    // view is left out (drawn and hit tested)
    struct Segment{
        ImVec2 a, b;
        size_t link;
        bool wire;
        ImVec2 curve[4];
    };
    std::set<std::pair<uint64_t, uint64_t>> directions;
    for (const Link& link : links) directions.insert({link.from, link.to});
    std::vector<Segment> segments;
    for (size_t i = 0; i < links.size(); i++){
        const Link& link = links[i];
        auto from = byId.find(link.from);
        auto to = byId.find(link.to);
        if (from == byId.end() || to == byId.end()) continue;
        if (!link.fromPort.empty() && !link.toPort.empty()){
            Segment segment{};
            segment.link = i;
            segment.wire = true;
            wireCurve(toScreen(portPosition(*from->second, link.fromPort, true)), toScreen(portPosition(*to->second, link.toPort, false)), zoom,
                      segment.curve);
            if (outside(segment.curve, 4, origin, regionMax)) continue;
            segment.a = segment.curve[0];
            segment.b = segment.curve[3];
            segments.push_back(segment);
            continue;
        }
        const ImVec2 fa = toScreen(from->second->position), fb = toScreen(add(from->second->position, from->second->size));
        const ImVec2 ta = toScreen(to->second->position), tb = toScreen(add(to->second->position, to->second->size));
        if (link.from == link.to){
            // a loop over the top edge
            const ImVec2 top((fa.x + fb.x) * 0.5f, fa.y);
            Segment segment{};
            segment.a = add(top, ImVec2(-12 * zoom, 0));
            segment.b = add(top, ImVec2(12 * zoom, 0));
            segment.link = i;
            segments.push_back(segment);
            continue;
        }
        const ImVec2 fc = mul(add(fa, fb), 0.5f), tc = mul(add(ta, tb), 0.5f);
        ImVec2 dir = sub(tc, fc);
        const float len = std::max(1e-3f, length(dir));
        dir = mul(dir, 1.0f / len);
        const bool paired = directions.count({link.to, link.from}) > 0;
        const ImVec2 side = paired ? mul(ImVec2(-dir.y, dir.x), 6.0f * zoom) : ImVec2(0, 0);
        Segment segment{};
        segment.a = add(boxEdge(fa, fb, tc), side);
        segment.b = add(boxEdge(ta, tb, fc), side);
        segment.link = i;
        const ImVec2 ends[2] = {segment.a, segment.b};
        if (outside(ends, 2, origin, regionMax)) continue;
        segments.push_back(segment);
    }
    auto linkAt = [&](ImVec2 screen) -> Link* {
        for (const Segment& s : segments){
            const float distance = s.wire ? distanceToWire(screen, s.curve) : distanceToSegment(screen, s.a, s.b);
            if (distance < 5.0f) return &links[s.link];
        }
        return nullptr;
    };
    auto groupTitleAt = [&](ImVec2 screen) -> const GroupBox* {
        for (auto it = groupBoxes.rbegin(); it != groupBoxes.rend(); ++it){
            if (screen.x >= it->a.x && screen.x <= it->b.x && screen.y >= it->a.y && screen.y <= it->a.y + it->title) return &*it;
        }
        return nullptr;
    };
    auto startNodeDrag = [&](){
        draggingNodes = true;
        dragMoved = false;
        dragStartMouse = mouse;
        dragStart.clear();
        for (const Node& n : nodes) if (n.selected) dragStart.push_back({n.id, n.position});
    };

    // input
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt){
        Node* node = nodeAt(mouse);
        selectionAtPress.clear();
        for (const Node& n : nodes) if (n.selected) selectionAtPress.push_back(n.id);
        PortRef pressedPort;
        const GroupBox* groupTitle = node ? nullptr : groupTitleAt(mouse);
        if (linkSource != 0){
            if (node){
                result.linkFrom = linkSource;
                result.linkTo = node->id;
            }
            linkSource = 0;
        }else if (portAt(mouse, pressedPort)){
            wiring = true;
            wireStart = pressedPort;
        }else if (groupTitle){
            result.clickedGroup = groupTitle->group->id;
            if (!io.KeyShift && !io.KeyCtrl){
                for (Node& n : nodes) n.selected = false;
                for (Link& l : links) l.selected = false;
            }
            for (Node& n : nodes){
                if (std::find(groupTitle->group->nodes.begin(), groupTitle->group->nodes.end(), n.id) != groupTitle->group->nodes.end()) n.selected = true;
            }
            result.selectionChanged = true;
            startNodeDrag();
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
            startNodeDrag();
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
        if (wiring){
            wiring = false;
            result.cancelled = true;
        }
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

    // the wire being dragged: the port under the mouse if it would take it
    PortRef wireTarget;
    bool wireTargetOk = false;
    if (wiring){
        PortRef under;
        if (portAt(mouse, under) && accepts(wireStart, under)){
            wireTarget = under;
            wireTargetOk = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            wiring = false;
            if (wireTargetOk){
                result.connectFrom = wireStart.output ? wireStart : wireTarget;
                result.connectTo = wireStart.output ? wireTarget : wireStart;
            }else if (!nodeAt(mouse) && !portAt(mouse, under) && hovered){
                result.wireDropped = true;
                result.wireSource = wireStart;
                result.wireDropCanvas = toCanvas(mouse);
            }
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

    // groups, behind everything
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * zoom;
    for (const GroupBox& box : groupBoxes){
        list->AddRectFilled(box.a, box.b, box.group->color, 6.0f * zoom);
        list->AddRectFilled(box.a, ImVec2(box.b.x, box.a.y + box.title), withAlpha(box.group->color, 2.5f), 6.0f * zoom, ImDrawFlags_RoundCornersTop);
        if (fontSize >= 5.0f){
            list->AddText(font, fontSize, ImVec2(box.a.x + 8.0f * zoom, box.a.y + (box.title - fontSize) * 0.5f), IM_COL32(220, 220, 220, 255),
                          box.group->title.c_str());
        }
    }

    // links
    for (const Segment& s : segments){
        const Link& link = links[s.link];
        const ImU32 color = link.selected ? IM_COL32(110, 170, 255, 255) : link.color;
        const float thickness = (link.selected ? 3.0f : 2.0f) * std::max(0.6f, zoom);
        if (s.wire){
            list->AddBezierCubic(s.curve[0], s.curve[1], s.curve[2], s.curve[3], color, thickness);
            continue;
        }
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

    // a wire being dragged, from its port to the mouse (or the port that would take it)
    ImU32 wireColor = IM_COL32(255, 255, 255, 220);
    if (wiring){
        auto from = byId.find(wireStart.node);
        if (from != byId.end()){
            for (const Port& port : wireStart.output ? from->second->outputs : from->second->inputs){
                if (port.id == wireStart.port) wireColor = port.color;
            }
            const ImVec2 start = toScreen(portPosition(*from->second, wireStart.port, wireStart.output));
            ImVec2 end = mouse;
            if (wireTargetOk){
                auto target = byId.find(wireTarget.node);
                if (target != byId.end()) end = toScreen(portPosition(*target->second, wireTarget.port, wireTarget.output));
            }
            ImVec2 curve[4];
            if (wireStart.output) wireCurve(start, end, zoom, curve);
            else wireCurve(end, start, zoom, curve);
            list->AddBezierCubic(curve[0], curve[1], curve[2], curve[3], wireColor, 2.5f * std::max(0.6f, zoom));
        }
    }

    // nodes (text scaled with the zoom)
    for (const Node& node : nodes){
        const ImVec2 a = toScreen(node.position);
        const ImVec2 b = toScreen(add(node.position, node.size));
        if (b.x < origin.x || a.x > regionMax.x || b.y < origin.y || a.y > regionMax.y) continue;
        if (isDataflow(node)){
            const float rounding = 6.0f * zoom;
            const float header = HEADER_HEIGHT * zoom;
            list->AddRectFilled(a, b, node.fill, rounding);
            list->AddRectFilled(a, ImVec2(b.x, a.y + header), node.header ? node.header : lighter(node.fill, 0.15f), rounding, ImDrawFlags_RoundCornersTop);
            if (node.error) list->AddRect(a, b, IM_COL32(230, 70, 60, 255), rounding, 0, 2.0f);
            if (node.highlighted) list->AddRect(a, b, IM_COL32(90, 200, 255, 255), rounding, 0, 2.5f);
            if (node.selected) list->AddRect(add(a, ImVec2(-2, -2)), add(b, ImVec2(2, 2)), IM_COL32(110, 170, 255, 255), rounding, 0, 2.0f);
            if (fontSize >= 5.0f){
                const ImVec2 at(a.x + PADDING * zoom, a.y + (header - fontSize) * 0.5f);
                float labelRight = b.x - PADDING * zoom;
                if (!node.cost.empty() && alpha > 0.0f){
                    const float costSize = fontSize * 0.85f;
                    const ImVec2 costText = font->CalcTextSizeA(costSize, 1e9f, 0.0f, node.cost.c_str());
                    labelRight -= costText.x + PADDING * zoom;
                    list->AddText(font, costSize, ImVec2(b.x - PADDING * zoom - costText.x, a.y + (header - costSize) * 0.5f),
                                  withAlpha(IM_COL32(190, 190, 160, 255), alpha), node.cost.c_str());
                }
                list->PushClipRect(a, ImVec2(std::max(a.x, labelRight), a.y + header), true);
                list->AddText(font, fontSize, at, node.text, node.label.c_str());
                list->PopClipRect();
            }
            // the ports: circles on the edges, their names inside
            if (alpha > 0.0f){
                for (int side = 0; side < 2; side++){
                    for (const Port& port : side ? node.outputs : node.inputs){
                        const ImVec2 p = toScreen(portPosition(node, port.id, side == 1));
                        bool dim = false;
                        if (wiring) dim = !(wireStart.node == node.id && wireStart.port == port.id) && !accepts(wireStart, {node.id, port.id, side == 1});
                        const float portAlpha = alpha * (dim ? 0.3f : 1.0f);
                        const float radius = 5.0f * std::max(0.7f, zoom);
                        if (port.connected) list->AddCircleFilled(p, radius, withAlpha(port.color, portAlpha));
                        else list->AddCircle(p, radius, withAlpha(port.color, portAlpha), 0, 2.0f);
                        if (fontSize >= 5.0f && !port.label.empty()){
                            const float labelSize = fontSize * 0.9f;
                            const ImVec2 text = font->CalcTextSizeA(labelSize, 1e9f, 0.0f, port.label.c_str());
                            const float x = side ? p.x - radius - 4.0f * zoom - text.x : p.x + radius + 4.0f * zoom;
                            list->AddText(font, labelSize, ImVec2(x, p.y - text.y * 0.5f), withAlpha(IM_COL32(200, 200, 200, 255), portAlpha), port.label.c_str());
                        }
                    }
                }
            }
            if (node.thumbnail){
                ImVec2 lo, hi;
                thumbnailRect(node, lo, hi);
                const ImVec2 sa = toScreen(lo), sb = toScreen(hi);
                list->AddRectFilled(sa, sb, IM_COL32(25, 25, 28, 255));
                if (drawThumbnail) drawThumbnail(list, node.id, sa, sb);
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
    list->PopClipRect();
    return result;
}
