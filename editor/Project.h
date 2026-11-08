// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "window/GameResolutions.h"
#include "Scene.h"
#include "render/SceneDisplaySettings.h"
#include "Catalog.h"
#include "sceneview/SceneViewState.h"
#include "command/CommandHistory.h"
#include "Conector.h"
#include "Generator.h"
#include "Configs.h"
#include "util/Prefab.h"
#include "LoadReport.h"
#include "StreamMigrations.h"
#include "SceneBackup.h"
#include "io/ScopedFileLock.h"
#include <optional>
#include "io/watch/FileWatcher.h"
#include "util/AssetDatabase.h"
#include "asset/AssetIndex.h"
#include "asset/AssetMap.h"
#include "asset/AssetRefSchema.h"
#include "asset/MissingAssetTracker.h"
#include "organisation/AssetLabels.h"
#include "organisation/Organisation.h"
#include "search/AssetSearchIndex.h"
#include "util/ScriptParser.h"
#include "util/ScopedDefaultEntityPool.h"
#include "util/CxxStandards.h"
#include "util/Util.h"
#include "physics/PlacementSettings.h"
#include "navigation/NavigationProjectSettings.h"
#include "render/shadow/ShadowTiers.h"
#include "render/LightingTiers.h"
#include "device/DeviceSettings.h"
#include "texture/TextureImportSettings.h"
#include "pixelart/PixelArtSettings.h"
#include "model/ModelImportSettings.h"
#include "vcs/VcsTypes.h"
#include "util/PlatformRules.h"
#include "util/ApplePrivacy.h"
#include "window/SpriteNormalsWindowSettings.h"
#include "input/InputActions.h"
#include "shader/ShaderGlobals.h"
#include "shader/ShaderListCache.h"

#include "yaml-cpp/yaml.h"

#include <filesystem>
#include <optional>
#include <chrono>
#include <future>
#include "jobs/JobSystem.h"
#include <map>
#include <cstdint>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <tuple>
#include <atomic>
#include <memory>
#include <future>
#include <mutex>
#include "render/RenderUtil.h"
#include "manager/SceneManager.h"

namespace jasperengine{
    struct AnimationPlayerClip;
    struct ScriptEntry;
}

namespace jasperengine::editor{

    class SceneRender;
    namespace prefabinternal { struct ScratchEntity; }
    class ScriptPropertyCache;  // editor/util/ScriptPropertyCache.h
    class ScriptAttach;         // editor/util/ScriptAttach.h
    struct BakedPrefab;
    namespace vcs {
        class VcsService;
        class SceneDiffTracker;
        struct VcsSettings;
    }

    // project.yaml versionControl (docs/specs/S29-version-control.md R7, R21, D2); MCP
    // editor.vcs.provider, editor.vcs.lfs, editor.vcs.lock_scenes
    struct VcsProjectSettings {
        vcs::ProviderChoice provider = vcs::ProviderChoice::Auto;
        // the generated .gitattributes stores the binaries in Git LFS, lockable
        bool lfs = false;
        // ... and scenes and prefabs lockable too (a lock on a file that is not an LFS object
        // depends on the server)
        bool lockScenes = false;

        bool operator==(const VcsProjectSettings& other) const {
            return provider == other.provider && lfs == other.lfs && lockScenes == other.lockScenes;
        }
        bool operator!=(const VcsProjectSettings& other) const { return !(*this == other); }
    };

    namespace cook {
        class SceneCooker;
    }

    struct LocalBuildSettings;

    enum class SceneType{
        SCENE_3D,
        SCENE_2D,
        SCENE_UI
    };

    // What closing a scene with unsaved changes does, the Unsaved Changes dialog's answer
    // given up front (Project's non-interactive APIs): Cancel fails and closes nothing
    enum class UnsavedChoice {
        Cancel,
        Save,
        Discard
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

    // Where a selection change comes from (Project::setSelection): the user (a standalone
    // change is an undo step), an undo/redo putting a stored selection back, or forced
    // (GameObjects destroyed, a scene reloaded). Only User changes are recorded.
    enum class SelectionCause{
        User,
        Restore,
        Forced
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

    // the 3D world capacity (project.yaml physics3DWorld; physics3D is the 3D physics switch); defaults = PhysicsSystem's
    struct Physics3DWorldSettings {
        unsigned int maxBodies = 8192;
        unsigned int maxBodyPairs = 8192;
        unsigned int maxContactConstraints = 8192;

        bool operator==(const Physics3DWorldSettings& other) const {
            return maxBodies == other.maxBodies && maxBodyPairs == other.maxBodyPairs && maxContactConstraints == other.maxContactConstraints;
        }
        bool operator!=(const Physics3DWorldSettings& other) const { return !(*this == other); }
    };

    // project.yaml held an older editor's Project Settings > Input (Unity's Input Manager as a flat
    // list, `input: {version: 1, actions: [...]}`): its actions as the Input Manager map of an
    // .inputactions asset (InputManagerFormat), written into the assets directory when the
    // project loads; project.yaml points to the asset once it is saved
    struct InputActionsMigration {
        bool pending = false;               // the asset is still to be written
        std::filesystem::path asset;        // project-relative
        std::string text;                   // the asset's text
        size_t actions = 0;                 // how many actions it converted
        bool reused = false;                // an earlier load wrote it already (kept as it is now)
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

    // What a scene's prefab instances are (Project::capturePlayPrefabState): Play mode puts
    // it back at Stop with the scene's snapshot
    struct PlayPrefabState {
        bool valid = false;
        std::map<std::filesystem::path, std::vector<Prefab::Instance>> instances;
        std::map<Entity, MissingPrefabInstance> missingPrefabs;
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
        // when the editor last moved its clock while stopped (Animate water, updateSceneRender)
        std::chrono::steady_clock::time_point editClockLast;
        bool editClockValid = false;
        // Extra frames to keep drawing after a change so late async uploads still show.
        int renderSettleFrames = 0;
        // Bumped on structural changes so the Hierarchy window can cache its tree.
        uint64_t structureVersion = 0;
        bool isModified = false;
        // CommandHistory::stampSave of its last write (Project::markSceneSaved), 0 = none since it
        // loaded: a capture that restores isModified compares it with the one it saw
        uint64_t savedAt = 0;
        // True while the id has never reached disk, so a scene replacing this one
        // can take it over instead of drawing a new one
        bool transientId = false;
        bool isVisible = false;
        bool opened = true;
        bool expandedInline = false;
        // a Play copy a game load put in place of the played scene: the editor camera's mirror in
        // it, which the Scene view draws it through (render/ViewCameraMirror.h)
        Entity sceneViewMirror = NULL_ENTITY;
        SceneDisplaySettings displaySettings;
        ScenePlayState playState = ScenePlayState::STOPPED;
        YAML::Node playStateSnapshot;
        std::set<Entity> playStateEntities;
        // isModified when the snapshot was taken: Stop puts both back
        bool playStateModified = false;
        // The prefab instances' bookkeeping when the snapshot was taken (capturePlayPrefabState)
        PlayPrefabState playPrefabState;
        // the maxValues block of a scene saved by an editor that sized fixed arrays (capacities) and
        // collider caps: nothing reads it, written back as it was so re-saving changes no line; a
        // scene saved since has none (null)
        YAML::Node legacyMaxValues;
        std::set<ShaderKey> shaderKeys;
        std::vector<ChildSceneRef> childScenes;
        std::vector<SceneScriptSource> cppScripts;
        // While the scene is closed: the write time and size of the file cppScripts were read
        // from (Project::collectClosedSceneCppScripts), so an unchanged file is not read again.
        // Forgotten when the scene closes: what it had in memory is not its file's.
        bool cppScriptsRead = false;
        fs::file_time_type cppScriptsReadStamp{};
        std::uintmax_t cppScriptsReadSize = 0;
        std::vector<PrefabSceneInfo> prefabs;
        // FBX model path -> the import its saved model parts were built from
        std::map<std::string, std::string> imports;
        YAML::Node editorCameraState;
        // this user's Scene view of it: camera settings, layout, bookmarks, measures, filters
        // (S26, workspace.yaml)
        SceneViewState viewState;
        // Missing Prefab instances by root entity (Project::getMissingPrefab validates them)
        std::map<Entity, MissingPrefabInstance> missingPrefabs;
        // Set while decoding when a prefab reference resolved to another path by its GUID:
        // the loaded scene is then marked modified
        bool prefabRefsChanged = false;
        // While the scene file is decoded: every entity id it holds (GameObjects, prefab
        // member ids, added GameObjects). A prefab member without a saved id never takes one
        // of these, which a GameObject further down the file needs.
        std::unordered_set<Entity> reservedEntityIds;
        // A scene of Project::instantiatePrefabPreview (a thumbnail): its Missing Prefabs are
        // not reported
        bool prefabPreview = false;
        // What its file's load could not read (docs/data-safety.md): with dropped issues
        // outstanding, saves to its own file are gated and autosave is off
        LoadReport loadReport;
        // SHA-1 of the file's bytes as last loaded or saved by this session (a crash backup
        // applies to exactly these bytes)
        std::string diskSha1;
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
    // editor/PrefabStage.cpp and editor/project/PREFABS.md, section 16.
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

    // An instance nested in an unpacked one as it was: unpacking makes what the prefab overrides
    // of it its own overrides (Project::promoteNestedAssetData)
    struct PrefabNestedState {
        Entity root = NULL_ENTITY;
        std::map<Entity, Prefab::ObjectOverrides> overrides;
        std::unordered_map<Entity, std::vector<Prefab::EntityMember>> removed;
        std::filesystem::path path;   // the level at that root (a variant's base level)
    };

    struct PrefabUnpackRecord;   // project/PrefabUnpackRecord.h

    // ---- Prefab Variants (editor/project/ProjectPrefabVariant.cpp, PREFABS.md 19) ----

    // One prefab level of the instances rooted at one GameObject. A Prefab Variant's root is
    // an instance of its base, so the root of an instance of a variant roots the instance of
    // that base too (and of the base's base, down the chain): one level each, the outermost
    // (the prefab the GameObject's PrefabComponent names) first.
    struct PrefabInstanceLevel {
        std::filesystem::path path;
        Prefab* prefab = nullptr;
        Prefab::Instance* instance = nullptr;
    };

    // An instance nested in another one, as the holding instance sees it: a member that is a
    // nested root, or, for an instance of a variant, the instance of its base at the same root
    struct NestedInstanceLevel {
        Entity registryEntity = NULL_ENTITY;      // in the holder's prefab: a nested root, or its root (variant)
        Entity localRoot = NULL_ENTITY;
        std::filesystem::path path;               // the nested instance's prefab
        Prefab::Instance* instance = nullptr;     // nullptr: a Missing Prefab
        bool variantBase = false;                 // the base level of a variant instance
    };

    // The instance holding another one (the next level outwards): the instance it is a member
    // of, or the instance of the variant whose base it is (same root)
    struct PrefabHolderLink {
        std::filesystem::path path;
        Prefab* prefab = nullptr;
        Prefab::Instance* instance = nullptr;
        Entity registryEntity = NULL_ENTITY;      // the held instance's root in `prefab`
        bool variant = false;
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

    // One override of a prefab instance (or of one nested in it), for the Overrides dropdown
    struct PrefabOverrideEntry {
        enum class Kind { Property, State, AddedComponent, RemovedComponent, AddedGameObject, RemovedGameObject };
        Kind kind = Kind::Property;
        Entity entity = NULL_ENTITY;        // the GameObject (an added one itself; a removed one: NULL)
        Entity parent = NULL_ENTITY;        // AddedGameObject: the prefab GameObject it hangs from
        ComponentType type = ComponentType::Transform;
        std::set<std::string> properties;   // Property: the overridden names ("*": the whole component)
        uint8_t state = 0;                  // State: PrefabObjectOverrides::State bits
        PrefabRemovalInfo removal;          // RemovedGameObject
    };

    // What putting prefab assets and the instances using them back takes: the undo of an
    // override operation (Project::capturePrefabUndoState)
    struct PrefabUndoState {
        struct Asset {
            std::filesystem::path path;
            YAML::Node node;          // Project::encodePrefabNode
            bool isModified = false;
            bool needsUpgrade = false;
            uint64_t savedAt = 0;        // Prefab::savedAt: a write since makes the restore modified
        };
        struct InstanceState {
            uint32_t sceneId = NULL_PROJECT_SCENE;
            Entity root = NULL_ENTITY;   // an outermost instance root
            YAML::Node rootNode;         // the root as the scene file writes it
            bool sceneModified = false;
            uint64_t sceneSavedAt = 0;   // SceneProject::savedAt, as the asset's
        };
        std::vector<Asset> assets;
        std::vector<InstanceState> instances;
    };

    // ---- Replace Prefab, previews, usage (editor/project/ProjectPrefabReplace.cpp) ----

    // Unity's PrefabReplacingSettings: how Replace Prefab carries an instance's overrides over
    // to the new asset (editor/project/PREFABS.md, section 13)
    struct PrefabReplaceSettings {
        enum class Match {
            ByName,       // the same name path from the root (Unity's default)
            ByHierarchy,  // the same sibling index path from the root
            None          // only the root: every other override is dropped
        };
        Match match = Match::ByName;
        bool keepOverrides = true;   // false: Unity's ClearAllNonDefaultOverrides
        bool renameRoot = true;      // Unity's changeRootNameToAssetName
    };

    // What a replace kept and dropped, for the log
    struct PrefabReplaceReport {
        size_t kept = 0;
        std::vector<std::string> dropped;
    };

    // Where a prefab is used: by scene (closed ones read from their file), and the prefabs
    // nesting it. Instances counts the instances in the scene itself (added prefab instances
    // included), nested the ones inside other prefab instances.
    struct PrefabSceneUsage {
        uint32_t sceneId = NULL_PROJECT_SCENE;
        std::string sceneName;
        bool loaded = false;
        size_t instances = 0;
        size_t nested = 0;
        size_t missing = 0;      // Missing Prefab instances of it (its GUID resolves to no file)
    };
    struct PrefabUsage {
        std::string guid;
        std::filesystem::path path;               // the last path known for a missing one
        std::vector<PrefabSceneUsage> scenes;
        std::vector<std::filesystem::path> nestedIn;
        // Prefab Variants (PREFABS.md 19): its base (a variant), and the variants of it
        std::filesystem::path base;
        bool baseMissing = false;
        std::vector<std::filesystem::path> variants;
        size_t totalInstances() const;
        size_t totalMissing() const;
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

    // A scene of the project whose file cannot be read: the shader list holds what it last knew of it
    struct UnreadableScene {
        std::string name;
        std::string path;   // project-relative
        std::string error;
    };

    // How much Project::refreshClosedSceneShaderKeys may read before it stops and says what is left
    struct ShaderListLimit {
        size_t maxScenes = SIZE_MAX;
        std::int64_t maxMillis = -1;  // -1: no limit
        std::uintmax_t maxFileBytes = UINTMAX_MAX;
    };

    struct ShaderListStatus {
        std::vector<std::string> pendingScenes;   // not read yet (their names): their keys are the last known
        std::vector<UnreadableScene> unreadableScenes;
        size_t scenesRead = 0;
        bool complete() const { return pendingScenes.empty(); }
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
        // Plain http:// to hosts other than localhost (Http, every platform and the editor's
        // Play); exports set Android cleartext and iOS/macOS ATS to match
        bool allowInsecureHttp = false;
    };

    // Player > Resolution and Presentation, shared by every platform: Unity's Default
    // Orientation and Allowed Orientations for Auto Rotation (Android's screenOrientation, the
    // iOS Info.plist), the initial Screen.sleepTimeout and Engine.targetFrameRate. The project's
    // VSync setting gives the initial Engine.vSyncCount.
    struct PresentationSettings {
        ScreenOrientation defaultOrientation = ScreenOrientation::AutoRotation;
        // PlatformRules::orientationBit of each; a fixed default is always in it
        uint8_t allowedOrientations = PlatformRules::DEFAULT_ALLOWED_ORIENTATIONS;
        bool keepScreenOn = true;
        // -1 = the platform's rate
        int targetFrameRate = -1;

        // the allowed set with a fixed default in it, never empty
        uint8_t effectiveAllowedOrientations() const;

        bool operator==(const PresentationSettings& other) const {
            return defaultOrientation == other.defaultOrientation && allowedOrientations == other.allowedOrientations
                && keepScreenOn == other.keepScreenOn && targetFrameRate == other.targetFrameRate;
        }
        bool operator!=(const PresentationSettings& other) const { return !(*this == other); }
    };

    // Name, identifier, version and build below are overrides: empty (or 0)
    // means the ApplicationSettings value.

    // An Info.plist value of the native plugin settings (ios.plist_entries, macos.plist_entries):
    // a string, a boolean, a number or a list of strings
    struct NativePlistValue {
        enum class Kind { String, Bool, Integer, Real, StringArray };
        Kind kind = Kind::String;
        std::string text;                   // String
        bool flag = false;                  // Bool
        long long integer = 0;              // Integer
        double real = 0.0;                  // Real
        std::vector<std::string> items;     // StringArray
        bool operator==(const NativePlistValue& other) const {
            if (kind != other.kind) return false;
            switch (kind) {
                case Kind::String: return text == other.text;
                case Kind::Bool: return flag == other.flag;
                case Kind::Integer: return integer == other.integer;
                case Kind::Real: return real == other.real;
                case Kind::StringArray: return items == other.items;
            }
            return false;
        }
        bool operator!=(const NativePlistValue& other) const { return !(*this == other); }
    };

    // A CocoaPod of the iOS export's Podfile (ios.cocoapods)
    struct NativeCocoaPod {
        std::string name;       // "Google-Mobile-Ads-SDK"
        std::string version;    // "~> 11.0"; "" = any
        std::string source;     // a spec repository URL; "" = the CocoaPods trunk
        bool operator==(const NativeCocoaPod& other) const { return name == other.name && version == other.version && source == other.source; }
    };

    // A Swift package of the iOS export (ios.swift_packages): up to the next major version
    struct NativeSwiftPackage {
        std::string url;                    // "https://github.com/RevenueCat/purchases-ios"
        std::string version;                // minimum version, "4.0.0"
        std::vector<std::string> products;  // linked into the iOS target, e.g. {"RevenueCat"}
        bool operator==(const NativeSwiftPackage& other) const { return url == other.url && version == other.version && products == other.products; }
    };

    struct WebProjectSettings {
        std::string applicationName;
        std::filesystem::path favicon;
        std::filesystem::path customHtmlShell;
        std::string headInclude;
        bool resizeCanvasToWindow = true;
        bool hideEmscriptenUI = false;
        // native plugins (web.script_urls): <script src> in <head>, before plugins/native/web/*.js
        std::vector<std::string> scriptUrls;
        // the page's viewport covers the whole screen (viewport-fit=cover): the canvas reaches
        // under a phone's notch and env(safe-area-inset-*) reports it (Screen.safeArea)
        bool viewportFitCover = true;
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
        // native plugins (macos.plist_entries, macos.frameworks): merged into Info.plist, linked
        std::map<std::string, NativePlistValue> plistEntries;
        std::vector<std::string> frameworks;   // "StoreKit"; "Name?" = weak
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
        // S30: the team that signs a device build, and the archive's export method
        std::string teamId;
        std::string exportMethod = "development";   // development, ad-hoc, app-store
        // native plugins (ios.*): what SDKs need from the Xcode project
        std::map<std::string, NativePlistValue> plistEntries;   // merged into Info.plist (replacing keys)
        std::string plistXml;              // raw <key>..</key><value> pairs appended to the top dict
        std::vector<std::string> frameworks;   // system frameworks linked: "StoreKit"; "Name?" = weak
        std::vector<NativeCocoaPod> cocoapods;  // workspaces/xcode/Podfile
        std::vector<NativeSwiftPackage> swiftPackages;
        std::string entitlementsXml;       // the <dict> body of ios/JasperEngine.entitlements
        std::string deploymentTarget = "15.0";   // IPHONEOS_DEPLOYMENT_TARGET, at least 15.0
    };

    // Android INTERNET permission: Auto adds it when the exported Lua or C++ uses Http (or the
    // permission is ticked), Require always adds it (Unity's Internet Access)
    enum class AndroidInternetAccess {
        Auto,
        Require
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
        // the orientation comes from PresentationSettings (Player > Resolution and Presentation)
        bool abiArmeabiV7a = true;
        bool abiArm64V8a = true;
        bool abiX86 = true;
        bool abiX86_64 = true;
        std::set<std::string> permissions;
        AndroidInternetAccess internetAccess = AndroidInternetAccess::Auto;
        bool allowBackup = true;
        bool fullscreen = true;
        // S30: a prebuilt player's package (androidFormatName: aab, the default; apk-per-abi; or
        // apk, the universal APK of every selected ABI), and the release keystore (its passwords
        // are never saved: the build asks, or JASPER_ANDROID_KS_PASS / JASPER_ANDROID_KEY_PASS);
        // no keystore: the debug one (~/.android/debug.keystore)
        std::string packageFormat = "aab";
        std::filesystem::path keystorePath;
        std::string keystoreAlias;
        // native plugins (android.*): what SDKs need from the Gradle project
        std::vector<std::string> gradleDependencies;   // "group:name:version" -> implementation; other text verbatim
        std::vector<std::string> gradleRepositories;   // Maven repository URLs
        std::vector<std::string> gradleProperties;     // "key=value" lines of gradle.properties
        std::vector<std::string> extraPermissions;     // full names: "com.google.android.gms.permission.AD_ID"
        std::string manifestXml;               // raw XML inside <manifest>, before <application>
        std::string manifestApplicationXml;    // raw XML inside <application>
    };

    struct ComponentRecoveryEntry {
        Entity entity;
        YAML::Node node;
    };

    using ComponentRecovery = std::map<std::string, ComponentRecoveryEntry>;

    // Project Settings > Player > Logging (project.yaml "playerLogging"). The defaults are what
    // games did before: everything from Info on to the platform log, no file, no backtraces.
    struct PlayerLoggingSettings {
        std::string level = "info";      // trace, debug, info, warn, error, off
        bool fileLog = false;            // desktop: <userdata>/logs/player.log, 1 MB x 2
        bool backtrace = false;          // errors record their call stack

        bool operator==(const PlayerLoggingSettings& other) const {
            return level == other.level && fileLog == other.fileLog && backtrace == other.backtrace;
        }
    };

    // editor/tests/PlayStartupScenesTest.cpp: builds a play session around a scenes vector
    struct PlaySessionTestAccess;
    // Project Settings > Player > Crash Handling (project.yaml "playerCrash", docs/crash-diagnostics.md).
    // The defaults are what a game does without the key.
    struct PlayerCrashSettings {
        static constexpr int minHangSeconds = 1;
        static constexpr int maxHangSeconds = 60;

        bool enabled = true;             // the crash handler writes <user data>/crash/reports/...
        bool includeLogTail = true;      // log-tail.txt with the last log lines
        bool dumpWithHeap = false;       // Windows Dump Type: Small (false) or With Heap
        bool hangWatchdog = true;        // Development Builds: the main-thread hang watchdog
        int hangSeconds = 5;             // its threshold
        bool webCrashOverlay = true;     // Web: "The game stopped working. [Reload]"

        bool operator==(const PlayerCrashSettings& other) const {
            return enabled == other.enabled && includeLogTail == other.includeLogTail && dumpWithHeap == other.dumpWithHeap &&
                   hangWatchdog == other.hangWatchdog && hangSeconds == other.hangSeconds && webCrashOverlay == other.webCrashOverlay;
        }
    };

    // What an asset-root change does or would do (Project::planAssetRootChange, changeAssetRoots);
    // absolute paths
    struct AssetRootChange {
        struct Move {
            std::filesystem::path from;
            std::filesystem::path to;
            bool folder = false;
        };
        std::vector<Move> moves;                              // planned, then done (companions included)
        std::vector<std::filesystem::path> alreadyThere;      // identical file at the destination: used, source kept
        std::vector<std::filesystem::path> keptOutside;       // outside both old roots: references kept as they are
        std::vector<std::filesystem::path> rewritten;         // scenes, prefabs, materials, project.yaml
        std::vector<std::filesystem::path> savedScenesWithEdits;  // open scenes that had unsaved changes
        std::vector<std::filesystem::path> unsavedBuffers;    // Code Editor buffers of files that move
        std::vector<std::filesystem::path> failedSaves;       // rewritten in memory, not on disk
        std::vector<std::filesystem::path> removedFolders;    // emptied by the moves
        std::vector<std::string> textReferencesBroken;        // "referrer:line (target)", project-relative
        std::string blocker;    // preflight refusal: nothing changed
        std::string error;      // failed while moving: what moved was put back, the roots are unchanged
        bool changed = false;   // the roots changed (also with failedSaves)

        bool ok() const { return blocker.empty() && error.empty(); }
        // the reason it did not change anything ("" when it did)
        const std::string& refusal() const { return blocker.empty() ? error : blocker; }
    };

    class Project{
        friend struct PlaySessionTestAccess;
    private:

        Conector conector;
        Generator generator;
        // how Play built the C++ scripts: a reload during Play builds the same way
        CMakeKit playBuildKit;
        unsigned int playBuildJobs = 0;
        struct CppReload {
            bool running = false;
            std::chrono::steady_clock::time_point firstSeen;
            std::chrono::steady_clock::time_point started;
            std::vector<std::filesystem::path> changed;
            // the new DPROPERTYs by class (header parsed when the reload started)
            std::map<std::string, std::vector<ScriptProperty>> properties;
        } cppReload;
        void applyCppReload();
        // Play's startup and stop jobs (process channel): waited for when the project goes
        jobs::Task<void> playStartupTask;
        jobs::Task<void> playStopTask;
        // the scene save writing on the io-write channel
        jobs::Task<void> sceneSaveTask;

        std::string name;

        unsigned int canvasWidth;
        unsigned int canvasHeight;
        Scaling scalingMode;
        // Project Settings > Graphics > UI Scale (the Canvas Scaler's project default)
        CanvasScalerSettings uiScale;
        // the Game view's image (setEditViewSize); 0 = the canvas
        float editViewWidth = 0.0f;
        float editViewHeight = 0.0f;
        float editViewDpi = 0.0f;
        TextureStrategy textureStrategy;
        bool vsyncEnabled;
        bool dynamicBatching = true;
        // Quality > Atmosphere Quality: desktop, mobile, web
        std::array<AtmosphereQuality, 3> atmosphereQuality;
        std::array<float, 3> foliageDistanceScale = defaultFoliageDistanceScale;
        PlayerLoggingSettings playerLogging;
        // Input: the actions asset Play and the game start with (project-relative, empty for
        // none) and the deadzone of AXIS and VECTOR2 actions without their own
        std::filesystem::path inputActionsAsset;
        float inputDeadzone = 0.15f;
        // Pixel Art: the palette used where none is named (S43 R4b), project-relative, empty for none
        std::filesystem::path defaultPalette;
        PlayerCrashSettings playerCrash;
        int htmlUiMsaaSamples;
        // graphics.pixelsPerUnit (S41): texels per world unit of the sprites and tilemaps that take it
        int pixelsPerUnit = 1;
        // Project Settings > Pixel Art (S42) and graphics.defaultFilterMode (bilinear or point)
        pixelart::PixelArtSettings pixelArtSettings;
        TextureFilterMode defaultFilterMode = TextureFilterMode::Bilinear;
        HtmlUiRenderSettings htmlUiRender;
        // graphics.gpu_instancing, quality.shadow_lod_bias (spec S55)
        DrawSubmissionSettings drawSubmission;
        // Project Settings > Quality > Device Tiers, LOD biases, Thermal Response and Graphics >
        // Upload Budget (spec S56): project.yaml's device block
        DeviceSettings deviceSettings;
        // graphics.shadowTier.<platform>: the shadow quality tier a scene of Quality Auto uses
        ShadowTier shadowTiers[ShadowTiers::PLATFORM_COUNT];
        // graphics.light_mode: how lit meshes find their pixel lights (S57 R1)
        LightMode lightMode = defaultLightMode;
        WindowMode windowMode;
        unsigned int windowWidth;
        unsigned int windowHeight;
        bool windowResizable;
        std::string windowTitle;  // empty = use the project name
        std::filesystem::path windowIcon;  // project-relative image path; empty = no icon
        std::filesystem::path assetsDir;
        // the trusted roots, resolved on the main thread when the project loads or a root setting
        // changes (refreshTrustedRoots) and published whole: workers read the snapshot, never
        // resolve or change it (std::atomic_load / std::atomic_store)
        std::shared_ptr<const std::vector<std::filesystem::path>> trustedRoots;
        std::filesystem::path luaDir;
        std::vector<std::filesystem::path> scriptDirs;  // extra C++ include and source roots
        int cxxStandard;  // C++ standard for Play and exported builds
        bool physics2DEnabled;  // Box2D in exported builds; Play always has it
        bool physics3DEnabled;  // Jolt in exported builds; Play always has it
        bool runtimeShadersEnabled;  // exported builds compile the shader variants they lack
        PbrTier mobilePbrTier;       // the lobes Android, iOS and HTML5 exports draw
        // Tags and Layers: user tags, and user layer names indexed by layer (MAX_LAYERS
        // entries, empty for built-in and unnamed layers)
        std::vector<std::string> tags;
        std::vector<std::string> layerNames;
        // Layer Collision Matrices of Physics (3D) and Physics 2D, all layers collide by default
        LayerCollisionRows layerCollisionMatrix3D = allLayersCollide();
        LayerCollisionRows layerCollisionMatrix2D = allLayersCollide();
        Physics3DWorldSettings physics3DWorld;
        // an input node of project.yaml that could not be read (an older editor's Input Manager
        // actions with a typo made by hand): written back as it was until an actions asset is
        // set, so nothing is lost
        std::string unreadableInputNode;
        InputActionsMigration inputMigration;
        // Project Settings > Graphics > Shader Globals: the global shader values the project
        // declares (name, type, default), applied to the engine's ShaderGlobals
        std::vector<ShaderGlobalDecl> shaderGlobals;
        bool packNativeResources;
        bool versionControlMetadata;
        VcsProjectSettings vcsProjectSettings;
        std::unique_ptr<vcs::VcsService> vcsService;
        std::unique_ptr<vcs::SceneDiffTracker> sceneDiffs;
        ShaderOverrides shaderOverrides;
        SourceCodeExportSettings sourceCodeExportSettings;
        DesktopExportSettings desktopExportSettings;
        // Build Settings > Development Build (S15 R39): a Release game with the profiler and its
        // --profile options (export: developmentBuild, default off)
        bool developmentBuild = false;
        std::string buildMethod = "auto";
        bool packContent = false;
        // Project Settings > Player > Strip Engine Code (docs/specs/S34-player-size.md R7): builds leave
        // out the engine modules the project cannot reach (export: stripEngineCode, default on)
        bool stripEngineCode = true;
        // S45 R18: "enabled" | "enabled_for_builds" | "disabled"
        std::string spriteAtlasMode = "enabled";
        ApplicationSettings applicationSettings;
        PresentationSettings presentationSettings;
        WebProjectSettings webProjectSettings;
        LinuxProjectSettings linuxProjectSettings;
        WindowsProjectSettings windowsProjectSettings;
        MacOSProjectSettings macOSProjectSettings;
        IOSProjectSettings iosProjectSettings;
        // the project's additions to the Apple privacy manifest (iOS and macOS): apple.privacy
        ApplePrivacy::Manifest applePrivacy;
        AndroidProjectSettings androidProjectSettings;
        CommandHistory projectHistory;
        uint64_t selectionGesture = 1;
        bool selectionGestureHeld = false;
        void installUndoHooks();

        uint32_t startSceneId;
        LoadingSettings loadingSettings;
        ThreadingProjectSettings threadingSettings;
        ShaderCreationProjectSettings shaderCreationSettings;
        AudioSettings audioSettings;
        DebugSettings debugSettings;
        // applyDebugSettingsForPlay ran for the running Play (until it stops): changes apply live
        mutable bool debugSettingsInPlay = false;
        TerrainEditorSettings terrainEditorSettings;
        uint64_t terrainEditorSettingsGeneration = 1;
        TextureProjectSettings textureSettings;
        ModelProjectSettings modelSettings;
        std::vector<std::filesystem::path> inspectedAssets;
        PhysicsPlacementSettings physicsPlacementSettings;
        NavigationProjectSettings navigationSettings;
        SpriteNormalsWindowSettings spriteNormalsSettings;
        bool simulateTouch;
        std::vector<GameResolution> customResolutions;

        // True while project.yaml holds the only copy of this user's editor state
        bool workspaceMigrationPending = false;
        bool skipWorkspaceTabs = false;
        // What project.yaml's load could not read: dropped issues gate its saves
        LoadReport projectLoadReport;
        // Scenes whose last saveScene the save gate refused (Save All goes on past them)
        std::unordered_set<uint32_t> refusedSaves;
        int prefabSaveScopeDepth = 0;
        DegradedSavePolicy prefabSaveScopePolicy = DegradedSavePolicy::Refuse;
        std::vector<std::filesystem::path> refusedPrefabSaves;

        // Data safety state (ProjectDataSafety.cpp)
        std::unique_ptr<io::ScopedFileLock> projectLock;
        bool ignoreProjectLock = false;
        bool skipProjectLock = false;
        bool readOnlyAssets = false;
        bool projectLockedFailure = false;
        std::string projectLockMessage;
        struct AutosaveScene {
            float lastWrite = 0.0f;
            float interval = 0.0f;
            // the last backup's bytes, as its worker thread wrote them
            struct Written {
                std::mutex mutex;
                std::string sha1;
                bool writing = false;
            };
            std::shared_ptr<Written> written = std::make_shared<Written>();
        };
        std::unordered_map<uint32_t, AutosaveScene> autosaveScenes;
        float autosaveClock = 0.0f;
        float autosaveNextCheck = 0.0f;
        // when tickAutosave last ran (the editor renders on demand: isAutosaveDue adds the time
        // since, which the next frame's delta brings in)
        std::chrono::steady_clock::time_point autosaveLastTick = std::chrono::steady_clock::now();
        std::shared_ptr<std::atomic<int>> autosaveWritesInFlight = std::make_shared<std::atomic<int>>(0);
        std::optional<RecoveryChoice> nextRecoveryChoice;
        RecoveryOutcome lastRecovery;

        // loadProject: false when another live editor holds the project and the host says no
        bool takeProjectLock(const std::filesystem::path& path);
        // R12: the temps of dead processes into .jasperengine/backup/orphans
        void sweepOrphanedTemporaryFiles();
        // loadScene: the bytes to load instead of the file's (Restore), false to load the file
        bool resolveSceneRecovery(const std::filesystem::path& relativePath, std::string& restoredBytes);
        // R17: untitled backups of dead sessions, offered once when the project opens
        void offerUntitledBackups();
        // After an explicit save or a Discard: the scene's backup has nothing left to protect
        void deleteSceneBackup(const SceneProject& sceneProject, const std::filesystem::path& previousPath);
        // The backup of one scene or prefab stage, if due
        void autosaveDocument(SceneProject& sceneProject, BackupKind kind, const std::filesystem::path& source, float interval);

        // True while the model describes what is on disk at projectPath.
        // createTempProject() resets it and then points projectPath at the project it
        // is about to open, so a non-empty projectPath alone does not imply it.
        bool modelLoaded = false;

        // True when the standalone build sources need an update on the main thread
        std::atomic<bool> generatedSourcesDirty{false};
        // An export and updateGeneratedSources never write .jasperengine/generated at once: the
        // exports running (beginGeneratedExport), and the lock the regeneration writes under
        mutable std::mutex generatedMutex;
        int generatedExports = 0;

        ShaderListCache shaderListCache;
        std::filesystem::path shaderListCacheProject;  // whose file it holds
        jobs::Task<bool> shaderListWrite;              // its file being written (ioWriteChannel)
        // A scene file that did not read as a scene, not read again until its stamp changes
        struct ShaderListFailure { std::int64_t writeTime = 0; std::uintmax_t size = 0; std::string error; };
        std::map<std::string, ShaderListFailure> shaderListFailures;

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
            // The Play copy (ownedRuntime), on the heap; null for the played scene itself, an
            // element of `scenes` that a scene added or removed meanwhile moves: runtimeOf()
            SceneProject* copy = nullptr;
            bool ownedRuntime = false;       // true when the scene used is a copy, deleted with the session
            bool initialized = false;        // true when scripts and library are initialized
            bool initializing = false;       // true while its scripts run, to stop re-entrant init
            bool scriptsRan = false;         // its scripts were started once (a load replacing it retires it)
            // the DontDestroyOnLoad scene (SceneManager's persistent scene): no load replaces it,
            // Stop deletes it with the session
            bool persistent = false;
        };

        // The scene a runtime entry plays (the Engine's while playing): its copy, or the played
        // scene found by id now (null if it is gone)
        SceneProject* runtimeOf(const PlayRuntimeScene& entry);
        Scene* runtimeSceneOf(const PlayRuntimeScene& entry);

        struct PlaySession {
            uint32_t mainSceneId = NULL_PROJECT_SCENE;
            std::vector<PlayRuntimeScene> runtimeScenes;
            // scenes a scene load replaced (script-lifecycle 9.4): out of the game, their scripts
            // released; a Play copy is deleted and the played scene restored at Stop
            std::vector<PlayRuntimeScene> retiredRuntimeScenes;
            // the variants the session's objects used, by source scene id: collected when a load
            // retires a scene and at Stop, before the scripts go (they own what they created)
            std::map<uint32_t, std::set<ShaderKey>> playShaderKeys;
            std::atomic<bool> cancelled{false};
            std::atomic<bool> startupThreadDone{false};  // Set when connect thread exits
            std::atomic<bool> startupSucceeded{false};   // True only if finalizeStart was called
        };

        mutable std::mutex playSessionMutex;
        std::shared_ptr<PlaySession> activePlaySession;
        size_t closedSceneScriptReads = 0;

        // Single-frame advance while paused (see step())
        uint32_t stepSceneId = NULL_PROJECT_SCENE;
        bool stepPending = false;
        float stepSavedMaxDeltatime = 0.0f;

        SceneProject* createRuntimeCloneFromSource(const SceneProject* source);
        // ...from a scene node (the file's, or the played scene's Play-start snapshot)
        SceneProject* createRuntimeCloneFromNode(const SceneProject* source, const YAML::Node& sceneNode);
        // A load factory's first step: the session's scenes that load replaced (their scripts
        // unloaded) leave the game, so the stack is built anew as an exported build does
        void retireReplacedRuntimeScenes();
        // SceneManager's persistent scene factory while the session plays: a runtime scene of
        // the session named "DontDestroyOnLoad" (null when nothing plays)
        Scene* createPersistentRuntimeScene();
        // What DontDestroyOnLoad / MoveGameObjectToScene moved: the lists the editor shows
        // (authored GameObjects, the selection) follow
        void onPlayObjectsMoved(Scene* from, Scene* to, const std::unordered_map<Entity, Entity>& moved);

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
        // the Scene view's picking (editor/picking) reads the pickable GameObjects and their bounds
        friend class ScenePicker;
        friend class SceneViewController;
        // the cooked scenes and prefabs of a prebuilt build read what the generated sources do (S30)
        friend class cook::SceneCooker;
        // and the modules a build needs, what the build ships (S34)
        friend class ModuleUsageDetector;
        friend class ScriptAttach;
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
        // the box select (R15): the BVH's frustum of the box, then every corner inside it; the
        // view's own scene (viewScene) takes its Layers filter
        bool selectEntitiesInRect(uint32_t sceneId, const std::vector<Entity>& entities, Scene* scene, const Matrix4& vpMatrix, Vector2 start, Vector2 end,
                                  const SceneProject* viewScene = nullptr);
        uint32_t selectedScene;
        uint32_t selectedSceneForProperties;

        using MaterialLinkKey = std::tuple<uint32_t, Entity, unsigned int>;
        struct MaterialLinkEntry {
            std::string filePath;
        };

        std::filesystem::path projectPath;
        // isTempProject() cache
        mutable std::filesystem::path tempCheckPath;
        mutable bool tempCheckResult = false;
        std::map<MaterialLinkKey, MaterialLinkEntry> materialFileLinks;
        bool watchFiles = false;

        std::map<std::filesystem::path, Prefab> prefabAssets;

        // Parsed prefab files, reused while the file on disk keeps its time and size:
        // pasting a hundred instances parses the file once instead of per instance
        struct PrefabFileCache {
            std::filesystem::file_time_type writeTime{};
            std::uintmax_t size = 0;
            YAML::Node node;
            std::string sha1;
            MigrationResult migration;
        };
        std::map<std::filesystem::path, PrefabFileCache> prefabFileCache;
        // The parsed and migrated file (its migrations and a newer format go into the installed
        // load report each time); sha1, when given, receives the bytes' SHA-1
        // migrated: set when the file's version was older than this editor's (the returned node
        // is already in the current format: the file is written again on its next save)
        YAML::Node loadPrefabFile(const std::filesystem::path& fullPath, std::string* sha1 = nullptr, bool* migrated = nullptr);
        // getPrefabFileBase: the base each .prefab file names (empty: an original), by the
        // file's time and size, looked at again at most once a second
        struct PrefabBaseCache {
            std::filesystem::file_time_type writeTime{};
            std::uintmax_t size = 0;
            std::string baseGuid;
            std::filesystem::path basePath;
            std::chrono::steady_clock::time_point checked{};
        };
        std::map<std::filesystem::path, PrefabBaseCache> prefabBaseCache;
        std::vector<std::filesystem::path> standalonePrefabs;  // built without a scene instance

        // GUID <-> path of the prefab files (and later other asset types)
        AssetDatabase assetDatabase;
        // the walk of the disk refresh(scan) compares, an S06 Background job (R23) started when
        // the file watcher reports a change (S05: no poll); applied on the main thread
        jobs::Task<AssetDatabase::DiskScan> assetScan;
        // a watcher batch arrived: walk again (after the walk in flight, if any)
        bool assetScanRequested = false;
        void waitForAssetScan();
        // Found while resolving references (in the middle of an import), applied on refresh
        std::vector<AssetDatabase::Change> pendingAssetChanges;
        // A tracked prefab file a read found gone before the database's walk did: the database
        // forgets it now (the file put back is then new to it, an Added that relinks the Missing
        // instances the read made), its removal waits for the next refresh
        void forgetGonePrefabFile(const std::filesystem::path& filepath);
        // Who references what (S04): closed files from disk, documents with unsaved edits from memory
        AssetIndex assetIndex;
        MissingAssetTracker missingAssets;
        // S25: asset labels (in metas and in-file keys), this user's bookmarks, saved searches
        // and palette recents, and the index the asset search evaluates
        AssetLabels assetLabels;
        Organisation organisation;
        AssetSearchIndex assetSearchIndex;
        // Lua's Asset module (AssetMap) in the editor and its Play: answered by the database
        class EditorAssetResolver : public AssetMap::Resolver {
        public:
            explicit EditorAssetResolver(const Project* project) : project(project) {}
            ~EditorAssetResolver() override { AssetMap::setResolver(nullptr); }
            bool findPath(const Guid& guid, std::string& path, AssetType& type) const override;
            Guid findGuid(const std::string& path) const override;
        private:
            const Project* project;
        };
        EditorAssetResolver assetResolver{this};

        // What the database reports changed: moved prefabs get their new path everywhere in
        // memory, removed ones leave Missing Prefab instances behind, added ones relink them;
        // the .anim changes go to applyClipAssetChanges
        void applyAssetChanges(const std::vector<AssetDatabase::Change>& changes);
        // .anim files moved, added (copies, a delete undone), removed or edited outside the
        // editor: the references in memory follow the GUIDs, documents and players reload; a
        // moved model: the sub-clip entries taken from it name it again
        void applyClipAssetChanges(const std::vector<AssetDatabase::Change>& changes);
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

        // FBX imports run on any thread; their models reload on the main one
        struct FbxImportFailure {
            uintmax_t size = 0;
            std::filesystem::file_time_type time;
        };
        std::mutex fbxImportMutex;
        std::mutex fbxImportCallMutex; // held for a whole importFbxModel call
        std::atomic<bool> fbxImportsPaused{false};
        std::set<std::filesystem::path> exportedImports; // FBX an export's scenes use, kept as they are
        std::set<std::filesystem::path> modelReloads; // FBX whose loaded models may be out of date
        std::map<std::filesystem::path, FbxImportFailure> failedFbxImports;
        std::chrono::steady_clock::time_point lastFbxImportCheck;
        static constexpr double fbxImportCheckIntervalSec = 1.0;
        // A Background job (S06); ~Project waits for it before the members it uses go
        jobs::Task<std::map<std::filesystem::path, FbxImportFailure>> fbxImportScan;

        void queueModelReload(const std::filesystem::path& fbxPath);
        // Reloads the models loaded from an older import; false while one of them is still loading
        bool reloadModelFile(const std::filesystem::path& fbxPath, bool synchronous = false);
        // Applies the queued reloads and returns an FBX left waiting for a model still loading
        std::filesystem::path applyModelReloads(bool synchronous);
        // FBX files the loaded scenes use, with the scenes where a model of one is not loaded
        std::map<std::filesystem::path, std::vector<const SceneProject*>> collectSceneModelFiles() const;
        void updateSceneImports(SceneProject* sceneProject);
        // Imports every FBX in the assets, since scripts can load any of them by path
        void importFbxModels();

        std::string libName;

        template<typename T>
        T* findScene(uint32_t sceneId) const;

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
        // The prefabs of the build, sorted by path (PrefabManager ids are their positions +
        // 1, in Play and in the export alike): every prefab a scene instantiates or a script
        // property references, the standalone ones, and the prefabs those nest or reference
        std::vector<PrefabSceneInfo> collectAllPrefabs() const;
        // Loads every prefab of the build, so their sources are generated
        void loadBuildPrefabs();
        // The prefabs the Prefab script properties of a registry reference (resolved paths)
        std::vector<std::filesystem::path> findScriptPrefabReferences(EntityRegistry* registry) const;
        // Lists them in the scene's prefabs (written in its file, read by the build)
        void addScriptPrefabReferences(SceneProject* sceneProject);
        // The C++ scripts of the build's prefabs, which no scene may hold an instance of
        void appendPrefabCppScripts(std::vector<SceneScriptSource>& scripts, std::unordered_set<std::string>& uniquePaths) const;

        // Play: a prefab instantiated by a script is copied from a template, imported once
        // per Play into a scene nobody sees (no YAML read per instance)
        struct PlayPrefabTemplate {
            std::vector<Entity> entities; // the root first, hierarchy order
        };
        std::unique_ptr<SceneProject> playTemplateProject;
        std::map<std::filesystem::path, PlayPrefabTemplate> playPrefabTemplates;
        const PlayPrefabTemplate* getPlayPrefabTemplate(const std::filesystem::path& prefabPath);
        void clearPlayPrefabTemplates();

        void pauseEngineScene(Scene* scene, bool pause) const;
        // Edit mode: every Vertex Animator previewing in an open scene or prefab stage goes back
        // to its start with Preview off (play, saves: vertex animation spec 3.6)
        void stopVertexAnimatorPreviews();

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
        // Runs transform over every stored asset reference (component textures and cubemap
        // faces, model and sound filenames, text fonts), rewriting the value in place.
        static bool visitAssetPathsInRegistry(EntityRegistry* registry, const std::function<bool(std::string&)>& transform);
        // The Animation Player clips of visitAssetPathsInRegistry (their path and GUID are not an AssetRef)
        static bool visitClipPathsInRegistry(EntityRegistry* registry, const std::function<bool(std::string&)>& transform);
        // The files of those references, for SceneManager::preloadScene()
        static SceneResources collectResourcesInRegistry(EntityRegistry* registry);
        // Same for Lua script entries; C++ ones are build inputs kept project-relative.
        static bool visitLuaPathsInRegistry(EntityRegistry* registry, const std::function<bool(std::string&)>& transform);
        bool visitAssetPathsInMaterialFiles(const std::function<bool(std::string&)>& transform,
                                            std::vector<std::filesystem::path>* changedFiles = nullptr,
                                            std::vector<std::filesystem::path>* failedFiles = nullptr, bool dryRun = false);
        // Reads every .material file of the project, calls visit on it and writes back the
        // ones visit changed (returns true for); `changedFiles` gets them, `failedFiles` the ones
        // that could not be written; dryRun writes nothing (changedFiles: what would be written)
        bool visitMaterialFiles(const std::function<bool(Material&)>& visit,
                                std::vector<std::filesystem::path>* changedFiles = nullptr,
                                std::vector<std::filesystem::path>* failedFiles = nullptr, bool dryRun = false);
        // planAssetRootChange and changeAssetRoots: one pass, applied or not
        AssetRootChange runAssetRootChange(const std::filesystem::path& newAssetsDir, const std::filesystem::path& newLuaDir,
                                           bool apply, bool refuseUnsavedBuffers);
        // writeProbe for the asset-root preflight (tests replace it)
        std::function<bool(const std::filesystem::path&)> assetRootWriteProbe;
        bool removeMissingChildSceneReferences(SceneProject& sceneProject);
        bool detachChildSceneFromParents(uint32_t childSceneId, const std::set<uint32_t>& skippedSceneIds);

        void destroyPlayCreatedEntities(SceneProject* sceneProject);
        void finalizeStart(SceneProject* mainSceneProject, std::vector<PlayRuntimeScene>& runtimeScenes);
        void finalizeStop(SceneProject* mainSceneProject, std::vector<PlayRuntimeScene> runtimeScenes,
                          const std::map<uint32_t, std::set<ShaderKey>>& playShaderKeys);
        // the variants `entries` drew with, objects made during Play included, into
        // session.playShaderKeys (the DontDestroyOnLoad scene's under the played scene)
        void collectPlayShaderKeys(PlaySession& session, const std::vector<PlayRuntimeScene>& entries);
        void runPlayStartup(const std::shared_ptr<PlaySession>& session, uint32_t sceneId, const LocalBuildSettings& buildSettings);
        // What Play's startup reads of the scenes, taken on the main thread (preparePlayStartup)
        // for its worker thread, which then only compiles shaders and C++ scripts
        struct PlayStartupPrepared {
            enum class Status { Ready, Cancelled, Failed };
            Status status = Status::Failed;
            std::string message;      // Failed: for the console
            std::string alertTitle;   // Failed: an alert besides the console's message (none when empty)
            std::string alertMessage;
            YAML::Node snapshot;      // the played scene as Stop puts it back
            PlayPrefabState prefabState;
            std::set<Entity> entities; // alive before Play: anything else is the game's
            std::set<ShaderKey> shaderKeys;
            bool hasCppScripts = false;
        };
        // On the main thread: the scenes' script properties, saves and generated sources, the
        // runtime copies, the Play snapshot and the shader keys, all at one moment the editor
        // does not change them (MCP edits, other scenes' views, a scene opened or created)
        PlayStartupPrepared preparePlayStartup(const std::shared_ptr<PlaySession>& session, uint32_t sceneId);
        void failPlayStartup(const std::shared_ptr<PlaySession>& session, uint32_t sceneId, const std::string& message,
                     const std::string& alertTitle = "", const std::string& alertMessage = "");
        bool saveSceneFile(SceneProject* sceneProject, const std::filesystem::path& path, bool stopTransientPreviews = true,
                DegradedSavePolicy policy = DegradedSavePolicy::Refuse, SaveResult* outcome = nullptr);
        // Its file holds it now: not modified, stamped (savedAt), and its history learns it
        // (CommandHistory::markSaved, on the main thread): an undo of a step in the file leaves it
        // modified
        void markSceneSaved(SceneProject* sceneProject);
        bool saveSceneForPlayStartup(SceneProject* sceneProject);
        bool saveLoadedSceneOnStop(SceneProject* loadedSceneProject, SceneProject* editorScene, bool keepModified);
        // Only the shaderKeys of the file on disk: what Stop saves of a scene that ran as a Play copy
        bool saveSceneShaderKeys(SceneProject* sceneProject);
        bool writeSceneToPath(uint32_t sceneId, const std::filesystem::path& path, bool stopTransientPreviews = true,
                DegradedSavePolicy policy = DegradedSavePolicy::Refuse, SaveResult* outcome = nullptr);
        void applyFirstDrawSettings(SceneProject* sceneProject) const;
        void writeSceneSource(SceneProject* sceneProject);
        bool hasMissingSceneSources() const;
        void writeMissingSceneSources();
        void configureGenerator();
        void saveModifiedChildScenes(uint32_t sceneId, std::function<void(bool)> callback = nullptr);
        bool hasSceneUnsavedChangesImpl(uint32_t sceneId, std::unordered_set<uint32_t>& visited) const;
        void saveSceneListSequentially(std::vector<uint32_t> sceneIds, std::function<bool(uint32_t)> needsSave, std::function<void(bool)> callback);

        void collectInvolvedScenes(uint32_t sceneId, std::vector<uint32_t>& involvedSceneIds);
        void collectStartActiveScenes(uint32_t sceneId, std::vector<uint32_t>& activeSceneIds);

        uint32_t createNewSceneInternal(std::string sceneName, SceneType type, uint32_t previousSceneId);
        bool createProjectInDirectory(const std::filesystem::path& path, const std::string& projectName, bool deleteIfExists);

        // Above zero while a non-interactive API runs: alertUser() only logs
        int nonInteractiveDepth = 0;
        void alertUser(const std::string& title, const std::string& message);
        // a scene save that failed: logged, and alerted once with the reason (from the main thread)
        void reportSceneSaveFailure(const std::string& message);
        // A scene path given to a non-interactive API: absolute, normalized, inside the
        // project, ".scene" added when withExtension; empty (and `error` set) when invalid
        std::filesystem::path resolveScenePath(const std::filesystem::path& path, bool withExtension, std::string& error) const;
        // A scene about to close with unsaved changes, as `unsaved` says
        bool settleUnsavedScene(uint32_t sceneId, UnsavedChoice unsaved, std::string& error);

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
        // and cannot compile: it runs anywhere, over the keys collectStandaloneShaderKeys read.
        void buildStandaloneShaderCache(const std::set<ShaderKey>& shaderKeys);

        // ---- S64: the cache of closed scenes' keys
        // What a closed scene's keys read besides its file: the prefabs every scene lists, the
        // files they and the collector read, the editor that computed them
        std::string shaderListContext() const;
        void loadShaderListCache();
        // Off the main thread, one at a time: what changes meanwhile goes with the next list
        void writeShaderListCache();
        std::filesystem::path shaderListCachePath() const;
        // What a closed scene needs, computed from its file: its keys (the file's, what its content
        // needs) and the prefabs it lists, which become sceneProject's. False with error when
        // the file cannot be read as a scene.
        bool readClosedSceneShaderKeys(SceneProject& sceneProject, ShaderListCache::Entry& entry, bool& prefabsChanged, std::string& error);
        // After a save: the file holds these keys, so closing it reads nothing
        void rememberSavedShaderKeys(const SceneProject& sceneProject, const std::string& sha1);
        // Forgets the prefab assets a read brought in, which no scene has an instance of
        void forgetPrefabsLoadedSince(const std::set<std::filesystem::path>& loadedBefore);
        void setClosedScenePrefabs(SceneProject& sceneProject, const std::vector<std::string>& prefabs) const;

    private:
        // see getInspectedAsset()
        std::filesystem::path inspectedAsset;

    public:
        // Unity's activeObject (animation clips spec 9.R30): the asset the Inspector shows, set by
        // the Project window when a single asset is clicked (T2b), cleared by any scene selection
        // change (T2c); empty = the Inspector shows the scene selection. Absolute path.
        void setInspectedAsset(const std::filesystem::path& absolutePath);
        void clearInspectedAsset();
        const std::filesystem::path& getInspectedAsset() const;
        // the inspected asset follows a rename or move of it (or of a folder holding it), and is
        // cleared when it (or its folder) is deleted; the file commands call these (T2b)
        void remapInspectedAsset(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        void cleanupInspectedAsset(const std::filesystem::path& deletedPath);

        // The C++ scripts the build compiles: every scene's and the build prefabs'
        std::vector<SceneScriptSource> getBuildCppScripts() const { return collectAllSceneCppScripts(); }
        // Writes what a C++ build of the scripts needs (scene and prefab sources, the generated
        // CMakeLists.txt), as Play does before it builds, without playing: the C++ script lists
        // of the loaded scenes are collected again first. Returns the scripts the build compiles.
        // Main thread, not while playing (MCP compile_scripts).
        std::vector<SceneScriptSource> prepareScriptBuild();

        // Factory defaults, also used by the settings dialog to restore a value
        static constexpr unsigned int defaultCanvasWidth = 1280;
        static constexpr unsigned int defaultCanvasHeight = 720;
        static constexpr Scaling defaultScalingMode = Scaling::FITWIDTH;
        static constexpr TextureStrategy defaultTextureStrategy = TextureStrategy::NONE;
        static constexpr bool defaultVSyncEnabled = true;
        static constexpr bool defaultDynamicBatching = true;
        static constexpr std::array<AtmosphereQuality, 3> defaultAtmosphereQuality = {AtmosphereQuality::HIGH, AtmosphereQuality::LOW, AtmosphereQuality::LOW};
        static constexpr std::array<float, 3> defaultFoliageDistanceScale = {1.0f, 1.0f, 1.0f};
        static constexpr int defaultHtmlUiMsaaSamples = 4;
        static constexpr int defaultPixelsPerUnit = 1;
        static constexpr int minPixelsPerUnit = 1;
        static constexpr int maxPixelsPerUnit = 256;
        // htmlUiGlyphAtlasMiB: 0 (the platform's default) or this range
        static constexpr int minHtmlUiGlyphAtlasMiB = 4;
        static constexpr int maxHtmlUiGlyphAtlasMiB = 256;
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
        static constexpr PbrTier defaultMobilePbrTier = PbrTier::REDUCED;
        static constexpr LightMode defaultLightMode = LightMode::AUTOMATIC;
        static constexpr int defaultCxxStandard = cxxStandards[0];

        Project();
        ~Project();
        Project(const Project&) = delete;
        Project& operator=(const Project&) = delete;

        // ---- The project's shader list (docs/specs/S64-shader-lists.md). One source for an export,
        // generate_shaders, get_build_info, Play's startup and the Build Settings window:
        // refreshClosedSceneShaderKeys (not needed right after an export's saves), then
        // collectBuildShaderKeys, then finishBuildShaderKeys.
        //
        // Makes each closed scene's shaderKeys what an export would find: its file's keys and what
        // its content needs, computed from its file as an export's load and save do, without
        // drawing or any GPU work. Cached by the file's hash (ShaderListCache): an unchanged
        // project reads no scene. Main thread; a scene read is a load's stall, so what can take long
        // goes through `limit` and comes again for what is pending. Unreadable scenes are named.
        ShaderListStatus refreshClosedSceneShaderKeys(const ShaderListLimit& limit = {});
        // What the scenes need, normalized: each one's keys, and what an open one has loaded and
        // needs now. Any thread an export runs on. Keys of project forks whose sources are gone
        // are left out (the build could only fail on them), their names added to deadForks.
        void collectBuildShaderKeys(std::set<ShaderKey>& keys, std::set<std::string>* deadForks = nullptr) const;
        // The list of a build from what the scenes need: what every build ships, the additions and
        // the warm-up list in (a deleted fork's left out, named in deadForks), the exclusions out
        std::set<ShaderKey> finishBuildShaderKeys(const std::set<ShaderKey>& sceneKeys, const std::set<ShaderKey>& additions,
                                                  const std::set<ShaderKey>& exclusions,
                                                  std::set<std::string>* deadForks = nullptr) const;

        // The standalone build reads shaders from the editor cache (JASPERENGINE_SHADER_PATH)
        // and cannot compile, so a variant the editor never draws with is missing there. The
        // keys are read from the scenes (main thread), the build runs anywhere. The same list as
        // an export's, whichever scenes are open (S64).
        std::set<ShaderKey> collectStandaloneShaderKeys();

        // Live-derive the shader keys a scene needs from its current components
        // (used at scene save and by the Export window's pre-populated list). With
        // includeRuntimeEntities, also every GameObject made during Play (scripts, prefab spawns)
        void collectSceneShaderKeys(const SceneProject* sceneProject, std::set<ShaderKey>& shaderKeys,
                                    bool includeRuntimeEntities = false) const;
        // Removes the keys of project forks whose sources no longer resolve (deleted forks: what
        // the export's shader build would fail on), adding their names to deadForks
        void dropDeadForkKeys(std::set<ShaderKey>& shaderKeys, std::set<std::string>* deadForks = nullptr) const;
        // a project script sets the scene's maxLightsPerObject (the export then ships the lit
        // variants of every light capacity tier, see ShaderVariantCollector)
        bool scriptsSetLightCapacity() const;
        // a project script sets the scene's post-processing (both states of its colour variant bit ship)
        bool scriptsSetShadowQuality() const;
        bool scriptsSetPostProcessing() const;
        // the PostProcessingSettings switches project scripts name (ShaderVariantCollector::
        // getScriptableEffectFields), for the uber variants an export ships
        std::vector<std::string> scriptsPostProcessingFields() const;
        // a project script contains text
        bool scriptsMention(const char* text) const;
        // The shader keys a loaded scene needs, derived from content without drawing anything:
        // its GameObjects and every prefab of the build (one a script spawns into it), in every
        // lighting setup they can make (see ShaderVariantCollector)
        void collectRequiredShaderKeys(const SceneProject* sceneProject, std::set<ShaderKey>& shaderKeys) const;
        // The shader keys a save writes into the scene file (shaderKeys): what the content needs
        // (collectRequiredShaderKeys) and the keys it had (its file's, Play's at Stop), never what
        // has drawn, so the same scene saves the same list whatever rendered before
        void collectSavedShaderKeys(const SceneProject* sceneProject, std::set<ShaderKey>& shaderKeys) const;

        // An export's sources of the scenes never saved (it cleared the generated folder; main.cpp
        // names every scene): written from what the editor holds, their names in `written`; a
        // never-saved scene that is not loaded has nothing to write and goes to `notOpen`.
        // Main thread.
        void writeUnsavedSceneSources(std::vector<std::string>& written, std::vector<std::string>& notOpen);
        // The scenes the generated main.cpp names that have no generated source
        std::vector<std::string> scenesWithoutSource() const;

        std::string getName() const;
        // The C++ scripts library's name (the project name as an identifier)
        const std::string& getLibName() const { return libName; }
        void setName(std::string name);

        void setCanvasSize(unsigned int width, unsigned int height);
        unsigned int getCanvasWidth() const;
        unsigned int getCanvasHeight() const;

        void setScalingMode(Scaling scalingMode);
        Scaling getScalingMode() const;

        // the Canvas Scaler every scene uses unless it overrides it; applied to the Engine at
        // Play and by the generated main of exports. A change relays the edit-mode UI scenes out.
        void setUIScale(const CanvasScalerSettings& uiScale);
        const CanvasScalerSettings& getUIScale() const;
        // The UI size an edit-mode scene lays out at: its Canvas Scaler for the Game view's
        // resolution (getEditViewSize), so the Scene view shows what the Game view will
        void applyEditCanvasScale(Scene* scene, SceneType type, SceneRender* sceneRender) const;
        void applyEditCanvasScaleToAll();
        // The view the Scene view lays UI out for: what the Game view shows (its fixed
        // resolution, else its size; GameWindow sets it), the canvas before; dpi 0 = unknown
        void setEditViewSize(float width, float height, float dpi);
        void getEditViewSize(float& width, float& height, float& dpi) const;

        void setTextureStrategy(TextureStrategy textureStrategy);
        TextureStrategy getTextureStrategy() const;

        void setVSyncEnabled(bool enabled);
        bool isVSyncEnabled() const;

        // Graphics > Dynamic Batching (on by default): consecutive Sprites, small meshes and UI
        // elements that share a material draw as one draw call; applied to the editor at once
        // (the game's Engine.dynamicBatching starts from it) and written into exports
        void setDynamicBatching(bool enabled);
        bool isDynamicBatching() const;

        // Quality > Atmosphere Quality (volumetric fog and clouds) of desktop, mobile and web
        // games; the editor's views run the desktop one (Engine::setAtmosphereQualityDefaults)
        void setAtmosphereQuality(const std::array<AtmosphereQuality, 3>& quality);
        const std::array<AtmosphereQuality, 3>& getAtmosphereQuality() const;

        // Quality > Foliage Distance of desktop, mobile and web games: scales every terrain foliage
        // layer's draw distances (Engine::setFoliageDistanceScaleDefaults; the editor's views run
        // the desktop one); clamped to [0, 10]
        void setFoliageDistanceScale(const std::array<float, 3>& scale);
        const std::array<float, 3>& getFoliageDistanceScale() const;

        // Player > Logging: what an exported game logs (JASPERENGINE_LOG_* in its CMake)
        void setPlayerLogging(const PlayerLoggingSettings& settings);
        const PlayerLoggingSettings& getPlayerLogging() const;
        void setPlayerCrash(const PlayerCrashSettings& settings);
        const PlayerCrashSettings& getPlayerCrash() const;

        // Project Settings > Input (docs/mcp/guide/input.md): the default Input Actions asset
        // (InputActions::loadDefault at Play and at game start) and the default deadzone
        static constexpr float defaultInputDeadzone = 0.15f;
        // the Default Palette (Project Settings, palettes.default; S43 R4b)
        void setDefaultPalette(const std::filesystem::path& palette) { defaultPalette = palette; }
        std::filesystem::path& getDefaultPalette() { return defaultPalette; }
        const std::filesystem::path& getDefaultPalette() const { return defaultPalette; }
        void setInputActionsAsset(const std::filesystem::path& asset);
        std::filesystem::path& getInputActionsAsset();
        const std::filesystem::path& getInputActionsAsset() const;
        // the asset as the engine loads it: relative to the assets directory ("" when there is
        // none or it is outside it, which a build would not ship)
        std::string getInputActionsEnginePath() const;
        void setInputDeadzone(float deadzone);
        float getInputDeadzone() const;

        // MSAA samples of the HTML UI targets (1, 2, 4 or 8; others round down to one of
        // them), applied to the editor's documents at once and written into exports
        void setHtmlUiMsaaSamples(int samples);
        int getHtmlUiMsaaSamples() const;
        // Pixels Per Unit (S41, 1-256, clamped): applied to the editor's scenes at once and written into exports
        void setPixelsPerUnit(int pixelsPerUnit);
        int getPixelsPerUnit() const;
        // Project Settings > Pixel Art (S42 R1, R18): turning Pixel Art Mode on or off changes
        // nothing else; it registers or removes the mode's import and Play hooks
        void setPixelArtSettings(const pixelart::PixelArtSettings& settings);
        const pixelart::PixelArtSettings& getPixelArtSettings() const { return pixelArtSettings; }
        // Graphics > Default Filter Mode (S42 R3: Bilinear or Point): what references start with
        // when assigned without import settings, and what the game's runtime-made Textures use
        void setDefaultFilterMode(TextureFilterMode mode);
        TextureFilterMode getDefaultFilterMode() const { return defaultFilterMode; }
        static bool isValidHtmlUiMsaaSamples(int samples);
        // HTML UI rendering (S16): htmlUiGlyphAtlasMiB (clamped into its range, 0 = the
        // platform's default), htmlUiSdfPrimitives, htmlUiTransformAwareText; applied to the
        // editor's documents at once and written into exports
        void setHtmlUiRenderSettings(const HtmlUiRenderSettings& settings);
        const HtmlUiRenderSettings& getHtmlUiRenderSettings() const;
        // the Device Tier table and the rest of the device settings (docs/device-tiers.md): the
        // editor's QualityResolver takes them at once (its Game view simulates a tier in Play),
        // exports bake them in (main.cpp's Device::configure, game.json's device)
        void setDeviceSettings(const DeviceSettings& settings);
        const DeviceSettings& getDeviceSettings() const;
        static bool isValidHtmlUiGlyphAtlasMiB(int mib);

        // GPU Instancing (spec S55): applied to the editor's scenes at once and written into exports
        void setDrawSubmissionSettings(const DrawSubmissionSettings& settings);
        const DrawSubmissionSettings& getDrawSubmissionSettings() const;

        // the shadow quality tier of a platform (Engine::Platform as an int), applied to the
        // editor's scenes at once and written into exports (docs/rendering/shadows.md)
        void setShadowTier(int platform, ShadowTier tier);
        ShadowTier getShadowTier(int platform) const;
        // lower-case platform names of the setting: macos, ios, web, android, linux, windows
        static const char* getShadowTierPlatformName(int platform);
        static int findShadowTierPlatform(const std::string& name);

        // Light Mode (Project Settings > Graphics): applied to the editor's rendering at once and
        // written into exports when not Automatic
        void setLightMode(LightMode mode);
        LightMode getLightMode() const;

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
        // Scans the project's assets (S04): every asset gets its GUID before anything references it
        void openAssetDatabase();

        void setLuaDir(const std::filesystem::path& luaDir);
        std::filesystem::path getLuaDir() const;

        // Roots added to the C++ build: each is an include directory, and the
        // sources under it compile without a script component referencing them.
        void setScriptDirs(std::vector<std::filesystem::path> scriptDirs);
        const std::vector<std::filesystem::path>& getScriptDirs() const;
        // What the Play library and the exports compile: getScriptDirs() plus plugins/native/cpp
        // (the C++ native plugins) when that folder exists
        std::vector<std::filesystem::path> getBuildScriptDirs() const;
        // plugins/native/cpp has C++ sources (the Play library is built for them)
        bool hasNativeCppPlugins() const;
        // The engine API sync leaves out SDK bundles (.framework, .xcframework, .bundle): their
        // headers are no engine API
        static bool isEngineApiSkippedDir(const std::filesystem::path& dir);

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
        // Project Settings > Graphics > Mobile PBR Tier: the extended lobes the Android, iOS and
        // HTML5 exports draw (Reduced: no iridescence, anisotropy direction maps or clear coat
        // normal maps, MaterialPbr). Desktop exports and the editor draw them all.
        void setMobilePbrTier(PbrTier tier);
        PbrTier getMobilePbrTier() const;

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

        // Project Settings > Physics > 3D World Capacity: the size of every 3D world made from
        // now on (Play scenes and exported builds alike, PhysicsSystem::setWorldCapacity3D).
        // Values are clamped to 16.. (and Jolt's body id range); a running Play keeps its world.
        void setPhysics3DWorldSettings(const Physics3DWorldSettings& settings);
        const Physics3DWorldSettings& getPhysics3DWorldSettings() const;

        // Project Settings > Input, Unity's Input Manager (docs/specs/S22-gameplay.md "Upstream
        // absorption"): the actions Input.getAxis / getButton read by their flat names, the
        // actions asset's "Input Manager" map, else (no asset) Unity's eleven defaults
        std::vector<InputManagerAction> getInputManagerActions() const;
        // the actions asset's text (the converted one while its migration is pending); false
        // when the project has none or it cannot be read
        bool readInputActionsAsset(std::string& text) const;
        // an older editor's Input Manager node (Stream::decodeProject), and its asset written
        // (Project::load): false + a Console error when the file cannot be written
        void setInputActionsMigration(const InputActionsMigration& migration);
        const InputActionsMigration& getInputActionsMigration() const;
        bool writeInputActionsMigration();
        void setUnreadableInputNode(const std::string& yaml);
        const std::string& getUnreadableInputNode() const;
        // Play starts and stops with clean input: no held keys, touches, rebinding overrides,
        // disabled actions or simulated sources of the last Play
        void resetPlayInput() const;

        // Project Settings > Graphics > Shader Globals. Setting them validates nothing (callers
        // do: ShaderGlobals::validate) and declares them to the engine's ShaderGlobals (the
        // Scene and Game views and a running Play read them at once)
        void setShaderGlobals(const std::vector<ShaderGlobalDecl>& decls);
        const std::vector<ShaderGlobalDecl>& getShaderGlobals() const;
        void applyShaderGlobals() const;
        // a Play starts and stops with the declared defaults (the values scripts set are gone)
        void resetPlayShaderGlobals() const;

        // Absolute roots stored references are relative to: assets for textures, models,
        // sounds and fonts ("asset://"), Lua for script entries ("lua://").
        std::filesystem::path getAssetsPath() const;
        std::filesystem::path getLuaPath() const;
        // The project's trusted roots (its directory, its configured assets and Lua directories),
        // resolved when the project loaded or a root setting changed, and kept: moving a folder
        // and putting a link in its place doesn't re-point them (util/SafeWrite.h pins them)
        std::vector<std::filesystem::path> getTrustedRoots() const;
        // Resolves the trusted roots of the current configuration and publishes them (main thread)
        void refreshTrustedRoots();

        std::filesystem::path normalizeToProjectRelative(const std::filesystem::path& path) const;
        std::filesystem::path resolveAssetPath(const std::filesystem::path& assetRelative) const;
        std::filesystem::path normalizeToAssetsRelative(const std::filesystem::path& path) const;
        // False for files the assets root only reaches with "..", which the runtime cannot resolve.
        bool isInsideAssetsPath(const std::filesystem::path& path) const;
        std::filesystem::path resolveLuaPath(const std::filesystem::path& luaRelative) const;
        std::filesystem::path normalizeToLuaRelative(const std::filesystem::path& path) const;

        // Shader fork files. A fork path (customShader, a material's shader, an include) is
        // project-relative; one the project root does not have is looked up in the assets
        // directory, the assets-relative spelling ("shaders/x" for "assets/shaders/x"). The
        // game resolves both alike: its asset root holds the assets directory and, at their
        // project-relative paths, every shader source (Exporter::copyCustomShaderSources,
        // which wins on a clash, as the project root wins here). The one place of the rule:
        // the absolute file, or empty when neither root has it.
        static std::filesystem::path findShaderFile(const std::filesystem::path& projectRoot, const std::filesystem::path& assetsRoot,
                                                    const std::string& relative);
        // the project-relative path of the file a fork path resolves to (as given when none has
        // it), to show, open and match it
        std::string resolveShaderFile(const std::string& relative) const;
        // Util::resolveCustomShaderPaths with each entry point through resolveShaderFile
        Util::CustomShaderPaths resolveCustomShaderFiles(const std::string& customShader) const;
        // reads a fork file as the shader compilers and manifests see it
        bool readShaderFile(const std::string& relative, std::string& content) const;

        // What changing the assets / Lua directories would do (project-window spec 6.9); nothing
        // changes (closed scenes are loaded and unloaded to read their references).
        // refuseUnsavedBuffers: an unsaved Code Editor buffer of a file that would move is a
        // blocker (agents) instead of being saved first (the Project Settings window)
        AssetRootChange planAssetRootChange(const std::filesystem::path& newAssetsDir, const std::filesystem::path& newLuaDir,
                                            bool refuseUnsavedBuffers);
        // Applies new roots: referenced files outside them are moved in keeping their layout
        // (with their .meta, .humanoid and Vertex Animation pair), then every stored reference is
        // rewritten and saved, and folders the moves emptied are removed. A blocker (the
        // preflight) or a failed move changes nothing; failed saves are listed. Reports nothing
        // itself: the caller says what happened.
        AssetRootChange changeAssetRoots(const std::filesystem::path& newAssetsDir, const std::filesystem::path& newLuaDir,
                                         bool refuseUnsavedBuffers = false);
        // The asset-root preflight's writability check (default: append-open the file and create a
        // probe folder beside it); tests replace it
        void setAssetRootWriteProbe(std::function<bool(const std::filesystem::path&)> probe) { assetRootWriteProbe = std::move(probe); }

        void setPackNativeResources(bool enabled);
        bool shouldPackNativeResources() const;

        void setVersionControlMetadata(bool enabled);

        bool hasVersionControlMetadata() const;

        // Version control (docs/specs/S29-version-control.md): the project's choice, saved in
        // project.yaml (versionControl), and the service that runs it. A new project uses Git
        // LFS for its binaries; one saved before without the key does not (R21).
        const VcsProjectSettings& getVcsProjectSettings() const { return vcsProjectSettings; }
        // apply: a new provider restarts the service, LFS and lockable scenes rewrite the
        // generated .gitattributes (false while project.yaml is read)
        void setVcsProjectSettings(const VcsProjectSettings& settings, bool apply = true);
        vcs::VcsService& getVcs();
        const vcs::VcsService& getVcs() const;
        // The open documents' diffs against their baseline (Scene Diff, Hierarchy, Inspector)
        vcs::SceneDiffTracker& getSceneDiffs();
        // Starts the service with these settings and the user's (Preferences > Version Control);
        // the editor does, at every project open (watchFiles); the CLI never
        void startVersionControl();
        // What the service runs with: the project's provider, the user's tools and cadences
        vcs::VcsSettings versionControlServiceSettings() const;
        // Undoable reverts keep the files they replaced here, per command (R11)
        std::filesystem::path getVcsBackupDir() const;
        // at project close: what the reverts in the history kept
        void removeVcsBackups();
        // S29 R19, R12: refuses to open a document (a scene or prefab in conflict) with why; and
        // what a save refused as read only (an unlocked lockable file) offers instead. Set by
        // the editor's version control UI; unset, nothing is refused and the save just fails.
        static std::function<bool(const std::filesystem::path& file, std::string& why)> documentOpenGuard;
        static std::function<void(const std::filesystem::path& file, std::function<void()> retrySave)> readOnlySaveHandler;
        // What saving an open scene or prefab stage would write (its unsaved edits included): a
        // scene's encodeSceneProject, a stage's prefab document; undefined for no such document,
        // or one whose encode would leave a GameObject out (Stream::SavedEntitiesCheck)
        YAML::Node encodeDocument(uint32_t sceneId);

        ShaderOverrides& getShaderOverrides();
        const ShaderOverrides& getShaderOverrides() const;
        SourceCodeExportSettings& getSourceCodeExportSettings();
        const SourceCodeExportSettings& getSourceCodeExportSettings() const;
        DesktopExportSettings& getDesktopExportSettings();
        const DesktopExportSettings& getDesktopExportSettings() const;
        bool isDevelopmentBuild() const { return developmentBuild; }
        void setDevelopmentBuild(bool enabled) { developmentBuild = enabled; }
        // Build Settings > Build Method (S30 R29): auto, prebuilt or compile; Pack Content
        const std::string& getBuildMethod() const { return buildMethod; }
        void setBuildMethod(const std::string& method) { buildMethod = method; }
        bool isPackContent() const { return packContent; }
        void setPackContent(bool enabled) { packContent = enabled; }
        bool isStripEngineCode() const { return stripEngineCode; }
        void setStripEngineCode(bool enabled) { stripEngineCode = enabled; }
        // Project Settings > 2D > Sprite Atlas > Mode (S45 R18); a change re-resolves which atlases
        // the editor draws from
        const std::string& getSpriteAtlasMode() const { return spriteAtlasMode; }
        void setSpriteAtlasMode(const std::string& mode);
        // S30: what the generated main.cpp gives SceneManager::registerScene, for game.json
        void collectInvolvedScenesForExport(uint32_t sceneId, std::vector<uint32_t>& involvedSceneIds) {
            collectInvolvedScenes(sceneId, involvedSceneIds);
        }
        void collectStartActiveScenesForExport(uint32_t sceneId, std::vector<uint32_t>& activeSceneIds) {
            collectStartActiveScenes(sceneId, activeSceneIds);
        }
        ApplicationSettings& getApplicationSettings();
        const ApplicationSettings& getApplicationSettings() const;
        PresentationSettings& getPresentationSettings();
        const PresentationSettings& getPresentationSettings() const;
        // Play start: Engine.targetFrameRate / vSyncCount, Screen and the application info from
        // these settings, before any script runs (the generated main of an export does the same)
        void applyPresentationSettings();
        // what the generated init() of a build sets (Generator::configure)
        PlayerSettings getPlayerSettings() const;

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
        // Project Settings > Player > iOS / macOS > Privacy Manifest: what the game adds to the
        // engine's PrivacyInfo.xcprivacy (ApplePrivacy::compose); edit with ApplePrivacySettingsCmd
        const ApplePrivacy::Manifest& getApplePrivacy() const;
        void setApplePrivacy(const ApplePrivacy::Manifest& privacy);
        AndroidProjectSettings& getAndroidProjectSettings();
        const AndroidProjectSettings& getAndroidProjectSettings() const;

        uint32_t getStartSceneId() const;
        void setStartSceneId(uint32_t sceneId);

        LoadingSettings& getLoadingSettings();
        const ThreadingProjectSettings& getThreadingSettings() const;
        // Stores them and applies what applies live (the reserve, the web pump budget) to
        // the editor's job system; the worker and reader counts apply on the next launch
        void setThreadingSettings(const ThreadingProjectSettings& settings);
        // the runtime form (Engine::setThreadingSettings)
        static ThreadingSettings toEngineThreading(const ThreadingProjectSettings& settings);
        // Project Settings > Graphics > Shader Creation (spec S32): the budget and warm draws apply
        // to the editor at once, asynchronous creation on its next start; exports bake them in
        const ShaderCreationProjectSettings& getShaderCreationSettings() const;
        void setShaderCreationSettings(const ShaderCreationProjectSettings& settings);
        // the project's shader warm-up list (ShaderWarmup), next to project.yaml
        std::filesystem::path getShaderWarmupPath() const;
        // Stop of a Play that recorded: the run's entries merged into the list file
        void finishShaderWarmupRecording();
        // empties the list file (and a recording in progress)
        bool clearShaderWarmup();
        const LoadingSettings& getLoadingSettings() const;

        // Project Settings > Audio (audio-mixer spec 9): the mixer asset, the voices, the sample
        // rate, the group new Audio Sources get. Setting them loads the mixer into the engine.
        const AudioSettings& getAudioSettings() const;

        // what the next Play does about its audio capture (audio_session arm); it is used once
        struct AudioCaptureArm {
            bool armed = false;
            bool capture = true;
            // 0: audio.capture_max_seconds
            int maxSeconds = 0;
            // -1: audio.capture_stems
            int stems = -1;
        };
        void armAudioCapture(const AudioCaptureArm& arm);
        const AudioCaptureArm& getAudioCaptureArm() const;
        // why this Play is not captured though it should be; "" when it is or need not be
        const std::string& getAudioCaptureError() const;
        // the session of the Play that just stopped, once ("" when it did not capture)
        std::string takeAudioCaptureSession();
        void setAudioSettings(const AudioSettings& settings);
        // the project mixer's file (absolute; empty when none is set)
        std::filesystem::path getAudioMixerFile() const;
        // where a new project mixer goes (audio/Main.mixer, assets-relative)
        static constexpr const char* defaultAudioMixerPath = "audio/Main.mixer";
        // Loads the project mixer into the engine's AudioMixer with the asset's values (Edit mode
        // and live edits keep their runtime; Play starts it over); a missing or broken file leaves
        // the implicit Master (logged). False + error when it does not read.
        bool applyAudioMixer(std::string* error = nullptr);
        // a file changed on disk (an editor write, its undo / redo, an external edit): the project
        // mixer reloads when it is that file
        void audioFilesChanged(const std::vector<std::filesystem::path>& changed);
        // Play starts from the start snapshot and the remembered values; Play stops back to the
        // asset's values (no script overrides, the listener unpaused, the global volume at 1)
        void resetPlayAudio(bool starting) const;
        // Play's audio capture (docs/specs/S35-agent-audio.md R4, R10): whether this Play captures and
        // the driver it needs, before anything of it loads; the session, around its game frames
        AudioCaptureArm audioCaptureArm;
        AudioCaptureArm playAudioCapture;
        bool playAudioCaptureWanted = false;
        bool playHoldsAudioDriver = false;
        std::string audioCaptureError;
        std::string playAudioCaptureSession;
        void preparePlayAudioCapture();
        void beginPlayAudioCapture(const SceneProject* mainSceneProject);
        void endPlayAudioCapture();
        void releasePlayAudioDriver();

        // Project Settings > Debug (project.yaml `debug:`). Changed through DebugSettingsCmd
        // (undoable), read by Generator and Play start; a running Play takes a change at once
        // (DebugOverlay::updateSettings: the console open, the stats picked stay)
        const DebugSettings& getDebugSettings() const;
        void setDebugSettings(const DebugSettings& settings);
        // the engine as Play starts: the log buffer and stack traces, the development build
        // flag, DebugOverlay::configure (development mode: on in the editor's Play)
        void applyDebugSettingsForPlay() const;

        TerrainEditorSettings& getTerrainEditorSettings();
        const TerrainEditorSettings& getTerrainEditorSettings() const;
        // Replaces them as a whole (a project's workspace loaded, the project closed): the Terrain
        // window reads them again, by the generation, before it writes its own values back
        void setTerrainEditorSettings(const TerrainEditorSettings& settings);
        uint64_t getTerrainEditorSettingsGeneration() const { return terrainEditorSettingsGeneration; }

        // Project Settings > Texture (S14 R24): per-family cook defaults, the Android ETC2 and
        // low-memory fallbacks, the runtime quality and budget per platform, the cook cache
        const TextureProjectSettings& getTextureSettings() const;
        // stores them; the cook queue re-keys what they change
        void setTextureSettings(const TextureProjectSettings& settings);
        // Project Settings > Mesh (S53 R19) and Quality's render budgets (R25): per-platform
        // model import defaults, the cook cache, scripts' mesh data
        const ModelProjectSettings& getModelSettings() const;
        // stores them; the model cook queue re-keys every model
        void setModelSettings(const ModelProjectSettings& settings);
        // The Physics Placement panel's settings, per user (workspace.yaml)
        PhysicsPlacementSettings& getPhysicsPlacementSettings();
        const PhysicsPlacementSettings& getPhysicsPlacementSettings() const;
        // Project Settings > Navigation (project.yaml `navigation:`): sets the engine's
        // navigation settings too (every scene's systems refresh their filters)
        const NavigationProjectSettings& getNavigationSettings() const;
        void setNavigationSettings(const NavigationProjectSettings& settings);

        // The Sprite Normal Map window's last settings, view and light, and the auto-regenerate
        // opt-in, per user (workspace.yaml)
        SpriteNormalsWindowSettings& getSpriteNormalsSettings();
        const SpriteNormalsWindowSettings& getSpriteNormalsSettings() const;

        // Play acts as a touch device: the mouse is one finger and isTouchDevice() is true
        bool isSimulateTouch() const;
        void setSimulateTouch(bool simulateTouch);
        // the Game view's custom resolutions (editor workspace, this user's)
        const std::vector<GameResolution>& getCustomResolutions() const;
        void setCustomResolutions(const std::vector<GameResolution>& resolutions);

        CommandHistory* getProjectCommandHistory();

        // the editor's own temporary project, in a folder of the temp directory no other editor uses
        bool createTempProject(std::string projectName, bool deleteIfExists = false);
        // `<tempRoot>/<name>`, else `<name> 2`, `<name> 3`... the first `isBusy` says is free
        static std::filesystem::path chooseTempProjectDir(const std::filesystem::path& tempRoot, const std::string& name,
                                                          const std::function<bool(const std::filesystem::path&)>& isBusy);
        // Save (path = the project's) or Save Project As (another, empty or new, folder): the
        // project is copied there (the temporary project moved) and the editor continues in the
        // copy. Refused before anything is copied when the folder is inside the project, holds it,
        // or is not empty; any failure removes what was copied and leaves the project where it was.
        bool saveProjectToPath(const std::filesystem::path& path);
        // why Save Project As can't go to `path` ("" when it can)
        std::string saveAsRefusal(const std::filesystem::path& path) const;
        void clearTrash();

        // Keeps .gitignore and .gitattributes written, skipping either if it exists
        // and was not generated here
        bool writeVersionControlMetadata();
        void deleteSceneProject(SceneProject* sceneProject);
        // A closed scene read from its file into a scene of its own, neither opened nor drawn (the
        // pixel-art validator's view of the build list, S42 D3); release it with
        // releaseSceneProbe. Null when the scene is open, unknown or unreadable.
        SceneProject* loadSceneProbe(uint32_t sceneId);
        void releaseSceneProbe(SceneProject* probe);
        void loadSceneProjectData(SceneProject* sceneProject, const YAML::Node& sceneNode);
        bool saveProject(bool userCalled = false, std::function<void()> callback = nullptr);
        // project.yaml; refused (RefusedDegraded) while its load report has dropped issues
        bool saveProjectFile(DegradedSavePolicy policy = DegradedSavePolicy::Refuse, SaveResult* outcome = nullptr);
        // Per-user editor state (.jasperengine/user/workspace.yaml), never project.yaml
        bool saveWorkspaceFile();
        // The next load opens the project without the scenes its workspace had open (the
        // Project Hub's Open Without Last Scene); takeSkipWorkspaceTabs reads and clears it
        void setSkipWorkspaceTabs(bool skip) { skipWorkspaceTabs = skip; }
        bool takeSkipWorkspaceTabs() { const bool skip = skipWorkspaceTabs; skipWorkspaceTabs = false; return skip; }

        bool loadProject(const std::filesystem::path path, bool updateLastOpened = true);

        // ---- data safety (editor/project/ProjectDataSafety.cpp, docs/data-safety.md) ----

        // One editor per project: loadProject takes .jasperengine/user/editor.lock. With the
        // lock ignored (--ignore-lock) a busy lock only warns.
        void setIgnoreProjectLock(bool ignore);
        // A session that never writes the project (CLI checks and exports): no lock at all
        void setSkipProjectLock(bool skip);
        // A session that only reads (the CLI's check, a scene dump that saves nothing), next to
        // an editor that may be upgrading the same files: the asset database writes no
        // identities (metas, in-file GUIDs) and no cache (AssetDatabase::setReadOnly). Before
        // loadProject.
        void setReadOnlyAssets(bool readOnly) { readOnlyAssets = readOnly; }
        // The last loadProject failed because another live editor holds the project
        bool wasProjectLocked() const;
        const std::string& getProjectLockMessage() const;
        // The lock's file, and whether this editor holds it (false on a file system without locks)
        std::filesystem::path getProjectLockPath() const;
        bool holdsProjectLock() const;

        // Scene Auto Save (R13-R15), from the editor loop every frame; userInteracting while an
        // ImGui item or a gizmo drag is active (the backup waits for the next frame)
        void tickAutosave(float deltaSeconds, bool userInteracting);
        // A scene's Auto Save backup is due (or a changed scene's first interval starts): the
        // editor renders a frame for it even when idle, as frames are what run tickAutosave
        bool isAutosaveDue() const;
        // The autosave interval in effect for a scene (it doubles when encoding it is slow)
        float getAutosaveInterval(uint32_t sceneId) const;

        // What the next scene load does with a usable crash backup, instead of asking the host
        // (MCP open_scene recovery=...); cleared by that load
        void setNextRecoveryChoice(RecoveryChoice choice);
        const RecoveryOutcome& getLastRecovery() const;
        // Every backup of the project (scenes, untitled scenes, prefabs) with its status
        std::vector<SceneBackupInfo> listSceneBackups() const;
        // Deletes the backup of an open scene (or of a project-relative scene path)
        bool discardSceneBackup(uint32_t sceneId);
        bool discardSceneBackup(const std::filesystem::path& scenePath);
        // The recovery modal's Restore: the open scene takes the backup's content, keeps its
        // path, is modified, starts with an empty undo history
        bool restoreSceneFromBackup(uint32_t sceneId);
        // An untitled scene's backup back as an untitled, modified scene
        bool restoreUntitledBackup(const SceneBackupInfo& backup);
        // What project.yaml's load could not read
        const LoadReport& getProjectLoadReport() const { return projectLoadReport; }
        // R30: Play and Export build from the live scene, without the components only its file
        // holds; one warning names how many (false when there are none)
        bool warnPreservedNotInBuild(const SceneProject& sceneProject) const;
        // A clean end of the session (project switch, quit): this process's backups are
        // deleted (R16), the lock released
        void endProjectSession();

        // Reimports changed FBX files of the scenes' models and reloads them
        void refreshFbxImports();
        // Imports an FBX into its .glb when that is missing or stale, or always when forced. Any thread.
        bool importFbxModel(const std::filesystem::path& fbxPath, bool force = false);
        // An FBX model is imported when needed; false when it has nothing to load
        bool prepareModelFile(const std::string& modelPath);
        // The .glb an FBX is imported into
        std::filesystem::path getImportedModelPath(const std::filesystem::path& fbxPath) const;
        // Stops the reimports while an export reads the scenes
        void setFbxImportsPaused(bool paused);
        // For exports: applies the reloads of imports done before, fails while one of their models loads
        bool flushFbxReloads(std::string& error);
        // For exports, with all scenes loaded: makes the imports of their FBX models current or fails
        bool prepareFbxModelsForExport(std::string& error);
        // The .material files submeshes are linked to (project-relative). Their changes on
        // disk reach the links through the hot reload (editor/reload, S05).
        std::vector<std::string> getLinkedMaterialFiles() const;
        // `relativePath` was read again: every submesh linked to it takes `material`
        void applyLinkedMaterialFile(const std::string& relativePath, const Material& material);

        //=== HTML UI hot reload ===
        // The scenes whose HTML UI documents hot reload (editor/reload): the loaded scenes, Prefab Mode
        // stages and the playing copies of child scenes
        std::vector<SceneProject*> getHtmlUIScenes();

        void updateGeneratedSources();
        void markGeneratedSourcesDirty();
        // Exporter::runExport, around its whole run (any thread): updateGeneratedSources waits until
        // the export has finished, keeping the dirty flag, so what changed meanwhile is regenerated
        // after it. Waits for a regeneration in progress.
        void beginGeneratedExport();
        // An export is running: its worker reads the project, so what changes it (reading a closed
        // scene's prefabs) waits
        bool isGeneratedExportRunning() const;
        void endGeneratedExport();

        //=== Linked Material part ===

        void linkMaterialFile(uint32_t sceneId, Entity entity, unsigned int submeshIndex, const std::string& filePath);
        bool isMaterialFileLinked(uint32_t sceneId, Entity entity, unsigned int submeshIndex) const;
        std::string getMaterialFilePath(uint32_t sceneId, Entity entity, unsigned int submeshIndex) const;
        void unlinkMaterialFile(uint32_t sceneId, Entity entity, unsigned int submeshIndex);
        void unlinkAllMaterialFiles(uint32_t sceneId, Entity entity);
        void unlinkAllMaterialFiles(uint32_t sceneId);

        //=== end Linked Material part ===

        // While Play runs (or starts or stops) no scene leaves the project: the session finds the
        // played scene by id and runs it in place. True when that refuses it now: the reason is
        // logged, put in *reason and, with alert (and not inside a non-interactive call), shown.
        // (Deleting a scene's file never takes its scene out: the reference stays, Missing, S04 R15.)
        bool refuseSceneRemovalWhilePlaying(const std::string& what, std::string* reason = nullptr, bool alert = true);

        void checkUnsavedAndExecute(uint32_t sceneId, std::function<void()> action);

        void saveScene(uint32_t sceneId, std::function<void(bool)> callback = nullptr);
        // A scene save that would drop what its file's load could not read is refused
        // (SaveResult::RefusedDegraded, nothing written) unless policy is SaveAnyway; Save As to
        // another file is not (docs/data-safety.md)
        bool saveSceneToPath(uint32_t sceneId, const std::filesystem::path& path, bool stopTransientPreviews = true,
                DegradedSavePolicy policy = DegradedSavePolicy::Refuse, SaveResult* outcome = nullptr);
        void saveSceneToPathAsync(uint32_t sceneId, const std::filesystem::path& path, std::function<void(bool)> callback = nullptr,
                DegradedSavePolicy policy = DegradedSavePolicy::Refuse);
        // True when saving the scene to path (its own file when empty) would drop data its load
        // report holds
        bool isSceneSaveGated(const SceneProject* sceneProject, const std::filesystem::path& path = {}) const;
        // The first scene to get a file becomes the start scene when none is set (or it names a scene
        // no longer listed)
        void ensureStartScene();
        // includeUntitled (File > Save All): the scenes never saved come after the modified ones,
        // each through the Save Scene dialog; the save before a quit or a project switch leaves them
        void saveAllScenes(std::function<void(bool)> callback = nullptr, bool includeUntitled = false);
        // unsaved changes, or loaded and never saved (not a Play copy or a prefab stage)
        bool sceneNeedsSave(uint32_t sceneId) const;
        void saveLastSelectedScene(std::function<void(bool)> callback = nullptr);

        uint32_t createNewScene(std::string sceneName, SceneType type);
        // False when the scene could not be read, which does not mean it is gone:
        // the file may be unreadable only in this checkout
        // allowOutside: a scene the project lists already (project.yaml, an undo) may be outside the
        // project folder (an old project's); a scene new to it may not
        bool loadScene(fs::path filepath, bool opened, bool isNewScene = true, bool loadSceneData = true, bool allowOutside = false);
        // An untitled scene from a document's text (a crash backup of one): modified, no path
        uint32_t loadUntitledScene(const std::string& text, const std::string& displayName);
        void openScene(fs::path filepath, bool closePrevious = true);
        void closeScene(uint32_t sceneId, bool systemClose = false);
        // Frees every scene, its render and the project state, as a project switch does. The
        // editor's Project lives until exit and frees none; code that drops a Project while the
        // engine runs (the CLI's tools, the unit tests) calls this first
        void releaseScenes();
        // The open scene (or prefab stage) is read again from its file, its unsaved changes
        // dropped (the hot reload's Reload, S05 R11; not an undo step). False (and `error`)
        // while a scene plays or when the file cannot be read (the open copy stays).
        bool reloadSceneFromDisk(uint32_t sceneId, std::string* error = nullptr);
        // The editor (and its MCP server) watch the open project's files; the CLI commands do
        // not (no watcher thread for a one-shot export or check). Off by default.
        void setWatchFiles(bool watch) { watchFiles = watch; }
        void removeScene(uint32_t sceneId);

        //=== Non-interactive scene and project APIs (MCP server, CLI) ===
        // They never open a dialog or an alert: what goes wrong is logged through Out and
        // returned in `error` when given. Main thread, between frames (a main-thread task);
        // they fail while a scene plays. Paths are project-relative or absolute inside the
        // project.

        // File > New Scene without the prompts: a scene named after `name` (made unique;
        // empty: saveAs's file name, else "New Scene"), 3D scenes with a sun and a sky,
        // selected and docked. saveAs saves it there at once (".scene" added when missing).
        // closePrevious closes the scene selected before, `unsaved` deciding its changes.
        // Returns its id, NULL_PROJECT_SCENE when nothing was created; a saveAs that fails
        // after creating it returns the id of the (untitled) scene with `error` set.
        uint32_t createSceneNow(const std::string& name, SceneType type, const std::filesystem::path& saveAs = {},
                                bool closePrevious = false, UnsavedChoice unsaved = UnsavedChoice::Cancel,
                                std::string* error = nullptr);
        // Opens a .scene file and selects it, adding it to the project when it is not listed
        // yet; an open one is only selected. closePrevious closes the scene selected before
        // (once the new one loaded), `unsaved` deciding its changes. Returns the scene's id,
        // NULL_PROJECT_SCENE on failure.
        uint32_t openSceneNow(const std::filesystem::path& path, bool closePrevious = false,
                              UnsavedChoice unsaved = UnsavedChoice::Cancel, std::string* error = nullptr);
        // Closes an open scene (its tab; a scene never saved leaves the project). Another
        // open scene is selected first when it is the selected one; the last open scene
        // cannot close. Save fails for a scene never saved (saveSceneNow it with a path).
        bool closeSceneNow(uint32_t sceneId, UnsavedChoice unsaved = UnsavedChoice::Cancel, std::string* error = nullptr);
        // Saves a scene now (and its modified child scenes, as Save does), synchronously.
        // pathIfUntitled is where a scene never saved goes (".scene" added when missing,
        // folders created); a saved scene stays in its own file (a different path is an
        // error: there is no Save As). A prefab stage's id saves its prefab asset.
        bool saveSceneNow(uint32_t sceneId, const std::filesystem::path& pathIfUntitled = {}, std::string* error = nullptr,
                DegradedSavePolicy policy = DegradedSavePolicy::Refuse, SaveResult* outcome = nullptr);
        // New Project, then Save Project into dir: the project `name` (empty: dir's name) in
        // dir (created when missing; one holding a project.yaml is an error), with one
        // untitled 3D scene, becomes the current project. Does not ask about the current
        // project's unsaved changes (check hasScenesUnsavedChanges() first). Switching
        // projects tears the editor's windows down: from a tool call run it through
        // EditorHost::requestProjectChange, then wait for getProjectPath() to change.
        bool createProjectAt(const std::filesystem::path& dir, const std::string& name, std::string* error = nullptr);
        // Opens the project in dir (or the one a project.yaml path names) as Open Project
        // does, without the prompts; same notes as createProjectAt
        bool openProjectNow(const std::filesystem::path& path, std::string* error = nullptr);

        std::vector<std::filesystem::path> findSceneFiles() const;
        uint32_t findSceneByPath(const std::filesystem::path& filepath) const;
        // One file per scene (docs/data-safety.md, "Scene files and identity"). The key two spellings of
        // one project file share: project-relative, lexically normal, generic, lower case on a
        // case-insensitive project volume
        static std::string sceneFileKeyOf(const std::filesystem::path& relative, bool caseInsensitive);
        // A scene whose file claims an id another (earlier) scene has gets a fresh one, written into
        // its file at once (Unity regenerates a duplicate GUID the same way)
        void giveSceneFileItsOwnId(SceneProject& sceneProject, const std::filesystem::path& fullPath);
        std::string sceneFileKey(const std::filesystem::path& path) const;
        // The scene (not `exceptSceneId`, not a Play copy) saved at `path`: by key, or the same file
        // through fs::equivalent when both exist. NULL_PROJECT_SCENE when none
        uint32_t findOtherSceneUsingFile(const std::filesystem::path& path, uint32_t exceptSceneId) const;
        // "" when `path` may become the file of `sceneId`, else why not (outside the project unless it
        // is the scene's own file, not a .scene, another scene's file, a folder)
        std::string sceneSavePathProblem(uint32_t sceneId, const std::filesystem::path& path) const;

        bool loadChildSceneInline(uint32_t childSceneId);
        void unloadChildSceneInline(uint32_t childSceneId);
        // An inline child scene is drawn as a layer (its own camera): loaded once, so its update
        // never runs without a camera (beside a running game nothing activates it)
        static void prepareInlineChildForView(Scene* scene);

        bool addChildScene(uint32_t sceneId, uint32_t childSceneId, bool startActive = true);
        void removeChildScene(uint32_t sceneId, uint32_t childSceneId);
        // Dirties parents of a child scene and forces the Engine layers to rebuild.
        void markParentScenesNeedUpdate(uint32_t childSceneId);
        bool hasChildScene(uint32_t sceneId, uint32_t childSceneId) const;
        // True when sceneId is childSceneId or one of its descendants: adding it would make a cycle
        bool wouldCreateChildSceneCycle(uint32_t sceneId, uint32_t childSceneId) const;
        bool isChildSceneStartActive(uint32_t sceneId, uint32_t childSceneId) const;
        void setChildSceneStartActive(uint32_t sceneId, uint32_t childSceneId, bool startActive);
        std::vector<uint32_t> getChildScenes(uint32_t sceneId) const;
        // The scenes that load together with `sceneId` through child-scene links: itself, its
        // ancestors (nearest first), then every descendant of those. Cross-scene GameObject
        // references are offered and accepted among them only.
        std::vector<uint32_t> getRelatedScenes(uint32_t sceneId) const;

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
        // The Play copies of sceneId's running game layers, as the Scene view draws them (in the
        // Engine's layer order: the persistent scene above the game's, the loading scene last)
        std::vector<SceneProject*> getRunningRuntimeLayerScenes(uint32_t sceneId);
        // The DontDestroyOnLoad scene of the session (Play id), NULL_PROJECT_SCENE without one
        uint32_t getPersistentPlaySceneId() const;
        bool isPersistentPlayScene(uint32_t sceneId) const;
        // SceneManager's persistent scene factory outside Play: refuses (DontDestroyOnLoad moves
        // nothing in edit mode); Play installs the session's (createPersistentRuntimeScene)
        static void refusePersistentScenes();
        // Once a frame: a Play copy no longer running, or its GameObjects destroyed, are
        // no longer selected
        void syncPlayRuntimeSelections();

        std::vector<SceneProject>& getScenes();
        const std::vector<SceneProject>& getScenes() const;
        // The scenes, then the prefab stages' scenes (Prefab Mode): for what applies to every
        // GameObject being edited (script refreshes, asset path changes, tools)
        std::vector<SceneProject*> getScenesAndPrefabStages();
        // Closes the holes destroyed entities left in the component arrays of every open scene,
        // prefab stage and loaded prefab (EntityRegistry::compactComponents): between editor
        // steps (MCP calls) and before a thread reads them
        void compactAllComponents();

        // Flushes compiled forked-shader caches and flags meshes that use a custom
        // shader for reload, so editing a shader source file updates the viewport.
        void invalidateCustomShaders();
        // Only the forks `which` names (their custom shader name): the hot reload of some shader
        // files keeps every other fork bound (S05 R11)
        void invalidateCustomShaders(const std::function<bool(const std::string& fork)>& which);

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
        // The top-level folders of a project that hold no assets (S27 R31): hidden ones ('.'),
        // build, the Recorder's Recordings and EditorSettings (swatch and curve libraries). The
        // exports, the asset scans and the generated .gitignore follow it.
        static bool isNonAssetTopLevelDir(const std::string& name);
        // a folder no scan enters: hidden or build anywhere, the top-level ones above at the
        // project's top
        bool isNonAssetDir(const std::filesystem::path& dir) const;
        bool isEditorTempProject() const;
        bool isTempUnsavedProject() const;
        std::filesystem::path getProjectPath() const;
        std::filesystem::path getProjectInternalPath() const;
        // Where imported models (FBX converted to .glb) are kept, out of the assets and version control
        std::filesystem::path getImportPath() const;

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

        // What the running scene made for itself: listed while playing, destroyed by Stop,
        // and never part of the authored entities a file keeps
        std::vector<Entity> getPlayCreatedEntities(const SceneProject* sceneProject) const;

        // Every write to a scene's selection: the GameObjects in order, the active one last.
        // A User change outside a command is an undo step ("Select Player", R15) unless the
        // timeline has a redo tail or Record Selection in Undo is off; changes made while a
        // command runs belong to that command's step.
        void setSelection(uint32_t sceneId, const SelectionState& state, SelectionCause cause = SelectionCause::User);
        SelectionState getSelection(uint32_t sceneId) const;
        // Selection changes until the end are one undo step (a box select drag); otherwise the
        // changes of one frame are (nextSelectionFrame, called once per editor frame)
        void beginSelectionGesture();
        void endSelectionGesture();
        void nextSelectionFrame();
        // Editor Settings > Undo > Record Selection in Undo
        static bool recordSelectionInUndo;

        // The Project window's selected files the Inspector shows (texture import settings),
        // project-relative; selecting a GameObject clears them
        void setInspectedAssets(std::vector<std::filesystem::path> assets);
        const std::vector<std::filesystem::path>& getInspectedAssets() const;

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
        bool hasLocalUnsavedChanges(uint32_t sceneId) const;
        bool hasSceneUnsavedChanges(uint32_t sceneId) const;
        bool hasUnsavedPrefabs(uint32_t sceneId) const;
        bool hasScenesUnsavedChanges() const;
        bool hasUnsavedPrefabs() const;

        // the declared properties of every script in the scene, from the saved files (one cache
        // for the pass when none is given)
        void updateAllScriptsProperties(uint32_t sceneId, ScriptPropertyCache* cache = nullptr);
        // Returns true when a script property definition changed. inMemoryContent: the code being
        // edited of the file at inMemoryPath. cache: shared by the calls of one pass
        bool updateScriptProperties(SceneProject* sceneProject, Entity entity, std::vector<ScriptEntry>& scripts, const std::string& inMemoryContent = "",
                                    const std::string& inMemoryPath = "", ScriptPropertyCache* cache = nullptr);
        // Where a script entry's properties come from (absolute, normalised): the Lua file, or the
        // C++ header; empty when it names none
        std::filesystem::path scriptPropertyFile(const ScriptEntry& entry) const;
        // the C++ source of an entry (the Lua file of a Lua one), absolute and normalised
        std::filesystem::path scriptSourceFile(const ScriptEntry& entry) const;
        // entry reads its properties from, or runs, the file at `absolute`
        bool scriptEntryUsesFile(const ScriptEntry& entry, const std::filesystem::path& absolute) const;

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

        //=== Asset identity (S04, editor/project/ProjectAssets.cpp) ===

        AssetIndex& getAssetIndex() { return assetIndex; }
        const AssetIndex& getAssetIndex() const { return assetIndex; }
        // S25 (editor/organisation, editor/search)
        AssetLabels& getAssetLabels() { return assetLabels; }
        const AssetLabels& getAssetLabels() const { return assetLabels; }
        Organisation& getOrganisation() { return organisation; }
        const Organisation& getOrganisation() const { return organisation; }
        // Brought up to date first (the database and the labels may have changed)
        AssetSearchIndex& getAssetSearchIndex();
        // The Missing Assets lists, brought up to date with the database and the index
        MissingAssetTracker& getMissingAssets();
        // The roots reference hints are relative to (assets and Lua directories), project-relative
        AssetIndex::Roots getAssetRoots() const;
        std::string assetHintToProjectPath(const std::string& hint, AssetPathBase base) const;
        std::string projectPathToAssetHint(const std::string& projectRelative, AssetPathBase base) const;
        // A reference to the asset at a path (relative to base), with its GUID
        AssetRef makeAssetRef(const std::string& hint, AssetPathBase base) const;
        // Resolves every reference of a loaded scene, prefab stage or registry (S04 R10).
        // loading: references read from a file (a reference without a GUID is a legacy one);
        // otherwise references edited in memory (a path without a GUID is the file at the
        // path, a path-fallback binding takes the file's GUID, as it is about to be saved).
        // True when a path hint changed (the owner is then modified).
        bool resolveSceneAssetRefs(SceneProject& sceneProject, bool loading);
        // Before a file moves or goes: the references of the loaded documents that name an
        // asset only by path (assigned since the last save) get its GUID, so they follow it
        void bindLoadedAssetRefs();
        bool resolveRegistryAssetRefs(EntityRegistry* registry, bool loading);
        // A material about to be written to a .material file: its references that name an
        // asset only by path (a tool's or a paste's new values) get the asset's GUID, so the
        // file follows the asset when it moves (R13 rewrites no referrer)
        void bindMaterialAssetRefs(Material& material);
        // The identity keys (guid, formerGuids) a scene, prefab or material file is written
        // with, put at the top of its root; returns the GUID (a new one for a new file)
        Guid prepareDocumentIdentity(YAML::Node& root, const std::filesystem::path& relativePath);
        // Writes a .material file with its identity (and registers it), through S03's material
        // save: a file that did not read completely is refused (RefusedDegraded, why in error)
        // unless the policy accepts the loss
        SaveResult writeMaterialFile(const std::filesystem::path& relativePath, const Material& material,
                                     DegradedSavePolicy policy = DegradedSavePolicy::Refuse, std::string* error = nullptr);
        // The edges of the loaded documents, unsaved edits included (the index overlay)
        void collectMemoryEdges(std::unordered_map<Guid, std::vector<DepEdge>>& edges);
        // After assets moved: the path hints of the loaded documents follow their GUIDs
        void refreshAssetHints();
        // What a file operation (AssetOps) or the refresh found changed reaches the editor:
        // prefabs in memory, the dependency index, the loaded documents' hints
        void onAssetFilesChanged(const std::vector<AssetDatabase::Change>& changes);
        // A scene file or a directory holding some moved: the scene list and the open tabs
        // follow (project.yaml is written); false when no scene was there
        bool moveSceneFileHints(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);

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
        // Assets moved, copied, deleted or edited outside the editor (S04 refresh: moves keep
        // their GUIDs and record redirects, orphan metas are adopted). Called every frame: it
        // applies the walk the file watcher's last changes started (applyFileChanges), and
        // starts none of its own (no poll). `force` (Assets > Refresh, a tool answering from
        // the database) looks at the disk now. Does nothing while a scene plays.
        void refreshAssetDatabase(bool force = false);
        // Every editor loop iteration: starts or applies the walk the watcher asked for
        void updateAssetScan() { refreshAssetDatabase(false); }
        // Files added, removed, renamed or edited on disk by another program (the file
        // watcher, S05 R14): the database looks at the disk again, on a Background job. False
        // while a scene plays (nothing applied: refreshAssetDatabase after Play).
        bool applyFileChanges(std::span<const FileChange> changes);
        // A prefab file or a directory holding some appeared (a copy, a deleted file put back
        // by an undo): they are registered, and Missing Prefab instances they resolve relinked
        void importPrefabFiles(const std::filesystem::path& path, bool copy = false, std::map<std::string, std::string>* copyGuids = nullptr);

        // ---- Animation clip references (animation clips spec 4.1, 9.R8/R26; ProjectPrefabPaths.cpp) ----
        // Where an Animation Player entry's .anim points now: by GUID (a GUID no file has keeps
        // its path: the Inspector shows it Missing, and undoing the delete relinks it), else by
        // its path (a file not in the database yet is added when `mayImport`). The path hint is
        // repaired (asset-relative) and a missing GUID filled in. Sub-clip entries
        // ("<model>#<clip>") are path-only. True when the entry changed.
        bool resolveClipReference(AnimationPlayerClip& entry, bool mayImport = true);
        // The same for any GUID-tracked asset reference held as (path hint, GUID): a Vertex
        // Animator's .vat. True when the hint or the GUID changed
        bool resolveAssetReference(std::string& path, std::string& guid, bool mayImport = true);
        // Every Animation Player entry and Vertex Animator animation of a registry (a scene, a
        // prefab asset, a play copy); true when one changed (they reload). `mayImport` false
        // reads the database only (the play startup worker)
        bool resolveRegistryClipRefs(EntityRegistry* registry, bool mayImport = true);
        // A file or directory moved through AssetOps (renamed, moved, a move undone): the open
        // clip documents (a clip's, a model's sub-clips), AnimationClipPool and the players learn
        // both paths. The database moved the clips' GUIDs and applyClipAssetChanges relinked the
        // references by them already.
        void remapClipFilePath(const std::filesystem::path& oldPath, const std::filesystem::path& newPath);
        // Every loaded prefab asset (clip users and reference walks)
        void forEachPrefabAsset(const std::function<void(const std::filesystem::path& path, Prefab& prefab)>& visit);

        // Missing Prefab instances (SceneProject::missingPrefabs)
        const MissingPrefabInstance* getMissingPrefab(uint32_t sceneId, Entity entity) const;
        bool isMissingPrefab(uint32_t sceneId, Entity entity) const;
        void setMissingPrefab(SceneProject* sceneProject, Entity root, const YAML::Node& data, const PrefabMemberIds* memberIds = nullptr);
        void clearMissingPrefab(uint32_t sceneId, Entity root);
        // Rebuilds the Missing Prefab instances whose asset resolves again; returns how many
        size_t relinkMissingPrefabs();

        // ---- Replace Prefab, previews, usage (ProjectPrefabReplace.cpp, PREFABS.md 12-14) ----
        // An instance root Replace Prefab can swap: an outermost or added instance root, or a
        // Missing Prefab root not nested in a live instance (a nested one belongs to its
        // prefab), while nothing plays
        bool canReplacePrefab(uint32_t sceneId, Entity entity) const;
        // The root node (Stream::encodeEntity) and member ids an instance root is rebuilt from
        bool capturePrefabInstanceState(uint32_t sceneId, Entity root, YAML::Node& node, PrefabMemberIds& memberIds);
        // The node making the captured instance an instance of newPath: its PrefabComponent
        // swapped, its overrides, removed and added GameObjects and member ids carried over to
        // the new prefab's GameObjects by settings.match, the rest dropped (in the report)
        bool buildReplacedPrefabNode(uint32_t sceneId, Entity root, const YAML::Node& node, const PrefabMemberIds& memberIds,
            const std::filesystem::path& newPath, const PrefabReplaceSettings& settings,
            YAML::Node& outNode, PrefabMemberIds& outMemberIds, PrefabReplaceReport* report = nullptr);
        // Makes the root (a live or Missing instance root) what the node says: its name, state
        // and components, then the instance of the node's prefab with the node's data, the
        // members taking memberIds (ReplacePrefabCmd, both ways)
        bool rebuildPrefabInstance(uint32_t sceneId, Entity root, const YAML::Node& node, const PrefabMemberIds& memberIds);
        // An instance of the prefab built in `scene`, a scene of its own outside the project
        // (thumbnails): returns its root, NULL_ENTITY when it cannot be made. Nothing of it is
        // tracked afterwards.
        Entity instantiatePrefabPreview(Scene* scene, const std::filesystem::path& filepath);
        // The prefab files an instance of the prefab depends on (itself, the nested ones)
        std::vector<std::filesystem::path> getPrefabDependencies(const std::filesystem::path& filepath);
        // Usage of every prefab, by GUID (by "path:<path>" for one without): loaded scenes
        // from their instances, closed ones from their file, and the prefab files nesting it
        std::map<std::string, PrefabUsage> collectPrefabUsage();
        // A raycast in the Scene view of sceneId leaving some GameObjects out (a dragged preview)
        Entity findObjectByRayIgnoring(uint32_t sceneId, float x, float y, const std::unordered_set<Entity>& ignored);
        // The scene's prefab list (SceneProject::prefabs) made again from its instances and
        // Missing Prefabs, after a change made outside the import paths
        void refreshScenePrefabList(uint32_t sceneId);

        // guid: the asset GUID to give the new file (a redo), empty for a new one
        bool createPrefab(uint32_t sceneId, fs::path filepath, YAML::Node entityNode, const std::string& guid = std::string());
        bool removePrefab(const std::filesystem::path& filepath);

        const std::vector<std::filesystem::path>& getStandalonePrefabs() const;
        void setStandalonePrefabs(std::vector<std::filesystem::path> prefabPaths);
        bool isStandalonePrefab(const std::filesystem::path& filepath) const;
        std::vector<std::filesystem::path> findPrefabFiles() const;

        // Refused (RefusedDegraded, nothing written) while the prefab's load report has dropped
        // issues, unless policy is SaveAnyway
        SaveResult savePrefabToDisk(const std::filesystem::path& filepath, DegradedSavePolicy policy = DegradedSavePolicy::Refuse);
        // While an API call (an MCP tool) edits prefab assets: the policy of the saves it
        // causes, and the assets the gate refused to write (endPrefabSaveScope returns them)
        void beginPrefabSaveScope(DegradedSavePolicy policy);
        std::vector<std::filesystem::path> endPrefabSaveScope();

        Prefab* getPrefab(const std::filesystem::path& filepath);
        const Prefab* getPrefab(const std::filesystem::path& filepath) const;
        std::map<std::filesystem::path, const Prefab*> getPrefabs(uint32_t sceneId) const;
        std::filesystem::path findPrefabPathFor(uint32_t sceneId, Entity entity) const;
        // Bulk lookup for UI passes that inspect every entity in a scene.
        std::unordered_map<Entity, std::filesystem::path> getPrefabPathsForScene(uint32_t sceneId) const;

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
        // The path of the prefab asset a path names: one file has one asset, whichever link or hard
        // link it is reached by (the path as it is when no asset is known at it)
        std::filesystem::path getPrefabAssetPath(const std::filesystem::path& filepath) const;
        // reconcileTracked false leaves dropping the destroyed entities from the scene's
        // tracked list to the caller (one pass after unimporting many instances); keepRoot
        // leaves the root entity in the scene (a Missing Prefab conversion)
        bool unimportPrefab(uint32_t sceneId, const std::filesystem::path& filepath, Entity rootEntity, const std::vector<Entity>& memberEntities,
                            bool reconcileTracked = true, bool keepRoot = false);

        // target: the prefab to add it to, the one whose GameObject `parent` is when empty; a
        // prefab holding that instance (one of getPrefabApplyTargets(parent)) gets it as a
        // GameObject it adds to that nested instance (PREFABS.md 17)
        bool addEntityToPrefab(uint32_t sceneId, Entity entity, Entity parent, bool createItself = true, const std::filesystem::path& target = {});
        bool addEntityToOuterPrefab(uint32_t sceneId, Entity entity, Entity parent, const std::filesystem::path& target);
        bool addEntityToPrefab(uint32_t sceneId, const NodeRecovery& recoveryData, Entity parent,
            bool createItself = true, std::unordered_map<Entity, Entity>* entityRemap = nullptr,
            std::vector<Entity>* restoredEntities = nullptr);
        NodeRecovery removeEntityFromPrefab(uint32_t sceneId, Entity entity, bool destroyItself = true);

        bool isEntityInPrefab(uint32_t sceneId, Entity entity) const;

        // ---- Unity's instance edits (editor/project/PREFABS.md, "Editing instances") ----
        // An edit on a GameObject of a prefab instance (its root included) is an override of
        // that instance only; the prefab asset changes through Apply or Prefab Mode. The
        // record* calls below come after the edit (the GameObject has the new value), and
        // return false when it is not an instance's GameObject or the edit is no override
        // (an instance root's default overrides, a component the instance added, Prefab Mode).
        // Commands keep the overrides from before the edit for their undo
        // (getPrefabEditOverrides / setPrefabEditOverrides, command/PrefabEditUndo.h).

        // True when `entity` is a GameObject of a prefab asset being
        // edited in Prefab Mode, whose edits are asset edits, not instance overrides
        bool isEditingPrefabAsset(uint32_t sceneId, Entity entity) const;
        // Property edits ({} or "*": the whole component)
        bool recordPrefabEdit(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties);
        // A component rewritten whole (reset, pasted values): the properties that now differ
        // from `before` (Stream components YAML holding that component) are recorded
        bool recordPrefabComponentEdit(uint32_t sceneId, Entity entity, ComponentType type, const YAML::Node& before);
        // Name, active, tag or layer (PrefabObjectOverrides::State bits)
        bool recordPrefabStateEdit(uint32_t sceneId, Entity entity, uint8_t stateBits);
        // A component added to / removed from the GameObject (already done in the scene):
        // an added component override, or a removed one (removing an added one only drops it)
        bool recordPrefabComponentAdded(uint32_t sceneId, Entity entity, ComponentType type);
        bool recordPrefabComponentRemoved(uint32_t sceneId, Entity entity, ComponentType type);
        // The overrides of a GameObject at its owning level, for undo; false when it is not a
        // GameObject of an instance
        bool getPrefabEditOverrides(uint32_t sceneId, Entity entity, Prefab::ObjectOverrides& overrides) const;
        void setPrefabEditOverrides(uint32_t sceneId, Entity entity, const Prefab::ObjectOverrides& overrides);
        // Apply writes prefab assets, which Play mode must leave alone (Unity's Overrides
        // are not applied while playing either)
        bool canApplyPrefabOverrides(std::string* reason = nullptr) const;
        // Whether an override (a component's properties, "*"/{} the whole; ComponentType
        // PrefabComponent with stateBits for state) can go to `target` (empty: the owning
        // prefab), with why not
        bool canApplyPrefabOverride(uint32_t sceneId, Entity entity, ComponentType type, const std::vector<std::string>& properties,
            const std::filesystem::path& target = {}, std::string* reason = nullptr) const;
        // Every override of the instance rooted at instanceRoot and of the instances nested in
        // it, GameObject by GameObject in hierarchy order (the Overrides dropdown)
        std::vector<PrefabOverrideEntry> getPrefabOverrideEntries(uint32_t sceneId, Entity instanceRoot) const;
        // Play mode: the instances' bookkeeping (overrides, members, removals, Missing Prefab
        // records) of a scene played in place, captured with the play snapshot and put back
        // at Stop with it, so nothing done while playing outlives Play
        PlayPrefabState capturePlayPrefabState(const SceneProject* sceneProject) const;
        void restorePlayPrefabState(SceneProject* sceneProject);
        // Stop's restore of a scene played in place: its snapshot (entities, components,
        // prefab bookkeeping) back, what Play created destroyed; false without a snapshot
        bool restorePlaySnapshot(SceneProject* sceneProject);

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
        // The GameObjects added below the Prefab GameObjects of that instance and the ones
        // nested in it (top ones only, with their parent), in hierarchy order
        std::vector<std::pair<Entity, Entity>> getPrefabAddedGameObjects(uint32_t sceneId, Entity instanceRoot) const;
        // Revert: the removed GameObject comes back with the Prefab's current values, in its
        // Prefab place, taking back the ids it had where they are free
        bool revertPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity, PrefabRemovalRevert& revert);
        void undoRevertPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity, const PrefabRemovalRevert& revert);
        // Apply: the GameObject is deleted from the Prefab asset, so from all its instances
        bool applyPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity, PrefabRemovalApply& apply);
        // Apply as Override in `target` (a prefab holding that instance: a variant of its
        // prefab, or one it is nested in, see getPrefabApplyTargets): the GameObject stays out
        // of the instances through that prefab's data (undo: PrefabUndoState)
        bool applyPrefabRemovalTo(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity,
            const std::filesystem::path& target);
        void undoApplyPrefabRemoval(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot, Entity registryEntity, PrefabRemovalApply& apply);

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
        bool isComponentOverridden(uint32_t sceneId, Entity entity, ComponentType type) const;
        // The overridden property names of a component ("*" for the whole of it)
        std::set<std::string> getOverriddenProperties(uint32_t sceneId, Entity entity, ComponentType type) const;
        Prefab::ObjectOverrides getPrefabObjectOverrides(uint32_t sceneId, Entity entity) const;
        // The value the GameObject's property (or whole component) would have without its
        // owning instance's own override: the prefab's, with what the prefabs holding the
        // instance override of it. False when the prefab GameObject has no such component.
        bool getPrefabSourceValue(uint32_t sceneId, Entity entity, ComponentType type, const std::string& property, PrefabSourceValue& value);
        YAML::Node getPrefabSourceComponent(uint32_t sceneId, Entity entity, ComponentType type);
        // The same for the GameObject's name, active state, tag and layer
        bool getPrefabSourceState(uint32_t sceneId, Entity entity, std::string& name, bool& active, std::string& tag, int& layer);
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
        std::vector<PrefabInstanceData*> getPrefabAssetLayers(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& level = {});
        // The prefabs a GameObject roots instances of, outermost first (a variant instance's
        // root: the variant, its base, ...; empty when it roots none)
        std::vector<std::filesystem::path> getInstanceRootLevels(uint32_t sceneId, Entity entity) const;
        // The value of a component of the root GameObject of `path` as an instance rooted at
        // localRoot has it from its prefab: a variant's from its base (down the chain) with
        // what it overrides of it, references in local ids
        bool buildRootSource(SceneProject* sceneProject, Entity localRoot, const std::filesystem::path& path, ComponentType type,
            prefabinternal::ScratchEntity& out, size_t depth = 0);
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
        // The same for its state (active, tag, layer) and a component removed from it
        void recordNestedRootAssetState(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryEntity, uint8_t stateBits);
        void recordNestedRootAssetRemoval(Prefab& prefab, const std::filesystem::path& prefabPath, Entity registryEntity, ComponentType type);
        // Whether the root of `path` has the component as its instances have it (a variant's:
        // its base's root with what the variant adds and removes)
        bool buildRootHas(const std::filesystem::path& path, ComponentType type, size_t depth = 0);
        // An override applied to `target`: what the prefabs between it and the instance (the
        // later apply targets: variants over it, prefabs holding it) override of the same
        // property (or state bits) goes from them, so the applied value is what shows (Unity)
        void clearIntermediateOverrides(uint32_t sceneId, const PrefabObjectRef& ref, const std::filesystem::path& target,
            ComponentType type, const std::vector<std::string>& properties, uint8_t stateBits);
        // Apply to the prefab of a nested instance root (`target`, which the owning level, the
        // instance holding that root, sees only as its asset): the values go onto that
        // prefab's root GameObject
        bool isNestedRootApplyTarget(uint32_t sceneId, const PrefabObjectRef& ref, const std::filesystem::path& target) const;
        // Prefab instances (local root, registry entity in `prefab`) that became nested ones
        // of `prefab`, whose instance is rooted at holderRoot: the GameObjects added to them
        // in the scene (at any depth) become the prefab's (added GameObjects of those nested
        // instances, PREFABS.md 17), what their scene data overrides becomes the prefab's
        // asset-level data for them (references in the prefab's ids; what refers outside
        // stays overridden in the scene), out of the scene's
        void moveNestedInstancesToAsset(Prefab& prefab, const std::filesystem::path& prefabPath, SceneProject* sceneProject, Entity holderRoot,
            const std::vector<Prefab::EntityMember>& nestedRoots);
        // Components of the instance's GameObjects referring outside it (and the instances
        // nested in it) become whole-component overrides: the prefab cannot hold them
        void overrideOutsideReferences(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& level = {},
            const std::unordered_set<Entity>* only = nullptr);

        // ---- Nested prefabs in assets, stripped objects (editor/project/PREFABS.md 17) ----

        // A reference held by the prefab of the instance rooted at instanceRoot to the scene
        // GameObject localTarget, in that prefab's ids: the registry entity of a GameObject of
        // the instance (its root included), the stripped id (made when the prefab has none
        // yet: the prefab is then modified) of a GameObject of an instance nested in it at
        // any depth, NULL_ENTITY for anything else (outside: a prefab cannot hold it)
        // (`level`: which instance at that root, for the levels of a variant instance, PREFABS.md
        // 18; empty: the outermost, the one the root's PrefabComponent names)
        Entity makePrefabReference(uint32_t sceneId, Entity instanceRoot, Entity localTarget, const std::filesystem::path& level = {});
        // The references of a component of `registry` (the scene, a prefab registry holding a
        // copy, or a scratch one), local ids of the scene, made the ids of the prefab of the
        // instance rooted at instanceRoot (makePrefabReference), in place. `properties`
        // limits it (empty: all). False when one pointed outside the instance (it is None
        // now). Unchanged ids where `registry` already holds the prefab's are not expected.
        bool remapReferencesToPrefab(uint32_t sceneId, Entity instanceRoot, EntityRegistry* registry, Entity entity, ComponentType type,
            const std::vector<std::string>& properties = {}, const std::filesystem::path& level = {});
        // A component of a scene GameObject as the prefab of the instance rooted at
        // instanceRoot stores it (references in its ids); `outside` reports a reference it
        // could not hold (None in the result)
        YAML::Node encodeComponentForPrefab(uint32_t sceneId, Entity instanceRoot, Entity entity, ComponentType type, bool* outside = nullptr,
            const std::filesystem::path& level = {});
        // Whether a component of a scene GameObject refers to something outside the instance
        // rooted at instanceRoot and the instances nested in it (what no prefab can hold)
        bool componentReferencesOutside(uint32_t sceneId, Entity instanceRoot, Entity entity, ComponentType type,
            const std::filesystem::path& level = {});
        // The root of the instance of `target` holding (at any depth) the instance rooted at
        // instanceRoot (itself when it is one of `target`), NULL_ENTITY when none. The
        // variants an instance is the base of hold it too, at the same root.
        Entity findHolderInstanceRoot(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& target,
            const std::filesystem::path& level = {});
        // Registry entity (and stripped id) -> local entity of the instance rooted at
        // instanceRoot (of `level`, the outermost when empty: see findInstanceAt)
        std::unordered_map<Entity, Entity> getPrefabReferenceMap(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& level = {});
        // The instance whose root is `root` (of the prefab its PrefabComponent names: for an
        // instance of a variant the outermost level, see findInstanceAt)
        Prefab::Instance* findInstanceRootedAt(SceneProject* sceneProject, Entity root, std::filesystem::path* path = nullptr);

        // ---- Prefab Variants (editor/project/ProjectPrefabVariant.cpp, PREFABS.md 19) ----

        // The instance of `path` rooted at `root` (any level of a variant instance's root);
        // the outermost (findInstanceRootedAt) for an empty path
        Prefab::Instance* findInstanceAt(SceneProject* sceneProject, Entity root, const std::filesystem::path& path,
            std::filesystem::path* resolved = nullptr);
        const Prefab::Instance* findInstanceAt(const SceneProject* sceneProject, Entity root, const std::filesystem::path& path,
            std::filesystem::path* resolved = nullptr) const;
        // Every level rooted at `root`, outermost first (one for an original prefab)
        std::vector<PrefabInstanceLevel> getInstanceLevels(SceneProject* sceneProject, Entity root);
        // The instances nested in an instance of `path`: its variant base level first, then
        // its members that are nested roots (a Missing one without instance)
        std::vector<NestedInstanceLevel> getNestedInstanceLevels(SceneProject* sceneProject, const std::filesystem::path& path,
            const Prefab::Instance& instance);
        std::vector<NestedInstanceLevel> getNestedInstanceLevels(const SceneProject* sceneProject, const std::filesystem::path& path,
            const Prefab::Instance& instance) const;
        // The next level outwards of the instance of `path` rooted at `root`; false for an
        // outermost instance
        bool findPrefabHolder(SceneProject* sceneProject, Entity root, const std::filesystem::path& path, PrefabHolderLink& link);
        // The prefab the instance rooted at a registry entity of `holder` instantiates (a
        // nested root, or the root of a variant: its base), by its PrefabComponent
        std::filesystem::path instancePathAt(const Prefab& holder, Entity registryEntity) const;
        // The base of a variant (resolved, read when not loaded), empty for an original
        // prefab or a base that cannot be found
        std::filesystem::path getVariantBasePath(const Prefab& prefab);
        std::filesystem::path getPrefabBasePath(const std::filesystem::path& filepath);
        bool isPrefabVariant(const std::filesystem::path& filepath);
        // The variant and its bases, the variant first; stops at a base that cannot be found
        // or would make a cycle (cycle set then)
        std::vector<std::filesystem::path> getPrefabVariantChain(const std::filesystem::path& filepath, bool* broken = nullptr);
        // Cheap and cached for the UI (Project window, Hierarchy rows, Prefabs dialog): the
        // base's path of a .prefab file as its file says (empty for an original), read again
        // when the file changes; no asset is loaded
        std::filesystem::path getPrefabFileBase(const std::filesystem::path& filepath);
        // The prefab files whose base is `filepath` (direct variants), from the files
        std::vector<std::filesystem::path> findPrefabVariantsOf(const std::filesystem::path& filepath);
        // The key PrefabMemberIds uses for the base levels of a variant root: depth 0 is the
        // root itself, depth n its n-th base
        static Entity prefabLevelKey(Entity root, size_t depth);
        // Unity's Create Prefab Variant: the instance rooted at `root` (an outermost instance
        // root, or an added one) becomes an instance of a new variant of its prefab saved at
        // `filepath`; its overrides, the GameObjects added to it and the references into it
        // become the variant's (as Create Prefab does for nested instances). guid: a redo's.
        bool createPrefabVariant(uint32_t sceneId, const std::filesystem::path& filepath, Entity root, const std::string& guid = std::string());
        // Undo of createPrefabVariant: the instance is one of its base again, with what it had
        bool removePrefabVariantInstance(uint32_t sceneId, const std::filesystem::path& filepath, Entity root, const YAML::Node& rootNode);
        // Project window, Create > Prefab Variant: a new variant asset of `base` (no instance)
        bool createPrefabVariantAsset(const std::filesystem::path& base, const std::filesystem::path& filepath, const std::string& guid = std::string());
        // Whether an instance root can be made a variant (an outermost or added live instance root)
        bool canCreatePrefabVariant(uint32_t sceneId, Entity root, std::string* reason = nullptr) const;
        // The scene GameObject a stripped object's way down names, from the holding
        // instance's registry-to-local map (holder: the prefab holding the stripped object,
        // whose registry says which prefab each step is in)
        Entity resolveStrippedObject(SceneProject* sceneProject, const std::unordered_map<Entity, Entity>& registryToLocal, const std::vector<Entity>& source,
            const Prefab* holder = nullptr);
        void addStrippedToMap(SceneProject* sceneProject, const Prefab& prefab, std::unordered_map<Entity, Entity>& registryToLocal);
        // The local root of the instance nested in `instance` a nested data entry is for
        Entity findNestedLocalRoot(SceneProject* sceneProject, const Prefab& prefab, const Prefab::Instance& instance, const PrefabInstanceData& entry);
        // Import internals: a holding prefab's data for the instance rooted at nestedRoot
        // (values in the holder's ids, made local by holderMap; the root's from the holder's
        // registry entity holderEntity when given), and the scene's data and added
        // GameObjects for an instance tree
        // (level: the prefab of the instance at that root, for a variant's base levels)
        void applyHolderData(SceneProject* sceneProject, Entity nestedRoot, const PrefabInstanceData& data, const std::unordered_map<Entity, Entity>& holderMap,
            const EntityRegistry* holderRegistry, Entity holderEntity, bool skipRoot, size_t depth = 0, const std::filesystem::path& level = {});
        void applySceneData(SceneProject* sceneProject, Entity instanceRoot, const PrefabInstanceData& data, size_t depth = 0,
            const std::filesystem::path& level = {});
        void decodeSceneAddedGameObjects(SceneProject* sceneProject, std::vector<Entity>* entities, Entity instanceRoot, const PrefabInstanceData& data,
            std::unordered_map<Entity, Entity>* entityRemap, std::vector<Entity>& allResult, size_t depth = 0, const std::filesystem::path& level = {});
        // What the prefabs holding the instance rooted at instanceRoot (outwards) override of
        // some of its GameObjects (registry entities) and of some instances nested in it
        void applyLayersAbove(SceneProject* sceneProject, Entity instanceRoot, const std::unordered_set<Entity>& registryEntities,
            const std::vector<Prefab::EntityMember>& nestedRoots, const std::filesystem::path& level = {});
        // Unpack of the instance rooted at instanceRoot (of `path`): what its prefab (and the
        // prefabs it nests, down the chain) override of the instances nested in it becomes
        // their own (scene) overrides and removed GameObjects, as Unity does; the GameObjects
        // it added to them become theirs by themselves. `before` gets their previous state
        // (repack puts it back); instances rooted in `skip` (unpacked too) are left alone.
        // includeBase: an instance of a variant unpacked one level becomes an instance of its
        // base, which takes what the variant overrides of it as its own (PREFABS.md 19)
        void promoteNestedAssetData(uint32_t sceneId, const std::filesystem::path& path, Entity instanceRoot,
            std::vector<PrefabNestedState>& before, const std::unordered_set<Entity>& skip, bool includeBase = false);
        // Prefab Mode (PrefabStage): the asset's members as a stage reads them (the GameObjects
        // the prefab adds to its nested instances become those instances' added GameObjects);
        // once decoded, the stage's references to stripped objects made the nested GameObjects'
        void prefabNodeToStage(const Prefab& prefab, YAML::Node members);
        void resolveStageStrippedReferences(SceneProject* stage, const Prefab& prefab);
        // A stage saved: `prefab` was just read from the stage's encoding; the GameObjects
        // added to its nested instances become the prefab's, references its ids (stripped
        // objects into the nested instances, the ones it had kept), anything else None
        void stageToPrefabAsset(SceneProject* stage, Prefab& prefab, const std::map<Entity, std::vector<Entity>>& previousStripped);
        // Moves a GameObject at its place among its parent's children (-1: last)
        static void placeAtChildIndex(Scene* scene, Entity parent, Entity local, int childIndex);
        // The scene GameObjects of the instance rooted at instanceRoot and of every instance
        // nested in it, at any depth (roots and members)
        std::unordered_set<Entity> collectInstanceTreeObjects(uint32_t sceneId, Entity instanceRoot, const std::filesystem::path& level = {});
        // Prefab assets baked for the export: each prefab instantiated in a scratch scene
        // (every layer applied, like the editor's instances), its GameObjects in
        // build_prefab_* order (Factory::BakedPrefab)
        std::map<std::filesystem::path, std::shared_ptr<BakedPrefab>> bakePrefabsForExport();

        //=== Prefab Mode (editor/PrefabStage.cpp, PREFABS.md section 16) ===

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
        // A stage whose asset did not load completely asks the host first (Refuse when headless)
        bool savePrefabStage(uint32_t sceneId, DegradedSavePolicy policy = DegradedSavePolicy::Refuse);
        // The stage's asset has dropped load issues: its saves are gated, Auto Save is paused
        bool isPrefabStageDegraded(uint32_t sceneId) const;
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
        // Whether a GameObject (with its branch) may go into the prefab at target (add_to_prefab,
        // an added GameObject applied): not when an instance in the branch is of target or of a
        // prefab holding it, at any depth (the prefab would contain itself)
        bool canAddToPrefab(uint32_t sceneId, Entity entity, const std::filesystem::path& target, std::string* reason = nullptr);
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

        // ---- C++ scripts reloaded during Play (S23 R10-R13; ScriptReloadService drives it)
        // Play loaded a C++ script library
        bool hasCppPlayLibrary() const { return conector.isLibraryConnected(); }
        // Starts the rebuild of the scripts (the old library keeps running); false and why
        // when it cannot (no library, the generation cap, a build already running)
        bool startCppReload(const std::vector<std::filesystem::path>& changed, std::chrono::steady_clock::time_point firstSeen, std::string& error);
        // Every frame while it builds, at the frame boundary: the new library is checked,
        // loaded next to the old one and swapped in once the build ends
        void updateCppReload();
        // Cancel Build: the reload's build stops (the old library keeps running)
        void cancelCppReload() { if (cppReload.running) generator.cancelBuild(); }

        bool isAnyScenePlaying() const;
        bool isPlaySessionActive() const;
        // The Scenes the active Play session runs (its main scene and the copies of the scenes
        // it plays with), once started; empty without one. Main thread.
        std::vector<Scene*> getPlayingScenes() const;
        // True only while the active play session's main scene is actually running
        // (PLAYING) — false when paused, loading, or no session is active.
        bool isMainScenePlaying() const;
        bool isAnySceneSaving() const;
        // Play mode is starting (its scene LOADING): the startup compiles shaders and C++
        // scripts from the scenes as they were when it read them
        bool isPlayStarting() const;

        // A closed scene's C++ scripts (Play builds them: SceneManager can load the scene while
        // playing), read from its file once per version of it (its write time): a scene without
        // any, as in a Lua-only project, would otherwise be decoded again on every Play. True if
        // it read the file now.
        bool collectClosedSceneCppScripts(SceneProject& sceneProject);
        // The C++ script list the scene file's header has (what it had when saved), as a closed
        // scene gets at project load; forgets collectClosedSceneCppScripts' read of it
        void readSceneFileCppScripts(SceneProject& sceneProject);
        // How many times that read a scene file (tests)
        size_t getClosedSceneScriptReads() const { return closedSceneScriptReads; }

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
        // the Camera Shaker Inspector's edit-mode shake preview (CameraControlSystem::stopPreview) of every
        // stopped scene: Play, a save
        void stopCameraShakePreviews();
        void waitForPlaySessionToFinish();

        std::vector<Scene*> getRunningRuntimeLayers(uint32_t sceneId);
        // A scene load replaced the played scene itself (another scene, or a fresh copy of it):
        // the game shows the stack that load built, not the played scene
        bool isPlayedSceneReplaced(uint32_t sceneId);
        // The running scene a game load put in place of the played scene `sceneId` (the one the
        // Game view shows), null when the played scene still runs: the Scene view draws it
        SceneProject* getReplacingMainScene(uint32_t sceneId);
    };

}
