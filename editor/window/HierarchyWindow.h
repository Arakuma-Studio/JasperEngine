// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "SceneWindow.h"
#include "command/type/CreateEntityCmd.h"
#include "imgui.h"
#include "util/EntityPayload.h"
#include <list>
#include <string>
#include <vector>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace jasperengine::editor{

    struct TreeNode {
        std::string icon;
        std::string name;
        std::filesystem::path prefabFilepath;
        std::filesystem::path nestedPrefabFilepath;
        bool isScene = false;
        bool isChildScene = false;
        bool isPrefab = false;
        bool isPrefabRoot = false;
        bool isMissingPrefab = false;       // Unity's Missing Prefab: its asset is gone (red)
        // Prefab row states (HierarchyPrefabRows.cpp, markPrefabRows)
        bool isAddedGameObject = false;     // Unity's added GameObject override ("+" badge)
        bool isInMissingPrefab = false;     // below a Missing Prefab root (added to it)
        bool isPrefabVariant = false;       // instance root of a Prefab Variant ([E] hook)
        bool canOpenPrefab = false;         // an instance root: the ">" button opens its prefab
        bool hasPrefabParent = false;
        bool isParentPrefab = false;
        bool isMainCamera = false;
        bool hasScript = false;
        bool hasChildScript = false;        // Prefab roots only
        bool isBone = false;
        bool isLocked = false;
        bool isPlayCreated = false;         // Made by the running scene, gone again on Stop
        bool canEditModelHierarchy = false;
        std::string hierarchyReason;        // Why a model node or part stays put, shown in its tooltip
        bool separator = false;
        bool hasTransform = false;
        bool matchesSearch = false;         // Node matches search term
        bool hasMatchingDescendant = false; // Has a descendant that matches search
        uint32_t id = 0;
        size_t order = 0;
        uint32_t parent = NULL_ENTITY;
        uint32_t childSceneId = 0;          // Scene ID for child scene nodes
        uint32_t ownerSceneId = 0;          // Parent/owner scene ID (for child scene nodes)
        uint32_t entitySceneId = 0;         // Scene ID that owns this entity (for child scene entity nodes)
        uint32_t runtimeSceneId = 0;        // Child scene nodes while the game runs: the running copy listed
        bool isRuntimeLayer = false;        // A running scene the game layered that is no child scene of this one
        bool isNotRunning = false;          // A child scene the running game does not show
        bool isPersistent = false;          // The DontDestroyOnLoad scene while the game runs (last row)
        std::list<TreeNode> children;
    };

    // Scene types a create entry is offered in (bit mask)
    enum CreateSceneMask : uint8_t {
        CREATE_IN_3D = 1 << 0,
        CREATE_IN_2D = 1 << 1,
        CREATE_IN_UI = 1 << 2,
        CREATE_IN_WORLD = CREATE_IN_3D | CREATE_IN_2D,
        CREATE_IN_ALL = CREATE_IN_3D | CREATE_IN_2D | CREATE_IN_UI
    };

    // One row of the GameObject create menu. The main menu bar's GameObject menu,
    // the Hierarchy "+" dropdown and the Hierarchy right-click Create items are all
    // built from this single table, so they cannot drift apart.
    struct CreateMenuEntry {
        const char* submenu;            // nullptr = top level, otherwise "3D Object", "Light", ...
        const char* label;
        const char* icon;
        EntityCreationType type;
        const char* entityName;         // Name given to the new GameObject
        uint8_t scenes;                 // CreateSceneMask
        bool separatorBefore = false;   // Within its menu level
        bool rootOnly = false;          // Only offered when creating at the scene root
        const char* command = nullptr;  // its command catalog id when it has its own (a shortcut)
    };

    struct EntitySelectionEntry {
        uint32_t sceneId = NULL_PROJECT_SCENE;
        Entity entity = NULL_ENTITY;
    };

    // tests (PlayStartupScenesTest.cpp): the tree the panel draws
    struct HierarchyTestAccess;

    class HierarchyWindow{
    private:

        Project* project;
        SceneWindow* sceneWindow;

        bool matchCase = false;
        char searchBuffer[256] = "";

        std::vector<uint32_t> selectedScenes;
        std::vector<EntitySelectionEntry> visibleEntitySelectionOrder;
        std::unordered_map<uint32_t, std::unordered_set<Entity>> collapsedEntities;

        bool windowOpen;
        bool focusRequested;

        // Cached entity tree — rebuilding costs several ms/frame for large scenes, so it
        // is only rebuilt when the structure changes (see cache keys) or on a timer.
        TreeNode cachedRoot;
        std::unordered_set<Entity> cachedSceneEntitiesSet;
        std::unordered_set<Entity> cachedPlayCreatedEntities;
        uint32_t cacheSceneId = NULL_PROJECT_SCENE;
        uint64_t cacheStructureVersion = UINT64_MAX;
        size_t cacheEntityCount = SIZE_MAX;
        size_t cacheChildSignature = SIZE_MAX;
        double cacheBuildTime = -1.0;
        std::string cachedSearch;
        bool cachedMatchCase = false;

        Entity openParent;
        uint32_t selectionAnchorSceneId = NULL_PROJECT_SCENE;
        Entity selectionAnchorEntity = NULL_ENTITY;

        // Selection made outside this panel: expand its ancestors and scroll to its row
        EntitySelectionEntry revealRequest;
        std::unordered_set<const TreeNode*> revealOpenNodes;
        const TreeNode* revealTargetNode = nullptr;

        std::vector<Entity> getTopLevelSelectedEntities(Entity draggedEntity);
        std::vector<Entity> getMovableDraggedEntities(Entity draggedEntity, const TreeNode& targetNode, InsertionType type);
        void moveDraggedEntitiesToTarget(const std::vector<Entity>& draggedEntities, Entity target, InsertionType type);
        void moveDraggedEntitiesToRootLevel(const std::vector<Entity>& draggedEntities, const std::unordered_set<Entity>& entitiesSet);
        uint32_t getNodeSceneId(const TreeNode& node) const;
        void collectVisibleEntitySelectionOrder(const TreeNode& node, bool hasSearch);
        std::vector<Entity> getEntitySelectionRange(uint32_t sceneId, Entity startEntity, Entity endEntity) const;
        void removeSelectedEntity(uint32_t sceneId, Entity entity);
        void applyEntitySelection(uint32_t sceneId, Entity entity, bool shiftPressed, bool ctrlPressed);
        void clearSubSelectionForEntity(uint32_t sceneId, Entity entity);
        void resetEntitySelectionAnchor();
        void moveCameraToEditorView(Entity cameraEntity);

        // What the inline rename field is editing: a GameObject row or the scene header
        enum class RenameTarget { None, Entity, Scene };
        RenameTarget renameTarget = RenameTarget::None;
        uint32_t renameSceneId = NULL_PROJECT_SCENE;
        Entity renameEntity = NULL_ENTITY;
        char renameBuffer[256] = "";
        int renameFocusFrames = 0;
        int renameMissedFrames = 0;     // frames the renamed row was not drawn
        bool windowFocused = false;
        bool focusedThisFrame = false;      // the panel took focus from this frame's click
        ImGuiID pendingContextMenuItem = 0; // row whose context menu opens next frame
        bool pendingEmptySpaceMenu = false;

        void showCreateMenuItems(bool atRoot, Entity parent, bool addToPrefab);
        void showSceneMenu(SceneProject* sceneProject);
        void showPrefabMenu(const TreeNode& node);

        // Prefab Mode (HierarchyPrefabMode.cpp): the stage's header menu, Unity's "Open Asset
        // in Context / in Isolation" items, and the row buttons (the header's back arrow, the
        // open arrow of an instance root)
        void showPrefabStageMenu(SceneProject* sceneProject);
        void showOpenPrefabItems(const TreeNode& node);
        void drawPrefabRowButtons(const TreeNode& node, bool renaming);
        // S26 R23 (HierarchyVisibilityGutter.cpp): the left gutter's eye (Scene view
        // visibility) and hand (pick lock) of a row, shown on hover or when set; a click sets
        // the row and its children, Alt+click the row alone
        static constexpr float VISIBILITY_GUTTER_ICONS = 2.0f;
        float visibilityGutterWidth() const;
        void drawVisibilityGutter(const TreeNode& node, float rowMinY, float rowMaxY);
        // The prefab asset and instance root a row opens in Prefab Mode (false if none)
        bool getOpenPrefabTarget(const TreeNode& node, std::filesystem::path& path, Entity& instanceRoot) const;
        void beginRename(RenameTarget target, uint32_t sceneId, Entity entity, const std::string& currentName);
        bool isRenaming(const TreeNode& node) const;
        void drawRenameField();
        void commitRename();
        void revealInProject(const std::filesystem::path& path);
        friend struct HierarchyTestAccess;
        void rebuildEntityTree(SceneProject* sceneProject, TreeNode& root, std::unordered_set<Entity>& sceneEntitiesSet);
        void showToolbar();
        void showTreeNode(TreeNode& node);
        void syncSceneWindowSelectionHierarchy(const TreeNode& node, uint32_t collapsedSceneId = NULL_PROJECT_SCENE, Entity collapsedAncestor = NULL_ENTITY);
        bool collectRevealPath(TreeNode& node);
        void pushNodeImGuiId(const TreeNode& node);
        void popNodeImGuiId(const TreeNode& node);
        void drawInsertionMarker(const ImVec2& p1, const ImVec2& p2);
        // A model file (.gltf, .glb, .obj, .fbx) dropped here makes a Model GameObject of the selected 3D scene;
        // a Vertex Animation (.vat) a GameObject playing it (Model = its .vat.glb + Vertex Animator)
        bool acceptsModelDrop(const std::string& filePath) const;
        void handleEntityFilesDrop(const std::vector<std::string>& filePaths, Entity parent = NULL_ENTITY);
        void handleSceneFilesDropAsChildScenes(const std::vector<std::string>& filePaths, uint32_t ownerSceneId);
        void showAddChildSceneMenu();

        // the drag's header (no YAML: a Project window drop encodes the selection itself)
        EntityPayload buildEntityPayload(const TreeNode& node, bool& allowed);
        std::filesystem::path getPrefabSaveDirectory() const;
        void saveNodeAsPrefab(const TreeNode& node);

        // ---- Prefab rows (HierarchyPrefabRows.cpp): markers, ">" button, Missing Prefab
        // menu, Replace Prefab (menu, popup, Alt-drop) ----
        struct ReplacePrefabRequest {
            uint32_t sceneId = NULL_PROJECT_SCENE;
            std::vector<Entity> roots;
        };
        ReplacePrefabRequest replaceRequest;          // opens the popup next frame
        ReplacePrefabRequest replaceTargets;          // what the open popup replaces
        char replaceFilter[128] = "";
        std::string replaceChoice;                    // project-relative prefab path
        PrefabReplaceSettings replaceSettings;
        std::vector<std::filesystem::path> replaceCandidates;

        // Added GameObjects, rows below Missing Prefabs, variants and ">" buttons, after a rebuild
        void markPrefabRows(TreeNode& root);
        // The "+" badge on an added GameObject's icon, right after its row was drawn
        void drawPrefabRowOverlay(const TreeNode& node, const ImVec2& rowMin, const ImVec2& rowMax);
        // S29 (HierarchyVcs.cpp): the version control state of a row, the toolbar's Show VCS
        // Changes toggle and the scene row's Version Control menu
        void drawVcsRowOverlay(const TreeNode& node, const ImVec2& rowMin, const ImVec2& rowMax);
        void showVcsToolbarToggle();
        void showVcsMenu(const SceneProject* sceneProject);
        bool showsVcsChanges() const;
        // The ">" button at the right end of an instance root's row
        void showPrefabRowArrow(const TreeNode& node);
        // Missing Prefab rows: the Prefab submenu
        void showMissingPrefabMenu(const TreeNode& node);
        void requestReplacePrefab(uint32_t sceneId, const std::vector<Entity>& roots);
        // Prefab > Create Variant: a variant of the instance's prefab made from it (PREFABS.md 19)
        void requestCreateVariant(uint32_t sceneId, Entity root);
        void showReplacePrefabPopup();
        // Unity's "Cannot restructure Prefab instance" dialog: a move of members of an instance
        // (MoveEntityOrderCmd::getPrefabRestructureBlock) is refused, Open Prefab offered.
        // True when one of `entities` cannot move (the dialog then opens)
        struct RestructureRefusal {
            bool pending = false;
            std::filesystem::path prefab;
            Entity instanceRoot = NULL_ENTITY;
            std::string name;
            int delay = 0;
        };
        RestructureRefusal restructureRefusal;
        bool refusePrefabRestructure(uint32_t sceneId, const std::vector<Entity>& entities);
        void showRestructurePopup();
        // Alt-drop of a prefab asset on an instance root row replaces it (Alt+Shift: and
        // clears its overrides); true when it took the drop
        bool handlePrefabReplaceDrop(const TreeNode& node);
        // Prefab Mode entry point of the ">" button: in context of instanceRoot (in the shown
        // scene), in isolation without one (HierarchyPrefabMode.cpp)
        void openPrefabFromHierarchy(const std::filesystem::path& path, Entity instanceRoot = NULL_ENTITY);
        // [E] Prefab Variants: whether the asset is a variant (its rows get the variant icon)
        bool isPrefabVariantAsset(const std::filesystem::path& path) const;

        // A lattice fitted around targets and bound to them (LatticeEditorOps::createLattice),
        // or a unit lattice at the Scene view's focus point without targets; one undo step
        void createLatticeAround(const std::vector<Entity>& targets);

        // Search-related methods
        void updateSearchAndSelectionOrder(TreeNode& root, bool treeRebuilt);

    public:
        // The YAML of a drag of `dragged` (with the selection it moves) from the Hierarchy of
        // `sceneId`: what a Project window drop makes a prefab of. Empty when that is not the
        // selected scene or the selection holds GameObjects Play created.
        std::string encodeDraggedSelection(uint32_t sceneId, Entity dragged);
        static constexpr const char* WINDOW_NAME = "Hierarchy";

        static std::string getObjectIcon(Signature signature, Scene* scene);

        // Shared GameObject create table (see CreateMenuEntry)
        static const std::vector<CreateMenuEntry>& getCreateMenuEntries();
        static bool isCreateEntryAvailable(const CreateMenuEntry& entry, SceneType sceneType);
        // Creates the entry in the selected scene; parent NULL_ENTITY = scene root
        void createGameObject(const CreateMenuEntry& entry, Entity parent = NULL_ENTITY, bool addToPrefab = false);
        // The single selected GameObject if it can take children, or NULL_ENTITY
        Entity getSelectedParentForCreate() const;
        // Inline rename (F2) of the single selected GameObject; false if there is nothing to rename
        bool renameSelected();
        bool isFocused() const;

        HierarchyWindow(Project* project, SceneWindow* sceneWindow);

        void show();

        // "This scene did not load completely" above the tree, when the scene has dropped load issues

        void showPartialLoadBanner(const SceneProject& sceneProject);
        void revealEntity(uint32_t sceneId, Entity entity);
        void setOpen(bool open);
        bool isOpen() const;
    };

}
