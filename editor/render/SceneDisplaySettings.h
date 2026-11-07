// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "navigation/NavigationOverlay.h"
#include "terrain/TerrainOverlay.h"
#include "render/ViewDrawMode.h"

namespace jasperengine::editor{

    struct SceneDisplaySettings {
        bool showAllJoints       = false;
        bool showAllBones        = false;
        bool showAllBodies       = false;
        bool hideCameraView      = false;
        bool hideLightIcons      = false;
        bool hideSoundIcons      = false;
        bool hideContainerGuides = false;
        bool showOrigin          = true;
        bool showGrid3D          = true;
        bool hideSelectionOutline = false;
        bool disableFaceCulling  = false;
        bool disableFog          = false;
        // the Scene view draws without the volumetric clouds
        bool disableClouds       = false;
        // the scene's post-processing in the Scene view (off by default: the Game view shows it)
        bool showPostProcessing  = false;
        bool showGrid2D          = false;
        float gridSpacing2D      = 50.0f;
        float gridSpacing3D      = 1.0f;
        bool snapToGrid          = false;
        bool snapTile            = false;
        bool snapRotation        = false;
        float rotationSnapDegrees = 15.0f;
        // every lattice's cage, not only the selected ones' (faint)
        bool showAllLattices     = false;
        // the AI Navigation overlay (S19; navigation/NavigationOverlay.h)
        NavOverlaySettings navigation;
        // the terrain overlay (T1 R65; terrain/TerrainOverlay.h)
        TerrainOverlaySettings terrainOverlay;
        // the Draw Mode (spec S54 R11) and the Stats overlay of the Scene view
        ViewDrawMode drawMode = ViewDrawMode::SHADED;
        bool showStats = false;
    };

}
