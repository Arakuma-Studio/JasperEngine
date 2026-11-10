// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SCENEMANAGER_H
#define SCENEMANAGER_H

#include "Export.h"
#include "Entity.h"
#include "texture/Texture.h"
#include <string>
#include <functional>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <cstdint>
#include <initializer_list>

namespace jasperengine {

    class Scene;

    // Files a scene loads, known before it is built so preloadScene() can load them
    struct SceneResources {
        std::vector<Texture> textures;
        std::vector<std::string> sounds;
        std::vector<std::string> models;
        // animation clips (.anim, asset-relative) and model sub-clips ("<model>#<name>"), loaded
        // into AnimationClipPool; a sub-clip waits for its model in `models`
        std::vector<std::string> clips;
        // Vertex Animation assets (.vat, asset-relative), loaded into VertexAnimationPool; their
        // .vat.glb meshes are Models, in `models`
        std::vector<std::string> vats;
        // Avatar Masks (.mask) and Animator Controllers (.controller), asset-relative, loaded into
        // AnimatorAssetPool (S21); a controller's clips and masks are listed in `clips` and
        // `masks` too
        std::vector<std::string> masks;
        std::vector<std::string> controllers;
        // bitmap fonts (.bitmapfont, .fnt; asset-relative) a Text draws, loaded into BitmapFontPool
        // (S51 R19)
        std::vector<std::string> bitmapFonts;
    };

    // SceneManager allows registering named scene stacks and switching between them at runtime.
    // A "scene stack" corresponds to an editor SceneProject: one main Scene plus zero or more
    // layer Scenes. The load factory replaces the running scenes with the stack; the add
    // factory only creates them, for a stack shown on top of the running one.
    //
    // Usage from C++ (standalone generated code):
    //   SceneManager::registerScene(1, "Level1", load_Level1, add_Level1);
    //   SceneManager::loadScene("Level1");     // by name
    //   SceneManager::loadScene(1);            // by id
    //   SceneManager::addChildScene("Hud");    // on top of Level1
    //
    // Usage from Lua script:
    //   SceneManager.loadScene("Level1")
    //   SceneManager.addChildScene("Hud")

    class JASPERENGINE_API SceneManager {
    private:
        struct SceneEntry {
            uint32_t id;
            std::string name;
            std::function<void()> loadFactory;
            std::function<void()> addFactory;
            std::vector<uint32_t> sceneIds;
            std::function<SceneResources()> resources;
        };

        // Files of a preloadScene() still loading, and the loaded ones held in their pools
        struct Preload {
            uint32_t id;
            bool started = false;
            bool cancelled = false; // kept until the loads in flight end
            size_t total = 0;
            SceneResources pending;
            std::vector<std::shared_ptr<void>> held;
            // removes a loaded file from its pool when no scene uses it
            std::vector<std::function<void()>> releases;
        };

        enum class LoadingState {
            None,
            Covering,   // loading scene over the old stack, switch pending
            Loading,    // new stack loading its resources
            Loaded      // new stack loaded, holds keep the loading scene up
        };

        static std::vector<SceneEntry> entries;
        static uint32_t currentId;
        static std::optional<uint32_t> pendingId;
        static std::map<uint32_t, Scene*> scenePtrs;
        // a load factory is building its scenes (and running their scripts' init)
        static bool factoryRunning;
        // the scenes the running load replaced (getUnloadingScenes)
        static std::vector<Scene*> unloadingScenes;

        static uint32_t loadingSceneId;
        static float loadingDelay;
        static float loadingTimeout;
        static LoadingState loadingState;
        static bool loadingSceneShown;
        static bool loadingSceneReady;
        static double loadingStateTime;
        static size_t loadingCount;
        static double loadingProgressTime;
        static float loadingProgress;
        static int loadingHolds;

        static std::vector<Preload> preloads;

        // A GameObject waiting for its move to another scene (dontDestroyOnLoad,
        // moveGameObjectToScene): the move runs at the next safe point (flushPersistentMoves)
        struct PendingMove {
            Scene* from;
            Entity root;
            Scene* to;
        };
        static Scene* persistentScene;
        static std::function<Scene*()> persistentCreate;
        static std::function<void(Scene*)> persistentDestroy;
        static std::vector<PendingMove> pendingMoves;
        static bool flushingMoves;
        static int startingScenes;

        static void raisePersistentScene();
        static bool queueMove(Scene* from, Entity root, Scene* to, const char* caller);

        static std::vector<uint32_t> buildSceneStackIds(uint32_t id, const std::vector<uint32_t>& sceneIds);
        static SceneEntry* findEntry(uint32_t id);
        static void runFactory(uint32_t id);

        static std::vector<Scene*> getRunningScenes(uint32_t id);
        static bool canShowLoadingScene(uint32_t id);
        static void raiseLoadingScene();
        static void startLoading(uint32_t id);
        static void resetLoadProgress();
        static bool isLoadProgressing(size_t loaded, double now);
        static void getLoadCount(uint32_t id, size_t& loaded, size_t& total);

        static Preload* findPreload(uint32_t id);
        static void startPreload(uint32_t id);
        static void releasePreload(Preload& preload);
        static void updatePreloads();

    public:
        // Register a named scene stack.
        // The load factory must call Engine::setScene() / Engine::addSceneLayer() to set up
        // the scene hierarchy. It should also call Engine::removeAllScenes() first if
        // a scene transition is desired (this is done automatically by loadScene()).
        // The add factory creates the scenes and registers them with setScenePtr();
        // addChildScene() runs it and puts them on screen, so it must be safe to call more
        // than once. A stack registered without one can only be added once something else
        // has created its scenes.
        // The initializer list overload is what keeps a braced fourth argument unambiguous
        // between the scene ids and the add factory.
        // The resources function lists the files a scene loads, for preloadScene().
        static void registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory);
        static void registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, const std::vector<uint32_t>& sceneIds);
        static void registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, std::initializer_list<uint32_t> sceneIds);
        static void registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, std::function<void()> addFactory);
        static void registerScene(uint32_t id, const std::string& name, std::function<void()> loadFactory, std::function<void()> addFactory, const std::vector<uint32_t>& sceneIds, std::function<SceneResources()> resources = nullptr);

        // Load a scene stack by name. Calls Engine::removeAllScenes() then invokes the
        // registered factory. Returns false if the name is not found.
        // While a scene is running (script callback, contact, input) the transition is
        // deferred to the next frame, as the factory tears down the running scenes and
        // scripts; the current scene only changes then.
        static bool loadScene(const std::string& name);

        // Load a scene stack by id.
        // Returns false if the id is not found.
        static bool loadScene(uint32_t id);

        // True between a deferred loadScene() and the frame that applies it.
        static bool isLoadPending();

        // Runs the deferred transition. Called by Engine before the scenes update.
        static void applyPendingLoad();

        // Scene stack shown on top while loadScene() switches stacks, until the new stack
        // has loaded. An empty name or 0 disables it.
        static bool setLoadingScene(const std::string& name);
        static bool setLoadingScene(uint32_t id);
        static uint32_t getLoadingSceneId();

        // Seconds the loading scene covers the old scenes before they are replaced.
        static void setLoadingDelay(float seconds);
        static float getLoadingDelay();

        // Seconds without load progress before a load stops waiting.
        static void setLoadingTimeout(float seconds);
        static float getLoadingTimeout();

        // True from loadScene() until the new stack has loaded and every hold is released.
        static bool isLoading();

        // Loaded share of the new stack's drawables and sounds, from 0 to 1. Never goes back.
        static float getLoadingProgress();

        // Keeps the loading scene up after the new stack has loaded, until released.
        // Returns false when nothing is loading.
        static bool holdLoading();
        static void releaseLoading();

        // True for a scene below the shown loading scene, which gets no UI input.
        static bool isCoveredByLoading(Scene* scene);

        // The loading scene is on screen (a screen transition reveals onto it, S48 R15)
        static bool isLoadingSceneShown();
        // The stack loadScene() switched to has loaded (holds may still keep the loading scene up),
        // or nothing is loading
        static bool isLoadComplete();

        // Tracks the loading of a stack started without loadScene(), as the editor's Play does.
        static void setCurrentScene(uint32_t id);

        // True while a load factory runs (it builds the new stack and starts its scripts)
        static bool isFactoryRunning();
        // While a load factory runs: the scenes it replaces, whose scripts were unloaded
        // (onDisable, onDestroy) before it started; the factory deletes or reuses them. A scene
        // loaded again is among them: it is built anew (also in the editor's Play).
        static const std::vector<Scene*>& getUnloadingScenes();

        // Loads the files of a scene stack in the background while the game runs, so a later
        // loadScene() finds them ready. They are held until that load or cancelPreload().
        // Without async loading one file loads per frame, and models are skipped.
        static bool preloadScene(const std::string& name);
        static bool preloadScene(uint32_t id);

        // Loaded share of a preloadScene(), from 0 to 1; 0 when it was not requested.
        static float getPreloadProgress(const std::string& name);
        static float getPreloadProgress(uint32_t id);

        static void cancelPreload(const std::string& name);
        static void cancelPreload(uint32_t id);

        // Called by Engine after the scenes draw.
        static void updateLoading();

        // Add a child scene stack on top of the running scenes, keeping the main scene.
        // Runs the stack's add factory, which creates the scenes that are missing and restores
        // the scripts of any that were torn down, then adds them in stack order. Unlike
        // loadScene() this takes effect at once, including inside a frame, as nothing is
        // destroyed: the scenes added run and draw from the same frame on.
        static bool addChildScene(uint32_t id);
        static bool addChildScene(const std::string& name);

        // Remove a child scene stack from the active scene list without deleting Scene objects. The main scene is kept.
        static bool removeChildScene(uint32_t id);
        static bool removeChildScene(const std::string& name);

        // Return the id of a scene by name, or 0 if not found.
        static uint32_t getSceneId(const std::string& name);

        // Return the name of the scene at the given id, or "" if not found.
        static std::string getSceneName(uint32_t id);

        // The name of a stored scene pointer's scene (setScenePtr), or "" (messages)
        static std::string getSceneNameOf(const Scene* scene);

        // Return all registered scene names in registration order.
        static std::vector<std::string> getSceneNames();

        // Return the total number of registered scenes.
        static int getSceneCount();

        // Return the id of the scene most recently loaded via loadScene(), or 0 if none.
        static uint32_t getCurrentSceneId();

        // Return the name of the scene most recently loaded via loadScene(), or "" if none.
        static std::string getCurrentSceneName();

        // Remove all registered scenes. Intended for editor use when resetting state. The
        // persistent scene goes too (destroyPersistentScene); its factory stays.
        static void clearAll();

        // Store a scene pointer associated with a scene ID for cross-scene entity resolution.
        static void setScenePtr(uint32_t id, Scene* scene);

        // Retrieve a stored scene pointer by ID. Returns nullptr if not found.
        static Scene* getScenePtr(uint32_t id);

        // Remove a stored scene pointer.
        static void removeScenePtr(uint32_t id);

        // Every stored scene pointer (the game's created scenes, also while their scripts
        // start, before they run), by scene ID.
        static std::vector<Scene*> getScenePtrs();

        // ---- Persistent objects (Unity's DontDestroyOnLoad)
        //
        // A root GameObject marked with dontDestroyOnLoad() moves, with its children, components
        // and running state (script instances, sounds, bodies, actions), into the persistent scene:
        // a scene layer named "DontDestroyOnLoad" that no loadScene() unloads, drawn above the
        // loaded scenes and below the loading scene. It is created on the first call and deleted
        // at quit (destroyPersistentScene, after ScriptRuntime::quitAll) or by clearAll().
        //
        // The move itself waits for a safe point, never inside a script callback or a system's
        // loop: the end of the outermost scene start (ScriptRuntime::awakeScene, before the
        // scene's documents start), ObjectManager::update (after this frame's update), the start
        // of the next frame, and always before a loadScene() tears the scenes down. isPersistent()
        // is true from the call on; the script instances' scene and entity change with the move.

        // The persistent scene's id in getScenePtr() (never a project scene id)
        static constexpr uint32_t PERSISTENT_SCENE_ID = 0xFFFFFFFEu;
        static constexpr const char* PERSISTENT_SCENE_NAME = "DontDestroyOnLoad";

        // Who makes and deletes the persistent scene: by default a new Scene (deleted with
        // `delete`); the editor's Play makes a runtime scene of its session (which deletes it at
        // Stop: its destroy only lets go). A create returning null refuses (logged by the
        // caller): the editor outside Play. Null functions bring back the default.
        static void setPersistentSceneFactory(std::function<Scene*()> create, std::function<void(Scene*)> destroy);
        static Scene* getPersistentScene();
        // Creates it if needed (and adds it as a running scene layer); null when refused
        static Scene* ensurePersistentScene();
        // Releases its scripts (their onDestroy ran at quit), takes it off the screen and deletes
        // it through the factory. Moves still pending are dropped.
        static void destroyPersistentScene();

        // Marks the root GameObject `root` of `scene` to persist. A GameObject with a parent is
        // refused with a warning (Unity), as is a missing one; one already persistent (or
        // queued) is a no-op. Returns true when the GameObject is (or will be) persistent.
        static bool dontDestroyOnLoad(Scene* scene, Entity root);
        // In the persistent scene, or below a root queued to move there
        static bool isPersistent(Scene* scene, Entity entity);
        // Unity's SceneManager.MoveGameObjectToScene: the root GameObject goes to `to`, a running
        // scene or the persistent scene (out of the persistent scene it stops persisting).
        // Errors (logged, false): not a root, missing, "target scene is not loaded".
        static bool moveGameObjectToScene(Scene* from, Entity root, Scene* to);
        // The queued moves now (the safe points above call it)
        static void flushPersistentMoves();
        // A queued move from or to this scene (the scene's GameObjects are still where they were)
        static bool hasPendingMoves(Scene* scene = nullptr);
        // The root GameObjects queued to move to the persistent scene, with their scenes
        static std::vector<std::pair<Scene*, Entity>> getPendingPersistentRoots();
        // A scene being deleted: its queued moves are dropped (ObjectManager::forgetScene)
        static void forgetPendingMoves(Scene* scene);
        // Scenes start outside a load factory (the editor's Play start): the end of each scene's
        // ScriptRuntime::awakeScene is a safe point for the moves, as it is inside a factory
        static void beginStartingScenes();
        static void endStartingScenes();
        // A load factory runs, or the scenes of a Play start
        static bool isStartingScenes();
    };

} // namespace jasperengine

#endif // SCENEMANAGER_H
