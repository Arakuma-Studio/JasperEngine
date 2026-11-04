// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The marquee rules and the click/drag drawing follow OpenEngine's SplineTool.cpp (MIT, see
// NOTICE.md), on our Scene view and commands.

#include "SplineSceneTools.h"

#include "imgui.h"
#include "render/SceneRender.h"
#include "spline/SplineEditor.h"

#include <map>
#include "navigation/NavGridBrush.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    enum class Capture{
        None,
        Stroke,         // a Freehand stroke
        ClickTangent,   // a Click-mode knot whose handle is being pulled
        Handle          // a Bezier handle of the edited spline
    };

    struct ViewState{
        Capture capture = Capture::None;
        uint32_t handleKnot = 0;
        bool handleOut = true;
        Vector3 pressPoint;
    };

    std::map<uint32_t, ViewState>& states(){
        static std::map<uint32_t, ViewState> map;
        return map;
    }

    // the spline the Edit Spline context edits in this view, when it is the one selected
    Entity editedIn(Project* project, SceneProject* sceneProject){
        const editor::SplineEditor& editor = editor::SplineEditor::get();
        if (!editor.isEditing() || editor.getSceneId() != sceneProject->id){
            return NULL_ENTITY;
        }
        const std::vector<Entity> selected = project->getSelectedEntities(sceneProject->id);
        if (selected.size() != 1 || selected[0] != editor.getEntity()){
            return NULL_ENTITY;
        }
        return editor.getEntity();
    }

}

editor::SplineSceneTools::Press editor::SplineSceneTools::mousePress(Project* project, SceneProject* sceneProject, float x, float y,
                                                                     bool gizmoHandleHovered){
    SplineEditor& editor = SplineEditor::get();
    ViewState& state = states()[sceneProject->id];
    state = ViewState();
    SceneRender* render = sceneProject->sceneRender;
    const ImGuiIO& io = ImGui::GetIO();

    // the Draw Spline tool takes every press of its view
    if (editor.isDrawing() && editor.getDrawSceneId() == sceneProject->id){
        const SplineEditor::DrawSettings& settings = editor.getDrawSettings();
        Vector3 point;
        if (!render->splinePointUnderCursor(editor.getDrawEntity(), x, y, settings.snap != SplineSnap::None, point)){
            return Press::Captured;
        }
        if (settings.mode == SplineDrawMode::Freehand){
            state.capture = Capture::Stroke;
            editor.addStrokePoint(point);
        }else{
            state.capture = Capture::ClickTangent;
            state.pressPoint = point;
            editor.addClickKnot(point, Vector3(0.0f, 0.0f, 0.0f));
        }
        sceneProject->needUpdateRender = true;
        return Press::Captured;
    }

    const Entity spline = editedIn(project, sceneProject);
    if (spline == NULL_ENTITY || gizmoHandleHovered){
        return Press::None;
    }
    // Ctrl+click: a knot on the curve, else one past the nearer end
    if (io.KeyCtrl){
        Vector3 point;
        if (render->hitTestSplineCurve(spline, x, y, point)){
            editor.insertOnCurve(project, point);
        }else if (render->splinePointUnderCursor(spline, x, y, true, point)){
            editor.extendTo(project, point);
        }
        sceneProject->needUpdateRender = true;
        return Press::Captured;
    }
    // a Bezier handle of a selected knot
    uint32_t knot = 0;
    bool out = true;
    if (render->hitTestSplineHandle(spline, x, y, knot, out)){
        state.capture = Capture::Handle;
        state.handleKnot = knot;
        state.handleOut = out;
        SplineEditor::endContinuousEdit(sceneProject->id);
        return Press::Captured;
    }
    // a knot: selects it (Shift/Ctrl add or toggle); the gizmo then sits on it
    const int hit = render->hitTestSplineKnot(spline, x, y);
    if (hit >= 0){
        if (io.KeyShift){
            editor.toggleSelected(static_cast<uint32_t>(hit));
        }else if (!editor.isSelected(static_cast<uint32_t>(hit))){
            editor.setSelection({static_cast<uint32_t>(hit)});
        }
        sceneProject->needUpdateRender = true;
        return Press::Selected;
    }
    return Press::None;
}

void editor::SplineSceneTools::mouseDrag(Project* project, SceneProject* sceneProject, float x, float y){
    SplineEditor& editor = SplineEditor::get();
    ViewState& state = states()[sceneProject->id];
    SceneRender* render = sceneProject->sceneRender;
    if (state.capture == Capture::Stroke){
        Vector3 point;
        if (render->splinePointUnderCursor(editor.getDrawEntity(), x, y, editor.getDrawSettings().snap != SplineSnap::None, point)){
            editor.addStrokePoint(point);
            sceneProject->needUpdateRender = true;
        }
    }else if (state.capture == Capture::ClickTangent){
        // a click-drag pulls the knot's Bezier handles (Unity)
        Vector3 point;
        if (render->splinePointUnderCursor(editor.getDrawEntity(), x, y, false, point)){
            editor.setLastClickTangent(point - state.pressPoint);
            sceneProject->needUpdateRender = true;
        }
    }else if (state.capture == Capture::Handle){
        render->dragSplineHandle(project, sceneProject->id, editor.getEntity(), state.handleKnot, state.handleOut, x, y);
        sceneProject->needUpdateRender = true;
    }
}

void editor::SplineSceneTools::mouseRelease(Project* project, SceneProject* sceneProject, float x, float y){
    SplineEditor& editor = SplineEditor::get();
    ViewState& state = states()[sceneProject->id];
    if (state.capture == Capture::Stroke){
        // a freehand stroke ends with the button
        std::string error;
        const Vector3 at = editor.getStroke().empty() ? Vector3(0.0f, 0.0f, 0.0f) : editor.getStroke().back();
        editor.finishDraw(project, sceneProject->sceneRender->worldPerPixelAt(at), &error);
        sceneProject->needUpdateRender = true;
    }else if (state.capture == Capture::Handle){
        SplineEditor::endContinuousEdit(sceneProject->id);
    }
    (void)x;
    (void)y;
    state = ViewState();
}

bool editor::SplineSceneTools::doubleClick(Project* project, SceneProject* sceneProject, float x, float y){
    SplineEditor& editor = SplineEditor::get();
    if (!editor.isDrawing() || editor.getDrawSceneId() != sceneProject->id || editor.getDrawSettings().mode != SplineDrawMode::Click){
        return false;
    }
    // the double click's second press placed a knot on the first: it is not one
    const std::vector<spline::SplineKnot>& clicked = editor.getClickKnots();
    if (clicked.size() >= 2 && (clicked.back().position - clicked[clicked.size() - 2].position).length() <=
            sceneProject->sceneRender->worldPerPixelAt(clicked.back().position) * 6.0f){
        editor.removeLastClickKnot();
    }
    std::string error;
    Vector3 at;
    sceneProject->sceneRender->splinePointUnderCursor(editor.getDrawEntity(), x, y, false, at);
    editor.finishDraw(project, sceneProject->sceneRender->worldPerPixelAt(at), &error);
    sceneProject->needUpdateRender = true;
    return true;
}

bool editor::SplineSceneTools::isCapturing(uint32_t sceneId){
    auto it = states().find(sceneId);
    return it != states().end() && it->second.capture != Capture::None;
}

bool editor::SplineSceneTools::marquee(Project* project, SceneProject* sceneProject, const Vector2& start, const Vector2& end, bool add){
    const Entity spline = editedIn(project, sceneProject);
    if (spline == NULL_ENTITY){
        return false;
    }
    SplineEditor& editor = SplineEditor::get();
    std::vector<uint32_t> knots = sceneProject->sceneRender->getSplineKnotsInRect(spline, start, end);
    if (add){
        const std::vector<uint32_t>& previous = editor.getSelection();
        knots.insert(knots.end(), previous.begin(), previous.end());
    }
    editor.setSelection(knots);
    sceneProject->needUpdateRender = true;
    return true;
}

bool editor::SplineSceneTools::handleKeys(Project* project, SceneProject* sceneProject){
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput){
        return false;
    }
    SplineEditor& editor = SplineEditor::get();
    const bool plain = !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper;
    if (editor.isDrawing() && editor.getDrawSceneId() == sceneProject->id){
        if (plain && ImGui::IsKeyPressed(ImGuiKey_Escape, false)){
            editor.cancelDraw();
            sceneProject->needUpdateRender = true;
            return true;
        }
        if (plain && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))){
            std::string error;
            const std::vector<spline::SplineKnot>& clicked = editor.getClickKnots();
            const Vector3 at = !clicked.empty() ? clicked.back().position : (editor.getStroke().empty() ? Vector3(0.0f, 0.0f, 0.0f) : editor.getStroke().back());
            editor.finishDraw(project, sceneProject->sceneRender->worldPerPixelAt(at), &error);
            sceneProject->needUpdateRender = true;
            return true;
        }
        return false;
    }
    if (editor.isEditing() && editor.getSceneId() == sceneProject->id && plain && ImGui::IsKeyPressed(ImGuiKey_Escape, false)){
        // Esc clears the knots, then leaves Edit Spline
        if (!editor.getSelection().empty()){
            editor.clearSelection();
        }else{
            editor.endEdit(project);
        }
        sceneProject->needUpdateRender = true;
        return true;
    }
    return false;
}

bool editor::SplineSceneTools::deleteRequested(Project* project){
    SplineEditor& editor = SplineEditor::get();
    if (editor.isDrawing() && editor.getDrawSettings().mode == SplineDrawMode::Click){
        // Backspace (and Delete) take back the last placed knot
        editor.removeLastClickKnot();
        return true;
    }
    if (editor.isEditing() && !editor.getSelection().empty()){
        editor.deleteSelected(project);
        return true;
    }
    return false;
}

bool editor::SplineSceneTools::frameBounds(Project* project, uint32_t sceneId, AABB& bounds){
    SplineEditor& editor = SplineEditor::get();
    if (!editor.isEditing() || editor.getSceneId() != sceneId){
        return false;
    }
    bounds = editor.selectionBounds(project);
    return !bounds.isNull();
}
