// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (anim layers spec 11.4, 11.5)
// - Purpose: the Animator Controller edits and the graph geometry of the Animator window, as pure
//   functions over anim::ControllerData (no ImGui, no project): what the window's buttons, menus,
//   drags and shortcuts run before committing through AnimatorControllerDocument, and what MCP
//   edit_animator can share. Every edit keeps the ControllerData invariants
//   (anim::normalizeController): unique names, transitions naming existing states, a valid
//   default state; renames and removals update every reference (transitions, default states,
//   conditions, blend trees, speed parameters).
// - Graph coordinates are canvas units (the window applies its pan and zoom): a state box's
//   `position` is its top-left corner; states without a position (hand-written files) are shown
//   on a grid (displayPosition) and get one when first moved.
// - Thread-safety: pure functions.

#pragma once

#include "animation/AnimatorController.h"

#include <array>
#include <string>
#include <vector>

namespace jasperengine::editor::animator{

    // ---- selection ----

    enum class ItemKind { None, Layer, Parameter, State, AnyState, Transition };

    struct Item{
        ItemKind kind = ItemKind::None;
        int layer = -1;         // Layer, State, AnyState, Transition
        int index = -1;         // the parameter, state or transition index

        bool operator==(const Item& other) const{ return kind == other.kind && layer == other.layer && index == other.index; }
        bool operator!=(const Item& other) const{ return !(*this == other); }
    };

    // The item a validation warning's JSON path names ("layers[0].states[2].clip" -> state 2 of
    // layer 0, "layers[1].transitions[3].conditions[0]" -> transition 3, "parameters[2]" ->
    // parameter 2, "layers[1].weight" -> layer 1); None for "" or an unknown path
    Item itemOfPath(const std::string& jsonPath);

    // ---- names ----

    // `base`, else "base 1", "base 2"... that the layer (states) or controller (layers) does not
    // have; parameters: "base1", "base2" (Lua identifiers)
    std::string uniqueStateName(const anim::ControllerLayer& layer, const std::string& base);
    std::string uniqueParameterName(const anim::ControllerData& data, const std::string& base);
    std::string uniqueLayerName(const anim::ControllerData& data, const std::string& base);

    // ---- edits (false + error when refused; indices are into the data as it is) ----

    // appends a state (an empty name becomes "New State"; a taken one gets a number; a state
    // without a position gets the next free grid cell); its index
    int addState(anim::ControllerData& data, int layer, anim::ControllerState state);
    bool renameState(anim::ControllerData& data, int layer, int state, const std::string& name, std::string* error);
    // removes the states and every transition from or to them; a removed default state hands
    // the default to the first remaining state
    void removeStates(anim::ControllerData& data, int layer, std::vector<int> states);
    bool setDefaultState(anim::ControllerData& data, int layer, int state, std::string* error);
    // a transition with Unity's defaults (exit time 0.75, 0.25 s) from a state (-1: Any State);
    // its index
    int addTransition(anim::ControllerData& data, int layer, int from, int to);
    void removeTransitions(anim::ControllerData& data, int layer, std::vector<int> transitions);

    // a parameter of `type` (a taken name gets a number: "Speed1"); its index, -1 when `name` is
    // not a Lua identifier (parameter names are; the engine drops others)
    int addParameter(anim::ControllerData& data, anim::ControllerParameterType type, const std::string& name);
    // updates the conditions, blend trees and speed parameters that read it; refuses a name that is
    // not a Lua identifier
    bool renameParameter(anim::ControllerData& data, int parameter, const std::string& name, std::string* error);
    // drops the conditions that read it, clears the blend trees and speed parameters that do
    void removeParameter(anim::ControllerData& data, int parameter);

    int addLayer(anim::ControllerData& data, const std::string& name);
    // not the base layer (index 0)
    bool removeLayer(anim::ControllerData& data, int layer, std::string* error);
    bool renameLayer(anim::ControllerData& data, int layer, const std::string& name, std::string* error);
    // layers above the base only (the base layer stays first: its weight is always 1)
    bool moveLayer(anim::ControllerData& data, int from, int to, std::string* error);

    // the states (and the transitions between them) to paste elsewhere
    struct Clipboard{
        std::vector<anim::ControllerState> states;
        std::vector<anim::ControllerTransition> transitions;    // `from` / `to` name states of `states`
        bool empty() const { return states.empty(); }
    };
    Clipboard copyStates(const anim::ControllerData& data, int layer, const std::vector<int>& states);
    // pasted with unique names, at `at` (the copied group's top-left moved there); the new indices
    std::vector<int> pasteStates(anim::ControllerData& data, int layer, const Clipboard& clipboard, const std::array<float, 2>& at);

    // ---- graph geometry ----

    constexpr float STATE_WIDTH = 170.0f;
    constexpr float STATE_HEIGHT = 44.0f;
    constexpr float GRID_X = 220.0f;
    constexpr float GRID_Y = 90.0f;
    // where the Any State box is when the file gives no position
    constexpr float ANY_STATE_X = 40.0f;
    constexpr float ANY_STATE_Y = 40.0f;

    struct Rect{
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
        bool contains(float px, float py) const{ return px >= x && px <= x + w && py >= y && py <= y + h; }
        bool overlaps(const Rect& other) const{ return x <= other.x + other.w && other.x <= x + w && y <= other.y + other.h && other.y <= y + h; }
        float centerX() const{ return x + w * 0.5f; }
        float centerY() const{ return y + h * 0.5f; }
    };

    // a state's top-left: its position, else its grid cell among the states without one
    std::array<float, 2> displayPosition(const anim::ControllerLayer& layer, int state);
    Rect stateRect(const anim::ControllerLayer& layer, int state);
    Rect anyStateRect(const anim::ControllerLayer& layer);
    // the next free grid cell (right of the Any State, row by row)
    std::array<float, 2> nextGridPosition(const anim::ControllerLayer& layer);

    // the state under a point (the last drawn wins), -2 for the Any State, -1 for none
    int hitState(const anim::ControllerLayer& layer, float x, float y);
    // the states a box selection touches
    std::vector<int> statesIn(const anim::ControllerLayer& layer, const Rect& box);

    // one arrow per (from, to) pair; `transitions` are their indices in list order (a "×N"
    // badge when several). Endpoints sit on the boxes' edges, shifted sideways when the reverse
    // pair has an arrow too, so the two directions do not overlap
    struct Arrow{
        int from = -1;          // -1: Any State
        int to = -1;
        std::vector<int> transitions;
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        bool self = false;      // from == to: a loop drawn beside the box
    };
    std::vector<Arrow> arrows(const anim::ControllerLayer& layer);
    // the arrow under a point (within `tolerance` canvas units of its segment), -1 for none
    int hitArrow(const std::vector<Arrow>& arrows, float x, float y, float tolerance);

    // the union of rects; false when empty
    bool bounds(const std::vector<Rect>& rects, Rect& out);
    // the pan (canvas point shown at the view's top-left) and zoom (clamped to [minZoom, maxZoom])
    // that show `content` centred in a view of `width` x `height` pixels with `margin` pixels around
    void frame(const Rect& content, float width, float height, float margin, float minZoom, float maxZoom,
               float& panX, float& panY, float& zoom);

    // The transition timeline (Unity's): in seconds of the source, where the exit starts and how
    // long the blend lasts, and where the destination starts in its own time
    struct Timeline{
        float sourceLength = 1.0f;      // seconds shown for the source
        float exitStart = 0.0f;         // seconds into the source the blend starts
        float blend = 0.0f;             // the blend's seconds
        float destinationLength = 1.0f;
        float destinationStart = 0.0f;  // seconds into the destination it starts
    };
    Timeline transitionTimeline(const anim::ControllerTransition& transition, float sourceLength, float destinationLength,
                                float destinationCycleOffset);

}
