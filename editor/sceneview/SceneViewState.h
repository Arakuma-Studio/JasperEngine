// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// What one user's Scene view of a scene keeps between sessions (docs/specs/S26-scene-view.md
// R17-R24, R36): the camera settings popup, the projection and the layout of panes, the camera
// bookmarks, the measures, the Layers filter and the per-GameObject visibility and pick lock.
// It lives in the scene's workspace.yaml entry (.jasperengine/user/), never in the scene file
// and never in a game build.

#include "Entity.h"
#include "math/Vector3.h"
#include "registry/TagManager.h"

#include "yaml-cpp/yaml.h"

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // Unity's Scene Camera settings (the camera icon of the Scene view toolbar)
    struct SceneCameraSettings {
        float fieldOfView = 60.0f;          // degrees, 4..120
        bool dynamicClipping = false;       // near and far follow the distance to the target
        float nearClip = 0.3f;
        float farClip = 4000.0f;
        float speed = 10.0f;                // fly speed, 0.01..1000 (the wheel steps it while flying)
        float speedMin = 0.5f;
        float speedMax = 1000.0f;
        float shiftMultiplier = 3.0f;       // Shift speeds the fly up this much
        bool acceleration = true;           // the fly ramps from 25% to 100% over 0.6 s
        bool easing = true;                 // frame, view changes and bookmarks animate (0.25 s)

        static constexpr float MIN_FOV = 4.0f;
        static constexpr float MAX_FOV = 120.0f;
        static constexpr float MIN_SPEED = 0.01f;
        static constexpr float MAX_SPEED = 1000.0f;
        static constexpr float EASING_SECONDS = 0.25f;
        static constexpr float ACCELERATION_SECONDS = 0.6f;
        static constexpr float ACCELERATION_START = 0.25f;

        // the values clamped to their ranges (near < far, min <= speed <= max)
        void clamp();
        bool operator==(const SceneCameraSettings& other) const;
    };

    // Where a camera looks from and how it projects
    struct SceneCameraPose {
        Vector3 position = Vector3(0.0f, 7.0f, -10.0f);
        Vector3 target = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 up = Vector3(0.0f, 1.0f, 0.0f);
        bool orthographic = false;
        float orthoSize = 10.0f;            // half the view's height in world units
        float fieldOfView = 60.0f;          // degrees
    };

    enum class SceneViewLayout : uint8_t { SINGLE, SPLIT, QUAD };

    // What a pane looks along: free (perspective, orbits) or one of the six axes (orthographic,
    // axis-locked)
    enum class SceneViewAxis : uint8_t { FREE, TOP, BOTTOM, FRONT, BACK, LEFT, RIGHT };

    // How an inactive pane draws: shadows are off by default there (R20)
    enum class PaneShading : uint8_t { SHADED, SHADED_SHADOWS };

    struct SceneViewPane {
        SceneViewAxis axis = SceneViewAxis::FREE;
        SceneCameraPose pose;
        PaneShading shading = PaneShading::SHADED;
        bool posed = false;                 // its pose was set (else it starts framing the main view's target)
    };

    // A camera bookmark: slot 0 is Home, 1..9 the numbered ones
    struct CameraBookmark {
        bool defined = false;
        std::string name;
        SceneCameraPose pose;
    };

    // A committed measure, world anchored (D12)
    struct SceneMeasure {
        uint32_t id = 0;
        Vector3 from;
        Vector3 to;
    };

    struct SceneLayerFilter {
        uint32_t visible = LAYER_MASK_EVERYTHING;   // the eyes of the Layers dropdown
        uint32_t pickable = LAYER_MASK_EVERYTHING;  // the locks
        bool operator==(const SceneLayerFilter& other) const { return visible == other.visible && pickable == other.pickable; }
    };

    struct SceneViewState {
        static constexpr int BOOKMARK_SLOTS = 10;   // Home + 1..9
        static constexpr int MAX_PANES = 4;

        SceneCameraSettings camera;

        // the main view's projection as the user chose it (Persp / Iso), and whether it is
        // orthographic now only because it is aligned to an axis (Unity's automatic ortho)
        bool orthographic = false;
        bool axisOrtho = false;
        float orthoSize = 10.0f;

        SceneViewLayout layout = SceneViewLayout::SINGLE;
        int activePane = 0;                 // the pane taking input and drawing the gizmos
        int maximizedPane = -1;             // a double-clicked pane header fills the view
        // panes[0] is the editor camera of the view; [1..3] the others of Split and Quad
        std::array<SceneViewPane, MAX_PANES> panes;

        std::array<CameraBookmark, BOOKMARK_SLOTS> bookmarks;
        std::vector<SceneMeasure> measures;
        uint32_t nextMeasureId = 1;

        SceneLayerFilter layerFilter;
        std::set<Entity> hiddenEntities;
        std::set<Entity> pickLockedEntities;

        // keys of the scene's workspace entry this version does not know, written back as read
        YAML::Node unknownKeys;

        int paneCount() const { return layout == SceneViewLayout::QUAD ? 4 : (layout == SceneViewLayout::SPLIT ? 2 : 1); }
        // the panes of a layout as Unity arranges them: Split = Persp | Top, Quad = Top, Front,
        // Side, Persp (the free pane is always panes[0], the editor camera)
        static SceneViewAxis defaultPaneAxis(SceneViewLayout layout, int pane);
        void setLayout(SceneViewLayout layout);
    };

    // workspace.yaml scene entry keys (R36): view, cameraSettings, bookmarks, measures,
    // layerFilter, hiddenEntities, pickLockedEntities
    namespace SceneViewStateStream {
        // Every R36 key of the state into `node` (unknown keys first, so ours win)
        void encode(const SceneViewState& state, YAML::Node& node);
        // A scene entry into a state. An older entry without cameraSettings has its
        // editorCamera's walkSpeed offset become speed 10 + offset, and its clips and fov.
        SceneViewState decode(const YAML::Node& node);
        // the keys this file writes besides displaySettings and editorCamera
        bool isOwnKey(const std::string& key);

        YAML::Node encodePose(const SceneCameraPose& pose);
        SceneCameraPose decodePose(const YAML::Node& node, const SceneCameraPose& fallback = SceneCameraPose());
        const char* axisName(SceneViewAxis axis);
        bool parseAxis(const std::string& name, SceneViewAxis& axis);
        const char* layoutName(SceneViewLayout layout);
        bool parseLayout(const std::string& name, SceneViewLayout& layout);
    }

}
