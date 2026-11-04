// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOLIAGE_OPS_H
#define FOLIAGE_OPS_H

// T5's operations on T1's engine (T5 R23, R54): scatter_by_rules, clear_foliage, place_trees,
// remove_trees, convert_placed_to_trees. They run inside applyOps' validation, overlay and result.
// Density writes go through the transaction's map store. Tree writes go to a working copy that the
// caller commits once the whole list succeeded, so a list that fails later leaves the trees alone.

#include "Export.h"
#include "terrain/FoliageRules.h"
#include "terrain/TerrainOps.h"
#include "terrain/TreeStore.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::terrain {

    // Everything an op needs that a map store cannot tell: the Terrain's tree data and its names
    class JASPERENGINE_API FoliageOpHost {
    public:
        struct Prototype {
            uint16_t id = 0;
            std::string name;       // the model's file stem, as tools call it
            bool snapToTerrain = true;
        };
        struct DetailLayer {
            std::string name;
            float density = 1.0f;
            bool hasMap = false;
        };
        // One placed instance of a host, in terrain-local space
        struct PlacedInstance {
            float position[3] = {0.0f, 0.0f, 0.0f};
            float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            float scale[3] = {1.0f, 1.0f, 1.0f};
            uint8_t color[4] = {255, 255, 255, 255};
        };
        struct PlacedHost {
            std::string name;
            uint16_t prototypeId = 0;
            std::vector<PlacedInstance> instances;
        };

        // The base may be null: then the tree ops refuse in validation
        explicit FoliageOpHost(std::shared_ptr<const TreeStore> base = nullptr);

        std::vector<Prototype> prototypes;
        std::vector<DetailLayer> layers;
        // Entity id as text -> the host's instances
        std::map<std::string, PlacedHost> placedHosts;
        // Spline name -> its polyline(s), terrain-local
        std::map<std::string, std::vector<std::vector<Vector2>>> splines;
        // Evaluates a Noise Graph over a grid; false with a message. Empty: the op refuses noise_mask (T2's seam)
        std::function<bool(const std::string& graph, const std::string& output, std::vector<float>& values, int& width, int& height, std::string& error)> noiseMask;
        // Polled per cell: a job's Cancel
        std::function<bool()> shouldStop;
        // Reports done cells out of a total for a job's progress
        std::function<void(int64_t done, int64_t total)> progress;

        bool hasTrees() const { return base != nullptr; }
        // What the list so far left: the base until an op writes
        const TreeStore& current() const;
        TreeStore& working();
        bool touched() const { return workingCopy != nullptr; }

        // The hosts' instances a convert took, to empty them in the same step
        std::vector<std::string> convertedHosts;

        // The delta from the base to what the list left, for the caller's undo
        TreeDelta delta() const;

    private:
        std::shared_ptr<const TreeStore> base;
        std::unique_ptr<TreeStore> workingCopy;
    };

    // The host applyOps' registered ops see while this lives (they take no user pointer, R89). Main thread.
    class JASPERENGINE_API FoliageOpScope {
    public:
        explicit FoliageOpScope(FoliageOpHost& host);
        ~FoliageOpScope();
        FoliageOpScope(const FoliageOpScope&) = delete;
        FoliageOpScope& operator=(const FoliageOpScope&) = delete;
    private:
        FoliageOpHost* previous;
    };

    // Registers the five; idempotent. Called where the engine starts and in tests.
    JASPERENGINE_API void registerFoliageOps();
    JASPERENGINE_API void unregisterFoliageOps();

    // A scatter's rules from an op's params, for the editor's Mass Place to share. False with error.field and message
    JASPERENGINE_API bool foliageOpParams(const FoliageOpHost& host, const TerrainMapStore& store, const Op& op, FoliageTreeScatter& trees, FoliageDensityScatter& density,
                                          bool& detailTarget, FoliageScatterMode& mode, OpError& error);

}

#endif // FOLIAGE_OPS_H
