// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Scene.h"
#include "Catalog.h"
#include "render/SceneRender.h"
#include "command/CommandHistory.h"
#include "render/preview/MaterialRender.h"
#include "Conector.h"
#include "Generator.h"
#include "Configs.h"
#include "util/Prefab.h"
#include "util/AssetDatabase.h"
#include "util/ScriptParser.h"
#include "util/ScopedDefaultEntityPool.h"
#include "util/CxxStandards.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <chrono>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <atomic>
#include <mutex>

namespace jasperengine::editor{
    namespace prefabinternal { struct ScratchEntity; }

    struct LocalBuildSettings;

    enum class SceneType{
        SCENE_3D,
        SCENE_2D,
        SCENE_UI
    };

    enum class ScenePlayState {
        STOPPED,
        SAVING,
        LOADING,
        PLAYING,
        PAUSED,
        CANCELLING
    };

    enum class InsertionType{
        BEFORE,
        AFTER,
        INTO
    };

    enum class TabType{
        SCENE,
        CODE_EDITOR,
        IMAGE_VIEWER
    };

    struct TabEntry{
        TabType type;
        std::string filepath;
    };

    struct SceneMaxValues {
        unsigned int maxSubmeshes = 0;
        unsigned int maxTilemapTilesRect = 0;
        unsigned int maxTilemapTiles = 0;
        unsigned int maxExternalBuffers = 0;
        unsigned int maxSpriteFrames = 0;
        unsigned int maxBones = 0;
        unsigned int maxColliders2D = 0;
        unsigned int maxColliderPoints2D = 0;
        unsigned int maxColliders3D = 0;
        unsigned int maxColliderVertices3D = 0;
        unsigned int maxColliderIndices3D = 0;
    };

    struct ChildSceneRef {
        uint32_t id = NULL_PROJECT_SCENE;
        bool startActive = true;
    };

    // Local ids the GameObjects of prefab instances had, by instance root then registry
    // entity: an instance made again (a Missing Prefab relinked) takes them back where free
    using PrefabMemberIds = std::unordered_map<Entity, std::unordered_map<Entity, Entity>>;

    // Unity's Missing Prefab: a prefab instance root whose asset cannot be found (deleted,
    // or a GUID no file has). The root stays with its own components; everything the scene
    // stored for the instance is kept as written and saved again unchanged, and the
    // instance is rebuilt from it when the asset comes back. See editor/project/PREFABS.md.
    struct MissingPrefabInstance {
        std::string guid;      // of the PrefabComponent when it went missing
        std::string path;      // last known path
        // The instance's keys, relative to this root: `prefabInstance` (or the keys written
        // before it: prefabOverrides, prefabLocalEntities, prefabRemovedGameObjects) and any
        // other key a scene file holds next to a prefab root, everything but the root's own
        // GameObject keys (type, entity, name, active, tag, layer, components, children)
        YAML::Node data;
        PrefabMemberIds memberIds;  // the ids its GameObjects had, when it went missing in this session
        Entity mainCamera = NULL_ENTITY;  // the scene's main camera, when it was one of them
    };

    struct SceneProject{
        uint32_t id = NULL_PROJECT_SCENE;
        std::string name = "Unknown";
        Scene* scene = nullptr;
        SceneType sceneType;
        std::vector<Entity> entities;
        Entity mainCamera = NULL_ENTITY;
        Entity defaultCamera = NULL_ENTITY;
        SceneRender* sceneRender = nullptr;
        std::vector<Entity> selectedEntities;
        fs::path filepath;
        bool needUpdateRender = true;
        // Extra frames to keep drawing after a change so late async uploads still show.
        int renderSettleFrames = 0;
        // Bumped on structural changes so the Hierarchy window can cache its tree.
        uint64_t structureVersion = 0;
        bool isModified = false;
        // True while the id has never reached disk, so a scene replacing this one
        // can take it over instead of drawing a new one
        bool transientId = false;
        bool isVisible = false;
        bool opened = true;
        bool expandedInline = false;
        SceneDisplaySettings displaySettings;
        ScenePlayState playState = ScenePlayState::STOPPED;
        YAML::Node playStateSnapshot;
        std::set<Entity> playStateEntities;
        // isModified when the snapshot was taken: Stop puts both back
        bool playStateModified = false;
        SceneMaxValues maxValues;
        std::set<ShaderKey> shaderKeys;
        std::vector<ChildSceneRef> childScenes;
        std::vector<SceneScriptSource> cppScripts;
        std::vector<PrefabSceneInfo> prefabs;
        YAML::Node editorCameraState;
        // Missing Prefab instances by root entity (Project::getMissingPrefab validates them)
        std::map<Entity, MissingPrefabInstance> missingPrefabs;
        // Set while decoding when a prefab reference resolved to another path by its GUID:
        // the loaded scene is then marked modified
        bool prefabRefsChanged = false;
        // While the scene file is decoded: every entity id it holds (GameObjects, prefab
        // member ids, added GameObjects). A prefab member without a saved id never takes one
        // of these, which a GameObject further down the file needs.
        std::unordered_set<Entity> reservedEntityIds;
    };

    // Unity's Prefab Mode, Context: how the scene an In Context prefab stage was opened from
    // shows behind it
    enum class PrefabContextVisibility {
        Normal,
        Gray,
        Hidden
    };

    class PrefabContextOverlay;  // editor/PrefabStage.h

    // Unity's prefab stage (Prefab Mode): a prefab asset opened for editing in a scene of its
    // own. Its GameObjects are the asset's (their ids are the asset's file ids), so every
    // edit made in it is an edit of the asset; the instances nested in it are instances, and
    // what they override is the asset's override of them. Stages stack (a nested prefab
    // opened from inside one goes on top, Unity's breadcrumbs); the top one is shown. See
    // editor/PrefabStage.cpp and editor/project/PREFABS.md, section 12.
    struct PrefabStage {
        SceneProject sceneProject;                     // the stage's scene (never in Project::scenes)
        std::filesystem::path assetPath;               // project-relative .prefab
        std::string guid;
        Entity root = NULL_ENTITY;                     // the prefab's root GameObject
        uint32_t returnSceneId = NULL_PROJECT_SCENE;   // the scene Prefab Mode was entered from
        // In Context: the instance it was opened from, drawn behind the stage
        uint32_t contextSceneId = NULL_PROJECT_SCENE;
        Entity contextRoot = NULL_ENTITY;
        Entity contextCamera = NULL_ENTITY;            // a system camera in the context scene
        bool contextHidden = false;                    // the instance is hidden while drawn
        bool contextWasActive = true;
        std::shared_ptr<PrefabContextOverlay> overlay; // the Gray context's veil
        std::vector<Entity> environment;               // system entities: light, sky (never saved)
        bool frameOnOpen = true;                       // the Scene view frames the root once sized
        int frameDelay = 3;                            // ... and drawn (bounds and camera settled)
        bool changePending = false;                    // a change Auto Save has not written yet
        bool autoSaveFailed = false;                   // retried after the next change only
        std::chrono::steady_clock::time_point lastChange{};
        std::chrono::steady_clock::time_point lastSyncCheck{};
        std::string syncedAsset;                       // the asset as the stage last read or wrote it
        bool outsideChangeWarned = false;

        bool inContext() const { return contextSceneId != NULL_PROJECT_SCENE; }
    };

    struct NestedPrefabRecovery {
        std::filesystem::path path;
        Entity rootEntity = NULL_ENTITY;
        std::vector<Prefab::EntityMember> members;
    };

    struct NodeRecoveryEntry {
        YAML::Node node;
        size_t transformIndex = 0;
        std::vector<Prefab::EntityMember> members;
        std::map<Entity, Prefab::ObjectOverrides> overrides;
        std::vector<NestedPrefabRecovery> nestedPrefabs;
        // Removed GameObject entries of the instance inside the branch the Prefab lost,
        // dropped with it. An entry with no node is an instance that did not hold the branch.
        std::unordered_map<Entity, std::vector<Prefab::EntityMember>> removed;
    };

    using NodeRecovery = std::map<std::string, NodeRecoveryEntry>;

    // A Prefab GameObject branch taken out of one prefab instance only: a removed GameObject
    // override, or what a Prefab change left below one. Holds what puts it back exactly.
    struct PrefabBranchRecovery {
        std::filesystem::path path;           // prefab of the instance
        Entity instanceRoot = NULL_ENTITY;
        Entity registryEntity = NULL_ENTITY;  // the branch's top in the prefab registry
        bool recorded = false;                // a removed GameObject entry was added for it
        YAML::Node node;                      // the branch; nested instances as instance roots
        Entity parent = NULL_ENTITY;
        size_t index = 0;                     // Transform index, or tracked list index
        bool hasTransform = false;
        std::vector<Prefab::EntityMember> members;  // the instance's members in the branch
        std::map<Entity, Prefab::ObjectOverrides> overrides;
        std::vector<NestedPrefabRecovery> nestedPrefabs;  // containing ones first

        bool valid() const { return registryEntity != NULL_ENTITY && node.IsMap(); }
    };

    // One removed GameObject override of an instance, or of an instance nested in it
    struct PrefabRemovalInfo {
        std::filesystem::path path;           // prefab that holds the GameObject
        Entity instanceRoot = NULL_ENTITY;    // instance that removed it
        Entity registryEntity = NULL_ENTITY;
        std::string name;                     // its name in the prefab
        bool belowRemoved = false;            // a removed GameObject above it keeps it out too
    };

    // What Revert of a removed GameObject did, for its undo
    struct PrefabRemovalRevert {
        std::vector<Prefab::EntityMember> entry;    // the entry's recorded members
        std::vector<Prefab::EntityMember> created;  // what the revert made, whose ids a redo takes
        Entity local = NULL_ENTITY;                 // the GameObject brought back, if any
    };

    // What Apply of a removed GameObject did, for its undo
    struct PrefabRemovalApply {
        std::vector<Prefab::EntityMember> entry;  // the applying instance's entry
        Entity registryParent = NULL_ENTITY;
        NodeRecovery recovery;                    // the GameObject in the Prefab and its instances
    };

    // A prefab instance unpacked by Project::unpackPrefabInstances, with what putting its
    // prefab link back takes: the members stay in the scene, only the bookkeeping goes
    struct PrefabUnpackRecord {
        std::filesystem::path path;       // prefab asset the instance followed
        size_t index = 0;                 // position in the asset's instance list for the scene
        Prefab::Instance instance;        // members, registry mapping and overrides
        bool hadComponent = false;
        PrefabComponent component;        // the root's PrefabComponent
    };

    // A GameObject of a prefab instance, at the level that owns its overrides: the instance
    // it is a member of (a nested instance root is a member of the instance holding it),
    // else the instance it is the root of (a top-level instance root)
    struct PrefabObjectRef {
        std::filesystem::path path;               // prefab of that instance
        Prefab* prefab = nullptr;
        Prefab::Instance* instance = nullptr;
        Entity localEntity = NULL_ENTITY;
        Entity registryEntity = NULL_ENTITY;      // in `prefab`
        bool isRoot = false;                      // the instance's root
    };

    // The value a prefab GameObject property has in its prefab (the instance's own override
    // left out): a copy of the component in a registry of its own, references already in the
    // instance's local ids
    struct PrefabSourceValue {
        std::unique_ptr<EntityRegistry> registry;
        Entity entity = NULL_ENTITY;
        PropertyData property{};                  // property.ref points into `registry`
        YAML::Node component;                     // the whole component as Stream encodes it
        bool valid() const { return registry && property.ref; }
    };

    // What putting prefab assets and the instances using them back takes: the undo of an
    // override operation (Project::capturePrefabUndoState)
    struct PrefabUndoState {
        struct Asset {
            std::filesystem::path path;
            YAML::Node node;          // Project::encodePrefabNode
            bool isModified = false;
            bool needsUpgrade = false;
        };
        struct InstanceState {
            uint32_t sceneId = NULL_PROJECT_SCENE;
            Entity root = NULL_ENTITY;   // an outermost instance root
            YAML::Node rootNode;         // the root as the scene file writes it
            bool sceneModified = false;
        };
        std::vector<Asset> assets;
        std::vector<InstanceState> instances;
    };

    struct SharedMoveRecoveryEntry {
        Entity oldParent;
        size_t oldIndex;
        size_t hasTransform;
        // An instance where the move changed whether the GameObject is there: it went below a
        // GameObject the instance removed (taken out, put back by the undo), or came out from
        // below one (brought in, taken out again by the undo)
        PrefabBranchRecovery left;
        Entity entered = NULL_ENTITY;
    };

    struct TerrainEditorSettings {
        int brushMode = 0;           // TerrainBrushMode::Raise
        int brushShape = 0;          // TerrainBrushShape::Circle
        int brushFalloff = 0;        // TerrainBrushFalloff::Smooth
        float brushSize = 4.0f;
        float brushStrength = 0.3f;  // normalized flow per second at full falloff
        float brushRotation = 0.0f;  // degrees, turns the brush mask
        std::string brushMaskPath;   // grayscale falloff, and the Stamp brush's relief
        float flattenHeight = 0.5f;
        int terraceSteps = 8;
        float noiseSize = 4.0f;      // width of one noise feature in world units
        int heightMapResolution = 512;
        int blendMapResolution = 512;
        int densityMapResolution = 512;
        bool normalizeBlendPaint = true;
        bool heightMapStartAtMiddle = true;
        bool flattenPickOnStroke = true;
        std::string placeAssetPath;
        bool placeInstanced = true;      // one draw per asset instead of one entity per object
        float placeSpacing = 2.0f;
        float placeMinScale = 1.0f;      // 1.0 keeps the scale the model file authored
        float placeMaxScale = 1.0f;
        float placeRotationJitter = 1.0f;
        float placeAlignToNormal = 0.0f;

        bool paintUseMask = false;       // restrict texture paint to a slope and height range
        float paintMinSlope = 0.0f;      // degrees
        float paintMaxSlope = 90.0f;
        float paintMinHeight = 0.0f;     // normalized against the terrain maxHeight
        float paintMaxHeight = 1.0f;
    };

    // Manual picks on top of the shaders every export mode collects from the scenes
    struct ShaderOverrides {
        std::set<ShaderKey> additions;
        std::set<ShaderKey> exclusions;
    };

    struct SourceCodeExportSettings {
        std::set<ShaderBackend> graphicBackends;
        bool graphicBackendsConfigured = false;
    };

    struct DesktopExportSettings {
        ShaderBackend graphicBackend = ShaderBackend::GLCore;
        bool graphicBackendConfigured = false;
    };

    struct ApplicationSettings {
        std::string name;  // empty = project name
        std::string identifier = "com.yourcompany.project";
        std::string version = "1.0";
        unsigned int build = 1;  // Apple build number and Android version code
    };

    // Name, identifier, version and build below are overrides: empty (or 0)
    // means the ApplicationSettings value.

    struct WebProjectSettings {
        std::string applicationName;
        std::filesystem::path favicon;
        std::filesystem::path customHtmlShell;
        std::string headInclude;
        bool resizeCanvasToWindow = true;
        bool hideEmscriptenUI = false;
    };

    struct LinuxProjectSettings {
        std::string applicationName;
        std::string comment;
        std::string categories = "Game;";
    };

    struct WindowsProjectSettings {
        std::string productName;
        std::string companyName;
        std::string fileVersion;
        std::string productVersion;
    };

    struct MacOSProjectSettings {
        std::string applicationName;
        std::string bundleIdentifier;
        std::string versionName;
        std::string buildNumber;
        std::filesystem::path icon;
        bool highDpi = true;
    };

    struct IOSProjectSettings {
        std::string applicationName;
        std::string bundleIdentifier;
        std::string versionName;
        std::string buildNumber;
        std::filesystem::path icon;
        bool hideStatusBar = true;
        bool hideHomeIndicator = true;
        bool supportsHighRefreshRate = true;
    };

    enum class AndroidOrientation {
        Unspecified,
        Portrait,
        Landscape,
        SensorPortrait,
        SensorLandscape,
        FullSensor
    };

    struct AndroidProjectSettings {
        std::string applicationName;
        std::string packageName;
        unsigned int versionCode = 0;
        std::string versionName;
        std::filesystem::path launcherIcon;
        std::filesystem::path adaptiveIconForeground;
        std::filesystem::path adaptiveIconBackground;
        unsigned int minSdk = 21;
        unsigned int targetSdk = 36;
        AndroidOrientation orientation = AndroidOrientation::Unspecified;
        bool abiArmeabiV7a = true;
        bool abiArm64V8a = true;
        bool abiX86 = true;
        bool abiX86_64 = true;
        std::set<std::string> permissions;
        bool allowBackup = true;
        bool fullscreen = true;
        bool keepScreenOn = false;
    };

    using SharedMoveRecovery = std::map<std::string, SharedMoveRecoveryEntry>;

    struct ComponentRecoveryEntry {
        Entity entity;
        YAML::Node node;
    };

    using ComponentRecovery = std::map<std::string, ComponentRecoveryEntry>;

    class Project{
    private:

        Conector conector;
        Generator generator;

        std::string name;

        unsigned int canvasWidth;
        unsigned int canvasHeight;
        Scaling scalingMode;
        TextureStrategy textureStrategy;
        bool vsyncEnabled;
        WindowMode windowMode;
        unsigned int windowWidth;
        unsigned int windowHeight;
        bool windowResizable;
        std::string windowTitle;  // empty = use the project name
        std::filesystem::path windowIcon;  // project-relative image path; empty = no icon
        std::filesystem::path assetsDir;
        std::filesystem::path luaDir;
        std::vector<std::filesystem::path> scriptDirs;  // extra C++ include and source roots
        int cxxStandard;  // C++ standard for Play and exported builds
        bool physics2DEnabled;  // Box2D in exported builds; Play always has it
        bool physics3DEnabled;  // Jolt in exported builds; Play always has it
        bool runtimeShadersEnabled;  // exported builds compile the shader variants they lack
        // Tags and Layers: user tags, and user layer names indexed by layer (MAX_LAYERS
        // entries, empty for built-in and unnamed layers)
        std::vector<std::string> tags;
        std::vector<std::string> layerNames;
        // Layer Collision Matrices of Physics (3D) and Physics 2D, all layers collide by default
        LayerCollisionRows layerCollisionMatrix3D = allLayersCollide();
        LayerCollisionRows layerCollisionMatrix2D = allLayersCollide();
        bool packNativeResources;
        bool versionControlMetadata;
        ShaderOverrides shaderOverrides;
        SourceCodeExportSettings sourceCodeExportSettings;
        DesktopExportSettings desktopExportSettings;
        ApplicationSettings applicationSettings;
        WebProjectSettings webProjectSettings;
        LinuxProjectSettings linuxProjectSettings;
        WindowsProjectSettings windowsProjectSettings;
        MacOSProjectSettings macOSProjectSettings;
        IOSProjectSettings iosProjectSettings;
        AndroidProjectSettings androidProjectSettings;
        CommandHistory projectHistory;

        uint32_t startSceneId;
        LoadingSettings loadingSettings;
        TerrainEditorSettings terrainEditorSettings;
        bool simulateTouch;

        // True while project.yaml holds the only copy of this user's editor state
        bool workspaceMigrationPending = false;

        // True while the model describes what is on disk at projectPath.
        // createTempProject() resets it and then points projectPath at the project it
        // is about to open, so a non-empty projectPath alone does not imply it.
        bool modelLoaded = false;

        // True when the standalone build sources need an update on the main thread
        std::atomic<bool> generatedSourcesDirty{false};

        std::vector<SceneProject> scenes;

        // Prefab Mode: the open prefab stages, bottom first (editor/PrefabStage.cpp)
        std::vector<std::unique_ptr<PrefabStage>> prefabStages;
        bool prefabAutoSave = true;
        PrefabContextVisibility prefabContextVisibility = PrefabContextVisibility::Gray;
        // Saved from a stage while a scene played: the scene restored by Stop follows them then
        std::set<std::filesystem::path> prefabsSavedWhilePlaying;
        SceneProject* findPrefabStageScene(uint32_t sceneId) const;
        bool pushPrefabStage(const std::filesystem::path& path, uint32_t contextSceneId, Entity contextRoot, bool inContext);
        bool loadPrefabStageContent(PrefabStage& stage);
        void destroyPrefabStageContent(PrefabStage& stage);
        void popPrefabStage();
        YAML::Node encodePrefabStage(PrefabStage& stage);
        void fixPrefabStageRoots(PrefabStage& stage);
        void releasePrefabStageContext(PrefabStage& stage);

        // Scenes project.yaml lists that could not be loaded, the position they held
        // in that list and the view state kept for them. They never become a
        // SceneProject, so a save would otherwise drop them from the shared file.
        std::vector<std::pair<size_t, std::filesystem::path>> unresolvedScenes;
        std::map<std::string, YAML::Node> unresolvedSceneStates;
        std::vector<TabEntry> tabs;

        struct PlayRuntimeScene {
            uint32_t sourceSceneId = NULL_PROJECT_SCENE;
            SceneProject* runtime = nullptr; // Scene used by the Engine while playing
            bool ownedRuntime = false;       // true when runtime was cloned and must be deleted
            bool initialized = false;        // true when scripts and library are initialized
            bool initializing = false;       // true while its scripts run, to stop re-entrant init
        };

        struct PlaySession {
            uint32_t mainSceneId = NULL_PROJECT_SCENE;
            std::vector<PlayRuntimeScene> runtimeScenes;
            std::atomic<bool> cancelled{false};
            std::atomic<bool> startupThreadDone{false};  // Set when connect thread exits
            std::atomic<bool> startupSucceeded{false};   // True only if finalizeStart was called
        };

        mutable std::mutex playSessionMutex;
        std::shared_ptr<PlaySession> activePlaySession;

        // Single-frame advance while paused (see step())
        uint32_t stepSceneId = NULL_PROJECT_SCENE;
        bool stepPending = false;
        float stepSavedMaxDeltatime = 0.0f;

        SceneProject* createRuntimeCloneFromSource(const SceneProject* source);

        // Play copies (the child scenes of the played scene run as copies, see PlaySession)
        // answer to ids of their own while the session lasts, so getScene(), selection,
        // the Hierarchy, the Inspector and commands reach them like any scene. Guarded by
        // its own mutex: getScene() runs under playSessionMutex in places.
        mutable std::mutex playRuntimeIdMutex;
        std::unordered_map<uint32_t, SceneProject*> playRuntimeIds;
        std::atomic<bool> hasPlayRuntimeIds{false};
        SceneProject* findPlayRuntimeScene(uint32_t sceneId) const;
        // Gives a Play copy its id (any thread)
        void assignPlayRuntimeId(SceneProject* runtime);
        // Takes it back before the copy goes: the copy has its source's id again (any thread)
        void releasePlayRuntimeId(SceneProject* runtime, uint32_t sourceSceneId);
        // What the editor kept for a Play copy id: selection, prefab instances, history
        void discardPlayRuntimeState(uint32_t runtimeId, uint32_t sourceSceneId, uint32_t mainSceneId);
        std::vector<SceneProject*> getPlayRuntimeScenesOf(uint32_t sceneId);
        // A selection change redraws the scene and the scenes it is an inline child of
        void markSelectionChanged(SceneProject* sceneProject);
        // The GameObjects a click and the box select from in a scene the Scene view draws:
        // the authored ones, and while it runs what the game made in it
        std::vector<Entity> getPickableEntities(const SceneProject* sceneProject) const;
        void prepareRuntimeScene(PlayRuntimeScene& entry);
        std::shared_ptr<PlaySession> buildRuntimeSceneStack(uint32_t sceneId, std::vector<size_t>& stackIndices);
        Entity getSceneCamera(const SceneProject* sceneProject) const;
        void cleanupPlaySession(const std::shared_ptr<PlaySession>& session);

        SceneRender* createSceneRender(SceneType type, Scene* scene) const;
        Entity createDefaultCamera(SceneType type, Scene* scene) const;
        AABB getEntityWorldAABB(Scene* scene, Entity entity, Scene* mainScene) const;
        AABB getEntityLocalAABB(Scene* scene, Entity entity) const;
        // sizeScene sizes the handles of points and lines; iconsDrawn: lights, cameras and
        // audio sources show icons in this view (only the view's own scene draws them)
        Entity findBestEntityByRay(const std::vector<Entity>& entities, Scene* scene, const Ray& ray, Scene* sizeScene, SceneType sceneType, float& distance, size_t& index, bool iconsDrawn = true) const;
        bool selectEntitiesInRect(uint32_t sceneId, const std::vector<Entity>& entities, Scene* scene, const Matrix4& vpMatrix, Vector2 start, Vector2 end);
        uint32_t selectedScene;
        uint32_t selectedSceneForProperties;

        using MaterialLinkKey = std::tuple<uint32_t, Entity, unsigned int>;
        struct MaterialLinkEntry {
            std::string filePath;
            std::filesystem::file_time_type lastWriteTime;
        };

        std::filesystem::path projectPath;
        bool resourcesFocused;
        std::map<MaterialLinkKey, MaterialLinkEntry> materialFileLinks;
        std::chrono::steady_clock::time_point lastMaterialRefreshTime;
        static constexpr double materialRefreshIntervalSec = 0.2;

        std::map<std::filesystem::path, Prefab> prefabAssets;

        // Parsed prefab files, reused while the file on disk keeps its time and size:
        // pasting a hundred instances parses the file once instead of per instance
        struct PrefabFileCache {
            std::filesystem::file_time_type writeTime{};
            std::uintmax_t size = 0;
            YAML::Node node;
        };
        std::map<std::filesystem::path, PrefabFileCache> prefabFileCache;
        YAML::Node loadPrefabFile(const std::filesystem::path& fullPath);
        std::vector<std::filesystem::path> standalonePrefabs;  // built without a scene instance

        // GUID <-> path of the prefab files (and later other asset types)
        AssetDatabase assetDatabase;
        std::chrono::steady_clock::time_point lastAssetRefreshTime;
        // Found while resolving references (in the middle of an import), applied on refresh
        std::vector<AssetDatabase::Change> pendingAssetChanges;

        // What the database reports changed: moved prefabs get their new path everywhere in
        // memory, removed ones leave Missing Prefab instances behind, added ones relink them
        void applyAssetChanges(const std::vector<AssetDatabase::Change>& changes);
        // The in-memory part of a prefab rename or move (a file or a directory): prefab assets,
        // scene and prefab PrefabComponents, scene prefab lists and the standalone list
        void remapPrefabPathsInMemory(const fs::path& oldRelative, const fs::path& newRelative);
        // Every instance of the prefab in the loaded scenes becomes a Missing Prefab
        // instance (its root and what the scene stored for it stay), then the asset is dropped
        void convertPrefabInstancesToMissing(const std::filesystem::path& filepath);
        void collectPrefabMemberIds(uint32_t sceneId, const Prefab::Instance& instance, PrefabMemberIds& ids, size_t depth = 0) const;
        // A listed standalone prefab not loaded yet (its file came back)
        void loadStandalonePrefab(const std::filesystem::path& filepath);
        // The registry of a prefab file, read and upgraded (Prefab::loadRegistry)
        bool readPrefabFile(const std::filesystem::path& filepath, Prefab& prefab);
        // Outer layers of the instance nested at registryRoot of `prefab`: its asset-level data
        // there, converted first when read from a file written before the real-root model
        PrefabInstanceData* nestedAssetLayer(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryRoot, Entity localRoot,
            SceneProject* sceneProject);
        // Prefab lists and the nested prefab references of a loaded prefab registry follow
        // their GUIDs (in memory only: the files keep the old path as a hint until saved)
        void resolveScenePrefabInfos(SceneProject& sceneProject);
        void resolveRegistryPrefabRefs(EntityRegistry* registry);
        // The generated source of a closed scene calls its prefabs by path-derived names:
        // one whose prefab moved, went or came back is written again (from its file)
        void invalidateClosedSceneSources(const std::function<bool(const PrefabSceneInfo&)>& affected);

        std::string libName;

        template<typename T>
        T* findScene(uint32_t sceneId) const;

        Entity createNewEntity(uint32_t sceneId, std::string entityName);
        bool createNewComponent(uint32_t sceneId, Entity entity, ComponentType component);
        void calculateSceneMaxValues(const SceneProject* sceneProject, SceneMaxValues& maxValues) const;
        void resetEngineConfigs(bool executeViewChanged);
        void resetConfigs();

        void updateSceneCppScripts(SceneProject* sceneProject);
        void updateScenePrefabs(SceneProject* sceneProject);
        void removePrefabInstanceTracking(uint32_t sceneId, Entity rootEntity);

        // removeEntityFromPrefab and addEntityToPrefab(recovery) on a registry entity: the
        // source instance is the one whose own copy destroyItself/createItself applies to
        NodeRecovery removeRegistryEntityFromPrefab(const std::filesystem::path& filepath, Entity registryEntity,
            uint32_t sourceSceneId, uint32_t sourceInstanceId, bool destroyItself);
        bool restoreRegistryEntityToPrefab(const std::filesystem::path& filepath, Entity registryParent, const NodeRecovery& recoveryData,
            uint32_t sceneId, uint32_t sourceInstanceId, bool createItself, std::unordered_map<Entity, Entity>* entityRemap,
            std::vector<Entity>* restoredEntities);
        // Creates in an instance the registry branch at registryEntity, less what the instance
        // removed, under its registry parent's GameObject and in Prefab order; preferredIds
        // (registry -> local pairs) are taken where free. Returns the members made, the top
        // first (nested instances not included); empty when its parent is absent.
        std::vector<Prefab::EntityMember> instantiatePrefabBranch(SceneProject* sceneProject, const std::filesystem::path& filepath, uint32_t instanceId,
            Entity registryEntity, const std::vector<Prefab::EntityMember>& preferredIds);
        // Puts a GameObject of an instance at its Prefab place among the siblings it holds
        void placeInPrefabOrder(SceneProject* sceneProject, const Prefab& prefab, const Prefab::Instance& instance,
            Entity registryEntity, Entity localEntity, Entity localParent, bool keepWorldTransform);

        std::vector<SceneScriptSource> collectAllSceneCppScripts() const;
        std::vector<PrefabSceneInfo> collectAllPrefabs() const;

        void pauseEngineScene(Scene* scene, bool pause) const;

        void copyEngineApiToProject();

        void registerSceneManager();
        SceneResources collectPlaySceneResources(uint32_t sceneId);
        void registerPrefabManager();

        SceneProject* findSceneProjectByScene(Scene* scene);

        static bool matchesRelativePath(const fs::path& relativeBase, const fs::path& currentPath);
        static bool matchesRelativeString(const fs::path& relativeBase, const std::string& currentPath);
        static std::vector<ChildSceneRef>::iterator findChildScene(std::vector<ChildSceneRef>& childScenes, uint32_t childSceneId);
        static std::vector<ChildSceneRef>::const_iterator findChildScene(const std::vector<ChildSceneRef>& childScenes, uint32_t childSceneId);
        static bool eraseChildSceneReference(std::vector<ChildSceneRef>& childScenes, uint32_t childSceneId);
        static bool remapRelativePath(const fs::path& oldRelative, const fs::path& newRelative,
                          const fs::path& currentPath, fs::path& updatedPath);
        static bool remapRelativeString(const fs::path& oldRelative, const fs::path& newRelative,
                        const std::string& currentPath, std::string& updatedPath);
        static bool remapScriptEntryPaths(ScriptEntry& scriptEntry, const fs::path& oldRelative,
                          const fs::path& newRelative);
        // Absolute paths: Lua entries match against the Lua root, C++ entries against the project.
        bool remapScriptPathsInRegistry(EntityRegistry* registry, const fs::path& oldPath,
                        const fs::path& newPath);
        bool cleanupScriptPathsInRegistry(EntityRegistry* registry, const fs::path& deletedPath);
        // Runs transform over every stored asset reference (component textures and cubemap
        // faces, model and sound filenames, text fonts), rewriting the value in place.
        static bool visitAssetPathsInRegistry(EntityRegistry* registry, const std::function<bool(std::string&)>& transform);
        // The files of those references, for SceneManager::preloadScene()
        static SceneResources collectResourcesInRegistry(EntityRegistry* registry);
        // Same for Lua script entries; C++ ones are build inputs kept project-relative.
        static bool visitLuaPathsInRegistry(EntityRegistry* registry, const std::function<bool(std::string&)>& transform);
        bool visitAssetPathsInMaterialFiles(const std::function<bool(std::string&)>& transform);
        // Runs an asset transform over every scene, prefab and material file,
        // marking what changed.
        void applyAssetPathChange(const std::function<bool(std::string&)>& transform);
        // Applies a transform to the customShader of every renderable component (Mesh/UI/
        // Points/Lines/Sky) across all scenes and prefabs, flagging shader reloads
        // and marking affected scenes/prefabs modified. The transform mutates the value in
        // place and returns whether it changed.
        void applyCustomShaderPathChange(const std::function<bool(std::string&)>& transform);
        bool removeMissingChildSceneReferences(SceneProject& sceneProject);
        bool detachChildSceneFromParents(uint32_t childSceneId, const std::set<uint32_t>& skippedSceneIds);

        void destroyPlayCreatedEntities(SceneProject* sceneProject);
        void finalizeStart(SceneProject* mainSceneProject, std::vector<PlayRuntimeScene>& runtimeScenes);
        void finalizeStop(SceneProject* mainSceneProject, std::vector<PlayRuntimeScene> runtimeScenes);
        void runPlayStartup(const std::shared_ptr<PlaySession>& session, uint32_t sceneId, const LocalBuildSettings& buildSettings);
        void failPlayStartup(const std::shared_ptr<PlaySession>& session, uint32_t sceneId, const std::string& message,
                     const std::string& alertTitle = "", const std::string& alertMessage = "");
        bool saveSceneFile(SceneProject* sceneProject, const std::filesystem::path& path, bool stopTransientPreviews = true);
        bool saveSceneForPlayStartup(SceneProject* sceneProject);
        bool saveLoadedSceneOnStop(SceneProject* loadedSceneProject, SceneProject* editorScene, bool keepModified);
        bool writeSceneToPath(uint32_t sceneId, const std::filesystem::path& path, bool stopTransientPreviews = true);
        void writeSceneSource(const SceneProject* sceneProject);
        bool hasMissingSceneSources() const;
        void writeMissingSceneSources();
        void configureGenerator();
        void saveModifiedChildScenes(uint32_t sceneId, std::function<void(bool)> callback = nullptr);
        bool hasSceneUnsavedChangesImpl(uint32_t sceneId, std::unordered_set<uint32_t>& visited) const;
        void saveSceneListSequentially(std::vector<uint32_t> sceneIds, std::function<void(bool)> callback);

        void collectInvolvedScenes(uint32_t sceneId, std::vector<uint32_t>& involvedSceneIds);
        void collectStartActiveScenes(uint32_t sceneId, std::vector<uint32_t>& activeSceneIds);

        uint32_t createNewSceneInternal(std::string sceneName, SceneType type, uint32_t previousSceneId);

        // Restore the view state kept while a file was unreadable, put the scene back
        // at its listed position, and roll back what a failed load built. Reordering
        // invalidates pointers into scenes.
        void applyRetainedSceneState(SceneProject& sceneProject);
        void restoreUnresolvedScenePosition(const std::filesystem::path& filepath);
        void discardHalfLoadedScene(SceneProject* sceneProject, bool appended);

        void markReferencedSceneIdsPersisted(EntityRegistry* registry);
        bool clearEntityReferencesInScene(SceneProject& sceneProject, uint32_t sceneId);
        void purgeReferencesToScene(uint32_t sceneId);
        std::vector<std::filesystem::path> findProjectFiles(const std::function<bool(const std::string&)>& matches) const;
        void openSceneInternal(fs::path filepath, uint32_t sceneToClose);

        // The standalone build reads shaders from the editor cache (JASPERENGINE_SHADER_PATH)
        // and cannot compile, so a variant the editor never draws with is missing there.
        void buildStandaloneShaderCache();

    public:
        // Factory defaults, also used by the settings dialog to restore a value
        static constexpr unsigned int defaultCanvasWidth = 1280;
        static constexpr unsigned int defaultCanvasHeight = 720;
        static constexpr Scaling defaultScalingMode = Scaling::FITWIDTH;
        static constexpr TextureStrategy defaultTextureStrategy = TextureStrategy::NONE;
        static constexpr bool defaultVSyncEnabled = true;
        static constexpr WindowMode defaultWindowMode = WindowMode::WINDOWED;
        static constexpr unsigned int defaultWindowWidth = defaultCanvasWidth;
        static constexpr unsigned int defaultWindowHeight = defaultCanvasHeight;
        static constexpr bool defaultWindowResizable = true;
        static constexpr const char* defaultWindowTitle = "";
        static constexpr const char* defaultAssetsDir = ".";
        static constexpr const char* defaultLuaDir = ".";
        static constexpr bool defaultPackNativeResources = false;
        static constexpr bool defaultVersionControlMetadata = true;
        static constexpr bool defaultPhysics2DEnabled = true;
        static constexpr bool defaultPhysics3DEnabled = true;
        static constexpr bool defaultRuntimeShadersEnabled = true;
        static constexpr int defaultCxxStandard = cxxStandards[0];

        Project();

        // Live-derive the shader keys a scene needs from its current components
        // (used at scene save and by the Export window's pre-populated list).
        void collectSceneShaderKeys(const SceneProject* sceneProject, std::set<ShaderKey>& shaderKeys) const;
        // The shader keys a loaded scene needs, derived from content without drawing anything:
        // its GameObjects and every prefab of the build (one a script spawns into it), in every
        // lighting setup they can make (see ShaderVariantCollector)
        void collectRequiredShaderKeys(const SceneProject* sceneProject, std::set<ShaderKey>& shaderKeys) const;

        std::string getName() const;
        void setName(std::string name);

        void setCanvasSize(unsigned int width, unsigned int height);
        unsigned int getCanvasWidth() const;
        unsigned int getCanvasHeight() const;

        void setScalingMode(Scaling scalingMode);
        Scaling getScalingMode() const;

        void setTextureStrategy(TextureStrategy textureStrategy);
        TextureStrategy getTextureStrategy() const;

        void setVSyncEnabled(bool enabled);
        bool isVSyncEnabled() const;

        void setWindowMode(WindowMode windowMode);
        WindowMode getWindowMode() const;

        void setWindowSize(unsigned int width, unsigned int height);
        unsigned int getWindowWidth() const;
        unsigned int getWindowHeight() const;

        void setWindowResizable(bool resizable);
        bool isWindowResizable() const;

        // Stored title; empty means "use the project name"
        void setWindowTitle(const std::string& title);
        std::string getWindowTitle() const;

        void setWindowIcon(const std::filesystem::path& iconPath);
        std::filesystem::path getWindowIcon() const;

        // Window configuration for generated/exported desktop builds,
        // with the title fallback already resolved
        WindowSettings getWindowSettings() const;

        void setAssetsDir(const std::filesystem::path& assetsDir);
        std::filesystem::path getAssetsDir() const;

        void setLuaDir(const std::filesystem::path& luaDir);
        std::filesystem::path getLuaDir() const;

        // Roots added to the C++ build: each is an include directory, and the
        // sources under it compile without a script component referencing them.
        void setScriptDirs(std::vector<std::filesystem::path> scriptDirs);
        const std::vector<std::filesystem::path>& getScriptDirs() const;

        void setCxxStandard(int standard);
        int getCxxStandard() const;

        void setPhysics2DEnabled(bool enabled);
        bool isPhysics2DEnabled() const;

        void setPhysics3DEnabled(bool enabled);
        bool isPhysics3DEnabled() const;

        // Project Settings > Build > Runtime Shader Compilation: an exported build carries
        // the shader compiler and builds the variants it does not ship precompiled
        // (scripted meshes and materials) instead of leaving out what draws with them.
        // The editor always compiles.
        void setRuntimeShadersEnabled(bool enabled);
        bool isRuntimeShadersEnabled() const;

        // Project Settings > Tags and Layers. Also sets the engine's TagManager, which the
        // Inspector and Play read; names are kept as TagManager accepts them (trimmed,
        // no empty or repeated tag, no repeated layer name, built-in layers untouched).
        void setTagsAndLayers(const std::vector<std::string>& tags, const std::vector<std::string>& layerNames);
        const std::vector<std::string>& getTags() const;
        // MAX_LAYERS entries, index = layer; empty for built-in and unnamed layers
        const std::vector<std::string>& getLayerNames() const;

        // Project Settings > Physics and Physics 2D > Layer Collision Matrix. Kept symmetric
        // (a pair collides only when both rows say so) and applied to the engine's
        // Physics and Physics2D, which the editor's Play reads.
        void setLayerCollisionMatrices(const LayerCollisionRows& matrix3D, const LayerCollisionRows& matrix2D);
        const LayerCollisionRows& getLayerCollisionMatrix3D() const;
        const LayerCollisionRows& getLayerCollisionMatrix2D() const;
        // Puts the project's matrices back into the engine, undoing what scripts changed
        // with Physics.ignoreLayerCollision during a Play
        void applyLayerCollisionMatrices() const;

        // Absolute roots stored references are relative to: assets for textures, models,
        // sounds and fonts ("asset://"), Lua for script entries ("lua://").
        std::filesystem::path getAssetsPath() const;
        std::filesystem::path getLuaPath() const;

        std::filesystem::path normalizeToProjectRelative(const std::filesystem::path& path) const;
        std::filesystem::path resolveAssetPath(const std::filesystem::path& assetRelative) const;
        std::filesystem::path normalizeToAssetsRelative(const std::filesystem::path& path) const;
        // False for files the assets root only reaches with "..", which the runtime cannot resolve.
        bool isInsideAssetsPath(const std::filesystem::path& path) const;
        std::filesystem::path resolveLuaPath(const std::filesystem::path& luaRelative) const;
        std::filesystem::path normalizeToLuaRelative(const std::filesystem::path& path) const;

        // Applies new roots: referenced files outside them are moved in keeping their
        // layout, then every stored reference is rewritten and saved.
        void changeAssetRoots(const std::filesystem::path& newAssetsDir, const std::filesystem::path& newLuaDir);

        void setPackNativeResources(bool enabled);
        bool shouldPackNativeResources() const;

        void setVersionControlMetadata(bool enabled);

        bool hasVersionControlMetadata() const;

        ShaderOverrides& getShaderOverrides();
        const ShaderOverrides& getShaderOverrides() const;
        SourceCodeExportSettings& getSourceCodeExportSettings();
        const SourceCodeExportSettings& getSourceCodeExportSettings() const;
        DesktopExportSettings& getDesktopExportSettings();
        const DesktopExportSettings& getDesktopExportSettings() const;
        ApplicationSettings& getApplicationSettings();
        const ApplicationSettings& getApplicationSettings() const;

        // The platform override when set, otherwise the shared block.
        std::string getApplicationName(const std::string& platformOverride = {}) const;
        std::string getApplicationIdentifier(const std::string& platformOverride = {}) const;
        std::string getApplicationVersion(const std::string& platformOverride = {}) const;
        std::string getApplicationBuild(const std::string& platformOverride = {}) const;
        unsigned int getApplicationVersionCode(unsigned int platformOverride = 0) const;
        std::string getApplicationFileVersion(const std::string& platformOverride = {}) const;

        // Version shapes each platform requires, also used by the settings UI.
        static std::string toFourPartVersion(const std::string& version);
        static std::string toAppleVersion(const std::string& version);

        WebProjectSettings& getWebProjectSettings();
        const WebProjectSettings& getWebProjectSettings() const;
        LinuxProjectSettings& getLinuxProjectSettings();
        const LinuxProjectSettings& getLinuxProjectSettings() const;
        WindowsProjectSettings& getWindowsProjectSettings();
        const WindowsProjectSettings& getWindowsProjectSettings() const;
        MacOSProjectSettings& getMacOSProjectSettings();
        const MacOSProjectSettings& getMacOSProjectSettings() const;
        IOSProjectSettings& getIOSProjectSettings();
        const IOSProjectSettings& getIOSProjectSettings() const;
        AndroidProjectSettings& getAndroidProjectSettings();
        const AndroidProjectSettings& getAndroidProjectSettings() const;

        uint32_t getStartSceneId() const;
        void setStartSceneId(uint32_t sceneId);

        LoadingSettings& getLoadingSettings();
        const LoadingSettings& getLoadingSettings() const;

        TerrainEditorSettings& getTerrainEditorSettings();
        const TerrainEditorSettings& getTerrainEditorSettings() const;

        // Play acts as a touch device: the mouse is one finger and isTouchDevice() is true
        bool isSimulateTouch() const;
        void setSimulateTouch(bool simulateTouch);

        CommandHistory* getProjectCommandHistory();

        bool createTempProject(std::string projectName, bool deleteIfExists = false);
        bool saveProjectToPath(const std::filesystem::path& path);
        void clearTrash();

        // Keeps .gitignore and .gitattributes written, skipping either if it exists
        // and was not generated here
        bool writeVersionControlMetadata();
        void deleteSceneProject(SceneProject* sceneProject);
        void loadSceneProjectData(SceneProject* sceneProject, const YAML::Node& sceneNode);
        bool saveProject(bool userCalled = false, std::function<void()> callback = nullptr);
        bool saveProjectFile();
        // Per-user editor state (.jasperengine/user/workspace.yaml), never project.yaml
        bool saveWorkspaceFile();

        bool loadProject(const std::filesystem::path path, bool updateLastOpened = true);

        void refreshLinkedMaterials(bool force = false);
        void updateGeneratedSources();
        void markGeneratedSourcesDirty();

        //=== Linked Material part ===

        void linkMaterialFile(uint32_t sceneId, Entity entity, unsigned int submeshIndex, const std::string& filePath);
        bool isMaterialFileLinked(uint32_t sceneId, Entity entity, unsigned int submeshIndex) const;
        std::string getMaterialFilePath(uint32_t sceneId, Entity entity, unsigned int submeshIndex) const;
        void unlinkMaterialFile(uint32_t sceneId, Entity entity, unsigned int submeshIndex);
        void unlinkAllMaterialFiles(uint32_t sceneId, Entity entity);
        void unlinkAllMaterialFiles(uint32_t sceneId);

        //=== end Linked Material part ===

        //=== File path remapping ===

        void remapMaterialFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        void remapSceneFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        void remapPrefabFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        void remapScriptFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        void remapAssetFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        void remapShaderFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        void cleanupMaterialFilePath(const std::filesystem::path& deletedPath);
        void cleanupSceneFilePath(const std::filesystem::path& deletedPath);
        void cleanupPrefabFilePath(const std::filesystem::path& deletedPath);
        void cleanupScriptFilePath(const std::filesystem::path& deletedPath);
        void cleanupAssetFilePath(const std::filesystem::path& deletedPath);
        void cleanupShaderFilePath(const std::filesystem::path& deletedPath);

        //=== end File path remapping ===

        void checkUnsavedAndExecute(uint32_t sceneId, std::function<void()> action);

        void saveScene(uint32_t sceneId, std::function<void(bool)> callback = nullptr);
        bool saveSceneToPath(uint32_t sceneId, const std::filesystem::path& path, bool stopTransientPreviews = true);
        void saveSceneToPathAsync(uint32_t sceneId, const std::filesystem::path& path, std::function<void(bool)> callback = nullptr);
        void saveAllScenes(std::function<void(bool)> callback = nullptr);
        void saveLastSelectedScene(std::function<void(bool)> callback = nullptr);

        uint32_t createNewScene(std::string sceneName, SceneType type);
        // False when the scene could not be read, which does not mean it is gone:
        // the file may be unreadable only in this checkout
        bool loadScene(fs::path filepath, bool opened, bool isNewScene = true, bool loadSceneData = true);
        void openScene(fs::path filepath, bool closePrevious = true);
        void closeScene(uint32_t sceneId, bool systemClose = false);
        void removeScene(uint32_t sceneId);

        std::vector<std::filesystem::path> findSceneFiles() const;
        uint32_t findSceneByPath(const std::filesystem::path& filepath) const;

        bool loadChildSceneInline(uint32_t childSceneId);
        void unloadChildSceneInline(uint32_t childSceneId);

        void addChildScene(uint32_t sceneId, uint32_t childSceneId, bool startActive = true);
        void removeChildScene(uint32_t sceneId, uint32_t childSceneId);
        // Dirties parents of a child scene and forces the Engine layers to rebuild.
        void markParentScenesNeedUpdate(uint32_t childSceneId);
        bool hasChildScene(uint32_t sceneId, uint32_t childSceneId) const;
        bool isChildSceneStartActive(uint32_t sceneId, uint32_t childSceneId) const;
        void setChildSceneStartActive(uint32_t sceneId, uint32_t childSceneId, bool startActive);
        std::vector<uint32_t> getChildScenes(uint32_t sceneId) const;

        // A scene as the Scene view of another draws it, see getSceneViewLayers()
        struct SceneViewLayer{
            uint32_t sceneId = NULL_PROJECT_SCENE;   // a scene, or a Play copy (getScene() takes both)
            SceneProject* sceneProject = nullptr;
            bool base = false;      // the view's own scene, through its editor camera
            bool running = false;   // a scene of the running game
            LayerView view;         // the others: their camera as the view draws them
        };
        // The scenes the Scene view of sceneId draws, bottom first: the scene itself, then
        // its inline child scenes while it is stopped, or the running game's layer scenes
        // (child scenes nested at any depth, and those scripts added) while it plays
        std::vector<SceneViewLayer> getSceneViewLayers(uint32_t sceneId);
        // A scene whose GameObjects the Scene view of viewSceneId shows as a layer
        bool isSceneViewLayer(uint32_t viewSceneId, uint32_t sceneId);

        // The topmost GameObject drawn at (x, y) of the Scene view of sceneId: layers over
        // the scene first, as drawn. Without outSceneId only the scene itself is tested.
        Entity findObjectByRay(uint32_t sceneId, float x, float y, uint32_t* outSceneId = nullptr);
        // A viewport click only lands on entities with a Transform and bounds of their own
        bool isEntityPickable(uint32_t sceneId, Entity entity) const;

        bool selectObjectByRay(uint32_t sceneId, float x, float y, bool shiftPressed);
        bool selectObjectsByRect(uint32_t sceneId, Vector2 start, Vector2 end);

        // The scene the transform tools of sceneId's Scene view work on: a layer whose
        // GameObjects are selected (the properties scene), else sceneId. Sets the edit
        // layer of its SceneRender to match and returns that scene's id.
        uint32_t syncSceneRenderEditLayer(uint32_t sceneId);
        // syncSceneRenderEditLayer(), then the SceneRender's update with the scene's selection
        void updateSceneRender(uint32_t sceneId);

        // The Play copy of sourceSceneId in the active session, NULL_PROJECT_SCENE if none
        uint32_t getPlayRuntimeSceneId(uint32_t sourceSceneId) const;
        // The scene a Play copy id is a copy of; any other id is returned as is
        uint32_t getPlaySourceSceneId(uint32_t sceneId) const;
        bool isPlayRuntimeScene(uint32_t sceneId) const;
        // The Play copies of sceneId's running game layers, as the Scene view draws them
        std::vector<SceneProject*> getRunningRuntimeLayerScenes(uint32_t sceneId);
        // Once a frame: a Play copy no longer running, or its GameObjects destroyed, are
        // no longer selected
        void syncPlayRuntimeSelections();

        std::vector<SceneProject>& getScenes();
        const std::vector<SceneProject>& getScenes() const;
        // The scenes, then the prefab stages' scenes (Prefab Mode): for what applies to every
        // GameObject being edited (script refreshes, asset path changes, tools)
        std::vector<SceneProject*> getScenesAndPrefabStages();

        // Flushes compiled forked-shader caches and flags meshes that use a custom
        // shader for reload, so editing a shader source file updates the viewport.
        void invalidateCustomShaders();

        SceneProject* getScene(uint32_t sceneId);
        const SceneProject* getScene(uint32_t sceneId) const;
        SceneProject* getSelectedScene();
        const SceneProject* getSelectedScene() const;

        std::vector<TabEntry>& getTabs();
        const std::vector<TabEntry>& getTabs() const;
        void addTab(TabType type, const std::string& filepath);
        void removeTab(TabType type, const std::string& filepath);
        bool hasTab(TabType type, const std::string& filepath) const;

        // Ids reach other developers through .scene files, startSceneId and
        // childScenes, so they are drawn at random rather than from a counter two
        // branches would advance to the same value
        uint32_t allocateSceneId() const;

        void setSelectedSceneId(uint32_t selectedScene);
        uint32_t getSelectedSceneId() const;

        void setSelectedSceneForProperties(uint32_t selectedScene);
        uint32_t getSelectedSceneForProperties() const;

        bool isTempProject() const;
        bool isTempUnsavedProject() const;
        std::filesystem::path getProjectPath() const;
        std::filesystem::path getProjectInternalPath() const;

        // Where Play mode sends the engine's data:// writes, created on demand, so a
        // playtest does not dirty the working tree
        std::filesystem::path getUserDataPath() const;

        // A scene listed in project.yaml that this checkout could not load. Kept so
        // saving does not remove a scene just because its file is absent here.
        void addUnresolvedScene(size_t listPosition, const std::filesystem::path& filepath);
        bool clearUnresolvedScene(const std::filesystem::path& filepath);
        const std::vector<std::pair<size_t, std::filesystem::path>>& getUnresolvedScenes() const;

        void setUnresolvedSceneStates(std::map<std::string, YAML::Node> states);
        const std::map<std::string, YAML::Node>& getUnresolvedSceneStates() const;

        fs::path getTerrainMapsDir() const;
        fs::path getThumbsDir() const;
        fs::path getThumbnailPath(const fs::path& originalPath) const;

        std::vector<Entity> getEntities(uint32_t sceneId) const;
        // What the running scene made for itself: listed while playing, destroyed by Stop,
        // and never part of the authored entities a file keeps
        std::vector<Entity> getPlayCreatedEntities(const SceneProject* sceneProject) const;

        void replaceSelectedEntities(uint32_t sceneId, std::vector<Entity> selectedEntities);
        // A running scene and its Stop destroy entities the Hierarchy lets the user select
        void deselectDestroyedEntities(SceneProject* sceneProject);
        void setSelectedEntity(uint32_t sceneId, Entity selectedEntity);
        void addSelectedEntity(uint32_t sceneId, Entity selectedEntity);
        bool isSelectedEntity(uint32_t sceneId, Entity selectedEntity);
        void clearSelectedEntities(uint32_t sceneId);
        void clearAllSelections(uint32_t sceneId);
        std::vector<Entity> getSelectedEntities(uint32_t sceneId) const;
        bool hasSelectedEntities(uint32_t sceneId) const;

        bool hasSelectedSceneUnsavedChanges() const;
        bool hasSelectedSceneUnsavedPrefabs() const;
        bool hasLocalUnsavedChanges(uint32_t sceneId) const;
        bool hasSceneUnsavedChanges(uint32_t sceneId) const;
        bool hasUnsavedPrefabs(uint32_t sceneId) const;
        bool hasScenesUnsavedChanges() const;
        bool hasUnsavedPrefabs() const;

        void updateAllScriptsProperties(uint32_t sceneId);
        // Returns true when a script property definition changed.
        bool updateScriptProperties(SceneProject* sceneProject, Entity entity, std::vector<ScriptEntry>& scripts, const std::string& inMemoryContent = "", const std::string& inMemoryPath = "");

        static std::vector<Entity> getTopLevelEntities(const EntityRegistry* registry, const std::vector<Entity>& orderedEntities);
        // Remaps every entity reference across a set of entities. References to a mapped
        // member are translated. By default references outside the map are cleared so raw
        // IDs cannot leak across a prefab boundary; recovery paths pass clearUnmapped=false
        // to preserve external references while repairing replacement IDs.
        static void remapEntityProperties(EntityRegistry* registry, const std::vector<Entity>& entities, const std::unordered_map<Entity, Entity>& entityMap, bool clearUnmapped = true);
        // Same rule as remapEntityProperties, scoped to one component. When `properties`
        // is non-empty only those are remapped; a request for an aggregate property (e.g.
        // "scripts") also covers the indexed references expanded from it.
        static void remapEntityPropertiesInComponent(EntityRegistry* registry, Entity entity, ComponentType componentType, const std::vector<std::string>& properties, const std::unordered_map<Entity, Entity>& entityMap);
        // Promotes to a per-instance override every member component holding an entity
        // reference that points outside the prefab (cross-scene, or a non-member local
        // entity). Such references cannot be shared, so overriding keeps and persists them.
        static void overrideExternalRefComponents(EntityRegistry* scene, Prefab::Instance& instance);

        //=== Prefab part ===

        // Asset GUIDs (editor/project/PREFABS.md)
        AssetDatabase& getAssetDatabase() { return assetDatabase; }
        const AssetDatabase& getAssetDatabase() const { return assetDatabase; }
        // The GUID of a prefab file: the database's, else the one an upgrade would give it
        std::string getPrefabGuid(const std::filesystem::path& filepath) const;
        // Where a prefab reference points now: by GUID (a GUID no file has is missing, even
        // when a file sits at the path), else, for a reference written before GUIDs, by the
        // GUID its path was upgraded to, else by the path. Empty when missing.
        // A file not in the database yet (made outside the editor a moment ago) is added.
        std::filesystem::path resolvePrefabPath(const std::string& guid, const std::filesystem::path& pathHint);
        // Resolves a PrefabComponent in place (path updated, a missing guid filled in);
        // false when missing. pathChanged reports a path the GUID moved.
        bool resolvePrefabReference(PrefabComponent& component, bool* pathChanged = nullptr);
        // External changes to prefab files (moved, copied, deleted or edited outside the
        // editor). Called every frame, looks at most every two seconds unless forced; does
        // nothing while a scene plays.
        void refreshAssetDatabase(bool force = false);
        // A prefab file or a directory holding some appeared (a copy, a deleted file put back
        // by an undo): they are registered, and Missing Prefab instances they resolve relinked
        void importPrefabFiles(const std::filesystem::path& path, bool copy = false, std::map<std::string, std::string>* copyGuids = nullptr);

        // Missing Prefab instances (SceneProject::missingPrefabs)
        const MissingPrefabInstance* getMissingPrefab(uint32_t sceneId, Entity entity) const;
        bool isMissingPrefab(uint32_t sceneId, Entity entity) const;
        void setMissingPrefab(SceneProject* sceneProject, Entity root, const YAML::Node& data, const PrefabMemberIds* memberIds = nullptr);
        void clearMissingPrefab(uint32_t sceneId, Entity root);
        // Rebuilds the Missing Prefab instances whose asset resolves again; returns how many
        size_t relinkMissingPrefabs();

        // guid: the asset GUID to give the new file (a redo), empty for a new one
        bool createPrefab(uint32_t sceneId, fs::path filepath, YAML::Node entityNode, const std::string& guid = std::string());
        bool removePrefab(const std::filesystem::path& filepath);

        const std::vector<std::filesystem::path>& getStandalonePrefabs() const;
        void setStandalonePrefabs(std::vector<std::filesystem::path> prefabPaths);
        bool isStandalonePrefab(const std::filesystem::path& filepath) const;
        std::vector<std::filesystem::path> findPrefabFiles() const;

        void savePrefabToDisk(const std::filesystem::path& filepath);

        Prefab* getPrefab(const std::filesystem::path& filepath);
        const Prefab* getPrefab(const std::filesystem::path& filepath) const;
        std::map<std::filesystem::path, const Prefab*> getPrefabs(uint32_t sceneId) const;
        std::filesystem::path findPrefabPathFor(uint32_t sceneId, Entity entity) const;

        YAML::Node encodePrefabNode(const std::filesystem::path& filepath) const;

        // Makes rootEntity an instance of the prefab: the prefab's root GameObject applied to
        // it, its other GameObjects created below it. Values come in layers: the prefab, then
        // assetLayers (what the prefabs holding this instance as a nested one override of it,
        // innermost first), then instanceData (the scene's own overrides, the only ones the
        // instance records; also its removed and added GameObjects and member ids). The
        // root's PrefabComponent, when it has one, says which prefab (by GUID) over filepath;
        // a prefab that cannot be found or read makes the root a Missing Prefab instance
        // holding the given instance data, and nothing is imported. preferredIds: local ids
        // for the members (of this instance and the nested ones), taken where free, over the
        // data's own. See editor/project/PREFABS.md.
        std::vector<Entity> importPrefab(SceneProject* sceneProject, std::vector<Entity>* entities, const std::filesystem::path& filepath, Entity rootEntity, bool needSaveScene = true, const PrefabInstanceData* instanceData = nullptr, std::unordered_map<Entity, Entity>* entityRemap = nullptr, const PrefabMemberIds* preferredIds = nullptr, const std::vector<PrefabInstanceData*>& assetLayers = {});
        // The prefab asset, read from its file when not loaded yet (nullptr when it cannot be)
        Prefab* loadPrefabAsset(const std::filesystem::path& filepath);
        // reconcileTracked false leaves dropping the destroyed entities from the scene's
        // tracked list to the caller (one pass after unimporting many instances); keepRoot
        // leaves the root entity in the scene (a Missing Prefab conversion)
        bool unimportPrefab(uint32_t sceneId, const std::filesystem::path& filepath, Entity rootEntity, const std::vector<Entity>& memberEntities,
                            bool reconcileTracked = true, bool keepRoot = false);

        bool addEntityToPrefab(uint32_t sceneId, Entity entity, Entity parent, bool createItself = true);
        bool addEntityToPrefab(uint32_t sceneId, const NodeRecovery& recoveryData, Entity parent,
            bool createItself = true, std::unordered_map<Entity, Entity>* entityRemap = nullptr,
            std::vector<Entity>* restoredEntities = nullptr);
        NodeRecovery removeEntityFromPrefab(uint32_t sceneId, Entity entity, bool destroyItself = true);

        bool prefabPropertyChanged(uint32_t sceneId, Entity entity, ComponentType componentType, std::vector<std::string> properties, bool changeItself = false);
        bool prefabNameChanged(uint32_t sceneId, Entity entity, std::string name, bool changeItself = false);
        // Active, tag or layer (PrefabObjectOverrides::State bits) of a prefab GameObject
        // changed: shared with the prefab like a name, an override on an instance root
        bool prefabStateChanged(uint32_t sceneId, Entity entity, uint8_t stateBits, bool changeItself = false);
        bool isEntityInPrefab(uint32_t sceneId, Entity entity) const;

        bool addComponentToPrefab(uint32_t sceneId, Entity entity, ComponentType componentType, bool addToItself = true);
        bool addComponentToPrefab(uint32_t sceneId, Entity entity, ComponentType componentType, const ComponentRecovery& recovery, bool addToItself = true);
        ComponentRecovery removeComponentFromPrefab(uint32_t sceneId, Entity entity, ComponentType componentType, bool encodeComponent = true, bool removeToItself = true);

        SharedMoveRecovery moveEntityFromPrefab(uint32_t sceneId, Entity entity, Entity target, InsertionType type, bool moveItself = true);
        bool undoMoveEntityInPrefab(uint32_t sceneId, Entity entity, Entity target, const SharedMoveRecovery& recovery, bool moveItself = true);

        void cleanupPrefabsForScene(uint32_t sceneId);

        // Root of the outermost prefab instance holding each entity (a nested instance root
        // resolves to the instance above it), NULL_ENTITY outside any instance. The result
        // is aligned with `entities`; the scene's instances are indexed once for all of them.
        std::vector<Entity> getOutermostPrefabRoots(uint32_t sceneId, const std::vector<Entity>& entities) const;
        // Unity's Unpack Prefab: the instances rooted at `roots` become plain GameObjects that
        // keep their current values, overrides included, and their entity IDs. Nested prefab
        // instances stay instances unless `completely`, which unpacks every instance below too.
        // Returns what repackPrefabInstances needs to undo it, in unpack order.
        std::vector<PrefabUnpackRecord> unpackPrefabInstances(uint32_t sceneId, const std::vector<Entity>& roots, bool completely);
        void repackPrefabInstances(uint32_t sceneId, std::vector<PrefabUnpackRecord> records);

        // Unity's removed GameObject overrides. removePrefabBranch takes a Prefab GameObject
        // (a member, a nested instance root included) and everything below it out of its
        // instance only, recording the removal when asked; restorePrefabBranch undoes it with
        // the same ids where they are free (the replacements go to entityRemap; without one,
        // the scene's references are repaired here).
        PrefabBranchRecovery removePrefabBranch(uint32_t sceneId, Entity entity, bool recordRemoval);
        bool restorePrefabBranch(uint32_t sceneId, const PrefabBranchRecovery& recovery,
            std::unordered_map<Entity, Entity>* entityRemap = nullptr, std::vector<Entity>* restoredEntities = nullptr);
        // The removed GameObjects of the instance rooted at instanceRoot and of the instances
        // nested in it, in prefab order
        std::vector<PrefabRemovalInfo> getPrefabRemovals(uint32_t sceneId, Entity instanceRoot) const;
        // The other overrides of that instance and the ones nested in it: overridden
        // components (GameObject, component), and the GameObjects added below its Prefab
        // GameObjects (top ones only, with their parent), in hierarchy order
        std::vector<std::pair<Entity, ComponentType>> getPrefabComponentOverrides(uint32_t sceneId, Entity instanceRoot) const;
        std::vector<std::pair<Entity, Entity>> getPrefabAddedGameObjects(uint32_t sceneId, Entity instanceRoot) const;
        // Revert: the removed GameObject comes back with the Prefab's current values, in its
        // Prefab place, taking back the ids it had where they are free
        bool revertPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity, PrefabRemovalRevert& revert);
        void undoRevertPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity, const PrefabRemovalRevert& revert);
        // Apply: the GameObject is deleted from the Prefab asset, so from all its instances
        bool applyPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity, PrefabRemovalApply& apply);
        void undoApplyPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity, PrefabRemovalApply& apply);
        // Apply of a component override: the instance's values become the Prefab's (and every
        // instance that follows it); false when it holds a reference outside the instance
        bool canApplyComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType) const;
        bool applyComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType, ComponentRecovery& recovery);
        void undoApplyComponentOverride(uint32_t sceneId, Entity entity, ComponentType componentType, const ComponentRecovery& recovery);

        // ---- Override model v2 (editor/project/PREFABS.md, "Overrides" and "APIs") ----

        // The level owning a GameObject's overrides (see PrefabObjectRef); false outside
        // any instance
        bool findPrefabObject(uint32_t sceneId, Entity entity, PrefabObjectRef& ref);
        bool findPrefabObject(uint32_t sceneId, Entity entity, PrefabObjectRef& ref) const;
        // Unity's default overrides of an instance root: its name, Transform position and
        // rotation, and RectTransform placement. Per instance always, never listed, never
        // applied, and what an instance root of an older file keeps of its own.
        static bool isPrefabRootDefaultOverride(ComponentType type, const std::string& property);
        // An outermost or nested instance root
        bool isPrefabInstanceRoot(uint32_t sceneId, Entity entity) const;

        // Queries (the owning level's overrides)
        bool isPropertyOverridden(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property) const;
        bool isComponentOverridden(uint32_t sceneId, Entity entity, ComponentType type) const;
        // The overridden property names of a component ("*" for the whole of it)
        std::set<std::string> getOverriddenProperties(uint32_t sceneId, Entity entity, ComponentType type) const;
        Prefab::ObjectOverrides getPrefabObjectOverrides(uint32_t sceneId, Entity entity) const;
        // The value the GameObject's property (or whole component) would have without its
        // owning instance's own override: the prefab's, with what the prefabs holding the
        // instance override of it. False when the prefab GameObject has no such component.
        bool getPrefabSourceValue(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property, PrefabSourceValue& value);
        YAML::Node getPrefabSourceComponent(uint32_t sceneId, Entity entity, ComponentType type);
        // The prefabs an override of the GameObject can be applied to: the one owning it
        // first, then each prefab holding that instance as a nested one, outwards
        std::vector<std::filesystem::path> getPrefabApplyTargets(uint32_t sceneId, Entity entity) const;

        // Primitives on the owning level, without undo (PrefabOverrideCmd wraps them).
        // record*: the bookkeeping only (the value is the GameObject's already);
        // revert*: the GameObject takes the source value back and the override goes;
        // apply*: the value goes into a prefab (target: one of getPrefabApplyTargets, empty
        // for the owning one) and the override goes; every instance follows.
        bool recordPropertyOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties);
        bool clearPropertyOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties);
        bool revertPropertyOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties);
        bool applyPropertyOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties, const std::filesystem::path& target = {});
        bool recordStateOverride(uint32_t sceneId, Entity entity, uint8_t stateBits);
        bool revertStateOverride(uint32_t sceneId, Entity entity, uint8_t stateBits);
        bool applyStateOverride(uint32_t sceneId, Entity entity, uint8_t stateBits, const std::filesystem::path& target = {});
        // An added component: recorded (the GameObject has it), reverted (removed), applied
        // (added to the prefab GameObject)
        bool recordAddedComponent(uint32_t sceneId, Entity entity, ComponentType type);
        bool revertAddedComponent(uint32_t sceneId, Entity entity, ComponentType type);
        bool applyAddedComponent(uint32_t sceneId, Entity entity, ComponentType type, const std::filesystem::path& target = {});
        // A removed component: recorded (the GameObject lost it), reverted (it comes back
        // with the source values), applied (removed from the prefab GameObject)
        bool recordRemovedComponent(uint32_t sceneId, Entity entity, ComponentType type);
        bool revertRemovedComponent(uint32_t sceneId, Entity entity, ComponentType type);
        bool applyRemovedComponent(uint32_t sceneId, Entity entity, ComponentType type, const std::filesystem::path& target = {});

        // Re-imports every open instance of a prefab (top-level and nested, in every loaded
        // scene) from its current registry, or from its file first (reloadFromDisk): each
        // keeps its overrides, member ids, added and removed GameObjects; the selection and
        // the undo history stay valid. For Prefab Mode saves, variants and external edits.
        void refreshPrefabInstances(const std::filesystem::path& filepath, bool reloadFromDisk = false);
        // Rebuilds an outermost instance from a root node (Stream's encoding of it, the
        // current one when null): the members, nested instances and added GameObjects take
        // their ids back
        bool reimportPrefabInstance(SceneProject* sceneProject, Entity root, const YAML::Node& rootNode);
        // The outermost instance roots, per loaded scene, holding an instance of `filepath`
        // at any depth (itself included)
        std::vector<std::pair<uint32_t, Entity>> findOutermostInstancesOf(const std::filesystem::path& filepath) const;
        // Undo support: what an override operation on `entity` (and, for an apply, on the
        // prefab `target`) changes, captured before it and put back by restore
        PrefabUndoState capturePrefabUndoState(uint32_t sceneId, Entity entity, const std::vector<std::filesystem::path>& targets = {});
        void restorePrefabUndoState(const PrefabUndoState& state);

        // Internals of the override model shared by the prefab translation units
        // The asset-level data of the instance nested at registryRoot of `prefab` (made, and
        // converted when read from an older file)
        PrefabInstanceData* nestedAssetLayerFor(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryRoot);
        // The asset layers (importPrefab's assetLayers) of the instance rooted at
        // instanceRoot: what the prefabs holding it as a nested one override of it, innermost
        // first; empty for a top-level instance
        std::vector<PrefabInstanceData*> getPrefabAssetLayers(uint32_t sceneId, Entity instanceRoot);
        // The prefab's value of a component of the GameObject (with the layers of the
        // prefabs holding its instance), references in the instance's local ids
        bool buildPrefabSource(uint32_t sceneId, Entity entity, ComponentType type, prefabinternal::ScratchEntity& scratch, PrefabObjectRef* ref = nullptr);
        // The data `target` (a prefab holding ref's instance as a nested one) keeps for that
        // instance, made where missing
        PrefabInstanceData* applyTargetLayer(uint32_t sceneId, const PrefabObjectRef& ref, const std::filesystem::path& target, Prefab** targetPrefab);
        // A change to a nested root of `prefab` is this prefab's override of the nested
        // prefab's root: recorded in its asset-level data, or it is lost on the next import
        void recordNestedRootAssetOverride(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryEntity, ComponentType type,
            const std::vector<std::string>& properties);
        // An edit-shared change on an instance root: its properties that differ from the
        // prefab root's are overrides, the ones equal are not
        void syncRootPropertyOverrides(uint32_t sceneId, Entity root, ComponentType type, const std::vector<std::string>& properties);
        // A prefab instance (rooted at localRoot) that became a nested one of `prefab` at
        // registryRoot: what its scene data overrides goes into the prefab's asset-level
        // data, out of the scene's (its added GameObjects stay the scene's)
        void moveNestedInstanceDataToAsset(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryRoot,
            SceneProject* sceneProject, Entity localRoot);

        //=== Prefab Mode (editor/PrefabStage.cpp, PREFABS.md section 12) ===

        // Opens the prefab asset in a prefab stage. contextSceneId/contextRoot: the instance
        // it is opened from (none: from the Project window); inContext draws that scene behind
        // the stage (Unity's In Context), else the prefab is alone (In Isolation). Opened from
        // an instance inside a stage, the new stage goes on top of that one (a breadcrumb);
        // otherwise the open stages are closed first, each modified one asking to be saved.
        // False when the asset cannot be opened (or a prompt was cancelled).
        bool openPrefabStage(const std::filesystem::path& path, uint32_t contextSceneId = NULL_PROJECT_SCENE,
            Entity contextRoot = NULL_ENTITY, bool inContext = true);
        // Closes the stages above `depth` (0: leaves Prefab Mode), top first; one with
        // unsaved changes asks Save / Discard / Cancel (choice: 0 ask, 1 save, 2 discard, 3
        // cancel, for scripted callers). done(false) when cancelled or a save failed.
        void closePrefabStages(size_t depth, std::function<void(bool)> done = nullptr, int choice = 0);
        // Writes the stage into its asset (version 3) and refreshes the asset's instances in
        // every loaded scene; scenes not loaded read the file when they load
        bool savePrefabStage(uint32_t sceneId);
        bool isPrefabStage(uint32_t sceneId) const;
        // Inside Prefab Mode every edit is an edit of the asset, not an instance override
        bool isEditingPrefabAsset(const SceneProject* sceneProject) const;
        PrefabStage* getPrefabStage(uint32_t sceneId);
        const PrefabStage* getPrefabStage(uint32_t sceneId) const;
        // The shown stage (the top one), nullptr outside Prefab Mode
        PrefabStage* getActivePrefabStage();
        size_t getPrefabStageCount() const { return prefabStages.size(); }
        PrefabStage* getPrefabStageAt(size_t index) { return index < prefabStages.size() ? prefabStages[index].get() : nullptr; }
        size_t getPrefabStageDepth(uint32_t sceneId) const;  // 1-based, 0 for a scene
        bool hasUnsavedPrefabStage(uint32_t sceneId) const;
        bool hasUnsavedPrefabStages() const;
        // A regular scene for what needs one (Play, the build's start scene, the workspace):
        // the scene Prefab Mode was entered from for a stage, sceneId itself otherwise
        uint32_t getSceneBehindPrefabStage(uint32_t sceneId) const;
        bool isPrefabStageRoot(uint32_t sceneId, Entity entity) const;
        // Whether an instance of prefabPath may go into the scene: never into the stage of a
        // prefab it holds (a prefab cannot contain itself)
        bool canInstantiatePrefabIn(uint32_t sceneId, const std::filesystem::path& prefabPath);
        // Once a frame: Auto Save (when the user is not in the middle of an edit), changes the
        // asset got from outside the stage, the stage root kept single, lost contexts
        void updatePrefabStages(bool userEditing);
        // A command changed a scene (CommandHistory::onSceneModified)
        void notePrefabStageChange(uint32_t sceneId);
        bool isPrefabAutoSave() const { return prefabAutoSave; }
        void setPrefabAutoSave(bool autoSave) { prefabAutoSave = autoSave; }
        PrefabContextVisibility getPrefabContextVisibility() const { return prefabContextVisibility; }
        void setPrefabContextVisibility(PrefabContextVisibility visibility);
        // The Scene view of a stage In Context: before its activation (the context scene under
        // it, its camera following the stage's), and around its draw (the instance edited hidden)
        void preparePrefabStageView(uint32_t sceneId, int width, int height);
        void beginPrefabStageDraw(uint32_t sceneId);
        void endPrefabStageDraw(uint32_t sceneId);

        //=== end Prefab part ===

        YAML::Node clearEntitiesNode(YAML::Node node);
        YAML::Node changeEntitiesNode(Entity& firstEntity, YAML::Node node);

        bool isAnyScenePlaying() const;
        bool isPlaySessionActive() const;
        // True only while the active play session's main scene is actually running
        // (PLAYING) — false when paused, loading, or no session is active.
        bool isMainScenePlaying() const;
        bool isAnySceneSaving() const;

        void start(uint32_t sceneId);
        void pause(uint32_t sceneId);
        void resume(uint32_t sceneId);
        void stop(uint32_t sceneId);
        void stopActivePlay();

        // Advances a paused game by exactly one frame (pausing it first if it plays).
        // The frame runs in the next draw of the scene, bracketed by the two calls below.
        void step(uint32_t sceneId);
        bool isStepPending(uint32_t sceneId) const;
        // True when a step was pending and the scene now runs for one frame
        bool beginStepFrame(uint32_t sceneId);
        void endStepFrame(uint32_t sceneId);

        // Main scene of the active play session, or NULL_PROJECT_SCENE
        uint32_t getPlaySessionSceneId() const;
        // Camera the game renders through: the main camera, else the scene default
        Entity getGameCamera(uint32_t sceneId) const;
        void waitForPlaySessionToFinish();

        std::vector<Scene*> getRunningRuntimeLayers(uint32_t sceneId);

        void debugSceneHierarchy();
    };

}
