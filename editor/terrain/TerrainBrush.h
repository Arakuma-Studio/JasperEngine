// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "terrain/TerrainShape.h"

#include <optional>
#include <vector>

namespace jasperengine::editor{

    // Stored as integers in the editor's settings: new tools go at the end (T1 R83)
    enum class TerrainBrushMode{
        Raise,
        Lower,
        Smooth,
        Flatten,
        Sharpen,
        Noise,
        Terrace,
        Stamp,
        Erode,
        Ramp,
        PaintBase,
        PaintLayer,
        PaintDensity,
        EraseDensity,
        PlaceObject,
        EraseObject,
        // T3 R41: no falloff, no strength: a texel is in the brush or it is not
        CutHole,
        FillHole
    };

    enum class TerrainBrushShape{
        Circle,
        Square
    };

    enum class TerrainBrushFalloff{
        Smooth,
        Linear,
        Constant
    };

    // What a map stroke paints with, fixed for the stroke
    struct TerrainStrokeBrush{
        TerrainBrushMode mode = TerrainBrushMode::Raise;
        TerrainBrushShape shape = TerrainBrushShape::Circle;
        TerrainBrushFalloff falloff = TerrainBrushFalloff::Smooth;
        float size = 4.0f;              // radius, terrain-local
        float strength = 0.3f;
        float rotation = 0.0f;          // degrees, of the mask
        float flattenTarget = 0.5f;     // normalized
        int terraceSteps = 8;
        float noiseSize = 4.0f;
        int textureLayer = 0;           // PaintLayer, PaintBase
        int foliageLayer = 0;           // PaintDensity, EraseDensity
        bool normalizeBlend = true;

        // Grayscale, row by row: the falloff's mask, or the Stamp brush's relief. Copied once.
        std::vector<float> mask;
        int maskWidth = 0;
        int maskHeight = 0;

        // The Slope Filter (T1 R66): texels outside it keep what they had. Paint Texture's alone,
        // unless allMaps (All Tools); an inverted range keeps what is outside it
        std::optional<terrain::FilterRange> slope;      // degrees
        std::optional<terrain::FilterRange> height;     // terrain-local
        bool filterAllMaps = false;
        bool invertSlope = false;
        bool invertHeight = false;

        // T1 R36, R18: a person's ramp is a re-application from its start to where the pointer is now;
        // sculpt_terrain's accumulates a stamp at a time, as it always did
        bool accumulateRamp = false;

        // T1 R37: what a pen's pressure scales; a mouse's pressure is 1
        bool pressureStrength = true;
        bool pressureSize = false;
    };

}
