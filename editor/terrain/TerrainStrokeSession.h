// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "TerrainBrush.h"
#include "terrain/OverlayMapStore.h"
#include "terrain/TerrainOps.h"
#include "terrain/TerrainStroke.h"

#include <cstdint>
#include <deque>
#include <optional>

namespace jasperengine::editor{

    // One brush stroke on one map (T1 R17, R18, R27): the stamps run through applyOps on a single
    // overlay, so a stroke's working memory is the tiles it touched (R26) and its floats survive
    // between stamps (DC2). The overlay's before tiles are the Esc and the undo delta.
    //
    // A stamp deposits strength * falloff * flow * seconds, whichever way the time was cut (R34, R18);
    // the stamps of a move fall evenly along it, spaced as TEW:1607-1628 spaced them.
    class TerrainStrokeSession{
    public:
        // terrainSize: what the stamps' spacing and brush radius floor are measured against
        TerrainStrokeSession(terrain::TerrainMapStore& base, terrain::MapId map, float terrainSize, TerrainStrokeBrush brush);

        TerrainStrokeSession(const TerrainStrokeSession&) = delete;
        TerrainStrokeSession& operator=(const TerrainStrokeSession&) = delete;

        // Where a ramp starts: the first point of the stroke. Its height comes from before the stroke.
        void setRampStart(float x, float z);

        // Stamps from the last point to this one over `seconds` of painting; a stroke's first move is
        // one stamp. False when no stamp landed (wholly off the terrain).
        bool moveTo(float x, float z, float seconds);
        // The next move starts afresh instead of drawing a streak from the last point
        void liftAnchor() { hasLastPoint = false; resampler.reset(); }

        // A person's drag (R33-R35, R38): a sample of the pointer at `time` seconds into the stroke.
        // StrokeResampler turns the samples into stamps, a click into one, each depositing what one
        // 1/60 s click does (R34); they wait in a queue for run(). A ramp re-applies at once (R36).
        void addSample(float time, float x, float z, float pressure);
        // Runs queued stamps, in order, until budgetSeconds of wall time have passed (0: all of
        // them). The stroke's bytes do not depend on the budget. True when none wait.
        bool run(double budgetSeconds);
        bool hasQueued() const { return !queue.empty(); }

        // The base shows the stroke so far; the floats stay (R17)
        bool flush();
        // Everything back as the stroke found it, with no whole-map copy (R27). Nothing more may run.
        void cancel();

        // Normalized height of a terrain-local point as the stroke found it (bilinear, as drawn)
        float heightBefore(float x, float z) const;

        terrain::OverlayMapStore& overlay() { return transaction; }
        const terrain::OverlayMapStore& overlay() const { return transaction; }
        terrain::MapId mapId() const { return map; }

        // The op a stamp of this brush runs: the shape's place and the deposit are set per stamp (tests)
        const terrain::Op& currentOp() const { return op; }

    private:
        void buildOp();
        void placeShape(float x, float z);
        bool applyStamp(float x, float z, float seconds);
        bool applyRamp(float x, float z, float strength);

        terrain::OverlayMapStore transaction;
        terrain::MapId map;
        terrain::MapFormat format;
        float terrainSize;
        float maxHeight;
        TerrainStrokeBrush brush;
        float radius = 0.0f;
        float flow = 0.0f;

        // Kept between stamps: a mask's pixels are not copied again for each one
        terrain::Op op;

        // R33: the drag's resampler, made by its first sample and again after a lift
        std::optional<terrain::StrokeResampler> resampler;
        std::deque<terrain::StrokeStamp> queue;
        std::vector<terrain::StrokeStamp> sampled;

        bool hasLastPoint = false;
        float lastX = 0.0f, lastZ = 0.0f;
        float rampStartX = 0.0f, rampStartZ = 0.0f;
        // R17: the droplets' fraction and seed carry across a stroke's stamps
        float erodeCarry = 0.0f;
        uint32_t erodeSeed = 0;
    };

}
