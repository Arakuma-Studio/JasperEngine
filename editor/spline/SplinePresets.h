// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "component/SplineComponent.h"
#include "component/SplineExtrudeComponent.h"
#include "component/SplineInstantiateComponent.h"
#include "texture/Material.h"

#include <string>
#include <vector>

namespace jasperengine{ class Scene; }

namespace jasperengine::editor{

    class Project;

    // The configured spline GameObjects of GameObject › Spline and build_spline's presets
    // (S18 R23, D12): one definition used by the menus, the MCP tools and the tests.
    enum class SplinePreset{
        Empty,      // a Spline without knots (draw it)
        Road,       // Crown road conformed to the terrain, its bed flattened by Apply to Terrain
        Path,       // Bevel path
        Wall,       // Rectangle wall, Linear with mitred corners
        Fence,      // posts and rails (Spline Instantiate Fence; built-in pieces)
        River,      // water filled to its banks, flowing (library:water FLOW + MESH_DEPTH)
        Lake,       // a closed spline's water filled to its banks
        Circle,     // a closed ring of eight knots
        Square,     // a closed Linear square
        Scatter,    // pieces every two units (built-in markers)
        Edge2D,     // 2D: an XY ribbon with a Box2D chain
        Fill2D,     // 2D: a filled concave shape
        Circle2D    // 2D: a ring in XY
    };

    struct SplinePresetSetup{
        SplineComponent spline;
        bool hasExtrude = false;
        SplineExtrudeComponent extrude;
        bool hasInstantiate = false;
        SplineInstantiateComponent instantiate;
        // the Mesh of an extrude: its material and shadows
        Material material;
        bool castShadows = true;
        bool receiveShadows = true;
    };

    class SplinePresets{
    public:
        // "road", "path", "wall", "fence", "river", "lake", "circle", "square", "scatter",
        // "edge2d", "fill2d", "circle2d", "empty"
        static bool fromName(const std::string& name, SplinePreset& preset);
        static const char* name(SplinePreset preset);
        static std::vector<std::string> names();
        static bool is2D(SplinePreset preset);
        // the menu's name for a new GameObject ("Road")
        static const char* displayName(SplinePreset preset);

        // The preset's components. Fence and Scatter write their built-in pieces into the
        // project's assets (splines/*.gltf) when missing.
        static SplinePresetSetup make(Project* project, SplinePreset preset, bool scene2D);
        // Adds the preset's components to a new GameObject (CreateEntityCmd)
        static void addTo(Scene* scene, Entity entity, const SplinePresetSetup& setup);

        // the built-in pieces' asset-relative paths (written when missing)
        static std::string ensurePiece(Project* project, const char* name);
    };

}
