// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "lua.hpp"

#include <string>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "LuaBridge.h"
#include "Out.h"

#include "Project.h"
#include "script/ScriptProperty.h"

#include "texture/Texture.h"
#include "ecs/Entity.h"
#include "render/Render.h"

#include "command/Command.h"
#include "ecs/ComponentFwd.h"

namespace jasperengine{
    struct LineRendererComponent;
    struct ScriptEntry;
    struct TrailRendererComponent;
}

namespace jasperengine::editor {

    class MultiPropertyCmd;

// ProjectUtils::makeUniqueEntityName for many names in a row, each then taken. Remembers,
// per base name, the run of numbered names found taken, so naming a thousand copies of
// "Sprite 1".."Sprite 1000" does not rescan the run for each: same names, linear time.
class UniqueEntityNames {
public:
    explicit UniqueEntityNames(std::unordered_set<std::string> existingNames);
    std::string make(const std::string& baseName);

private:
    std::unordered_set<std::string> existingNames;
    std::unordered_map<std::string, std::pair<size_t, size_t>> takenRuns; // base -> [first, last] all taken
};

// ProjectUtils::getLockedEntityParent for many entities of one scene (a delete of many): the
// models' and panels' references indexed once instead of scanned for each entity (O(1) an
// entity instead of O(the scene's model nodes)). The same answers while the scene is unchanged.
class LockedParentIndex {
public:
    explicit LockedParentIndex(Scene* scene);
    Entity get(Entity entity) const;

private:
    struct ModelReference {
        Entity owner;
        bool byTransformParent; // a bone or node: its Transform parent when it has one
    };
    Scene* scene;
    std::unordered_map<Entity, ModelReference> modelReferences;
    std::unordered_map<Entity, Entity> panelParts;
};

class ProjectUtils {
public:
    // the editor's new Line Renderer (GameObject > Effects > Line, Add Component,
    // create_entity, add_component): Unity's, a world-space polyline (0,0,0)-(0,0,1) of width
    // 0.1, in a 2D scene (pixel units) (0,0)-(100,0) of width 4. The component's own defaults
    // stay the old hairline (scenes and scripts unchanged)
    static void setupNewLineRenderer(LineRendererComponent& lines, bool scene2D);
    // the editor's new Trail Renderer: width 0.2 (8 in a 2D scene, whose minimum vertex
    // distance is 2 pixels), thinning (1 -> 0) and fading out (alpha 1 -> 0) to the tail
    static void setupNewTrailRenderer(TrailRendererComponent& trail, bool scene2D);

    static Entity getLockedEntityParent(Scene* scene, Entity entity);
    // getLockedEntityParent's UI part (a control's label, bar, ... below it); `checkPanels`
    // when the panels' header parts are still to look at
    static Entity lockedByParentUI(Scene* scene, Entity entity, Signature signature, bool& checkPanels);
    static bool isEntityLocked(Scene* scene, Entity entity);
    static Entity getParentModel(Scene* scene, Entity entity);
    // Model owning the imported parts an entity is or contains, never a model itself
    static Entity getModelBranchOwner(Scene* scene, Entity entity);
    // Any entity the loader generated for the model: nodes, parts, bones, skeleton
    static bool isModelNode(const ModelComponent& model, Entity entity);
    static bool canEditModelBranch(Scene* scene, Entity entity, std::string* reason = nullptr);
    static bool hasCustomMeshParenting(Scene* scene, Entity model);
    static Entity getEffectiveParent(Scene* scene, Entity entity);
    static bool canMoveLockedEntityOrder(Scene* scene, Entity source, Entity target, InsertionType type, std::string* reason = nullptr);
    static std::string makeUniqueEntityName(const std::string& baseName, const std::unordered_set<std::string>& existingNames);
    // the soft round particle image the UI Particle System starts with ("ui/particle_soft.png"
    // in the assets, made once); its project-relative path ("assets/ui/particle_soft.png", as
    // Texture paths are), "" when it cannot be written
    static std::string ensureSoftParticleTexture(Project* project);
    // The on-screen controls' circle images (ui-multitouch), written once into the project's
    // assets as assets/ui/onscreen/stick_base.png and stick_knob.png (generated, not bundled);
    // the assets-relative path a Texture stores, "" when it could not be written
    static std::string ensureOnScreenControlTexture(const std::filesystem::path& assetsPath, bool knob);
    static std::string makeUniqueEntityName(Scene* scene, const std::vector<Entity>& entities, const std::string& baseName, const std::unordered_set<Entity>& ignoredEntities = {});

    static size_t getTransformIndex(EntityRegistry* registry, Entity entity);
    // The Transform array holds each branch in one run, a GameObject after its parent and
    // before the next GameObject outside the parent's branch (findBranchLastIndex, the moves and
    // the scene and prefab encoders read it so). A GameObject decoded with a Transform under a
    // parent already placed (a prefab instance imported again, a GameObject put back below one)
    // is appended at the end of the array: this moves its branch to the end of its parent's
    // branch, as its last child, when it is not in it. Returns whether it moved.
    static bool keepInParentBranch(EntityRegistry* registry, Entity entity);
    // The parent whose branch the Transform array splits (a GameObject of it comes after one
    // outside it), the first one found; NULL_ENTITY when every branch is in one run
    static Entity findSplitBranch(EntityRegistry* registry);
    // Whether a GameObject's branch sits where it belongs: in its parent's branch, and not between
    // another GameObject and that one's children
    static bool isBranchPlace(EntityRegistry* registry, Entity entity);
    // Transform order; a GameObject without a Transform stays after the branch of the top-level
    // GameObject it followed (not `moved`, which a move just placed elsewhere), in its order
    static void sortEntitiesByTransformOrder(EntityRegistry* registry, std::vector<Entity>& entities, Entity moved = NULL_ENTITY);
    // Puts each GameObject without a Transform back after the GameObject it followed in `before`
    // (the first one still listed; the front when none), the others as they are: an undo that
    // cannot tell from the list what moved gives it back as it was
    static void keepUntransformedPlaces(EntityRegistry* registry, const std::vector<Entity>& before, std::vector<Entity>& entities);

    static bool moveEntityOrderByTarget(EntityRegistry* registry, std::vector<Entity>& entities, Entity source, Entity target, InsertionType type, Entity& oldParent, size_t& oldIndex, bool& hasTransform);
    // sortEntities false leaves the tracked list to one sortEntitiesByTransformOrder by the
    // caller after a batch of moves
    static void moveEntityOrderByIndex(EntityRegistry* registry, std::vector<Entity>& entities, Entity source, Entity parent, size_t index, bool hasTransform, bool sortEntities = true);
    static void moveEntityOrderByTransform(EntityRegistry* registry, std::vector<Entity>& entities, Entity source, Entity parent, size_t transformIndex, bool enableMove = true, bool sortEntities = true);

    static void addEntityComponent(EntityRegistry* registry, Entity entity, ComponentType componentType, std::vector<Entity>& entities, YAML::Node componentNode = YAML::Node());
    static YAML::Node removeEntityComponent(EntityRegistry* registry, Entity entity, ComponentType componentType, std::vector<Entity>& entities, bool encodeComponent = false);
    static void reconcileTrackedEntities(EntityRegistry* registry, std::vector<Entity>& entities, Project* project = nullptr, uint32_t sceneId = NULL_PROJECT_SCENE);

    // --- Virtual children utilities ---
    static Entity getVirtualParent(Scene* scene, Entity entity);
    static std::vector<Entity> getVirtualChildren(Scene* scene, const std::vector<Entity>& parentEntities, const std::unordered_set<Entity>& excludedEntities = {});

    // --- Ownership cascade utilities ---
    static std::vector<Entity> getOwnedCascadeEntities(EntityRegistry* registry, Entity owner, ComponentType componentType);

    // --- Lua script utilities ---
    static ScriptPropertyValue luaValueToScriptPropertyValue(lua_State* L, int idx, ScriptPropertyType type);
    // The properties of a Lua script (its file, or its code being edited: errors ignored), read
    // in an isolated state (readLuaScriptProperties): any thread
    static void loadLuaScriptProperties(ScriptEntry& entry, const std::string& luaPath);
    static void loadLuaScriptPropertiesFromString(ScriptEntry& entry, const std::string& scriptContent, const std::string& chunkName);
    // What reading a Lua script's properties did: the script failed to load (syntax) or to
    // run its top level, or a property declaration can't be read (a default of the wrong type).
    // message is Lua's ("<chunkName>:<line>: ..." when Lua knows the line).
    struct LuaPropertiesResult {
        enum Status { Ok, LoadError, RunError, PropertyError } status = Ok;
        std::string message;
    };
    // Reads a Lua script's properties as the editor does: runs its top level in L (this
    // executes the file) and reads the `properties` of the table it returns, all in protected
    // calls. The chunk is named "@<chunkName>". entry keeps its properties on failure.
    static LuaPropertiesResult runLuaScriptProperties(lua_State* L, ScriptEntry& entry, const std::string& code, const std::string& chunkName);
    // runLuaScriptProperties in a fresh state of its own (ProjectChecks::newIsolatedLuaState: the
    // engine's globals are inert stand-ins, require reads the project's Lua directory), never
    // the engine's: safe from any thread, and the script's top level touches no game state.
    static LuaPropertiesResult readLuaScriptProperties(ScriptEntry& entry, const std::string& code, const std::string& chunkName);

    static void collectModelEntities(Scene* scene, const ModelComponent& model, std::vector<Entity>& out);

    // True when a model root has no renderable submeshes of its own while its
    // generated model children do. Components that only render same-entity mesh
    // data (such as InstancedMeshComponent) cannot render that root directly.
    static bool hasModelMeshChildrenWithoutRootGeometry(EntityRegistry* registry, Entity entity);

    static void collectEntities(const YAML::Node& entityNode, std::vector<Entity>& allEntities);

    // Engine class wrapping an entity, used to generate C++ that names it (subclass
    // script parents, DPROPERTY members). Every class here is declared in the flat
    // header "<name>.h" and has a (Scene*, Entity) constructor.
    struct EntityClassInfo {
        std::string name;           // e.g. "Terrain", declared in "Terrain.h"
        bool derivesObject = false; // has jasperengine::GameObject API (getPosition, setPosition, ...)
        bool derivesMesh = false;   // has jasperengine::Mesh API (setColor, ...), implies derivesObject
    };
    static EntityClassInfo getEntityClassInfo(Scene* scene, Entity entity);

    // Returns entity type name (e.g. "Mesh", "Camera", "Light") based on its components
    static std::string getEntityTypeName(Scene* scene, Entity entity);

    // An editor preview (an action's, an animation's) restored the GameObject: its particles go
    // with it (a re-decoded Particle System keeps its runtime)
    static void clearPreviewParticles(Scene* scene, Entity entity);

    // Fills a Texture with the editor built-in default skybox cubemap.
    static void setDefaultSkyTexture(Texture& outTexture);

    // Resolved plan for a built-in shader fork: the project-relative base path (no
    // extension), the absolute target paths, and the source contents to write. Computed
    // without touching the filesystem so it can drive an undoable command.
    struct ShaderForkPlan {
        bool valid = false;
        std::string error;
        std::string base;                  // e.g. "shaders/myMesh"
        std::filesystem::path vertPath;    // absolute
        std::filesystem::path fragPath;    // absolute
        // Empty unless includes were forked, in which case the fork owns this directory
        // and holds its entry points plus the copied includes/ tree inside it.
        std::filesystem::path forkDirPath;      // absolute
        std::string vertContent;
        std::string fragContent;
        struct IncludeFile {
            std::filesystem::path path;    // absolute
            std::string content;
        };
        std::vector<IncludeFile> includeFiles;
    };

    // Default used only by non-interactive callers. The editor dialog lets the user
    // select any directory inside the project for each individual fork.
    static std::filesystem::path defaultShaderForkDir();

    // Converts an entity/scene label to a safe filename and adds a numeric suffix until
    // neither fork layout (loose .vert/.frag pair, or a directory of that name) is taken.
    static std::string makeUniqueShaderName(const std::filesystem::path& absDir,
                                            ShaderType shaderType,
                                            const std::string& desiredName);

    // Plans a built-in shader fork without writing anything. targetDirRel must be a safe
    // project-relative directory and baseName the exact filename stem. Without includes
    // the entry points stay loose in it; with them the fork takes <targetDirRel>/<base>/,
    // so its copies override the engine library for no other shader.
    static ShaderForkPlan prepareShaderFork(Project* project, ShaderType shaderType,
                                            const std::filesystem::path& targetDirRel,
                                            const std::string& baseName,
                                            bool forkIncludes = false);

    // Writes all planned files. It refuses collisions and removes partial output when a
    // later write fails, so a failed command never truncates or strands source files.
    static bool writeShaderFork(const ShaderForkPlan& plan);

    // Removes only files owned by the plan, then prunes the directories it created.
    static void removeShaderFork(const ShaderForkPlan& plan);

    // Builds a MultiPropertyCmd that removes tile at tileIndex from a TilemapComponent.
    // Returns the command (caller must add it to CommandHandle) or nullptr if invalid.
    static Command* buildDeleteTileCmd(Project* project, uint32_t sceneId, Entity entity, unsigned int tileIndex);

    // Builds a PropertyCmd that removes the instance at instanceIndex from an InstancedMeshComponent.
    // Returns the command (caller must add it to CommandHandle) or nullptr if invalid.
    static Command* buildDeleteInstanceCmd(Project* project, uint32_t sceneId, Entity entity, unsigned int instanceIndex);

    // Builds a PropertyCmd that duplicates the instance at instanceIndex in an InstancedMeshComponent.
    // Returns the command (caller must add it to CommandHandle) or nullptr if invalid.
    static Command* buildDuplicateInstanceCmd(Project* project, uint32_t sceneId, Entity entity, unsigned int instanceIndex);

    // Builds a MultiPropertyCmd that duplicates tile at tileIndex in a TilemapComponent.
    // The new tile is placed with a small offset. Returns nullptr if invalid or array is full.
    static Command* buildDuplicateTileCmd(Project* project, uint32_t sceneId, Entity entity, unsigned int tileIndex);
};

} // namespace jasperengine::editor
