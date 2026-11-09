// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Paint Trees' and Paint Details' panels (T5 R46-R53): the prototype grid and its foldout, the tree
// brush, Mass Place Trees, Bake Impostor, the stats and warnings. The Terrain window only calls in
// (programme D2), and keeps its own map brushes and placed GameObjects.

#include "ecs/Entity.h"
#include "terrain/TreeStore.h"
#include "math/Vector3.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace jasperengine {
    struct TerrainComponent;
    struct TerrainTreePrototype;
}

namespace jasperengine::editor {

    class Project;
    struct SceneProject;
    class FoliageOpRunner;
    class TerrainEditWindow;
    class TerrainWorldTreeEdit;

    class TerrainFoliagePanel {
    public:
        TerrainFoliagePanel(Project* project, TerrainEditWindow* window);
        ~TerrainFoliagePanel();

        // The rows above Paint Trees' own. Returns true when they are the whole tool (Place As: Tree)
        bool drawTrees(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain, float& brushSize);
        // R47's rows the window does not draw, the stats, and Show Density
        void drawDetails(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain, int layer);

        bool placesTrees() const { return placeAsTree; }
        void setPlacesTrees(bool value) { placeAsTree = value; }
        bool isReady(const TerrainComponent& terrain) const;
        int selectedPrototype() const { return prototype; }

        // The brush: Shift erases every prototype, Ctrl+Shift only the selected one, Ctrl erases as today.
        // On a Terrain World each tree goes to the tile under it (T4 R27), and the stroke's tiles are one
        // step when it ends.
        bool beginStroke(SceneProject* sceneProject, Entity entity, const Vector3& local, float brushSize, bool shift, bool ctrl);
        bool stamp(SceneProject* sceneProject, Entity entity, const Vector3& local, float brushSize);
        void endStroke();
        // Esc before the release: what the stroke changed goes
        void cancelStroke();
        // sculpt_terrain's paint_trees and erase_trees: the brush along the points, as one stroke with this
        // prototype and spacing. changed: trees added or erased. False, with why, when it could not run or
        // landed on no ground at all.
        bool scriptedStroke(SceneProject* sceneProject, Entity entity, const std::vector<Vector3>& localPoints, float brushSize, bool erase,
                            int prototypeIndex, float strokeSpacing, int& changed, std::string& error);
        // Alt with no drag: the prototype under the cursor (R46); false when none is near
        bool pickPrototype(SceneProject* sceneProject, Entity entity, const Vector3& local, float radius);

        // A capsule standing on `base` as pairs of points: R46's gizmo; public for tests
        static void appendCapsuleLines(const Vector3& base, float radius, float height, std::vector<Vector3>& lines);

        // Runs each frame: Mass Place's job and the bake reach their end here
        void update();

        // The Stats foldout's numbers for a tier (R50), and its warning colour: public for tests
        struct Estimate {
            uint64_t detailVisible = 0;
            uint64_t treesVisible = 0;
            uint64_t meshTrees = 0;
            uint32_t draws = 0;
            uint64_t cpuBytes = 0;
            uint64_t gpuBytes = 0;
            bool over = false;
        };
        static Estimate estimateFor(int tier, const TerrainComponent& terrain, uint64_t detailVisibleHigh, uint64_t treesVisible, uint64_t treesStored, uint32_t draws);

    private:
        void drawPrototypeGrid(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain);
        void drawPrototypeFoldout(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain);
        void drawImpostor(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain, TerrainTreePrototype& authored);
        void drawMassPlace(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain);
        void drawStats(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain);
        void drawWarnings(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain);
        void drawTerrainSettings(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain);
        void drawConvert(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain);
        void showOverlay(SceneProject* sceneProject, const char* label, const char* source, int layer);
        bool setPrototypes(SceneProject* sceneProject, Entity entity, const std::vector<TerrainTreePrototype>& prototypes, const std::string& name);
        void addPrototype(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain, const std::string& meshPath);
        void removePrototype(SceneProject* sceneProject, Entity entity, TerrainComponent& terrain, int index);
        void commitStamp(SceneProject* sceneProject, Entity entity, terrain::TreeDelta&& delta);
        bool stampWorld(TerrainComponent& terrain, const Vector3& local, float brushSize);
        // the record a paint stamp makes at a point, after its surface: size, turn, tint (one draw order for both kinds of Terrain)
        terrain::TreeRecord strokeTree(const TerrainTreePrototype& authored);

        Project* project;
        TerrainEditWindow* window;

        int prototype = 0;
        bool placeAsTree = true;
        float treeDensity = 0.25f;      // trees per square unit under the brush
        float treeHeight = 1.0f;
        float treeHeightRandom = 0.2f;
        bool lockWidthToHeight = true;
        float treeWidth = 1.0f;
        float treeWidthRandom = 0.2f;
        bool randomRotation = true;
        float colorVariation = 0.1f;
        float normalAlignment = 0.0f;
        float spacing = 2.0f;

        // the stroke
        bool strokeActive = false;
        bool strokeErase = false;
        bool strokeEraseSelectedOnly = false;
        uint64_t strokeId = 0;
        uint64_t nextStrokeId = 1;
        uint32_t strokeSeed = 1;
        std::mt19937 random;
        float pendingTrees = 0.0f;
        int strokeChanged = 0;
        // a Terrain World's stroke (T4 R27): its tiles, whether it touched ground, what it said
        std::unique_ptr<TerrainWorldTreeEdit> worldStroke;
        std::string strokeName;
        bool strokeGround = false;
        bool strokeOverAbsent = false;
        std::string strokeStatus;

        // Mass Place Trees (R48)
        int massCount = 1000;
        bool massKeepExisting = true;
        int massSeed = 1;
        std::vector<float> massWeights;
        float massSlope[2] = {0.0f, 45.0f};
        float massHeight[2] = {0.0f, 100000.0f};
        bool massExcludeHoles = true;
        std::shared_ptr<FoliageOpRunner> massRunner;
        std::string massStatus;
        bool massOpen = false;

        // the Bake
        int bakeViews = 8;
        int bakeResolution = 256;
        double bakeShare = -1.0;
        std::string bakeStatus;

        // the Stats foldout, at 2 Hz
        double nextStats = 0.0;
        int estimateTier = 0;
        Estimate shown;
        uint64_t shownTotal = 0;
        uint64_t shownResident = 0;
        uint64_t shownVisible = 0;
        uint32_t shownCells = 0;
        uint32_t shownPending = 0;

        // Show Density
    };

}
