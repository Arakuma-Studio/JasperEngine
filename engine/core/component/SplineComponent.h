// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SPLINE_COMPONENT_H
#define SPLINE_COMPONENT_H

#include "ecs/Entity.h"
#include "spline/SplineData.h"

#include <cstdint>

namespace jasperengine{

    // where a spline's terrain bake acts (S18 R11)
    enum class SplineTerrainRegion{
        Path,       // a band along the curve: radius + extraWidth, then falloff
        Area        // the inside of a closed spline, then falloff
    };

    // how a spline's terrain bake changes the heights
    enum class SplineFlattenMode{
        Flatten,    // to the road bed, up or down
        CutOnly,    // only lowers
        FillOnly    // only raises
    };

    // The Spline's settings for "Apply to Terrain" (editor bake; S18 R11, D8). The bake is
    // destructive and undoable: nothing here runs in a game.
    struct SplineTerrainSettings{
        SplineTerrainRegion region = SplineTerrainRegion::Path;
        float extraWidth = 0.5f;            // world units added to the knots' radius on each side
        float falloff = 2.0f;               // world units of blend to the untouched terrain
        bool flatten = true;
        SplineFlattenMode flattenMode = SplineFlattenMode::Flatten;
        float heightOffset = 0.0f;          // world units: the bed sits this far from the curve
        bool paint = false;
        unsigned int paintLayer = 0;        // surface layer (blend map m = layer / 3, channel layer % 3)
        float paintStrength = 1.0f;         // 0-1
        float paintFalloff = 1.0f;          // world units of fade past the band
        float paintNoise = 0.0f;            // 0-1: edge break-up of the painted band
        bool clearFoliage = false;
        bool clearTrees = false;            // T5 R29: the bake removes the Terrain's trees in the footprint
        uint32_t foliageMask = 0xFFFFFFFFu; // bit n clears foliage layer n
        Entity terrain = NULL_ENTITY;       // NULL_ENTITY: the first terrain under the spline
    };

    // A curve of knots in the GameObject's local space (Unity's SplineContainer; S18 R1-R5). The
    // consumers on the same GameObject (Spline Extrude, Spline Instantiate) and Spline Animate on
    // others read it; SplineSystem regenerates them whenever spline.version() changes.
    struct SplineComponent{
        // component checkbox: a disabled spline generates nothing (its consumers are cleared)
        bool enabled = true;

        spline::SplineData spline;
        SplineTerrainSettings terrain;
    };

}

#endif //SPLINE_COMPONENT_H
