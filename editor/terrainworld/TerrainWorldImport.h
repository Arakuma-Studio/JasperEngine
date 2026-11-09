// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// T4 R32, R41: Import Heightmap on a Terrain World, as a job with progress and cancel. A 16-bit RAW or
// R16 file is read a band of rows at a time (its range and byte order found in one pass first, as
// T1's import guesses them); PNG, EXR and other raw depths decode whole, up to 8,193 samples a side.
// The job slices it against the world's tiles as the files hold them and writes every tile it changes
// into a stash under .jasperengine/trash; the main thread then swaps them in as one undo step
// (TerrainWorldImportCmd). A cancel, a failure or a world edited meanwhile leaves the world's folder
// untouched. Spacing and max height are the world's: the file's samples land one on each of its own.

#include "ecs/Entity.h"
#include "terrain/world/WorldImport.h"
#include "terrain/world/WorldLattice.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine {
    class TerrainWorldRuntime;
}

namespace jasperengine::editor {

    class Project;

    struct TerrainWorldImportRequest {
        std::string path;               // in the project, or absolute
        int width = 0, height = 0;      // a raw file's samples a side; 0: from its length (square)
        bool flipVertically = false;    // the file's first row at the world's highest z
        terrain::TileCoord origin;      // the tile the file's first sample lands on
    };

    struct TerrainWorldImportPlan {
        std::filesystem::path file;
        int width = 0, height = 0;
        bool sizeGuessed = false;
        // read a band of rows at a time (16-bit raw); else decoded whole
        bool banded = false;
        int bytesPerSample = 2;
        bool flipVertically = false;
        terrain::TileCoord origin;
        std::vector<terrain::TileCoord> covered;    // the tiles the file covers
        std::vector<terrain::TileCoord> changed;    // those and their present neighbours
        int made = 0;                               // covered tiles not in the world yet
    };

    // PNG, EXR and raws of other depths decode whole, up to this many samples a side (R32)
    inline constexpr int TERRAIN_WORLD_IMPORT_WHOLE_SIDE = 8193;

    // The file's size from its header or length, and the tiles it reaches, before anything changes;
    // false with why and its fix (no file, a size it cannot be, a tile past the world's limits)
    bool planTerrainWorldImport(Project* project, TerrainWorldRuntime& runtime, const TerrainWorldImportRequest& request, TerrainWorldImportPlan& plan,
                                std::string& error);

    struct TerrainWorldImportResult {
        bool ok = false;
        bool cancelled = false;
        std::string error;
        int tiles = 0;      // covered
        int changed = 0;    // written
    };

    // A running import: progress in [0, 1] (or below 0 while a file decodes whole) and a cancel that
    // stops it before anything changes
    class TerrainWorldImportTask {
    public:
        void cancel() { cancelled.store(true); }
        bool cancelRequested() const { return cancelled.load(); }
        double progress() const { return progressValue.load(); }
        std::string phase() const;
        bool finished() const { return isFinished.load(); }

        // the job's side
        void report(double value, const std::string& text);
        void finish() { isFinished.store(true); }

    private:
        std::atomic<bool> cancelled{false};
        std::atomic<bool> isFinished{false};
        std::atomic<double> progressValue{0.0};
        mutable std::mutex mutex;
        std::string phaseText;
    };

    // Starts the import of a plan made just now; done runs on the main thread (EditorHost's queue) once
    // the step is on the scene's history ("Import Heightmap (Name)"), or with why nothing changed
    std::shared_ptr<TerrainWorldImportTask> startTerrainWorldImport(Project* project, uint32_t sceneId, Entity world, const TerrainWorldImportPlan& plan,
                                                                    std::function<void(const TerrainWorldImportResult&)> done);

}
