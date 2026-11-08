// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "component/TerrainComponent.h"
#include "command/CommandHistory.h"
#include "Project.h"
#include "math/Ray.h"
#include "math/Vector4.h"
#include "terrain/EditorTerrainStore.h"
#include "terrain/TerrainBrush.h"
#include "terrain/TerrainRawIO.h"
#include "terrain/TerrainStroke.h"
#include "terrain/TerrainStrokeSession.h"
#include "util/TerrainMapUtils.h"

#include "imgui.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "io/watch/FileIdentity.h"

namespace jasperengine{
    struct TerrainComponent;
    struct TerrainFoliageLayer;
    struct TerrainSurfaceLayer;
}

namespace jasperengine::editor{

    // Unity's terrain tool bar: the tool whose settings the window shows.
    enum class TerrainToolCategory{
        Sculpt,
        Texture,
        Details,
        Trees,
        Settings    // T1 R59: Terrain Settings
    };

    struct TerrainBrushCursor{
        bool visible = false;
        // World-space closed loops draped over the terrain surface: the brush
        // boundary and (when the falloff has a gradient) the half-strength contour.
        std::vector<Vector3> outerPoints;
        std::vector<Vector3> innerPoints;
        // T1 R55: the effective operation's colour (Table 4), the texel-size warning colour under one texel
        Vector4 color = Vector4(1.0f, 0.78f, 0.22f, 1.0f);
        // T5 R46: pairs of points, the selected prototype's collision capsule at the cursor
        std::vector<Vector3> capsuleLines;
        // T3 R41: when close, the texels a hole brush will change, one loop each
        std::vector<std::vector<Vector3>> texelLoops;
    };

    // A brush stroke run without the mouse (MCP tools): the settings the stroke uses. The
    // window's own brush settings are left as they were.
    struct TerrainScriptedBrush{
        TerrainBrushMode mode = TerrainBrushMode::Raise;
        TerrainBrushShape shape = TerrainBrushShape::Circle;
        TerrainBrushFalloff falloff = TerrainBrushFalloff::Smooth;
        float size = 4.0f;              // radius, world units
        float strength = 0.3f;
        float rotation = 0.0f;          // degrees
        float flattenHeight = -1.0f;    // normalized 0-1; < 0 picks the height under the first point
        int terraceSteps = 8;
        float noiseSize = 4.0f;
        int textureLayer = 0;           // PaintLayer: surface layer index
        int foliageLayer = 0;           // PaintDensity / EraseDensity
        bool normalizeBlend = true;
        std::string maskPath;           // grayscale brush mask (Stamp's relief)
        float secondsPerPoint = 0.05f;  // painting time each point is held for
        int mapResolution = 0;          // map created when missing; 0 = the editor setting
        // PlaceObject
        std::string placeAssetPath;
        bool placeInstanced = true;
        float placeSpacing = 2.0f;
        float placeMinScale = 1.0f;
        float placeMaxScale = 1.0f;
        float placeRotationJitter = 1.0f;
        float placeAlignToNormal = 0.0f;
    };

    struct ActiveStroke{
        bool active = false;
        bool treeStroke = false; // T5: the panel's brush
        // The stroke as one undo step: Esc (or Ctrl+Z) before the mouse is released puts the
        // map back and drops what a placement stroke made (S24 R12)
        InteractiveEdit edit;
        bool editStarted = false;
        bool wasModified = false;
        uint32_t sceneId = NULL_PROJECT_SCENE;
        Entity entity = NULL_ENTITY;
        TerrainMapRef ref;
        // Brush mode for this stroke (modifiers can override the selected mode)
        TerrainBrushMode effectiveMode = TerrainBrushMode::Raise;
        // A map stroke's transaction, and the store under it (in this order: the store goes last)
        std::unique_ptr<EditorTerrainStore> store;
        std::unique_ptr<TerrainStrokeSession> session;
        // The first stroke on a map it had to move to a file of the terrain's own undoes as a
        // structural step, from what it found (T1 R28, R32)
        bool rebound = false;
        TerrainMapSnapshot rebindSnapshot;
        // the samples' clock (T1 R33, R35: only holding still deposits by time)
        bool started = false;
        std::chrono::steady_clock::time_point startTime;
        // Placement strokes write entities instead of a map, so none of the map state applies
        bool placement = false;
        // Fixed at stroke start, cleared if the asset turns out not to be instanceable
        bool instanced = false;
        uint64_t placementStrokeId = 0;
        // Terrain-local XZ of every prop already on this terrain, grown as the stroke places more
        std::vector<Vector2> placedPoints;
        // The drag's spaced stamps (T1 R21)
        std::optional<terrain::StrokeResampler> placementPath;
    };

    class TerrainFoliagePanel;

    class TerrainEditWindow{
    private:
        class TerrainInstancePlaceCmd;
        class TerrainInstanceEraseCmd;
        class TerrainObjectStrokeCmd;

        Project* project;

        bool windowOpen;
        // the generation of the project's Terrain settings the members were read from (0: none). While
        // it is not the project's, show() writes nothing back, so constructor defaults or another
        // project's values never overwrite them
        uint64_t loadedSettingsGeneration = 0;
        bool focusRequested;
        bool brushActive;
        bool normalizeBlendPaint;
        bool heightMapStartAtMiddle;
        bool flattenPickOnStroke;

        uint32_t selectedSceneId;
        Entity selectedEntity;

        TerrainBrushMode brushMode;
        TerrainBrushShape brushShape;
        TerrainBrushFalloff brushFalloff;

        float brushSize;
        float brushStrength;
        float brushRotation;
        float flattenHeight;
        int terraceSteps;
        float noiseSize;

        // Grayscale falloff for every brush, and the relief the Stamp brush lays down
        std::string brushMaskPath;
        std::string brushMaskLoadedPath;
        std::vector<float> brushMaskPixels;
        int brushMaskWidth;
        int brushMaskHeight;

        int heightMapResolution;
        int blendMapResolution;
        int densityMapResolution;
        int selectedFoliageLayer;
        TerrainToolCategory toolCategory = TerrainToolCategory::Sculpt;
        int selectedTextureLayer;
        // which layers show their settings under the layer row
        bool expandedLayers[MAX_TERRAIN_LAYERS] = {};

        // Placed props are ordinary entities, so the palette is tool state, not component data
        std::string placeAssetPath;
        bool placeInstanced;
        float placeSpacing;
        std::unique_ptr<TerrainFoliagePanel> foliagePanel;
        float placeMinScale;
        float placeMaxScale;
        float placeRotationJitter;
        float placeAlignToNormal;

        bool paintUseMask;
        bool filterAllTools = false;
        bool filterInvert = false;
        bool heightFilterInvert = false;
        // T1 R63: the next click picks the Set Height value instead of painting
        bool pickHeightArmed = false;
        float paintMinSlope;
        float paintMaxSlope;
        float paintMinHeight;
        float paintMaxHeight;

        // An asset the load proved cannot be instanced, so the brush stops retrying it
        std::string instancingRejectedAsset;
        uint64_t placementStrokeCounter = 0;
        uint32_t placementSeed = 0;

        ActiveStroke stroke;

        // T1 R61: Import Raw... and Export Raw...
        struct ImportRawDialog{
            bool open = false;
            bool justOpened = false;
            std::string path;
            std::vector<uint8_t> bytes;
            HeightImportOptions options;
            HeightImport preview;
            bool setHeightFromRange = false;
            std::string message;
        };
        struct ExportRawDialog{
            bool open = false;
            bool justOpened = false;
            HeightExportOptions options;
            std::string message;
        };
        ImportRawDialog importRaw;
        ExportRawDialog exportRaw;
        // T1 R59: a new Terrain Height keeps the surface where it is
        bool keepWorldHeights = true;
        // T1 R62: the point under the pointer, from the last cursor update
        mutable bool infoValid = false;
        mutable Entity infoEntity = NULL_ENTITY;
        mutable Vector3 infoLocal;

        void drawIssuesBanner(SceneProject* sceneProject, TerrainComponent& terrain);
        void drawSettingsTool(SceneProject* sceneProject, TerrainComponent& terrain);
        void drawImportRawDialog(SceneProject* sceneProject, TerrainComponent& terrain);
        void drawExportRawDialog(SceneProject* sceneProject, TerrainComponent& terrain);
        void drawInfoPanel(SceneProject* sceneProject, TerrainComponent& terrain);
        // T1 R67: Paint by Rules under Paint Texture; Apply is one undo step
        void drawPaintByRules(SceneProject* sceneProject, TerrainComponent& terrain);
        std::string paintRulesMessage;
        void frameTerrain(SceneProject* sceneProject);
        // An undoable property of the selected terrain
        template<typename T> bool setTerrainProperty(const char* property, const T& value);
        // Terrain Height, with the heights kept in place when keepWorldHeights (one undo step)
        bool setTerrainHeight(SceneProject* sceneProject, float height, std::string& error);
        bool importRawFile(SceneProject* sceneProject, std::string& error);

        void showTooltip(const char* text, ImGuiHoveredFlags flags = 0);
        bool iconButton(const char* icon, const char* id, const char* tooltip, bool selected, const ImVec2& size);
        void drawMapSettings(const TerrainMapRef& ref, const char* label, int& resolution);
        float drawAssetThumbnail(const std::string& path, const char* id, bool selected = false, float scale = 3.0f);
        void drawTextureLayers(TerrainComponent& terrain);
        void drawLayerSurface(TerrainComponent& terrain, int index);
        bool setSurfaceLayers(const std::vector<TerrainSurfaceLayer>& layers, bool merge = false);
        void applyLayerMaterial(TerrainComponent& terrain, int index, const std::string& materialPath);
        void drawBrushMask();
        void setBrushMask(const std::string& path);
        bool loadBrushMask();
        void drawFoliageMesh(const TerrainFoliageLayer& layer);
        void drawPlacementAsset();
        bool raycastTerrainStrokeSurface(const Ray& localRay, TerrainComponent& terrain, const ActiveStroke* activeStroke, Vector3& localPoint, float& localHeight) const;

        SceneProject* findSceneProject(Scene* scene) const;
        SceneProject* getTargetSceneProject() const;
        bool updateTargetFromSelection();
        bool hasValidTarget(SceneProject* sceneProject = nullptr) const;
        int mapResolutionFor(TerrainMapTarget target) const;
        TerrainMapRef getBrushMapRef() const;
        bool isHeightBrush() const;
        // the point under the brush's cursor, for the hole brush's info (T3 R41)
        mutable bool hoverValid = false;
        mutable Vector3 hoverLocal;
        bool isBlendBrush() const;
        bool isDensityBrush() const;
        bool isPlacementBrush() const;
        // Erase needs no asset, placing does
        bool isPlacementReady() const;
        bool findTerrainHit(Scene* scene, const Ray& ray, Entity& entity, Vector3& localPoint, Vector3& worldPoint, float& localHeight, const ActiveStroke* activeStroke = nullptr) const;
        TerrainStrokeBrush makeStrokeBrush(TerrainBrushMode mode, const TerrainComponent& terrain, float pickedHeight, bool scripted) const;
        // Sets the stroke's session up on the map, moved to a file of the terrain's own first if it must be
        bool openMapStroke(SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref, TerrainBrushMode mode,
                           const Vector3& startPoint, float pickedHeight, bool scripted);
        bool strokeTerrainIsThere(SceneProject* sceneProject);
        bool paintMapStroke(SceneProject* sceneProject, const Vector3& localPoint);
        bool placeAlongStroke(SceneProject* sceneProject, Entity entity, const Vector3& localPoint);
        bool isHoleBrush() const;
        // T3 R29: a stroke that filled the last hole takes the map away, as one step with the fill
        bool removeFilledHoleMap(SceneProject* sceneProject);
        // T3 R41: hole or solid under the cursor, the texel and collision cell, and the small hole's warning
        void drawHoleBrushInfo(TerrainComponent& terrain);
        std::vector<Vector2> collectPlacedPoints(SceneProject* sceneProject, Entity terrainEntity) const;
        // Instanced placement keeps one host entity per asset under the terrain
        static Entity findInstanceHost(SceneProject* sceneProject, Entity terrainEntity, const std::string& assetPath);
        bool useInstancedPlacement() const;
        Command* makePlacementCommand(SceneProject* sceneProject, Entity terrainEntity, const Vector3& localPosition, const Quaternion& rotation, const Vector3& scale);
        void addStrokeObjectCommand(SceneProject* sceneProject, Command* command);
        bool applyPlacement(SceneProject* sceneProject, Entity entity, const Vector3& localPoint);
        bool applyObjectErase(SceneProject* sceneProject, Entity entity, const Vector3& localPoint);
        void clearStroke();
        // The stroke's undo step opens with it (mouse strokes); false once Esc cancelled it,
        // the map then put back as it was
        void openStrokeEdit(SceneProject* sceneProject);
        bool strokeCancelled();

        bool createMapForTarget(const TerrainMapRef& ref, int width, int height);
        bool deleteMapForTarget(const TerrainMapRef& ref);
        bool setFoliageLayers(const std::vector<TerrainFoliageLayer>& layers);
        template<typename T> bool setFoliageLayerProperty(const char* field, const T& value);

    public:
        // A layer with no density map gets an empty one, in memory until something paints it (T5: edit_terrain)
        bool ensureDensityMap(Project* project, SceneProject* sceneProject, Entity entity, int layer);
        static constexpr const char* WINDOW_NAME = "Terrain";

        // T1 R60: the size a brush may have anywhere; each terrain narrows it (brushSizeLimits)
        static constexpr float MIN_BRUSH_SIZE = 0.01f;
        static constexpr float MAX_BRUSH_SIZE = 100000.0f;
        static constexpr float MIN_BRUSH_STRENGTH = 0.01f;
        static constexpr float MAX_BRUSH_STRENGTH = 1.0f;
        static constexpr int MIN_TERRACE_STEPS = 2;
        static constexpr int MAX_TERRACE_STEPS = 64;

        // Returns false when terrain map edits could not be persisted to disk —
        // the caller should keep the scene marked unsaved so another save (which
        // retries the writes) is prompted for.
        static bool cleanUnusedTerrainMaps(Project* project);

        TerrainEditWindow(Project* project);
        ~TerrainEditWindow();

        // An 8-bit heightmap rewritten as 16-bit with the same heights, one undo step (the
        // spline bake's "Convert heightmap to 16-bit", S18 R24)
        bool convertHeightMapTo16Bit(uint32_t sceneId, Entity terrain, std::string& error);

        void show();
        void open();
        void openForEntity(Entity entity, uint32_t sceneId);
        // Reads the project's TerrainEditorSettings into the window when they were replaced since
        void loadSettings();
        // A project switch: the next project's settings are read on the next frame
        void resetProjectState();
        void setOpen(bool open);
        bool isOpen() const;

        bool isEditingScene(Scene* scene) const;
        bool beginStroke(Scene* scene, const Ray& ray);
        bool paintStroke(Scene* scene, const Ray& ray);
        void endStroke();
        void updateFoliagePreview();
        bool updateCursor(Scene* scene, const Ray& ray, TerrainBrushCursor& cursor) const;

        // T1 R60: half a heightmap texel up to the whole terrain
        static void brushSizeLimits(const TerrainComponent& terrain, float& minSize, float& maxSize);
        // T1 R21: what one placed object is, for the brush and for edit_terrain's place_objects: a prefab root or
        // a model GameObject under the terrain; null for any other asset
        static Command* placementCommand(Project* project, uint32_t sceneId, const std::string& asset, Entity terrainEntity,
                                         const Vector3& localPosition, const Quaternion& rotation, const Vector3& scale);
        // The terrain's placed GameObjects, instanced hosts left out, with their terrain-local x and z: what
        // place_objects spaces against and erase_objects may take
        static std::vector<std::pair<Entity, Vector2>> placedGameObjects(SceneProject* sceneProject, Entity terrainEntity);

        // The operation a stroke started now would run: Ctrl inverts, Shift smooths (T1 R56)
        TerrainBrushMode effectiveModeFor(bool ctrl, bool shift) const;
        // T1 R55, Table 4: the decal's colour of an operation
        static Vector4 decalColor(TerrainBrushMode mode);

        void adjustBrushSize(float factor);
        void adjustBrushStrength(float factor);

        // One stroke through terrain-local points (y ignored) with `brush`, the same brush code
        // a mouse drag runs, as undoable commands (a missing map is created first). Returns
        // false with `error` when the target or the settings can't stroke. `placed` counts
        // objects placed or erased by the placement brushes.
        bool scriptedStroke(uint32_t sceneId, Entity entity, const TerrainScriptedBrush& brush,
                            const std::vector<Vector3>& localPoints, std::string& error, int* placed = nullptr);
    };

}
