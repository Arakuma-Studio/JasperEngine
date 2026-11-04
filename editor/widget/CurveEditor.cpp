// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "CurveEditor.h"

#include "shortcuts/CommandCatalog.h"

#include "Theme.h"
#include "window/AnimationWindowClipCurves.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::ui;
using curveops::Hit;
using curveops::KeyRef;
using curveops::Side;

namespace {

    constexpr float kKeyRadius = 4.0f;
    constexpr float kHandleLength = 40.0f;
    constexpr float kHitRadius = 7.0f;

    ImU32 withAlpha(ImU32 color, float alpha) {
        const ImU32 a = (ImU32)std::clamp((int)(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha), 0, 255);
        return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    }

    std::vector<const Curve*> constCurves(const std::vector<CurveBinding>& curves) {
        std::vector<const Curve*> out;
        for (const CurveBinding& b : curves) out.push_back(b.curve);
        return out;
    }

    std::vector<Curve*> mutableCurves(std::vector<CurveBinding>& curves) {
        std::vector<Curve*> out;
        for (CurveBinding& b : curves) out.push_back(b.curve);
        return out;
    }

    std::vector<bool> editableCurves(const std::vector<CurveBinding>& curves) {
        std::vector<bool> out;
        for (const CurveBinding& b : curves) out.push_back(b.editable && b.visible);
        return out;
    }

    bool isSelected(const std::vector<KeyRef>& selection, KeyRef ref) {
        return std::find(selection.begin(), selection.end(), ref) != selection.end();
    }

}

const char* ui::wrapModeLabel(CurveWrapMode mode) {
    switch (mode) {
        case CurveWrapMode::Clamp: return "Clamp";
        case CurveWrapMode::Loop: return "Loop";
        case CurveWrapMode::PingPong: return "Ping Pong";
        case CurveWrapMode::Linear: return "Linear";
        case CurveWrapMode::LoopWithOffset: return "Loop with Offset";
    }
    return "Clamp";
}

void CurveEditor::setRange(const curveops::CurveRange* r) {
    hasRange = r != nullptr;
    if (r) range = *r;
}

int CurveEditor::activeCurve(const std::vector<CurveBinding>& curves) const {
    for (const KeyRef& ref : selection) {
        if (ref.curve >= 0 && (size_t)ref.curve < curves.size() && curves[(size_t)ref.curve].editable) return ref.curve;
    }
    for (size_t i = 0; i < curves.size(); i++) {
        if (curves[i].editable && curves[i].visible) return (int)i;
    }
    return -1;
}

void CurveEditor::frame(const std::vector<CurveBinding>& curves) {
    std::vector<const Curve*> visible;
    std::vector<KeyRef> sel;
    for (size_t i = 0; i < curves.size(); i++) {
        visible.push_back(curves[i].curve);
    }
    for (const KeyRef& ref : selection) sel.push_back(ref);
    curveops::View framedView = curveops::frame(visible, sel, view, 0.1f, 0.1f);
    if (hasRange && sel.empty()) {
        // the range in view too
        framedView.t0 = std::min(framedView.t0, std::min(range.t0, range.t1));
        framedView.t1 = std::max(framedView.t1, std::max(range.t0, range.t1));
        framedView.v0 = std::min(framedView.v0, std::min(range.v0, range.v1));
        framedView.v1 = std::max(framedView.v1, std::max(range.v0, range.v1));
    }
    view.t0 = framedView.t0;
    view.t1 = framedView.t1;
    view.v0 = framedView.v0;
    view.v1 = framedView.v1;
    framed = true;
}

void CurveEditor::beginGesture(const std::vector<CurveBinding>& curves, CurveEditorResult& result) {
    originals.clear();
    for (const CurveBinding& b : curves) originals.push_back(*b.curve);
    originalSelection = selection;
    result.began = true;
}

void CurveEditor::restoreGesture(std::vector<CurveBinding>& curves) {
    for (size_t i = 0; i < curves.size() && i < originals.size(); i++) *curves[i].curve = originals[i];
    selection = originalSelection;
}

void CurveEditor::drawGrid(ImDrawList* drawList) {
    const ImU32 minor = IM_COL32(255, 255, 255, 18);
    const ImU32 axis = IM_COL32(255, 255, 255, 60);
    const ImU32 text = IM_COL32(200, 200, 200, 160);
    const float timeStep = clipcurves::niceStep(view.t1 - view.t0, view.x1 - view.x0, 60.0f);
    for (float t : clipcurves::ticks(view.t0, view.t1, timeStep)) {
        const float x = view.toX(t);
        drawList->AddLine(ImVec2(x, view.y0), ImVec2(x, view.y1), std::fabs(t) < timeStep * 0.01f ? axis : minor);
        char label[32];
        std::snprintf(label, sizeof(label), "%g", t);
        drawList->AddText(ImVec2(x + 2.0f, view.y1 - ImGui::GetTextLineHeight()), text, label);
    }
    const float valueStep = clipcurves::niceStep(view.v1 - view.v0, view.y1 - view.y0, 30.0f);
    for (float v : clipcurves::ticks(view.v0, view.v1, valueStep)) {
        const float y = view.toY(v);
        drawList->AddLine(ImVec2(view.x0, y), ImVec2(view.x1, y), std::fabs(v) < valueStep * 0.01f ? axis : minor);
        char label[32];
        std::snprintf(label, sizeof(label), "%g", v);
        drawList->AddText(ImVec2(view.x0 + 2.0f, y - ImGui::GetTextLineHeight()), text, label);
    }
    if (hasRange) {
        const ImVec2 a(view.toX(range.t0), view.toY(range.v1));
        const ImVec2 b(view.toX(range.t1), view.toY(range.v0));
        drawList->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)),
                          IM_COL32(255, 255, 255, range.clamp ? 110 : 60), 0.0f, 0, range.clamp ? 1.5f : 1.0f);
    }
}

void CurveEditor::drawCurves(ImDrawList* drawList, const std::vector<CurveBinding>& curves) {
    const float step = 2.0f;
    // the min-max bands first, under the curves
    for (const CurveBinding& b : curves) {
        if (!b.visible || b.partner < 0 || (size_t)b.partner >= curves.size() || b.curve->keys.empty()) continue;
        const Curve& other = *curves[(size_t)b.partner].curve;
        if (other.keys.empty()) continue;
        const ImU32 band = withAlpha(b.color, 0.25f);
        for (float x = view.x0; x < view.x1; x += step) {
            const float ta = view.toTime(x), tb = view.toTime(std::min(x + step, view.x1));
            const ImVec2 p0(x, view.toY(b.curve->evaluate(ta))), p1(std::min(x + step, view.x1), view.toY(b.curve->evaluate(tb)));
            const ImVec2 q0(x, view.toY(other.evaluate(ta))), q1(p1.x, view.toY(other.evaluate(tb)));
            drawList->AddTriangleFilled(p0, p1, q1, band);
            drawList->AddTriangleFilled(p0, q1, q0, band);
        }
    }
    for (size_t c = 0; c < curves.size(); c++) {
        const CurveBinding& b = curves[c];
        if (!b.visible || b.curve->keys.empty()) continue;
        const Curve& curve = *b.curve;
        const float first = curve.keys.front().time, last = curve.keys.back().time;
        ImVec2 previous(view.x0, view.toY(curve.evaluate(view.toTime(view.x0))));
        for (float x = view.x0 + step; x <= view.x1 + step * 0.5f; x += step) {
            const float t = view.toTime(x);
            const ImVec2 point(x, view.toY(curve.evaluate(t)));
            // outside the keys the wrap is ghosted
            const bool inside = t >= first && view.toTime(x - step) <= last;
            drawList->AddLine(previous, point, inside ? b.color : withAlpha(b.color, 0.35f), inside ? 2.0f : 1.0f);
            previous = point;
        }
        if (!b.editable) continue;
        for (size_t k = 0; k < curve.keys.size(); k++) {
            const CurveKey& key = curve.keys[k];
            const ImVec2 at(view.toX(key.time), view.toY(key.value));
            const bool sel = isSelected(selection, {(int)c, (int)k});
            if (sel) {
                for (Side side : {Side::In, Side::Out}) {
                    if ((side == Side::In && k == 0) || (side == Side::Out && k + 1 == curve.keys.size())) continue;
                    float hx, hy;
                    curveops::handlePosition(key, side, view, kHandleLength, hx, hy);
                    drawList->AddLine(at, ImVec2(hx, hy), IM_COL32(200, 200, 200, 200));
                    drawList->AddCircleFilled(ImVec2(hx, hy), 3.5f, IM_COL32(230, 230, 230, 255));
                }
            }
            drawList->AddQuadFilled(ImVec2(at.x, at.y - kKeyRadius), ImVec2(at.x + kKeyRadius, at.y), ImVec2(at.x, at.y + kKeyRadius),
                                    ImVec2(at.x - kKeyRadius, at.y), sel ? IM_COL32(255, 220, 80, 255) : IM_COL32(240, 240, 240, 255));
            drawList->AddQuad(ImVec2(at.x, at.y - kKeyRadius), ImVec2(at.x + kKeyRadius, at.y), ImVec2(at.x, at.y + kKeyRadius),
                              ImVec2(at.x - kKeyRadius, at.y), IM_COL32(0, 0, 0, 255));
        }
    }
}

void CurveEditor::drawKeyMenu(std::vector<CurveBinding>& curves, CurveEditorResult& result) {
    if (!ImGui::BeginPopup("##curveKeyMenu")) return;
    // one action on every key the menu opened on
    auto apply = [&](auto&& op) {
        beginGesture(curves, result);
        for (const KeyRef& ref : menuSelection) {
            if (ref.curve < 0 || (size_t)ref.curve >= curves.size()) continue;
            Curve& curve = *curves[(size_t)ref.curve].curve;
            if (ref.key >= 0 && (size_t)ref.key < curve.keys.size()) op(curve, (size_t)ref.key);
        }
        if (hasRange) {
            for (CurveBinding& b : curves) curveops::clampToRange(*b.curve, range);
        }
        result.changed = true;
        result.ended = true;
    };
    const CurveKey* first = nullptr;
    if (!menuSelection.empty()) {
        const KeyRef& ref = menuSelection.front();
        if (ref.curve >= 0 && (size_t)ref.curve < curves.size() && ref.key >= 0 && (size_t)ref.key < curves[(size_t)ref.curve].curve->keys.size()) {
            first = &curves[(size_t)ref.curve].curve->keys[(size_t)ref.key];
        }
    }
    const bool joined = first && !first->broken() && first->inMode() == first->outMode();
    if (ImGui::MenuItem("Clamped Auto", nullptr, joined && first->inMode() == CurveTangentMode::ClampedAuto)) {
        apply([](Curve& c, size_t k) { curveops::setClampedAuto(c, k); });
    }
    if (ImGui::MenuItem("Auto", nullptr, joined && first->inMode() == CurveTangentMode::Auto)) {
        apply([](Curve& c, size_t k) { curveops::setAuto(c, k); });
    }
    if (ImGui::MenuItem("Free Smooth", nullptr, joined && first->inMode() == CurveTangentMode::Free)) {
        apply([](Curve& c, size_t k) { curveops::setFreeSmooth(c, k); });
    }
    if (ImGui::MenuItem("Flat", nullptr, joined && first->inMode() == CurveTangentMode::Free && first->inTangent == 0.0f && first->outTangent == 0.0f)) {
        apply([](Curve& c, size_t k) { curveops::setFlat(c, k); });
    }
    const bool broken = first && first->broken();
    if (ImGui::MenuItem("Broken", nullptr, broken)) {
        apply([broken](Curve& c, size_t k) { curveops::setBroken(c, k, !broken); });
    }
    ImGui::Separator();
    struct SideMenu {
        const char* label;
        Side side;
    };
    for (const SideMenu& menu : {SideMenu{"Left Tangent", Side::In}, SideMenu{"Right Tangent", Side::Out}, SideMenu{"Both Tangents", Side::Both}}) {
        if (!ImGui::BeginMenu(menu.label)) continue;
        struct ModeItem {
            const char* label;
            CurveTangentMode mode;
        };
        for (const ModeItem& item : {ModeItem{"Free", CurveTangentMode::Free}, ModeItem{"Linear", CurveTangentMode::Linear},
                                     ModeItem{"Constant", CurveTangentMode::Constant}}) {
            bool checked = false;
            if (first) {
                const bool in = first->inMode() == item.mode, out = first->outMode() == item.mode;
                checked = menu.side == Side::In ? in : (menu.side == Side::Out ? out : (in && out));
            }
            if (ImGui::MenuItem(item.label, nullptr, checked)) {
                const Side side = menu.side;
                const CurveTangentMode mode = item.mode;
                apply([side, mode](Curve& c, size_t k) { curveops::setTangentMode(c, k, side, mode); });
            }
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete Key", CommandCatalog::editor().menuShortcutText("curve.delete").c_str())) {
        beginGesture(curves, result);
        std::vector<Curve*> all = mutableCurves(curves);
        selection = menuSelection;
        curveops::deleteKeys(all, selection);
        result.changed = true;
        result.ended = true;
    }
    ImGui::EndPopup();
}

void CurveEditor::drawBottomBar(std::vector<CurveBinding>& curves, CurveEditorResult& result) {
    // the selected key's Time and Value (one key)
    if (selection.size() == 1 && selection.front().curve >= 0 && (size_t)selection.front().curve < curves.size()) {
        const KeyRef ref = selection.front();
        Curve& curve = *curves[(size_t)ref.curve].curve;
        if (ref.key >= 0 && (size_t)ref.key < curve.keys.size()) {
            CurveKey key = curve.keys[(size_t)ref.key];
            bool edited = false;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Time");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Theme::dpi(70.0f));
            edited |= ImGui::DragFloat("##keyTime", &key.time, 0.01f, 0.0f, 0.0f, "%.3f");
            const bool activatedTime = ImGui::IsItemActivated(), deactivatedTime = ImGui::IsItemDeactivated();
            ImGui::SameLine();
            ImGui::TextUnformatted("Value");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(Theme::dpi(70.0f));
            edited |= ImGui::DragFloat("##keyValue", &key.value, 0.01f, 0.0f, 0.0f, "%.3f");
            const bool activatedValue = ImGui::IsItemActivated(), deactivatedValue = ImGui::IsItemDeactivated();
            if ((activatedTime || activatedValue) && !numberEditing) {
                numberEditing = true;
                beginGesture(curves, result);
            }
            if (edited && std::isfinite(key.time) && std::isfinite(key.value)) {
                if (!numberEditing) {
                    numberEditing = true;
                    beginGesture(curves, result);
                }
                CurveKey clamped = key;
                if (hasRange && range.clamp) {
                    clamped.time = std::clamp(clamped.time, std::min(range.t0, range.t1), std::max(range.t0, range.t1));
                    clamped.value = std::clamp(clamped.value, std::min(range.v0, range.v1), std::max(range.v0, range.v1));
                }
                const int moved = curve.moveKey((size_t)ref.key, clamped);
                if (moved >= 0) selection.front().key = moved;
                result.changed = true;
            }
            if ((deactivatedTime || deactivatedValue) && numberEditing) {
                numberEditing = false;
                result.ended = true;
            }
            ImGui::SameLine();
        }
    }
    // the wraps of the curve acted on
    const int active = activeCurve(curves);
    if (active < 0) return;
    Curve& curve = *curves[(size_t)active].curve;
    const CurveWrapMode modes[] = {CurveWrapMode::Clamp, CurveWrapMode::Loop, CurveWrapMode::PingPong, CurveWrapMode::Linear,
                                   CurveWrapMode::LoopWithOffset};
    for (int pre = 0; pre < 2; pre++) {
        CurveWrapMode& wrap = pre == 0 ? curve.preWrapMode : curve.postWrapMode;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(pre == 0 ? "Pre Wrap" : "Post Wrap");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(Theme::dpi(110.0f));
        if (ImGui::BeginCombo(pre == 0 ? "##preWrap" : "##postWrap", wrapModeLabel(wrap))) {
            for (CurveWrapMode mode : modes) {
                if (ImGui::Selectable(wrapModeLabel(mode), mode == wrap) && mode != wrap) {
                    beginGesture(curves, result);
                    wrap = mode;
                    result.changed = true;
                    result.ended = true;
                }
            }
            ImGui::EndCombo();
        }
        if (pre == 0) ImGui::SameLine();
    }
}

bool CurveEditor::canRun(Command command, const std::vector<CurveBinding>& curves) const {
    if (drag != Drag::None) return false;
    switch (command) {
        case Command::Frame:
            return true;
        case Command::Delete:
            return !selection.empty();
        case Command::Copy:
            return !selection.empty() && activeCurve(curves) >= 0;
        case Command::Paste:
            return !clipboard.empty() && activeCurve(curves) >= 0;
    }
    return false;
}

void CurveEditor::runCommand(Command command, std::vector<CurveBinding>& curves, CurveEditorResult& result, bool hovered, float mouseX) {
    const int active = activeCurve(curves);
    switch (command) {
        case Command::Frame:
            frame(curves);
            break;
        case Command::Delete: {
            beginGesture(curves, result);
            std::vector<Curve*> targets = mutableCurves(curves);
            curveops::deleteKeys(targets, selection);
            result.changed = true;
            result.ended = true;
            break;
        }
        case Command::Copy:
            clipboard = curveops::copyKeys(*curves[(size_t)active].curve, selection, active);
            break;
        case Command::Paste: {
            beginGesture(curves, result);
            const float time = hovered ? view.toTime(mouseX) : dragStartTime;
            const std::vector<int> pasted = curveops::pasteKeys(*curves[(size_t)active].curve, clipboard, time, hasRange ? &range : nullptr);
            selection.clear();
            for (int index : pasted) selection.push_back({active, index});
            result.changed = true;
            result.ended = true;
            break;
        }
    }
}

CurveEditorResult CurveEditor::draw(const char* id, std::vector<CurveBinding>& curves, ImVec2 size) {
    CurveEditorResult result;
    ImGui::PushID(id);
    const float barHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (size.x <= 0.0f) size.x = avail.x;
    if (size.y <= 0.0f) size.y = std::max(avail.y - barHeight, Theme::dpi(80.0f));
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + size.x, min.y + size.y);
    ImGui::InvisibleButton("##graph", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    view.x0 = min.x;
    view.y0 = min.y;
    view.x1 = max.x;
    view.y1 = max.y;
    if (!framed) frame(curves);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(min, max, true);
    drawList->AddRectFilled(min, max, IM_COL32(28, 28, 30, 255));
    drawGrid(drawList);
    drawCurves(drawList, curves);

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const std::vector<const Curve*> all = constCurves(curves);
    const std::vector<bool> editable = editableCurves(curves);

    // zoom around the cursor: the wheel both axes, Ctrl time only, Shift values only
    if (hovered && io.MouseWheel != 0.0f) {
        const float factor = std::pow(0.85f, io.MouseWheel);
        view = curveops::zoom(view, mouse.x, mouse.y, io.KeyShift ? 1.0f : factor, io.KeyCtrl ? 1.0f : factor);
    }

    if (hovered && drag == Drag::None) {
        const bool pan = ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
        if (pan) {
            drag = Drag::Pan;
            dragStart = mouse;
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const Hit hit = curveops::hitTest(all, editable, view, mouse.x, mouse.y, kHitRadius, kHandleLength, selection);
            int target = hit.kind == Hit::Kind::Curve ? hit.curve : -1;
            float time = view.toTime(mouse.x);
            if (target < 0 && hit.kind == Hit::Kind::None) target = activeCurve(curves);
            if (target >= 0 && (hit.kind == Hit::Kind::Curve || hit.kind == Hit::Kind::None)) {
                beginGesture(curves, result);
                Curve& curve = *curves[(size_t)target].curve;
                const curveops::CurveRange* r = hasRange ? &range : nullptr;
                const int index = hit.kind == Hit::Kind::Curve ? curveops::addKeyOnCurve(curve, time, r)
                                                               : curveops::addKeyAt(curve, time, view.toValue(mouse.y), r);
                if (index >= 0) selection = {{target, index}};
                result.changed = index >= 0;
                result.ended = true;
                if (!result.changed) result.began = false;
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Hit hit = curveops::hitTest(all, editable, view, mouse.x, mouse.y, kHitRadius, kHandleLength, selection);
            dragStart = mouse;
            dragStartTime = view.toTime(mouse.x);
            dragStartValue = view.toValue(mouse.y);
            if (hit.kind == Hit::Kind::Key) {
                const KeyRef ref{hit.curve, hit.key};
                if (io.KeyShift || io.KeyCtrl) {
                    auto it = std::find(selection.begin(), selection.end(), ref);
                    if (it != selection.end()) selection.erase(it);
                    else selection.push_back(ref);
                } else if (!isSelected(selection, ref)) {
                    selection = {ref};
                }
                if (isSelected(selection, ref)) {
                    drag = Drag::Keys;
                    originals.clear();
                    for (const CurveBinding& b : curves) originals.push_back(*b.curve);
                    originalSelection = selection;
                }
            } else if (hit.kind == Hit::Kind::InHandle || hit.kind == Hit::Kind::OutHandle) {
                drag = Drag::Handle;
                dragKey = {hit.curve, hit.key};
                dragSide = hit.kind == Hit::Kind::InHandle ? Side::In : Side::Out;
                originals.clear();
                for (const CurveBinding& b : curves) originals.push_back(*b.curve);
                originalSelection = selection;
            } else {
                if (!io.KeyShift && !io.KeyCtrl) selection.clear();
                drag = Drag::Box;
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            const Hit hit = curveops::hitTest(all, editable, view, mouse.x, mouse.y, kHitRadius, kHandleLength, selection);
            if (hit.kind == Hit::Kind::Key) {
                const KeyRef ref{hit.curve, hit.key};
                if (!isSelected(selection, ref)) selection = {ref};
                menuSelection = selection;
                ImGui::OpenPopup("##curveKeyMenu");
            } else {
                dragStartTime = view.toTime(mouse.x);
                dragStartValue = view.toValue(mouse.y);
                ImGui::OpenPopup("##curveCanvasMenu");
            }
        }
    }

    // the drag in progress
    const bool moved = std::fabs(mouse.x - dragStart.x) > 2.0f || std::fabs(mouse.y - dragStart.y) > 2.0f;
    switch (drag) {
        case Drag::Pan:
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                view = curveops::pan(view, mouse.x - dragStart.x, mouse.y - dragStart.y);
                dragStart = mouse;
            } else {
                drag = Drag::None;
            }
            break;
        case Drag::Keys:
        case Drag::Handle:
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                // Esc: the curves as the drag found them, nothing recorded
                if (gestureOpen) {
                    restoreGesture(curves);
                    result.changed = true;
                    result.cancelled = true;
                    result.ended = true;
                }
                gestureOpen = false;
                drag = Drag::None;
                break;
            }
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (!moved) break;
                if (!result.began && originals.size() == curves.size() && !gestureOpen) {
                    gestureOpen = true;
                    result.began = true;
                }
                std::vector<Curve*> targets = mutableCurves(curves);
                if (drag == Drag::Keys) {
                    selection = originalSelection;
                    curveops::moveKeys(targets, originals, selection, view.toTime(mouse.x) - dragStartTime, view.toValue(mouse.y) - dragStartValue,
                                       hasRange ? &range : nullptr);
                } else if (dragKey.curve >= 0 && (size_t)dragKey.curve < curves.size()) {
                    Curve& curve = *curves[(size_t)dragKey.curve].curve;
                    if (dragKey.key >= 0 && (size_t)dragKey.key < curve.keys.size()) {
                        const float slope = curveops::slopeTowards(curve.keys[(size_t)dragKey.key], dragSide, view, mouse.x, mouse.y);
                        curveops::dragTangent(curve, (size_t)dragKey.key, dragSide, slope);
                    }
                }
                result.changed = true;
            } else {
                if (gestureOpen) result.ended = true;
                gestureOpen = false;
                drag = Drag::None;
            }
            break;
        case Drag::Box:
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const ImVec2 a(std::min(dragStart.x, mouse.x), std::min(dragStart.y, mouse.y));
                const ImVec2 b(std::max(dragStart.x, mouse.x), std::max(dragStart.y, mouse.y));
                drawList->AddRectFilled(a, b, IM_COL32(120, 160, 255, 40));
                drawList->AddRect(a, b, IM_COL32(120, 160, 255, 160));
            } else {
                if (moved) {
                    const float ta = std::min(view.toTime(dragStart.x), view.toTime(mouse.x)), tb = std::max(view.toTime(dragStart.x), view.toTime(mouse.x));
                    const float va = std::min(view.toValue(dragStart.y), view.toValue(mouse.y)), vb = std::max(view.toValue(dragStart.y), view.toValue(mouse.y));
                    for (size_t c = 0; c < curves.size(); c++) {
                        if (!editable[c]) continue;
                        for (size_t k = 0; k < curves[c].curve->keys.size(); k++) {
                            const CurveKey& key = curves[c].curve->keys[k];
                            const KeyRef ref{(int)c, (int)k};
                            if (key.time >= ta && key.time <= tb && key.value >= va && key.value <= vb && !isSelected(selection, ref)) {
                                selection.push_back(ref);
                            }
                        }
                    }
                }
                drag = Drag::None;
            }
            break;
        case Drag::None:
            break;
    }
    drawList->PopClipRect();

    // the catalog's curve.* commands since the last draw
    for (const Command command : requested) {
        if (canRun(command, curves)) runCommand(command, curves, result, hovered, mouse.x);
    }
    requested.clear();

    drawKeyMenu(curves, result);
    if (ImGui::BeginPopup("##curveCanvasMenu")) {
        if (ImGui::MenuItem("Add Key")) {
            const int active = activeCurve(curves);
            if (active >= 0) {
                beginGesture(curves, result);
                const int index = curveops::addKeyAt(*curves[(size_t)active].curve, dragStartTime, dragStartValue, hasRange ? &range : nullptr);
                if (index >= 0) selection = {{active, index}};
                result.changed = index >= 0;
                result.ended = true;
                if (!result.changed) result.began = false;
            }
        }
        if (ImGui::MenuItem("Frame All", CommandCatalog::editor().menuShortcutText("curve.frame").c_str())) {
            selection.clear();
            frame(curves);
        }
        ImGui::EndPopup();
    }

    drawBottomBar(curves, result);
    ImGui::PopID();
    return result;
}
