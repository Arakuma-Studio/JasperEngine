// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef engine_h
#define engine_h

#if defined(__EMSCRIPTEN__)
#  if !defined(__EMSCRIPTEN_PTHREADS__)
#    define NO_THREAD_SUPPORT
#  endif
#endif

// Lights of the legacy u_fs_lighting block, which forks written before per-object lights
// declare: the engine fills it with the object's first MAX_LIGHTS pixel lights. A scene
// itself may hold any number of lights.
#ifndef MAX_LIGHTS
#define MAX_LIGHTS 6
#endif

// Lights one object is lit by at most (SceneSettings::maxLightsPerObject is clamped to it);
// the per-object lights blocks come in capacity tiers of 4, 8 and 16.
#ifndef MAX_LIGHTS_PER_OBJECT
#define MAX_LIGHTS_PER_OBJECT 16
#endif

// Tiles of the projected spot mask atlas: spot lights with a mask past it use the cone.
#ifndef SPOT_MASK_ATLAS_TILES
#define SPOT_MASK_ATLAS_TILES 6
#endif

// 2D lights (Light2DComponent). Also the row count of the 1D polar shadow atlas:
// each shadow-casting 2D light renders its occluders into one atlas row.
#ifndef MAX_LIGHTS_2D
#define MAX_LIGHTS_2D 16
#endif

#ifndef SHADOW_CUBE_FACES
#define SHADOW_CUBE_FACES 6
#endif

#ifndef MAX_SHADOWCASCADES
#define MAX_SHADOWCASCADES 4
#endif

// Projective shadow atlas (directional + spot). Each slot holds one 2D depth map
// (a spot light, or a single directional cascade) and maps 1:1 to a v_lightProjPos
// vertex varying, so the slot count is bounded by GPU vertex-output limits: keep the
// grid small (a 6x6 / 36-slot grid overflows the varyings and breaks shader compiles).
#ifndef SHADOW_ATLAS_COLS
#define SHADOW_ATLAS_COLS 3
#endif

#ifndef SHADOW_ATLAS_ROWS
#define SHADOW_ATLAS_ROWS 3
#endif

#ifndef MAX_SHADOW_ATLAS_SLOTS
#define MAX_SHADOW_ATLAS_SLOTS (SHADOW_ATLAS_COLS * SHADOW_ATLAS_ROWS)
#endif

// Point shadow atlas (omnidirectional). Each shadow-casting point light consumes
// SHADOW_CUBE_FACES consecutive slots (one per cube face) and is sampled only in the
// fragment shader, so this atlas is bounded by texture size rather than varyings.
#ifndef SHADOW_POINT_ATLAS_COLS
#define SHADOW_POINT_ATLAS_COLS 6
#endif

#ifndef SHADOW_POINT_ATLAS_ROWS
#define SHADOW_POINT_ATLAS_ROWS 4
#endif

#ifndef MAX_POINT_SHADOW_ATLAS_SLOTS
#define MAX_POINT_SHADOW_ATLAS_SLOTS (SHADOW_POINT_ATLAS_COLS * SHADOW_POINT_ATLAS_ROWS)
#endif

#ifndef MAX_POINT_SHADOW_LIGHTS
#define MAX_POINT_SHADOW_LIGHTS (MAX_POINT_SHADOW_ATLAS_SLOTS / SHADOW_CUBE_FACES)
#endif

#ifndef MAX_SUBMESHES
#define MAX_SUBMESHES 16
#endif

// detail levels per submesh, level 0 being the source geometry
#ifndef MAX_MESH_LODS
#define MAX_MESH_LODS 4
#endif

#ifndef MAX_TILEMAP_TILESRECT
#define MAX_TILEMAP_TILESRECT 200
#endif

#ifndef MAX_TILEMAP_TILES
#define MAX_TILEMAP_TILES 2048
#endif

#ifndef MAX_SPRITE_FRAMES
#define MAX_SPRITE_FRAMES 128
#endif

// main font of a text plus its fallbacks
#ifndef MAX_TEXT_FONTS
#define MAX_TEXT_FONTS 4
#endif

#ifndef MAX_BONES
#define MAX_BONES 128
#endif

#ifndef MAX_MORPHTARGETS
#define MAX_MORPHTARGETS 8
#endif

// number of hemisphere samples in the SSAO kernel; must match the
// SSAO_KERNEL_SIZE define injected for the ssao.frag shader (ShaderBuilder)
#ifndef SSAO_KERNEL_SIZE
#define SSAO_KERNEL_SIZE 32
#endif

#ifndef MAX_EXTERNAL_BUFFERS
#define MAX_EXTERNAL_BUFFERS 30
#endif

#ifndef MAX_BROADPHASELAYER_3D
#define MAX_BROADPHASELAYER_3D 6
#endif

#ifndef MAX_COLLIDERS_2D
#define MAX_COLLIDERS_2D 10
#endif

#ifndef MAX_COLLIDER_POINTS_2D
#define MAX_COLLIDER_POINTS_2D 16
#endif

#ifndef MAX_COLLIDERS_3D
#define MAX_COLLIDERS_3D 10
#endif

#ifndef MAX_COLLIDER_VERTICES_3D
#define MAX_COLLIDER_VERTICES_3D 256
#endif

#ifndef MAX_COLLIDER_INDICES_3D
#define MAX_COLLIDER_INDICES_3D 768
#endif

#include "Export.h"
#include "System.h"
#include "util/FunctionSubscribe.h"
#include "math/Rect.h"
#include "util/ThreadUtils.h"
#include "texture/Framebuffer.h"
#include "ecs/Entity.h"
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

#define JASPERENGINE_INIT \
    void init(); \
    namespace { \
        struct InitRegistrar { \
            InitRegistrar() { \
                jasperengine::Engine::getOnInit().add("user_init", &init); \
            } \
        }; \
        static InitRegistrar _jasperengine_init_registrar; \
    }

namespace jasperengine {

    class JASPERENGINE_API Scene;

    enum class Scaling{
        FITWIDTH,
        FITHEIGHT,
        LETTERBOX,
        CROP,
        STRETCH,
        NATIVE
    };

    // texture power of two strategy
    enum class TextureStrategy{
        FIT,
        RESIZE,
        NONE
    };


    enum class Platform{
        MacOS,
        iOS,
        Web,
        Android,
        Linux,
        Windows
    };

    enum class GraphicBackend{
        GLCORE,
        GLES3,
        D3D11,
        METAL,
        WGPU,
        VULKAN
    };

    enum class BodyType{
        STATIC,
        KINEMATIC,
        DYNAMIC
    };

    enum class ResourceLoadState {
        NotStarted,
        Loading,
        Finished,
        Failed
    };

    class JASPERENGINE_API Engine {
    public:
        // draw calls, instances and triangles submitted by the last systemDraw
        struct FrameStats{
            uint32_t drawCalls = 0;
            uint32_t instances = 0;
            uint64_t triangles = 0;
        };

        // How many GPU objects of each kind can be alive at once (sokol's resource pools, each
        // allocated up front at its size; sokol's own limit is 65,535). An object made past a
        // limit fails: SokolSystem logs which pool is full once, and a mesh that cannot get one
        // is counted as failed (not drawn, not loading). Per skinned mesh: one image, view and
        // texture view on OpenGL (bones texture), one storage buffer and view elsewhere; vertex
        // and index buffers of a model file are shared (MeshBufferPool). The defaults hold
        // ~10,000 skinned meshes besides the editor's own objects. Pool slots cost (measured,
        // sizeof sokol's slot structs + 8 bytes) 88 / 96 / 400 / 424 bytes per buffer / image /
        // sampler / view on Vulkan, 68 / 84 / 72 / 104 on OpenGL, 1-5 KB per pipeline and
        // shader: these defaults take ~23 MB on Vulkan and ~21 MB on OpenGL, 10.7 MB and 3.9 MB
        // more than the earlier 8,192 buffers, 2,048 images and 4,096 views.
        //
        // Fixed per-frame and fixed-size buffers of sokol_gfx, by backend: what each holds, sokol's
        // default, what happens when a frame needs more, and what the engine does about it.
        //
        //   buffer (backend)                   sokol default  past it (sokol)                engine
        //   uniform buffer (Vulkan, Metal,     4 MB / frame   Vulkan: the draw dropped, an     uniformBytes; every block counted,
        //     WebGPU)                                         error per draw; Metal, WebGPU:   a draw past it skipped, the first
        //                                                     writes past the end (asserts)    such frame logged
        //   stream staging (Vulkan)            16 MB / frame  the upload dropped, an error     streamStagingBytes; every STREAM
        //                                                     per upload, every later upload   upload counted, one past it skipped
        //                                                     of the frame dropped too (a      (its owner uploads it next frame),
        //                                                     still mesh never got its bones)  the first such frame logged
        //   descriptor buffer (Vulkan)         16 MB / frame  the draw dropped, an error per   descriptorBytes; every draw checked
        //                                                     draw, every later draw of the    against the room left, one past it
        //                                                     frame dropped too                skipped, the first such frame logged
        //   copy staging (Vulkan: immutable    4 MB           never: bigger uploads go in      left at sokol's default (each chunk
        //     data, DYNAMIC updates)                          4 MB chunks, each a blocking     is a queue submit and wait: DYNAMIC
        //                                                     submit and queue wait            resources should not change per frame)
        //   bind group cache (WebGPU)          1,024 slots    never: a miss or collision       bindGroupsCacheSize (a power of 2)
        //                                                     creates a bind group (CPU cost)
        //   STREAM / DYNAMIC updates (GL,      none           no per-frame buffer: written     counted (stream bytes) on every
        //     D3D11, Metal, WebGPU)                           straight into the resource       backend, not limited
        //   uniforms (GL, D3D11)               none           glUniform / a constant buffer    counted, not limited
        //                                                     per block
        //   append past a buffer's size (all)  the buffer     the append refused, its draw     callers size their buffers
        //                                                     fails validation                 (RmlRenderer pages)
        //   resource pools (all)               128 .. 256     the object not made              buffers .. shaders above
        //
        // So Metal has one such buffer (uniforms), D3D11 and GL none, WebGPU uniforms and the bind
        // group cache, Vulkan uniforms, stream staging and descriptors. sokol's per-frame buffers
        // are allocated once per frame in flight (2) in host-visible memory, so their defaults
        // depend on the platform (desktop / Android, iOS and the web). The
        // budgets: SystemRender::getUniformBytes*, getStreamBytes*, getDescriptorBytes*.

        // GpuResourceLimits' per-frame buffer defaults for this platform: uniformBytes 64 MB
        // desktop, 16 MB mobile/web; streamStagingBytes 48 MB / 16 MB; descriptorBytes 32 MB / 16 MB
        static int defaultUniformBytes();
        static int defaultStreamStagingBytes();
        static int defaultDescriptorBytes();

        struct GpuResourceLimits{
            int buffers = 16384;   // vertex, index, storage (bones on Vulkan, Metal, D3D11), instances
            int images = 16384;    // textures and render targets (bones textures on OpenGL)
            int samplers = 2048;   // one per texture; the data textures (bones, lattice) share one
            int views = 24576;     // one per texture and storage buffer, and per render target attachment
            int pipelines = 8192;  // shared by meshes with the same shader variant and state
            int shaders = 1024;    // one per shader variant
            // bytes of uniform blocks a frame can apply on Vulkan, Metal and WebGPU (each block
            // rounded up to 256 bytes): past it a draw is skipped and the first such frame logged
            // (SokolSystem::reserveUniformBytes); GL and D3D11 have no such buffer. A skinned hero
            // of 3 materials in shadow-casting light takes 13,056 bytes a frame (51 blocks: colour
            // pass and shadow cascades, measured by SharedMeshBuffersTest). sokol allocates the
            // buffer once per frame in flight (2) in host-visible memory, so the default depends on
            // the platform: 64 MB on desktop (~5,000 such heroes; 128 MB allocated), 16 MB on
            // Android, iOS and the web (~1,200; 32 MB allocated). sokol's own default, 4 MB, held
            // ~300. Crowds beyond that should be one instanced Vertex Animation (one draw for all).
            int uniformBytes = defaultUniformBytes();
            // bytes a frame can upload into STREAM buffers and textures on Vulkan (sokol's
            // per-frame stream staging buffer, each upload rounded up to 16 bytes): the bone
            // matrices of every skinned mesh whose pose changed (an animated hero of
            // hero_animated.gltf: 8,192 bytes a frame, measured by SharedMeshBuffersTest; a still
            // one uploads once), HTML UI geometry, video and Lottie textures, instance data,
            // terrain nodes. Past it an upload is skipped and tried again next frame by its
            // owner (bones, instances, terrain nodes, HTML UI pages), and the first such frame
            // is logged (SokolSystem::reserveStreamBytes); counted, not limited, on the other
            // backends (they write into the resource). Allocated once per frame in flight (2) in
            // host-visible memory: 48 MB on desktop (~6,000 animated heroes; 96 MB allocated),
            // 16 MB on Android, iOS and the web (sokol's default, ~2,000; 32 MB allocated).
            int streamStagingBytes = defaultStreamStagingBytes();
            // bytes of descriptors a frame can bind on Vulkan (sokol's per-frame descriptor
            // buffer, VK_EXT_descriptor_buffer): each draw writes its textures', samplers' and
            // storage buffers' descriptors and its uniform blocks' addresses, each set rounded up
            // to the device's descriptor buffer alignment (64 bytes on NVIDIA). Past it a draw is
            // skipped and the first such frame logged (SokolSystem::reserveDescriptorBytes). A
            // skinned hero of 3 materials in shadow-casting light takes 768 bytes a frame on an
            // NVIDIA GTX 1650 (4-16 byte descriptors; measured by SharedMeshBuffersTest); devices
            // with 32-64 byte image descriptors (AMD) take up to ~6x that. Allocated once per
            // frame in flight (2) in host-visible memory: 32 MB on desktop (~7,000 such heroes
            // with the largest descriptors; 64 MB allocated), 16 MB on Android, iOS and the web
            // (sokol's default; 32 MB allocated).
            int descriptorBytes = defaultDescriptorBytes();
            // slots of WebGPU's bind group cache (a power of 2): a draw whose textures, samplers
            // and storage buffers are not in it creates a bind group (CPU time, never a dropped
            // draw); each skinned mesh has bindings of its own (its bones buffer)
            int bindGroupsCacheSize = 4096;
        };

    private:
        //-----JasperEngine config-----
        static std::vector<Scene*> scenes;
        static std::unordered_set<Scene*> oneTimeScenes;

        static Scene* mainScene;
        
        static int canvasWidth;
        static int canvasHeight;
        
        static int preferredCanvasWidth;
        static int preferredCanvasHeight;
        
        static Rect viewRect;

        static Scaling scalingMode;
        static TextureStrategy textureStrategy;
        
        static bool callMouseInTouchEvent;
        static bool callTouchInMouseEvent;
        static bool useDegrees;

        static bool allowEventsOutCanvas;

        static bool ignoreEventsHandledByUI;

        static bool uiEventReceived;

        static uint64_t lastTime;
        static double updateTimeCount;
        static int fixedStepsLeft;
        
        static double deltatime;
        static double maxDeltatime;
        static float framerate;
        static FrameStats frameStats;
        static FrameStats currentFrameStats;
        
        static double updateTime;

        static double timeScale;
        static double fixedDeltatime;
        static double gameTime;
        static uint64_t gameFrames;
        static std::atomic<bool> simulationHeld;
        static bool clampNextDelta;

        static std::atomic<bool> viewLoaded;
        static std::atomic<bool> paused;
        static std::atomic<bool> focused;

        // Engine.targetFrameRate / vSyncCount and the pacing the platform applies
        static int targetFrameRate;
        static int vSyncCount;
        static FramePacing framePacing;
        // last orientation systemOrientationChanged saw (0: none yet), and since when (system
        // time) a quarter turn waits for its view size (0: none waits)
        static int lastOrientation;
        static double orientationPendingSince;
        static std::atomic<bool> asyncLoading;
        static bool frameRunning;

        static CursorType mouseCursorType;
        static MouseMode mouseMode;

        static Semaphore drawSemaphore;

        // drawSemaphore only excludes async threads from systemDraw(). Main-thread code
        // outside systemDraw() (scene activation, view callbacks, input dispatch) never takes
        // it, so it can still touch scenes/oneTimeScenes while a worker thread is inside
        // executeSceneOnce(). This mutex is what actually guards those two containers.
        // Never block on drawSemaphore while holding it.
        static std::mutex sceneListMutex;

        static Framebuffer* framebuffer;
        // stacked scenes draw here so the frame keeps a single swapchain pass
        static Framebuffer compositeFramebuffer;

        static bool transformCoordPos(float& x, float& y);
        // systemDraw: a high surrogate that never got its low one reaches documents as U+FFFD
        static void flushCharInput();
        // systemDraw: the held gamepad directions a document took repeat (UI clock)
        static void updateGamepadNavigation();
        static void calculateCanvas();
        // logs once that every scene is loaded and no shader build is pending
        // (JASPERENGINE_READY_SIGNAL set: tools/smoke-test.sh waits for it)
        static void signalReadyOnce();
        static void includeScene(Scene* scene);
        // Copy for iteration outside sceneListMutex: callbacks invoked while iterating are
        // free to add or remove scenes without invalidating the caller's traversal.
        static std::vector<Scene*> getScenesSnapshot();
        static bool ensureCompositeFramebuffer();
        static void beginCompositeFramebuffer();
        static void endCompositeFramebuffer();
        static void runFrame(bool draw);
        // recomputes the frame pacing and hands it to the System
        static void applyFramePacing();
        
    public:
        class AsyncThreadScope {
        public:
            AsyncThreadScope() { Engine::startAsyncThread(); }
            ~AsyncThreadScope() { Engine::endAsyncThread(); }

            AsyncThreadScope(const AsyncThreadScope&) = delete;
            AsyncThreadScope& operator=(const AsyncThreadScope&) = delete;
        };

        //-----JasperEngine config-----
        static void setScene(Scene* scene);
        static Scene* getScene();
        static void addSceneLayer(Scene* scene);
        static void executeSceneOnce(Scene* scene);
        static void removeScene(Scene* scene);
        static void removeAllSceneLayers(bool removeOneTimeScenes);
        static void removeAllScenes();

        static bool isSceneRunning(Scene* scene);
        static bool hasScenesToExecuteOnce();
        static Scene* getMainScene();
        static Scene* getLastScene();
        // every running scene: the main scene and its layers (child scenes), bottom first
        static std::vector<Scene*> getSceneLayers();

        static void pauseGameEvents(bool pause);
        
        static int getCanvasWidth();
        static int getCanvasHeight();
        static void setCanvasSize(int canvasWidth, int canvasHeight);
        
        static int getPreferredCanvasWidth();
        static int getPreferredCanvasHeight();
        
        static Rect getViewRect();

        // The canvas size and view rect scalingMode gives a screen of screenWidth x
        // screenHeight: systemViewChanged's rule, for a view other than the Engine's own
        static void computeViewLayout(int screenWidth, int screenHeight, int preferredWidth, int preferredHeight, Scaling scalingMode,
                                      int& canvasWidth, int& canvasHeight, Rect& viewRect);
        
        static void setScalingMode(Scaling scalingMode);
        static Scaling getScalingMode();

        static void setTextureStrategy(TextureStrategy textureStrategy);
        static TextureStrategy getTextureStrategy();
        
        static void setCallMouseInTouchEvent(bool callMouseInTouchEvent);
        static bool isCallMouseInTouchEvent();

        static void setCallTouchInMouseEvent(bool callTouchInMouseEvent);
        static bool isCallTouchInMouseEvent();
        
        static void setUseDegrees(bool useDegrees);
        static bool isUseDegrees();

        static void setAllowEventsOutCanvas(bool allowEventsOutCanvas);
        static bool isAllowEventsOutCanvas();

        static void setIgnoreEventsHandledByUI(bool ignoreEventsHandledByUI);
        static bool isIgnoreEventsHandledByUI();

        static bool isUIEventReceived();
        
        static void setUpdateTimeMS(unsigned int updateTimeMS);
        static void setUpdateTime(float updateTime);
        static float getUpdateTime();

        // Interpolation alpha in [0,1) representing how far between the previous and the next
        // fixed-update step the current rendered frame is. Useful for visual interpolation of
        // physics-driven entities to remove temporal aliasing.
        static double getInterpolationAlpha();

        // Fixed steps still to run in this frame, the current one included (1 outside the fixed phase)
        static int getFixedStepsLeft();

        static void setMouseCursor(CursorType type);
        static CursorType getMouseCursor();

        static void setMouseMode(MouseMode mode);
        static MouseMode getMouseMode();
        static void setMousePosition(float x, float y);
        
        static Platform getPlatform();
        static GraphicBackend getGraphicBackend();
        static bool isOpenGL();
        static float getFramerate();
        static float getDeltatime();

        static void beginFrameStats();
        static void addDrawStats(uint32_t instances, uint64_t triangles);
        static const FrameStats& getFrameStats();

        // Upper bound (in seconds) for the value returned by getDeltatime() and used by the update loop. Default 0.25.
        static float getMaxDeltatime();
        static void setMaxDeltatime(float seconds);

        // Monotonic wall-clock time in seconds, independent of scene pause / update loop.
        static double getSystemTime();

        // Game time scale (Unity's Time.timeScale): multiplies the frame delta given to the
        // update, the fixed-step accumulator (physics), actions, animations, tweens, particles
        // and getDeltatime(). 1 (default) = real time, 0.5 = half speed, 0 = frozen (frames
        // still draw and onUpdate still runs, with a zero delta). Clamped to [0, 100].
        static void setTimeScale(float scale);
        static float getTimeScale();

        // Fixed frame delta: when > 0, every frame advances exactly this many seconds of game
        // time (before timeScale) whatever the wall clock measured, so a run is reproducible
        // and a slow machine plays in slow motion instead of skipping. 0 (default) = the
        // measured frame time (capped by maxDeltatime).
        static void setFixedDeltatime(double seconds);
        static double getFixedDeltatime();

        // Scaled seconds and frames the update loop has run (since start; not while paused or held)
        static double getGameTime();
        static uint64_t getGameFrames();

        // A host (the editor's MCP server) holds the simulation: frames may still draw, but no
        // update runs and no game time passes. Released, the first frame measures at most one
        // fixed step, not the time it was held. Default false.
        static void setSimulationHeld(bool held);
        static bool isSimulationHeld();

        static void setAsyncLoading(bool enable);
        static bool isAsyncLoading();

        static void startAsyncThread();
        static void commitThreadQueue();
        static void endAsyncThread();
        static bool isAsyncThread();
        static bool isViewLoaded();
        // inside systemDraw(): scenes are updating, a stack must not be torn down
        static bool isFrameRunning();

        static void setMaxResourceLoadingThreads(size_t maxThreads);
        static size_t getQueuedResourceCount();

        // GpuResourceLimits: set before the engine starts (the render system is set up with
        // them; later calls apply at the next setup). A pool size outside 1 - 65,535, a
        // per-frame buffer outside 64 KB - 1 GB or a bind group cache size that is not a power
        // of 2 in 2 - 32,768 keeps the default.
        static void setGpuResourceLimits(const GpuResourceLimits& limits);
        static const GpuResourceLimits& getGpuResourceLimits();

        static void setFramebuffer(Framebuffer* framebuffer);
        static Framebuffer* getFramebuffer();
        static void clearPools();
        // Drops pool entries no longer referenced (e.g. after scenes are destroyed on project switch)
        static void clearUnusedPools();

        static void clearAllSubscriptions(bool includeLifecycle);
        // Callbacks scripts left on a scene's components and physics events. Needed before the
        // code behind them goes away, like the editor unloading the project library
        static void clearComponentSubscriptions(Scene* scene);
        static void removeSubscriptionsByTag(const std::string& substring);

#ifdef JASPERENGINE_EDITOR
        // One scene of a secondary view and the camera it is seen through
        struct ViewLayer{
            Scene* scene = nullptr;
            Entity camera = 0;
            // the part of the target the camera covers
            Rect viewRect;
            // draws the scene's editor gizmos and icons
            bool editorOverlays = false;
            // renders at the scene's fixed game resolution when it has one
            bool fixedResolution = false;
            // draws as if the scene had no fog
            bool disableFog = false;
            // canvas an auto-resize camera covers in this view (0: as the camera is);
            // the camera component itself is left alone
            float canvasWidth = 0.0f;
            float canvasHeight = 0.0f;
            // the camera's passes cull no face (the Scene view's debug toggle)
            bool disableFaceCulling = false;
            // the scene's post-processing applies to the camera (the Game view; the Scene view
            // has it off unless its display setting turns it on)
            bool postProcessing = true;
        };

        // Draws a secondary view: the editor's Scene view while the game plays, or its
        // Game view preview. Each layer's scene is drawn through its camera into target,
        // the first one clearing it and the others on top. Draw-only, after this frame's
        // systemDraw(): no scene updates, no view events, and the canvas, view rect and
        // framebuffer of the game stay as they are. viewId keys what each scene caches
        // for the view (see RenderSystem::drawView).
        static void drawView(uint32_t viewId, Framebuffer* target, const std::vector<ViewLayer>& layers);
#endif

        //-----JasperEngine API functions-----
        static void systemInit(int argc, char* argv[], System* system);
        static void systemViewLoaded();
        static void systemViewChanged();
        static void systemDraw();
        // systemDraw() without drawing, for a view that can't be seen (a minimized window)
        static void systemUpdate();
        static void systemViewDestroyed();
        static void systemShutdown();

        // The app goes to the background (pause) and comes back (resume). Idempotent: a
        // second pause or resume in a row does nothing. Leaving, a focused app loses focus
        // first (onFocus(false), then onPause); onResume always comes before onFocus(true).
        static void systemPause();
        static void systemResume();
        // The app became frontmost and gets input (true), or something covers it: a system
        // dialog, the notification shade, another window (false). Idempotent; gaining focus
        // while paused resumes first. Fires onFocus.
        static void systemFocus(bool hasFocus);
        // After a rotation or resize: fires onOrientationChanged when Screen::getOrientation()
        // differs from the last one seen. systemViewChanged calls it after onCanvasChanged; a
        // quarter turn reported before the view size flips waits for it (at most
        // ORIENTATION_SIZE_TIMEOUT seconds, checked every frame), a half turn fires at once.
        static void systemOrientationChanged();
        static constexpr double ORIENTATION_SIZE_TIMEOUT = 0.5;
        // The system is short of memory: onLowMemory, before the platform frees its own caches
        static void systemLowMemory();
        // Play stop / start in the editor: focused, not paused, no orientation seen yet, and the
        // default frame pacing again, without any event (nothing listens any more)
        static void resetApplicationState();

        //-----Frame pacing and application state-----
        // Unity's Application.targetFrameRate: -1 (default) is the platform's rate (60 on
        // mobile, every animation frame on the web, as fast as vsync lets on desktops); 0 and
        // below count as -1, more than 1000 as 1000. Desktops and the editor ignore it while
        // vSyncCount > 0, as Unity. Kept across scene loads.
        static void setTargetFrameRate(int fps);
        static int getTargetFrameRate();
        // Unity's QualitySettings.vSyncCount: present every Nth vblank (0..4; 0 = no vsync).
        // Starts from the project's VSync setting; mobile and the web ignore it.
        static void setVSyncCount(int count);
        static int getVSyncCount();
        // what the two values above give on this platform (PlatformRules::computeFramePacing)
        static const FramePacing& getFramePacing();
        // Unity's Application.isFocused (true at start)
        static bool isFocused();
        static bool isPaused();

        static void systemTouchStart(int pointer, float x, float y);
        static void systemTouchEnd(int pointer, float x, float y);
        static void systemTouchMove(int pointer, float x, float y);
        static void systemTouchCancel();
        // One interrupted finger. Not a release: no pointer-up and no click.
        static void systemTouchCancel(int pointer);

        static void systemMouseDown(int button, float x, float y, int mods);
        static void systemMouseUp(int button, float x, float y, int mods);
        static void systemMouseMove(float x, float y, int mods);
        static void systemMouseScroll(float xoffset, float yoffset, int mods);
        static void systemMouseEnter();
        static void systemMouseLeave();

        static void systemKeyDown(int key, bool repeat, int mods);
        static void systemKeyUp(int key, bool repeat, int mods);

        // One UTF-16 unit where wchar_t is 16 bits (Windows): surrogate pairs are combined
        // before HTML UI documents see them, the game and component UI get each unit
        static void systemCharInput(wchar_t codepoint);
        // Committed text: an IME commit, a paste, a platform string. Documents get the whole
        // string; unless a document consumed it, each code point then goes to onCharInput and
        // the component UI.
        static void systemTextInput(const std::string& utf8);
        // IME composition (preedit) start or update for the focused document text field;
        // cursor in code points from the start of the composition, -1 selects all of it
        static void systemImeComposition(const std::string& utf8, int cursorCodepoints);
        // drops the current composition, restoring the text it replaced
        static void systemImeCancel();
        // a document text field has focus: platforms route IME composition to it
        static bool wantsTextComposition();
        // the caret area of the focused document text field, canvas coordinates; converted
        // to screen pixels for System::setTextInputArea
        static void setTextInputArea(const Rect& canvasRect);

        // Gamepads reach the focused document first, as keys (UIDocumentSystem::eventOnGamepadKeyDown):
        // the buttons, the left stick's directions (pressed past 0.5, released below 0.3) and the
        // triggers (pressed past half way); held d-pad and stick directions repeat after 0.4 s, every
        // 0.1 s. What a document consumed does not reach onGamepadButtonDown/Up and onGamepadAxisMove
        // while ignoreEventsHandledByUI; an axis the documents take is first reset to its rest value
        // for the game. Input's polling functions always read the device
        static void systemGamepadConnect(int gamepad, const std::string& name);
        static void systemGamepadDisconnect(int gamepad);
        static void systemGamepadButtonDown(int gamepad, int button);
        static void systemGamepadButtonUp(int gamepad, int button);
        static void systemGamepadAxisMove(int gamepad, int axis, float value);
        // an element of a document has focus, so the d-pad, the left stick and A go to the UI
        // (a game that polls Input can check it)
        static bool wantsGamepadNavigation();

        //-----JasperEngine user events-----
        // Safe accessor for the Init event
        static FunctionSubscribe<void()>& getOnInit();

        // Existing events (keep these as they are safe for runtime usage)
        static FunctionSubscribe<void()> onViewLoaded;
        static FunctionSubscribe<void()> onViewChanged;
        static FunctionSubscribe<void()> onViewDestroyed;
        static FunctionSubscribe<void()> onDraw;
        static FunctionSubscribe<void()> onUpdate;
        static FunctionSubscribe<void()> onFixedUpdate;
        static FunctionSubscribe<void()> onPostUpdate;
        static FunctionSubscribe<void()> onPause;
        static FunctionSubscribe<void()> onResume;
        static FunctionSubscribe<void()> onShutdown;
        // systemFocus: the app gained (true) or lost (false) focus
        static FunctionSubscribe<void(bool)> onFocus;
        // the interface rotated: the new ScreenOrientation, after onCanvasChanged
        static FunctionSubscribe<void(int)> onOrientationChanged;
        // the system is short of memory (Android, iOS)
        static FunctionSubscribe<void()> onLowMemory;
        // The stack loaded by SceneManager is ready, before its loading scene closes.
        static FunctionSubscribe<void()> onSceneLoaded;
        static FunctionSubscribe<void(int,float,float)> onTouchStart;
        static FunctionSubscribe<void(int,float,float)> onTouchEnd;
        static FunctionSubscribe<void(int,float,float)> onTouchMove;
        // One finger was interrupted. Not a pointer-up, so it does not click.
        static FunctionSubscribe<void(int,float,float)> onTouchCancel;
        static FunctionSubscribe<void(int,float,float,int)> onMouseDown;
        static FunctionSubscribe<void(int,float,float,int)> onMouseUp;
        static FunctionSubscribe<void(float,float,int)> onMouseScroll;
        static FunctionSubscribe<void(float,float,int)> onMouseMove;
        static FunctionSubscribe<void()> onMouseEnter;
        static FunctionSubscribe<void()> onMouseLeave;
        static FunctionSubscribe<void(int,bool,int)> onKeyDown;
        static FunctionSubscribe<void(int,bool,int)> onKeyUp;
        static FunctionSubscribe<void(wchar_t)> onCharInput;
        static FunctionSubscribe<void(int)> onGamepadConnect;
        static FunctionSubscribe<void(int)> onGamepadDisconnect;
        static FunctionSubscribe<void(int,int)> onGamepadButtonDown;
        static FunctionSubscribe<void(int,int)> onGamepadButtonUp;
        static FunctionSubscribe<void(int,int,float)> onGamepadAxisMove;

    };
    
}

#endif /* engine_h */
