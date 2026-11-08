// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "component/TerrainComponent.h"
#include "command/CommandHistory.h"
#include "Project.h"
#include "math/Ray.h"
#include "util/TerrainMapUtils.h"

#include "imgui.h"

#include <chrono>
#include <random>
#include <string>
#include <vector>
#include "io/watch/FileIdentity.h"

namespace jasperengine{
    struct TerrainComponent;
    struct TerrainFoliageLayer;
    struct TerrainSurfaceLayer;
}

namespace jasperengine::editor{

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
        EraseObject
    };

    // Unity's terrain tool bar: the tool whose settings the window shows.
    enum class TerrainToolCategory{
        Sculpt,
        Texture,
        Details,
        Trees
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

    struct TerrainBrushCursor{
        bool visible = false;
        // World-space closed loops draped over the terrain surface: the brush
        // boundary and (when the falloff has a gradient) the half-strength contour.
        std::vector<Vector3> outerPoints;
        std::vector<Vector3> innerPoints;
    };

    struct TerrainMapSnapshot{
        bool empty = true;
        std::string path;
        std::string id;
        TextureFilter minFilter = TextureFilter::LINEAR;
        TextureFilter magFilter = TextureFilter::LINEAR;
        TextureWrap wrapU = TextureWrap::REPEAT;
        TextureWrap wrapV = TextureWrap::REPEAT;
        ColorFormat colorFormat = ColorFormat::RGBA;
        int width = 0;
        int height = 0;
        int channels = 0;
        std::vector<unsigned char> pixels;
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
        // The stroke as one undo step: Esc (or Ctrl+Z) before the mouse is released puts the
        // map back and drops what a placement stroke made (S24 R12)
        InteractiveEdit edit;
        bool editStarted = false;
        bool wasModified = false;
        uint32_t sceneId = NULL_PROJECT_SCENE;
        Entity entity = NULL_ENTITY;
        TerrainMapRef ref;
        TerrainMapSnapshot beforeSnapshot;
        // Brush mode for this stroke (modifiers can override the selected mode)
        TerrainBrushMode effectiveMode = TerrainBrushMode::Raise;
        // Normalized flatten target, sampled from the terrain at stroke start
        float flattenTarget = 0.5f;
        // Ramp runs from where the stroke started to wherever it is now
        Vector3 rampStart = Vector3::ZERO;
        float rampStartHeight = 0.5f;
        // Fractional droplets left over from the last stamp, so a slow frame still erodes
        float erodeCarry = 0.0f;
        uint32_t erodeSeed = 0;
        // Stamp pacing: previous stamp position/time for path interpolation and
        // time-based flow
        bool hasLastPoint = false;
        Vector3 lastPoint = Vector3::ZERO;
        std::chrono::steady_clock::time_point lastStampTime;
        // Float-space copy of the edited map (1 channel for heights, 4 for blend):
        // stamps accumulate here and are quantized to the stored bit depth only on
        // write-back, so gentle repeated stamps aren't lost to rounding.
        std::vector<float> workingPixels;
        int workingWidth = 0;
        int workingHeight = 0;
        // Texels written so far, cut into the undo patch when the stroke ends
        TerrainMapRegion dirtyRegion;
        // Placement strokes write entities instead of a map, so none of the texture state applies
        bool placement = false;
        // Fixed at stroke start, cleared if the asset turns out not to be instanceable
        bool instanced = false;
        uint64_t placementStrokeId = 0;
        // Terrain-local XZ of every prop already on this terrain, grown as the stroke places more
        std::vector<Vector2> placedPoints;
        bool heightReferenceValid = false;
        float heightReferenceTerrainSize = 0.0f;
        float heightReferenceMaxHeight = 0.0f;
        int heightReferenceWidth = 0;
        int heightReferenceHeight = 0;
        int heightReferenceChannels = 0;
        int heightReferenceBytesPerChannel = 1;
        std::vector<unsigned char> heightReferencePixels;
    };

    class TerrainEditWindow{
    private:
        class TerrainTextureEditCmd;
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
        float placeMinScale;
        float placeMaxScale;
        float placeRotationJitter;
        float placeAlignToNormal;

        bool paintUseMask;
        float paintMinSlope;
        float paintMaxSlope;
        float paintMinHeight;
        float paintMaxHeight;

        // An asset the load proved cannot be instanced, so the brush stops retrying it
        std::string instancingRejectedAsset;
        uint64_t placementStrokeCounter = 0;
        std::mt19937 placementRandom;

        uint64_t editTextureCounter = 1;

        ActiveStroke stroke;
        // >= 0 while a scripted stroke runs: the painting time of each applyBrush call
        float scriptedStampSeconds = -1.0f;

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
        // Brush-local coordinates in [-1, 1], rotated by brushRotation
        float sampleBrushMask(float localX, float localY) const;
        void drawFoliageMesh(const TerrainFoliageLayer& layer);
        void drawPlacementAsset();
        std::string makeEditableTextureId(uint32_t sceneId, Entity entity, const TerrainMapRef& ref);
        std::string makeEditableTexturePath(Project* project, uint32_t sceneId, Entity entity, const TerrainMapRef& ref);
        std::string makeEditableTextureFile(Project* project, uint32_t sceneId, Entity entity, const TerrainMapRef& ref);
        int expectedChannels(TerrainMapTarget target);
        ColorFormat expectedFormat(TerrainMapTarget target);
        static float decodeHeightTexel(const unsigned char* pixels, size_t texelIndex, int channels, int bytesPerChannel);
        static void encodeHeightTexel(unsigned char* pixels, size_t texelIndex, int bytesPerChannel, float value);
        unsigned char clampByte(float value);
        bool setFileBackedTextureData(Project* project, Texture& texture, const std::string& relativePath, int width, int height, ColorFormat format, int channels, const std::vector<unsigned char>& pixels);
        bool isOwnedEditableTexturePath(const std::string& path, uint32_t sceneId, Entity entity, const TerrainMapRef& ref);
        bool loadTerrainTextureDataFromPath(Project* project, const std::string& path, TextureData& data);
        std::vector<unsigned char> copyTexturePixels(TextureData& data);
        std::vector<unsigned char> convertTexturePixels(TextureData& data, TerrainMapTarget target);
        std::vector<unsigned char> makeInitialMapPixels(TerrainMapTarget target, int width, int height);
        void setOwnedTextureData(Texture& texture, const std::string& id, int width, int height, ColorFormat format, int channels, const std::vector<unsigned char>& pixels);
        TerrainMapSnapshot captureSnapshot(Project* project, Texture& texture, bool forcePixels);
        bool snapshotsEqual(const TerrainMapSnapshot& a, const TerrainMapSnapshot& b);
        void applySnapshotToTexture(Project* project, Texture& texture, const TerrainMapSnapshot& snapshot);
        bool ensureEditableMap(Project* project, SceneProject* sceneProject, Entity entity, const TerrainMapRef& ref, int resolution);
        void writeHeight(TextureData& data, int x, int y, float value);
        static float bilinearHeightSample(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float texelX, float texelY);
        bool raycastTerrainStrokeSurface(const Ray& localRay, TerrainComponent& terrain, const ActiveStroke* activeStroke, Vector3& localPoint, float& localHeight) const;

        SceneProject* findSceneProject(Scene* scene) const;
        SceneProject* getTargetSceneProject() const;
        bool updateTargetFromSelection();
        bool hasValidTarget(SceneProject* sceneProject = nullptr) const;
        int mapResolutionFor(TerrainMapTarget target) const;
        TerrainMapRef getBrushMapRef() const;
        bool isHeightBrush() const;
        bool isBlendBrush() const;
        bool isDensityBrush() const;
        bool isPlacementBrush() const;
        // Erase needs no asset, placing does
        bool isPlacementReady() const;
        static bool isScalarTarget(TerrainMapTarget target);

        void captureStrokeHeightReference(TerrainComponent& terrain);
        bool findTerrainHit(Scene* scene, const Ray& ray, Entity& entity, Vector3& localPoint, Vector3& worldPoint, float& localHeight, const ActiveStroke* activeStroke = nullptr) const;
        bool applyBrush(SceneProject* sceneProject, Entity entity, const Vector3& localPoint);
        bool stampBrush(TerrainComponent& terrain, TextureData& data, TerrainMapTarget target, const Vector3& localPoint, float deltaTime);
        bool addStrokePatchCommand(SceneProject* sceneProject, Texture& texture);
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
        static constexpr const char* WINDOW_NAME = "Terrain";

        static constexpr float MIN_BRUSH_SIZE = 0.1f;
        static constexpr float MAX_BRUSH_SIZE = 50.0f;
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
