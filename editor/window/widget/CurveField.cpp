// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The curve field (CurveField.h): the inline preview and the popup editor. The popup shows the
// preset thumbnails and Fit, the canvas (nice ticks from clipcurves, the curves sampled with
// AnimationCurve::evaluate, dimmer outside their keys where the wrap modes play, the band
// between two curves, key points, the selected key's tangent handles), the selected key's
// tangent modes and Time / Value, and the wrap combos. Every edit goes through curvefield
// (CurveFieldLogic.h). Mouse: left drags keys and handles, double-click adds a key, right opens
// the key / canvas menu, middle (or Alt + left) pans, the wheel zooms (Shift: time only, Ctrl:
// value only). Keys: Delete / Backspace delete the selected key, F fits.

#include "CurveField.h"
#include "CurveFieldLogic.h"

#include "imgui.h"
#include "external/IconsFontAwesome6.h"
#include "Theme.h"
#include "window/AnimationWindowClipCurves.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::curvefield;

namespace {

    enum class Drag : uint8_t{ None, Key, Tangent, Pan };

    // the popup's UI state of one field (by ImGui ID)
    struct CurveEditorState{
        View view;
        bool viewValid = false;          // fitted when the popup opens
        int selCurve = -1;               // 0: the curve, 1: the second one
        int selKey = -1;
        Drag drag = Drag::None;
        TangentSide dragSide = TangentSide::In;
        float grabDx = 0.0f, grabDy = 0.0f;   // the mouse minus the dragged key, pixels
        bool dragChanged = false;        // the running drag modified the curve
        bool fieldPending = false;       // Time / Value changed and is still being edited
        std::vector<char> broken[2];     // Free keys (CurveKey has no flag), per curve
        int menuCurve = 0;               // the canvas menu's target and time
        float menuTime = 0.0f;
    };

    std::unordered_map<ImGuiID, CurveEditorState> states;

    const char* const WRAP_NAMES[3] = {"Clamp", "Loop", "Ping Pong"};
    const char* const MODE_NAMES[4] = {"Auto", "Linear", "Constant", "Free"};

    ImU32 color(const ImVec4& c, float alpha = 1.0f){
        return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
    }

    ImU32 curveColor(float alpha = 1.0f){
        return color(ImVec4(0.48f, 0.84f, 0.38f, 1.0f), alpha);
    }

    ImVec2 toImVec(Point p){
        return ImVec2(p.x, p.y);
    }

    std::string formatTick(float value, float step){
        int decimals = 0;
        if (step < 1.0f && step > 0.0f) decimals = (int)std::ceil(-std::log10(step) - 1e-4f);
        decimals = std::max(0, std::min(6, decimals));
        char text[32];
        std::snprintf(text, sizeof(text), "%.*f", decimals, value);
        return text;
    }

    void syncFlags(std::vector<char>& flags, size_t count){
        if (flags.size() != count) flags.assign(count, 0);
    }

    void moveFlag(std::vector<char>& flags, int from, int to){
        if (from < 0 || to < 0 || (size_t)from >= flags.size() || (size_t)to >= flags.size() || from == to) return;
        const char flag = flags[(size_t)from];
        flags.erase(flags.begin() + from);
        flags.insert(flags.begin() + to, flag);
    }

    // the curve polyline across the view, every `stepPx` pixels: dim outside the keys (the wrap
    // modes), `bright` over them
    void drawCurve(ImDrawList* drawList, const AnimationCurve& curve, const View& view, ImU32 bright, ImU32 dim, float thickness,
                   float stepPx){
        const float x0 = view.canvas.x, x1 = view.canvas.x + view.canvas.width;
        if (!(x1 >= x0) || !std::isfinite(x0) || !std::isfinite(x1)) return;
        stepPx = std::max(0.5f, stepPx);
        std::vector<ImVec2> all;
        all.reserve((size_t)(view.canvas.width / stepPx) + 2);
        for (float x = x0;; x += stepPx){
            x = std::min(x, x1);
            all.push_back(ImVec2(x, view.valueToY(curve.evaluate(view.xToTime(x)))));
            if (x >= x1) break;
        }
        drawList->AddPolyline(all.data(), (int)all.size(), dim, ImDrawFlags_None, thickness);
        if (curve.keys.size() < 2) return;

        const float k0 = std::max(x0, view.timeToX(curve.firstTime()));
        const float k1 = std::min(x1, view.timeToX(curve.lastTime()));
        if (!(k1 > k0)) return;
        std::vector<ImVec2> keyed;
        for (float x = k0;; x += stepPx){
            x = std::min(x, k1);
            keyed.push_back(ImVec2(x, view.valueToY(curve.evaluate(view.xToTime(x)))));
            if (x >= k1) break;
        }
        drawList->AddPolyline(keyed.data(), (int)keyed.size(), bright, ImDrawFlags_None, thickness);
    }

    // the band between two curves
    void drawBand(ImDrawList* drawList, const AnimationCurve& a, const AnimationCurve& b, const View& view, ImU32 fill, float stepPx){
        const float x0 = view.canvas.x, x1 = view.canvas.x + view.canvas.width;
        if (!(x1 >= x0) || !std::isfinite(x0) || !std::isfinite(x1)) return;
        stepPx = std::max(0.5f, stepPx);
        float px = x0;
        float pa = view.valueToY(a.evaluate(view.xToTime(px)));
        float pb = view.valueToY(b.evaluate(view.xToTime(px)));
        while (px < x1){
            const float x = std::min(px + stepPx, x1);
            const float t = view.xToTime(x);
            const float ya = view.valueToY(a.evaluate(t));
            const float yb = view.valueToY(b.evaluate(t));
            drawList->AddQuadFilled(ImVec2(px, pa), ImVec2(x, ya), ImVec2(x, yb), ImVec2(px, pb), fill);
            px = x;
            pa = ya;
            pb = yb;
        }
    }

    void drawThumbnail(ImDrawList* drawList, const AnimationCurve& curve, const AnimationCurve* second, const Rect& rect, ImU32 line){
        const View view = fitView(curve, rect, second, 0.0f, 0.15f);
        drawList->PushClipRect(ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.width, rect.y + rect.height), true);
        if (second) drawBand(drawList, curve, *second, view, curveColor(0.22f), 2.0f);
        drawCurve(drawList, curve, view, line, line, 1.5f, 2.0f);
        if (second) drawCurve(drawList, *second, view, line, line, 1.5f, 2.0f);
        drawList->PopClipRect();
    }

    // the key nearest to `mouse` over the shown curves: (curve, key) or (-1, -1)
    void hitAnyKey(AnimationCurve* const curves[2], int count, const View& view, Point mouse, float radius, int& outCurve, int& outKey){
        outCurve = outKey = -1;
        float best = radius * radius;
        for (int c = 0; c < count; c++){
            const int k = hitKey(*curves[c], view, mouse, radius);
            if (k < 0) continue;
            const Point p = keyPosition(view, curves[c]->keys[(size_t)k]);
            const float d = (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y);
            if (d <= best){
                best = d;
                outCurve = c;
                outKey = k;
            }
        }
    }

    // the curve nearer to `value` at `time` (two curves), else 0
    int nearerCurve(AnimationCurve* const curves[2], int count, float time, float value){
        if (count < 2) return 0;
        const float d0 = std::fabs(curves[0]->evaluate(time) - value);
        const float d1 = std::fabs(curves[1]->evaluate(time) - value);
        return d1 < d0 ? 1 : 0;
    }

    void markEdit(CurveFieldResult& result, AnimationCurve& curve, bool finished){
        curve.normalize();
        result.changed = true;
        if (finished) result.finished = true;
    }

    void addKeyAt(CurveEditorState& state, AnimationCurve* const curves[2], int target, float time, CurveFieldResult& result){
        AnimationCurve& curve = *curves[target];
        const int index = insertKey(curve, time);
        if (index < 0) return;
        std::vector<char>& flags = state.broken[target];
        syncFlags(flags, curve.keys.size() - 1);
        flags.insert(flags.begin() + index, 0);
        state.selCurve = target;
        state.selKey = index;
        markEdit(result, curve, true);
    }

    void deleteSelected(CurveEditorState& state, AnimationCurve* const curves[2], CurveFieldResult& result){
        if (state.selCurve < 0 || state.selKey < 0) return;
        AnimationCurve& curve = *curves[state.selCurve];
        std::vector<char>& flags = state.broken[state.selCurve];
        if (!deleteKey(curve, (size_t)state.selKey)) return;
        if ((size_t)state.selKey < flags.size()) flags.erase(flags.begin() + state.selKey);
        state.selKey = -1;
        state.selCurve = -1;
        markEdit(result, curve, true);
    }

    void applyMode(CurveEditorState& state, AnimationCurve* const curves[2], TangentMode mode, CurveFieldResult& result){
        if (state.selCurve < 0 || state.selKey < 0) return;
        AnimationCurve& curve = *curves[state.selCurve];
        std::vector<char>& flags = state.broken[state.selCurve];
        syncFlags(flags, curve.keys.size());
        if (mode == TangentMode::Free){
            setFree(curve, (size_t)state.selKey, flags);
        }else{
            setMode(curve, (size_t)state.selKey, mode);
            flags[(size_t)state.selKey] = 0;
        }
        markEdit(result, curve, true);
    }

    // the mode a key's button row shows; `known` false for a mirrored hand-made tangent (no
    // button lit, Unity's Free Smooth)
    TangentMode shownMode(const AnimationCurve& curve, const std::vector<char>& flags, size_t key, bool& known){
        known = true;
        if (key < flags.size() && flags[key]) return TangentMode::Free;
        const TangentMode mode = detectMode(curve, key);
        if (mode == TangentMode::Free && !isBroken(curve.keys[key])) known = false;
        return mode;
    }

    void keyMenu(CurveEditorState& state, AnimationCurve* const curves[2], int count, CurveFieldResult& result){
        if (!ImGui::BeginPopup("##curveKeyMenu")) return;
        auto valid = [&](){
            return state.selCurve >= 0 && state.selCurve < count && state.selKey >= 0 &&
                   (size_t)state.selKey < curves[state.selCurve]->keys.size();
        };
        if (valid() && ImGui::MenuItem(ICON_FA_TRASH_CAN "  Delete Key", nullptr, false, curves[state.selCurve]->keys.size() > 1)){
            deleteSelected(state, curves, result);
        }
        if (valid()){
            ImGui::Separator();
            bool known = true;
            const TangentMode current = shownMode(*curves[state.selCurve], state.broken[state.selCurve], (size_t)state.selKey, known);
            for (int m = 0; m < 4; m++){
                if (ImGui::MenuItem(MODE_NAMES[m], nullptr, known && (int)current == m)){
                    applyMode(state, curves, (TangentMode)m, result);
                }
            }
        }else{
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void canvasMenu(CurveEditorState& state, AnimationCurve* const curves[2], CurveFieldResult& result){
        if (!ImGui::BeginPopup("##curveCanvasMenu")) return;
        if (ImGui::MenuItem(ICON_FA_PLUS "  Add Key")){
            addKeyAt(state, curves, state.menuCurve, state.menuTime, result);
        }
        ImGui::EndPopup();
    }

    void editorCanvas(CurveEditorState& state, AnimationCurve* const curves[2], int count, CurveFieldResult& result){
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 size(Theme::dpi(460.0f), Theme::dpi(240.0f));
        const ImVec2 c0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered();
        const Rect rect{c0.x, c0.y, size.x, size.y};
        if (!state.viewValid){
            state.view = fitView(*curves[0], rect, count > 1 ? curves[1] : nullptr);
            state.viewValid = true;
        }
        state.view.canvas = rect;

        const float keyRadius = Theme::dpi(4.5f);
        const float hitRadius = Theme::dpi(7.0f);
        const float handleLength = Theme::dpi(40.0f);
        const Point mouse{io.MousePos.x, io.MousePos.y};

        // ---- input ----
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)){
            const bool pan = io.KeyAlt;
            TangentHit handle;
            if (!pan && state.selCurve >= 0 && state.selCurve < count){
                handle = hitTangentHandle(*curves[state.selCurve], state.view, state.selKey, mouse, hitRadius, handleLength);
            }
            int hitCurve = -1, hitIndex = -1;
            if (!pan && handle.key < 0) hitAnyKey(curves, count, state.view, mouse, hitRadius, hitCurve, hitIndex);
            if (pan){
                state.drag = Drag::Pan;
            }else if (handle.key >= 0){
                state.drag = Drag::Tangent;
                state.dragSide = handle.side;
                state.dragChanged = false;
            }else if (hitIndex >= 0){
                state.selCurve = hitCurve;
                state.selKey = hitIndex;
                const Point p = keyPosition(state.view, curves[hitCurve]->keys[(size_t)hitIndex]);
                state.grabDx = mouse.x - p.x;
                state.grabDy = mouse.y - p.y;
                state.drag = Drag::Key;
                state.dragChanged = false;
            }else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){
                const float time = state.view.xToTime(mouse.x);
                addKeyAt(state, curves, nearerCurve(curves, count, time, state.view.yToValue(mouse.y)), time, result);
            }else{
                state.selCurve = -1;
                state.selKey = -1;
            }
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)){
            state.drag = Drag::Pan;
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && state.drag == Drag::None){
            int hitCurve = -1, hitIndex = -1;
            hitAnyKey(curves, count, state.view, mouse, hitRadius, hitCurve, hitIndex);
            if (hitIndex >= 0){
                state.selCurve = hitCurve;
                state.selKey = hitIndex;
                ImGui::OpenPopup("##curveKeyMenu");
            }else{
                state.menuTime = state.view.xToTime(mouse.x);
                state.menuCurve = nearerCurve(curves, count, state.menuTime, state.view.yToValue(mouse.y));
                ImGui::OpenPopup("##curveCanvasMenu");
            }
        }

        const bool selValid = state.selCurve >= 0 && state.selCurve < count && state.selKey >= 0 &&
                              (size_t)state.selKey < curves[state.selCurve]->keys.size();
        const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
        if (state.drag == Drag::Pan){
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle)){
                if (moved) state.view = panView(state.view, io.MouseDelta.x, io.MouseDelta.y);
            }else{
                state.drag = Drag::None;
            }
        }else if (state.drag == Drag::Key || state.drag == Drag::Tangent){
            if (!selValid){
                state.drag = Drag::None;
            }else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)){
                AnimationCurve& curve = *curves[state.selCurve];
                std::vector<char>& flags = state.broken[state.selCurve];
                syncFlags(flags, curve.keys.size());
                if (moved && state.drag == Drag::Key){
                    const float time = state.view.xToTime(mouse.x - state.grabDx);
                    const float value = state.view.yToValue(mouse.y - state.grabDy);
                    const CurveKey before = curve.keys[(size_t)state.selKey];
                    const int to = moveKey(curve, (size_t)state.selKey, time, value);
                    if (to >= 0){
                        moveFlag(flags, state.selKey, to);
                        state.selKey = to;
                        if (curve.keys[(size_t)to] != before){
                            state.dragChanged = true;
                            markEdit(result, curve, false);
                        }
                    }
                }else if (moved && state.drag == Drag::Tangent){
                    const size_t key = (size_t)state.selKey;
                    const bool broken = isBroken(curve.keys[key]) || flags[key];
                    if (dragTangent(curve, key, state.dragSide, mouse, state.view, broken)){
                        state.dragChanged = true;
                        markEdit(result, curve, false);
                    }
                }
            }else{
                if (state.dragChanged) result.finished = true;
                state.drag = Drag::None;
                state.dragChanged = false;
            }
        }

        if (hovered && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)){
            const float wheel = io.MouseWheel != 0.0f ? io.MouseWheel : io.MouseWheelH;
            const float factor = std::pow(1.15f, wheel);
            const float timeFactor = io.KeyCtrl ? 1.0f : factor;
            const float valueFactor = io.KeyShift ? 1.0f : factor;
            state.view = zoomView(state.view, mouse, timeFactor, valueFactor);
        }

        // ---- drawing ----
        const View& view = state.view;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 c1(c0.x + size.x, c0.y + size.y);
        drawList->AddRectFilled(c0, c1, IM_COL32(36, 36, 36, 255));
        drawList->PushClipRect(c0, c1, true);

        // outside the keys (the wrap modes play there): darker
        float first = curves[0]->firstTime(), last = curves[0]->lastTime();
        if (count > 1 && !curves[1]->keys.empty()){
            if (curves[0]->keys.empty()){
                first = curves[1]->firstTime();
                last = curves[1]->lastTime();
            }else{
                first = std::min(first, curves[1]->firstTime());
                last = std::max(last, curves[1]->lastTime());
            }
        }
        const float fx = std::max(c0.x, std::min(c1.x, view.timeToX(first)));
        const float lx = std::max(c0.x, std::min(c1.x, view.timeToX(last)));
        drawList->AddRectFilled(c0, ImVec2(fx, c1.y), IM_COL32(0, 0, 0, 50));
        drawList->AddRectFilled(ImVec2(lx, c0.y), c1, IM_COL32(0, 0, 0, 50));

        // grid and labels
        const ImU32 label = color(Theme::Colors::SubtleText);
        const float timeStep = clipcurves::niceStep(view.timeMax - view.timeMin, size.x, Theme::dpi(64.0f));
        for (float t : clipcurves::ticks(view.timeMin, view.timeMax, timeStep)){
            const float x = view.timeToX(t);
            drawList->AddLine(ImVec2(x, c0.y), ImVec2(x, c1.y), t == 0.0f ? IM_COL32(255, 255, 255, 50) : IM_COL32(255, 255, 255, 18));
            const std::string text = formatTick(t, timeStep);
            drawList->AddText(ImVec2(x + Theme::dpi(2.0f), c1.y - ImGui::GetTextLineHeight() - Theme::dpi(1.0f)), label, text.c_str());
        }
        const float valueStep = clipcurves::niceStep(view.valueMax - view.valueMin, size.y, Theme::dpi(28.0f));
        for (float v : clipcurves::ticks(view.valueMin, view.valueMax, valueStep)){
            const float y = view.valueToY(v);
            drawList->AddLine(ImVec2(c0.x, y), ImVec2(c1.x, y), v == 0.0f ? IM_COL32(255, 255, 255, 50) : IM_COL32(255, 255, 255, 18));
            const std::string text = formatTick(v, valueStep);
            drawList->AddText(ImVec2(c0.x + Theme::dpi(3.0f), y - ImGui::GetTextLineHeight() - Theme::dpi(1.0f)), label, text.c_str());
        }

        // curves
        const float step = Theme::dpi(2.0f);
        if (count > 1) drawBand(drawList, *curves[0], *curves[1], view, curveColor(0.18f), step);
        for (int c = 0; c < count; c++){
            const float thickness = Theme::dpi(c == state.selCurve ? 2.0f : 1.5f);
            drawCurve(drawList, *curves[c], view, curveColor(), curveColor(0.35f), thickness, step);
        }

        // keys, the selected one on top with its handles
        const ImU32 keyOutline = IM_COL32(20, 20, 20, 255);
        for (int c = 0; c < count; c++){
            for (size_t k = 0; k < curves[c]->keys.size(); k++){
                if (c == state.selCurve && (int)k == state.selKey) continue;
                const ImVec2 p = toImVec(keyPosition(view, curves[c]->keys[k]));
                drawList->AddCircleFilled(p, keyRadius, curveColor());
                drawList->AddCircle(p, keyRadius, keyOutline, 0, 1.0f);
            }
        }
        if (selValid){
            const AnimationCurve& curve = *curves[state.selCurve];
            const size_t key = (size_t)state.selKey;
            const ImVec2 p = toImVec(keyPosition(view, curve.keys[key]));
            const ImU32 handleColor = IM_COL32(220, 220, 220, 255);
            for (TangentSide side : {TangentSide::In, TangentSide::Out}){
                if (!hasTangentHandle(curve, key, side)) continue;
                const ImVec2 h = toImVec(tangentHandlePosition(curve, key, side, view, handleLength));
                drawList->AddLine(p, h, handleColor, Theme::dpi(1.0f));
                drawList->AddCircleFilled(h, Theme::dpi(3.5f), handleColor);
            }
            drawList->AddCircleFilled(p, keyRadius + Theme::dpi(0.5f), IM_COL32(255, 255, 255, 255));
            drawList->AddCircle(p, keyRadius + Theme::dpi(0.5f), color(Theme::Colors::Selection), 0, Theme::dpi(1.5f));
        }

        drawList->PopClipRect();
        drawList->AddRect(c0, c1, IM_COL32(0, 0, 0, 160));

        if (hovered && state.drag == Drag::None){
            ImGui::SetItemTooltip("Double-click: add a key. Right-click: key menu. Wheel: zoom (Shift: time, Ctrl: value). "
                                  "Middle or Alt + drag: pan. F: fit.");
        }
    }

    void editorPopup(CurveEditorState& state, AnimationCurve* const curves[2], int count, CurveFieldResult& result){
        ImGuiIO& io = ImGui::GetIO();

        // keep the selection in range (the value may change under the popup: undo)
        for (int c = 0; c < count; c++) syncFlags(state.broken[c], curves[c]->keys.size());
        if (state.selCurve < 0 || state.selCurve >= count || state.selKey < 0 ||
            (size_t)state.selKey >= curves[state.selCurve]->keys.size()){
            state.selCurve = -1;
            state.selKey = -1;
        }
        const int target = state.selCurve >= 0 ? state.selCurve : 0;

        // presets and Fit
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 thumb(Theme::dpi(44.0f), Theme::dpi(24.0f));
        for (int i = 0; i < CURVE_PRESET_COUNT; i++){
            if (i > 0) ImGui::SameLine();
            ImGui::PushID(i);
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::InvisibleButton("##preset", thumb);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::SetItemTooltip("%s", presetName((CurvePreset)i));
            drawList->AddRectFilled(p0, ImVec2(p0.x + thumb.x, p0.y + thumb.y),
                                    ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
            const AnimationCurve preview = presetCurve((CurvePreset)i);
            const float pad = Theme::dpi(3.0f);
            drawThumbnail(drawList, preview, nullptr, Rect{p0.x + pad, p0.y + pad, thumb.x - 2.0f * pad, thumb.y - 2.0f * pad},
                          curveColor());
            if (clicked){
                *curves[target] = presetCurve((CurvePreset)i, *curves[target]);
                state.broken[target].assign(curves[target]->keys.size(), 0);
                state.selCurve = -1;
                state.selKey = -1;
                markEdit(result, *curves[target], true);
                state.view = fitView(*curves[0], state.view.canvas, count > 1 ? curves[1] : nullptr);
            }
            ImGui::PopID();
        }
        ImGui::SameLine();
        const bool fit = ImGui::Button(ICON_FA_EXPAND "  Fit", ImVec2(0.0f, thumb.y));
        ImGui::SetItemTooltip("Zoom to fit the curve (F)");

        editorCanvas(state, curves, count, result);
        keyMenu(state, curves, count, result);
        canvasMenu(state, curves, result);

        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const bool typing = io.WantTextInput || ImGui::IsAnyItemActive();
        if (fit || (focused && !typing && ImGui::IsKeyPressed(ImGuiKey_F, false))){
            state.view = fitView(*curves[0], state.view.canvas, count > 1 ? curves[1] : nullptr);
        }
        if (focused && !typing && state.selKey >= 0 &&
            (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))){
            deleteSelected(state, curves, result);
        }

        // the selected key
        const bool selValid = state.selCurve >= 0 && state.selCurve < count && state.selKey >= 0 &&
                              (size_t)state.selKey < curves[state.selCurve]->keys.size();
        if (selValid){
            AnimationCurve& curve = *curves[state.selCurve];
            std::vector<char>& flags = state.broken[state.selCurve];
            syncFlags(flags, curve.keys.size());
            bool known = true;
            const TangentMode current = shownMode(curve, flags, (size_t)state.selKey, known);
            for (int m = 0; m < 4; m++){
                if (m > 0) ImGui::SameLine(0.0f, Theme::dpi(2.0f));
                const bool lit = known && (int)current == m;
                if (lit) ImGui::PushStyleColor(ImGuiCol_Button, Theme::Colors::ButtonActivated);
                if (ImGui::Button(MODE_NAMES[m])) applyMode(state, curves, (TangentMode)m, result);
                if (lit) ImGui::PopStyleColor();
            }

            const float fieldWidth = Theme::dpi(78.0f);
            ImGui::SameLine(0.0f, Theme::dpi(12.0f));
            ImGui::SetNextItemWidth(fieldWidth);
            const CurveKey key = curve.keys[(size_t)state.selKey];
            float time = key.time;
            const float timeSpeed = std::max(1e-4f, (state.view.timeMax - state.view.timeMin) / 400.0f);
            if (ImGui::DragFloat("Time", &time, timeSpeed, 0.0f, 0.0f, "%.3f") && std::isfinite(time) && time != key.time){
                const int to = moveKey(curve, (size_t)state.selKey, time, key.value);
                if (to >= 0){
                    moveFlag(flags, state.selKey, to);
                    state.selKey = to;
                    if (curve.keys[(size_t)to] != key){
                        markEdit(result, curve, false);
                        state.fieldPending = true;
                    }
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit()){
                result.finished = true;
                state.fieldPending = false;
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(fieldWidth);
            const CurveKey now = curve.keys[(size_t)state.selKey];
            float value = now.value;
            const float valueSpeed = std::max(1e-4f, (state.view.valueMax - state.view.valueMin) / 400.0f);
            if (ImGui::DragFloat("Value", &value, valueSpeed, 0.0f, 0.0f, "%.3f") && std::isfinite(value) && value != now.value){
                const int to = moveKey(curve, (size_t)state.selKey, now.time, value);
                if (to >= 0){
                    moveFlag(flags, state.selKey, to);
                    state.selKey = to;
                    markEdit(result, curve, false);
                    state.fieldPending = true;
                }
            }
            if (ImGui::IsItemDeactivatedAfterEdit()){
                result.finished = true;
                state.fieldPending = false;
            }

            ImGui::SameLine();
            ImGui::BeginDisabled(curve.keys.size() <= 1);
            if (ImGui::Button(ICON_FA_TRASH_CAN)) deleteSelected(state, curves, result);
            ImGui::SetItemTooltip("Delete the key (Delete)");
            ImGui::EndDisabled();
        }else{
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Click a key to edit it; double-click to add one");
        }

        // wrap modes (two curves: both take the choice)
        const float comboWidth = Theme::dpi(110.0f);
        int wraps[2] = {(int)curves[target]->preWrap, (int)curves[target]->postWrap};
        const char* labels[2] = {"Pre Wrap", "Post Wrap"};
        for (int w = 0; w < 2; w++){
            if (w > 0) ImGui::SameLine(0.0f, Theme::dpi(12.0f));
            ImGui::SetNextItemWidth(comboWidth);
            const int shown = std::max(0, std::min(2, wraps[w]));
            if (ImGui::BeginCombo(labels[w], WRAP_NAMES[shown])){
                for (int option = 0; option < 3; option++){
                    if (ImGui::Selectable(WRAP_NAMES[option], option == shown) && option != shown){
                        for (int c = 0; c < count; c++){
                            if (w == 0) curves[c]->preWrap = (CurveWrap)option;
                            else curves[c]->postWrap = (CurveWrap)option;
                            markEdit(result, *curves[c], true);
                        }
                    }
                }
                ImGui::EndCombo();
            }
        }

        // a field edit ended without a deactivation event
        if (state.fieldPending && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)){
            result.finished = true;
            state.fieldPending = false;
        }
    }

}

CurveFieldResult editor::curveField(const char* id, AnimationCurve& curve, bool mixed, float width){
    return curveField(id, curve, nullptr, mixed, width);
}

CurveFieldResult editor::curveField(const char* id, AnimationCurve& curve, AnimationCurve* second, bool mixed, float width){
    CurveFieldResult result;
    ImGui::PushID(id);
    const ImGuiID stateId = ImGui::GetID("##curveField");
    CurveEditorState& state = states[stateId];
    AnimationCurve* const curves[2] = {&curve, second};
    const int count = second ? 2 : 1;

    // the inline preview
    const float w = std::max(Theme::dpi(8.0f), width < 0.0f ? ImGui::GetContentRegionAvail().x : width);
    const ImVec2 size(w, ImGui::GetFrameHeight());
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    const bool clicked = ImGui::InvisibleButton("##preview", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    drawList->AddRectFilled(p0, p1, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), style.FrameRounding);
    if (style.FrameBorderSize > 0.0f){
        drawList->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border), style.FrameRounding, 0, style.FrameBorderSize);
    }
    if (mixed){
        const char* dash = "\xE2\x80\x94";
        const ImVec2 ts = ImGui::CalcTextSize(dash);
        drawList->AddText(ImVec2(p0.x + (size.x - ts.x) * 0.5f, p0.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), dash);
    }else{
        const float pad = Theme::dpi(3.0f);
        drawThumbnail(drawList, curve, second, Rect{p0.x + pad, p0.y + pad, size.x - 2.0f * pad, size.y - 2.0f * pad}, curveColor());
        if (hovered){
            float lo = 0.0f, hi = 0.0f;
            curve.valueRange(curve.firstTime(), curve.lastTime(), lo, hi);
            if (second){
                float lo2 = 0.0f, hi2 = 0.0f;
                second->valueRange(second->firstTime(), second->lastTime(), lo2, hi2);
                lo = std::min(lo, lo2);
                hi = std::max(hi, hi2);
            }
            ImGui::SetItemTooltip("Values %.3g to %.3g. Click to edit", lo, hi);
        }
    }

    if (clicked){
        state.viewValid = false;
        state.selCurve = -1;
        state.selKey = -1;
        state.drag = Drag::None;
        state.dragChanged = false;
        state.fieldPending = false;
        ImGui::OpenPopup("##curveEditor");
    }
    if (ImGui::BeginPopup("##curveEditor", ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar)){
        editorPopup(state, curves, count, result);
        ImGui::EndPopup();
    }else if (state.drag != Drag::None || state.fieldPending){
        // closed during an edit (Escape, a click outside): the edit ends
        if (state.dragChanged || state.fieldPending) result.finished = true;
        state.drag = Drag::None;
        state.dragChanged = false;
        state.fieldPending = false;
    }

    ImGui::PopID();
    return result;
}
