// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The Scene view's terrain overlays (T1 R65, R90, DC8): a sheet over each terrain, an editor-only
// mesh like the AI Navigation overlay's (never in a game or camera image), with a generated RGBA8
// image baked by a source. T1 registers height, slope, contours and layer_weights; later specs
// add theirs through registerSource, and visualize_terrain takes any registered name. A Terrain
// World draws T4's own overlays instead (tiles, tile_state, lod, collision; R42), over its tiles.

#include "Scene.h"
#include "math/Vector4.h"
#include "terrain/TerrainMapStore.h"
#include "util/Signal.h"

#include "yaml-cpp/yaml.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace jasperengine {
    class Mesh;
    struct TerrainComponent;
}

namespace jasperengine::editor {

    struct SceneProject;

    // Saved with the workspace, per scene ("terrainOverlay" under the scene's view settings)
    struct TerrainOverlaySettings {
        std::string source;             // empty: none
        float contourInterval = 0.0f;   // world units; 0: chosen from the height range
        float slopeMin = -1.0f;         // degrees; below zero: the four default bands
        float slopeMax = -1.0f;
        int layer = -1;                 // layer_weights: one layer only; -1: all
        float opacity = 0.7f;

        bool operator==(const TerrainOverlaySettings& other) const {
            return source == other.source && contourInterval == other.contourInterval && slopeMin == other.slopeMin &&
                   slopeMax == other.slopeMax && layer == other.layer && opacity == other.opacity;
        }
        bool operator!=(const TerrainOverlaySettings& other) const { return !(*this == other); }
    };

    YAML::Node encodeTerrainOverlaySettings(const TerrainOverlaySettings& settings);
    void decodeTerrainOverlaySettings(const YAML::Node& node, TerrainOverlaySettings& settings);

    struct TerrainOverlayLegendStop {
        std::string label;
        Vector4 color;
    };

    // What a source bakes from: the terrain, its maps and the image's size. A texel's centre is at
    // localOf(x, y), terrain-local.
    struct TerrainOverlayBake {
        // the terrain's scene and entity: what a source reads beside the maps (T5's trees)
        Scene* scene = nullptr;
        Entity entity = NULL_ENTITY;
        TerrainComponent* terrain = nullptr;
        const terrain::TerrainMapStore* store = nullptr;
        const TerrainOverlaySettings* settings = nullptr;
        int width = 0;
        int height = 0;
        // world metres per local height unit and the world Y of local height 0 (labels in world units)
        float scaleY = 1.0f;
        float originY = 0.0f;

        Vector2 localOf(int x, int y) const;
    };

    class TerrainOverlay {
    public:
        // RGBA8, width * height * 4, row y at local Z = -size/2 + (y + 0.5) * size / height
        using Bake = std::function<void(const TerrainOverlayBake& context, std::vector<uint8_t>& rgba)>;
        using Legend = std::function<std::vector<TerrainOverlayLegendStop>(const TerrainOverlayBake& context)>;

        static constexpr int MAX_CELLS = 255;
        static constexpr int MAX_IMAGE = 1024;
        static constexpr double MIN_SECONDS = 0.1;

        // R90: a source by name; registering a name again replaces it
        static void registerSource(const std::string& name, Bake bake, Legend legend);
        static bool hasSource(const std::string& name);
        static std::vector<std::string> sourceNames();
        // One terrain's image and legend, outside any view (visualize_terrain, tests)
        static bool bake(Scene* scene, Entity entity, const TerrainOverlaySettings& settings, int width, int height,
                         std::vector<uint8_t>& rgba, std::vector<TerrainOverlayLegendStop>& legend, std::string& error);

        explicit TerrainOverlay(Scene* scene);
        ~TerrainOverlay();

        // Once a Scene view frame: builds, refreshes (at most every MIN_SECONDS, after an edit) or hides
        void update(SceneProject& sceneProject, const TerrainOverlaySettings& settings, double nowSeconds);
        void hide();

        // the legend of the last bake (the Scene view's corner)
        const std::vector<TerrainOverlayLegendStop>& getLegend() const { return legend; }
        size_t getSheetCount() const { return sheets.size(); }

    private:
        struct Sheet {
            Mesh* mesh = nullptr;
            uint32_t revision = 0;
            bool dirty = true;
            std::string textureId;
            int imageWidth = 0;
            int imageHeight = 0;
            // T4 R42: a Terrain World's sheet follows its runtime: what the image and the grid show
            uint64_t worldImage = 0;
            uint64_t worldGeometry = 0;
        };

        Scene* scene;
        std::map<Entity, Sheet> sheets;
        TerrainOverlaySettings drawn;
        std::vector<TerrainOverlayLegendStop> legend;
        double lastBake = -1.0;
        // T4 R42: a World's LOD Level that render has not recorded a cut for yet: the next frame bakes again
        bool worldPending = false;
        Subscription edits;

        void refresh(Entity entity, Sheet& sheet, const TerrainOverlaySettings& settings);
        // T4 R42: a Terrain World's overlay (editor/terrainworld/TerrainWorldOverlay.h)
        void refreshWorld(Entity entity, Sheet& sheet, const TerrainOverlaySettings& settings, bool changed);
        void releaseWorld(Entity entity);
        void ensureMesh(Sheet& sheet);
        void setImage(Sheet& sheet, const std::vector<uint8_t>& rgba, int width, int height);
    };

}
