// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Scene view's Path Tester (docs/specs/S75-navigation-diagnostics.md R25, R26; docs/navigation.md
// "Path Tester"): Shift+click sets the start, a click the end, a press near a handle drags it and
// queries again every frame. Modes Path, Raycast, Closest Edge and Locate call the same
// NavigationSystem queries as query_navigation and draw through the overlay's query draw
// (NavigationEditor::setQueryDraw). Locate also asks the walkability probe why a point is not on
// the mesh. It edits nothing (no undo step) and saves nothing; it works in edit and in play.

#include "ecs/Entity.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "navigation/NavTypes.h"
#include "navigation/NavWalkabilityProbe.h"

#include <cstdint>
#include <functional>
#include <string>

namespace jasperengine {
    class NavigationSystem;
}

namespace jasperengine::editor {

    struct SceneProject;

    class NavPathTester {
    public:
        enum class Mode { Path, Raycast, ClosestEdge, Locate };
        enum class Handle { None, Start, End };

        // a press this close to a handle (view pixels) drags it
        static constexpr float HANDLE_PIXELS = 10.0f;

        static NavPathTester& get();

        // ---- on / off (the Scene view toolbar, the AI Navigation section, navigation.pathTester)
        void start(SceneProject& sceneProject);
        // ends it and takes its drawing away
        void stop();
        void toggle(SceneProject& sceneProject);
        bool isActive() const { return active; }
        bool isActive(uint32_t sceneId) const { return active && sceneId == activeScene; }
        uint32_t getSceneId() const { return activeScene; }

        // ---- the query (each change queries again)
        Mode getMode() const { return mode; }
        void setMode(SceneProject& sceneProject, Mode value);
        int getAgentType() const { return agentType; }
        void setAgentType(SceneProject& sceneProject, int value);
        uint16_t getAreaMask() const { return areaMask; }
        void setAreaMask(SceneProject& sceneProject, uint16_t value);
        void setStart(SceneProject& sceneProject, const Vector3& point);
        void setEnd(SceneProject& sceneProject, const Vector3& point);
        bool hasStart() const { return startSet; }
        bool hasEnd() const { return endSet; }
        const Vector3& getStart() const { return startPoint; }
        const Vector3& getEnd() const { return endPoint; }
        // Closest Edge and Locate use one point, the start
        bool usesEnd() const { return mode == Mode::Path || mode == Mode::Raycast; }
        nav::QueryFilter filter() const;

        // ---- Scene view input: `world` is what the surface placement hit under the pointer,
        // `screen` the pointer and `toView` a world point's place in the same view pixels
        using Projector = std::function<bool(const Vector3& world, Vector2& screen)>;
        // a press near a handle starts its drag; elsewhere Shift sets the start and a click the end
        // (Closest Edge, Locate: the point)
        void press(SceneProject& sceneProject, const Vector3& world, const Vector2& screen, bool shift, const Projector& toView,
                   float handlePixels = HANDLE_PIXELS);
        void drag(SceneProject& sceneProject, const Vector3& world);
        // a drag ends (Locate: the probe runs for where it ended)
        void release(SceneProject& sceneProject);
        Handle getDragged() const { return dragged; }

        // ---- what the last query answered
        struct Result {
            // false until the points the mode needs are set
            bool ran = false;
            // Path
            nav::NavPath path;
            // Raycast (blocked: hit is where it stops), Closest Edge (the edge point, its distance)
            nav::NavHit hit;
            bool blocked = false;
            // Closest Edge: an edge was found; Locate: a polygon within 5 m
            bool found = false;
            // Locate
            nav::NavLocation location;
            // Locate: the probe of the point (its location and the reasons it is not walkable)
            bool probed = false;
            Entity probeSurface = NULL_ENTITY;
            NavProbeResult probe;
        };
        const Result& getResult() const { return result; }
        // queries again (probe: Locate's walkability probe too)
        void requery(SceneProject& sceneProject, bool probe = true);
        // every frame while on: queries again when a navmesh of the scene, the navigation
        // settings or the play state changed since the last answer
        void refresh(SceneProject& sceneProject);

        // the equivalent query_navigation arguments, as JSON ("Copy Query")
        std::string queryJson() const;

        // ---- ImGui, from the Scene view (SceneWindow): the panel at the view's top right (below
        // `top`), and Locate's reasons in a popup at the point. Screen coordinates.
        void drawPanel(SceneProject& sceneProject, float viewMinX, float viewWidth, float top,
                       const std::function<bool(const Vector3& world, float& x, float& y)>& toScreen);

    private:
        bool active = false;
        uint32_t activeScene = 0;
        Mode mode = Mode::Path;
        int agentType = 0;
        uint16_t areaMask = nav::ALL_AREAS;
        Vector3 startPoint, endPoint;
        bool startSet = false, endSet = false;
        Handle dragged = Handle::None;
        Result result;
        // Locate's reasons show beside the point (a click or the end of a drag opens them)
        bool popupOpen = false;
        // what the last answer was computed against (refresh)
        uint64_t answeredKey = 0;

        uint64_t stateKey(SceneProject& sceneProject) const;
        void publish(SceneProject& sceneProject);
    };

}
