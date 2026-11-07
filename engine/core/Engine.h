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

// Point and spot lights a lit particle system takes per vertex (besides the main directional
// light and the ambient), the ones nearest its bounds
#ifndef MAX_PARTICLE_LIGHTS
#define MAX_PARTICLE_LIGHTS 4
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

// Projective shadow atlas (directional + spot). Each slot holds one 2D depth map (a spot
// light, or a single directional cascade) of its own resolution, packed into the atlas by
// ShadowSlotPlanner. Shaders compute the shadow coordinates in the fragment stage, but forks
// written before that still write one v_lightProjPos vertex varying per slot (and every slot
// costs 9 fragment uniform vectors), so keep the count small: a 6x6 / 36-slot count overflows
// the varyings and breaks such shaders.
#ifndef SHADOW_ATLAS_COLS
#define SHADOW_ATLAS_COLS 3
#endif

#ifndef SHADOW_ATLAS_ROWS
#define SHADOW_ATLAS_ROWS 3
#endif

#ifndef MAX_SHADOW_ATLAS_SLOTS
#define MAX_SHADOW_ATLAS_SLOTS (SHADOW_ATLAS_COLS * SHADOW_ATLAS_ROWS)
#endif

// Point shadow atlas (omnidirectional), a depth atlas like the projective one. Each
// shadow-casting point light consumes SHADOW_CUBE_FACES consecutive slots (one per cube face)
// and is sampled only in the fragment shader, so this atlas is bounded by texture size.
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

// detail levels per submesh, level 0 being the source geometry
#ifndef MAX_MESH_LODS
#define MAX_MESH_LODS 4
#endif

// main font of a text plus its fallbacks
#ifndef MAX_TEXT_FONTS
#define MAX_TEXT_FONTS 4
#endif

#ifndef MAX_MORPHTARGETS
#define MAX_MORPHTARGETS 8
#endif

// number of hemisphere samples in the SSAO kernel; must match the
// SSAO_KERNEL_SIZE define injected for the ssao.frag shader (ShaderBuilder)
#ifndef SSAO_KERNEL_SIZE
#define SSAO_KERNEL_SIZE 32
#endif

#ifndef MAX_BROADPHASELAYER_3D
#define MAX_BROADPHASELAYER_3D 6
#endif

#include "util/CanvasScaler.h"
#include "Export.h"
#include "render/GpuCreation.h"
#include "System.h"
#include "util/FunctionSubscribe.h"
#include "math/Rect.h"
#include "util/ThreadUtils.h"
#include "texture/Framebuffer.h"
#include "ecs/Entity.h"
#include "render/AtmosphereQuality.h"
#include "pixelart/PixelPerfectLayout.h"
#include "math/Vector2.h"
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
    class Scheduler;

    class JASPERENGINE_API Scene;

    enum class Scaling{
        FITWIDTH,
        FITHEIGHT,
        LETTERBOX,
        CROP,
        STRETCH,
        NATIVE,
        // whole-number scales of the preferred canvas (S41, pixelart::computePixelPerfectLayout at
        // PPU 1): bars around it, or a canvas that grows to fill the screen (bars under the scale)
        INTEGER,
        INTEGER_EXPAND
    };

    // What a Pixel Perfect Camera on the main scene's main camera asks of the Engine's view
    // (RenderSystem sets it, S41 R3): its layout replaces the Scaling's canvas and view rect
    struct PixelPerfectViewRequest {
        bool active = false;
        int referenceWidth = 0;     // the base resolution (the preferred canvas when 0)
        int referenceHeight = 0;
        PixelUpscaleMode mode = PixelUpscaleMode::INTEGER;
        bool smoothing = false;
        int pixelsPerUnit = 1;

        bool operator==(const PixelPerfectViewRequest& other) const {
            return active == other.active && referenceWidth == other.referenceWidth && referenceHeight == other.referenceHeight &&
                   mode == other.mode && smoothing == other.smoothing && pixelsPerUnit == other.pixelsPerUnit;
        }
        bool operator!=(const PixelPerfectViewRequest& other) const { return !(*this == other); }
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

    // Project Settings > Player > Threading (project.yaml `threading:`). -1 is Auto.
    struct ThreadingSettings {
        int workers = -1;                // job system workers: applies when it starts (next launch)
        int reservedFrameWorkers = -1;   // workers kept free of Background work: applies live
        int ioThreads = -1;              // asset reader threads: applies when the reader service starts
        double webPumpBudgetMs = 4.0;    // single-thread mode: Background work per frame, live
    };

    class JASPERENGINE_API Engine {
    public:
        // draw calls, instances and triangles submitted by the last systemDraw
        struct FrameStats{
            uint32_t drawCalls = 0;
            uint32_t instances = 0;
            uint64_t triangles = 0;
            // mesh draws the passes' visible lists kept and culled (spec S55 R4)
            uint32_t visible = 0;
            uint32_t culled = 0;
            uint64_t vertices = 0;
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
        // desktop, 16 MB mobile/web; streamStagingBytes 48 MB / 16 MB; descriptorBytes 64 MB / 16 MB
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
            // with 32-64 byte image descriptors (AMD) take up to ~6x that; S10's cascades draw it in more passes
            // (15,120 bytes on lavapipe). Allocated once per frame in flight (2) in host-visible memory: 64 MB
            // on desktop (~4,400 such heroes with the largest descriptors; 128 MB allocated), 16 MB on Android,
            // iOS and the web (sokol's default; 32 MB allocated).
            int descriptorBytes = defaultDescriptorBytes();
            // slots of WebGPU's bind group cache (a power of 2): a draw whose textures, samplers
            // and storage buffers are not in it creates a bind group (CPU time, never a dropped
            // draw); each skinned mesh has bindings of its own (its bones buffer)
            int bindGroupsCacheSize = 4096;
        };

    private:
        // TEST-ONLY seam: a view loaded without a GPU (canvas refresh tests)
        friend struct EngineTestAccess;

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
        // S41: the project's Pixels Per Unit, the Pixel Perfect Camera's request, its layout and
        // the Game view's Pixel Scale; the screen pixels the smoothed image is shifted by
        static int pixelsPerUnit;
        static PixelPerfectViewRequest pixelPerfectRequest;
        static pixelart::PixelPerfectLayout pixelPerfectLayout;
        static int pixelPerfectForcedScale;
        static Vector2 viewShift;
        static void applyPixelPerfectView(int screenWidth, int screenHeight);
        // the project's Canvas Scaler (Project Settings > Graphics > UI Scale)
        static CanvasScalerSettings defaultCanvasScaler;
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
        static double framePeriod;
        static double frameCpuTime;
        static double frameDrawCpuTime;
        static FrameStats frameStats;
        static FrameStats currentFrameStats;
        
        static double updateTime;

        static double timeScale;
        static double fixedDeltatime;
        static double gameTime;
        // the shader clock of the frame (latchShaderTime), and whether it last advanced by fixedDeltatime
        static double shaderTime;
        static bool shaderTimeFixed;
        // setPinnedShaderTime's time, negative when not pinned
        static double pinnedShaderTime;
        // read by Log from any thread (the frame of a line)
        static std::atomic<uint64_t> gameFrames;
        static double unscaledDeltatime;
        static double unscaledTime;
        static double fixedTime;
        static double levelLoadGameTime;
        static uint64_t startupTime;
        static bool inFixedStep;
        static float smoothDeltatime;
        static std::atomic<bool> simulationHeld;
        static bool clampNextDelta;
        // pauseGameEvents: the editor's Pause (and edit mode), which stops the scheduler's game clocks
        static bool gameEventsPaused;

        static std::atomic<bool> viewLoaded;
        static std::atomic<bool> paused;
        // the two holds of `paused` (main thread)
        static bool osPaused;
        static bool nativePaused;
        static void applyPause();
        static std::atomic<bool> focused;
        // systemVisibilityChanged: the window can be seen
        static std::atomic<bool> visible;

        // Dynamic Batching: what draws now (Engine.dynamicBatching) and the project's setting it
        // goes back to with the application state
        static bool dynamicBatching;
        static bool dynamicBatchingDefault;

        // Engine.targetFrameRate / vSyncCount and the pacing the platform applies
        static int targetFrameRate;
        static int vSyncCount;
        static FramePacing framePacing;
        // Engine.setTextureQuality's value (the pool drops QualityResolver's resolution of it)
        static int textureQuality;
        // the QualityResolver revision the frame pacing, texture quality and upload budget follow
        static uint32_t qualityRevision;
        // last orientation systemOrientationChanged saw (0: none yet), and since when (system
        // time) a quarter turn waits for its view size (0: none waits)
        static int lastOrientation;
        static double orientationPendingSince;
        static std::atomic<bool> asyncLoading;
        static bool frameRunning;
        static bool developmentBuild;

        static CursorType mouseCursorType;
        static MouseMode mouseMode;
        // the platform window has the keyboard (systemFocusChanged)
        static bool windowFocused;

        // the atmosphere quality tier of each platform group (desktop, mobile, web) and the
        // game's runtime override, if any
        static AtmosphereQuality atmosphereQualityDefaults[3];
        static bool atmosphereQualityOverridden;
        static AtmosphereQuality atmosphereQualityOverride;
        static float foliageDistanceScaleDefaults[3];
        static bool foliageDistanceScaleOverridden;
        static float foliageDistanceScaleOverride;
        static bool gpuInstancingEnabled;

        static Semaphore drawSemaphore;

        // drawSemaphore only excludes async threads from systemDraw(). Main-thread code
        // outside systemDraw() (scene activation, view callbacks, input dispatch) never takes
        // it, so it can still touch scenes/oneTimeScenes while a worker thread is inside
        // executeSceneOnce(). This mutex is what actually guards those two containers.
        // Never block on drawSemaphore while holding it.
        static std::mutex sceneListMutex;

        static Framebuffer* framebuffer;
        // stacked scenes draw here so the frame keeps a single swapchain pass, and so does the
        // capture frame of a --screenshot run (docs/specs/S30-shipping.md R20), read back after it
        static Framebuffer compositeFramebuffer;
        static bool captureFrame;
        // the composite of the frame that just drew, for the run control's screenshot
        static bool readCompositeFrame(std::vector<uint8_t>& rgba, int& width, int& height, std::string& error);

        static bool transformCoordPos(float& x, float& y);
        // ui-multitouch D6: a pointer ends without a click in the documents and the component
        // UI of every scene (a finger or mouse button released outside the canvas; touch false:
        // pointerOrButton is the mouse button), or every pointer (pause, focus loss)
        static void cancelUIPointer(int pointerOrButton, bool touch);
        static void cancelAllUIPointers();
        // systemDraw: a high surrogate that never got its low one is typed as U+FFFD
        static void flushCharInput();
        // whole typed code points to Input.inputString, the documents, onCharInput and the
        // component UI (systemCharInput, flushCharInput)
        static void deliverCodepoints(const std::vector<uint32_t>& codepoints);
        // systemDraw: the held gamepad directions a document took repeat (UI clock)
        static void updateGamepadNavigation();
        // the canvas and view rect for the current screen, then what follows them: every scene's
        // and the debug overlay's camera (Canvas Scaler), Screen.safeArea; with notify, the events
        // of a view change when the canvas or the view rect changed (a nested call does not notify)
        static void refreshCanvas(bool notify);
        // logs once that every scene is loaded and no shader build is pending
        // (JASPERENGINE_READY_SIGNAL set: tools/smoke-test.sh waits for it)
        static void signalReadyOnce();
        static void includeScene(Scene* scene);
        // Copy for iteration outside sceneListMutex: callbacks invoked while iterating are
        // free to add or remove scenes without invalidating the caller's traversal.
        static std::vector<Scene*> getScenesSnapshot();
        // the snapshot with the debug overlay's Scene on top when it has one (keys: only while
        // a field of the overlay has the focus)
        static std::vector<Scene*> getInputScenes(bool keys);
        static bool ensureCompositeFramebuffer();
        static void beginCompositeFramebuffer();
        static void endCompositeFramebuffer();
        static void runFrame(bool draw);
        // recomputes the frame pacing and hands it to the System
        static void applyFramePacing();
        // a new device tier, thermal step or memory drop: the pacing, texture quality and upload
        // budget again (once a frame, one compare when nothing changed)
        static void applyQualityChanges();
        // desktop builds: JASPERENGINE_SIMULATE_SAFE_AREA as the platform's insets
        static void applySimulatedSafeArea();
        
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
        // The scene drawn first, which clears the frame: the main scene, or before there is one
        // (a loading scene covering the start scene) the bottom running scene
        static Scene* getBaseScene();
        static Scene* getLastScene();
        // every running scene: the main scene and its layers (child scenes), bottom first
        static std::vector<Scene*> getSceneLayers();

        static void pauseGameEvents(bool pause);
        static bool areGameEventsPaused();
        // One frame of the scheduler's Game (scaled), Unscaled and Frames clocks, unless the
        // game is paused, held or its events are (the editor's Pause). True when they ran.
        static bool advanceGameClocks(Scheduler& scheduler, double frameTime);
        
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

        // Project Settings > Graphics > Pixels Per Unit (1-256, default 1: a world unit is a base
        // pixel). Sprites and tilemaps with a pixelsPerUnit of 0 take it, and so does a Pixel
        // Perfect Camera's span (docs/pixel-art.md)
        static void setPixelsPerUnit(int pixelsPerUnit);
        static int getPixelsPerUnit();

        // The layout a Pixel Perfect Camera asks for (S41): on, it replaces the Scaling's canvas
        // (B / PPU, rounded to whole units) and view rect (the integer viewport); a change
        // recomputes them at once. RenderSystem sets it from the main scene's main camera.
        static void setPixelPerfectView(const PixelPerfectViewRequest& request);
        static const PixelPerfectViewRequest& getPixelPerfectView();
        // what the Pixel Perfect Camera on that camera asks of a view (false: none, disabled, or a
        // perspective camera): RenderSystem, the editor's Game view and MCP lay a view out with it
        static bool getPixelPerfectRequest(Scene* scene, Entity cameraEntity, PixelPerfectViewRequest& request);
        // the layout in effect (an empty one while no Pixel Perfect Camera drives the view)
        static const pixelart::PixelPerfectLayout& getPixelPerfectLayout();
        // the request's layout on a screen of that size (the editor's Game view, MCP)
        static pixelart::PixelPerfectLayout computePixelPerfectView(const PixelPerfectViewRequest& request, int screenWidth,
                                                                    int screenHeight, int forcedScale);
        // caps the scale (the Game view's Pixel Scale, 0 = Auto)
        static void setPixelPerfectForcedScale(int scale);
        static int getPixelPerfectForcedScale();
        // the screen pixels (x right, y down) the drawn image is moved by (sub-pixel smoothing):
        // input maps through the same shift, so a click hits what is seen
        static void setViewShift(Vector2 shift);
        static Vector2 getViewShift();

        // The UI's own space, the project default each scene uses unless it overrides it
        // (Scene::setCanvasScaler). FollowGameCanvas (the default) lays the UI out in the game
        // canvas above. A change recomputes every running scene's UI size.
        static void setDefaultCanvasScaler(const CanvasScalerSettings& settings);
        static const CanvasScalerSettings& getDefaultCanvasScaler();

        static void setTextureStrategy(TextureStrategy textureStrategy);
        static TextureStrategy getTextureStrategy();

        // Cooked textures (spec S14 R12, docs/textures.md): quality 0 (full) to 3 (an eighth)
        // drops top mips of streamable textures, like Unity's Global Mipmap Limit; the memory
        // budget (MB, 0 = none) drops more mips of the largest ones until the resident bytes fit
        static void setTextureQuality(int quality);
        static int getTextureQuality();
        static void setTextureMemoryBudget(int megabytes);
        static int getTextureMemoryBudget();
        // resident texture bytes the budget counts
        static uint64_t getTextureMemoryUsage();
        // RGB565 / RGBA4 when a GL-family device samples none of a texture's formats
        static void setTextureLowMemoryFallback(bool enabled);
        static bool isTextureLowMemoryFallback();
        // "BC7", "ASTC_6x6"...: the device samples it with filtering
        static bool isTextureFormatSupported(const std::string& format);
        // Project Settings > Texture per platform group (desktop, mobile, web): the generated
        // init applies the group this build runs on
        struct TexturePlatformDefaults {
            int quality = 0;
            int memoryBudgetMB = 0;
            bool lowMemoryFallback = false;
        };
        static void setTextureDefaults(const TexturePlatformDefaults& desktop, const TexturePlatformDefaults& mobile,
                                       const TexturePlatformDefaults& web);

        // every scene drawn is loaded (its HTML UI documents too), no shader build is pending and
        // no texture is still loading (textures false: whatever the texture loads, as the texture
        // pool's own frame budget asks)
        static bool areScenesLoaded(bool textures = true);
        // what keeps areScenesLoaded() false, for a person: the shaders still building, each
        // scene's objects not loaded yet (a few, then how many more), its HTML UI documents
        static std::string describeNotLoaded();
        
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

        // MeshRaycast and the editor's picking test the triangles of meshes up to this many
        // (default 2M); larger ones answer with their bounds (docs/scripting/mesh-raycast.md)
        static void setMeshRaycastMaxTriangles(unsigned int maxTriangles);
        static unsigned int getMeshRaycastMaxTriangles();

        static bool isUIEventReceived();
        // Input.isPointerOverUI (Unity's EventSystem.IsPointerOverGameObject, ui-multitouch D15):
        // that pointer (-1 / -2 / -3 the mouse, >= 0 a finger; INT_MIN the mouse or any finger)
        // is over a component UI element that takes events or HTML content, or either holds it
        static bool isPointerOverUI(int pointerId);
        
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

        // CAPTURED locks the pointer: Input::getPointerLockState is LOCKED once the platform
        // did it (a browser waits for a click or key press: PENDING until then)
        static void setMouseMode(MouseMode mode);
        static MouseMode getMouseMode();
        // Warps the pointer (canvas pixels); the next move's Input::getMouseDelta starts there
        static void setMousePosition(float x, float y);
        static bool isWindowFocused();
        
        static Platform getPlatform();
        static GraphicBackend getGraphicBackend();

        // Atmosphere quality (volumetric fog and clouds, docs/specs/S12-atmosphere.md): the
        // project's tier per platform group (Project Settings > Quality; desktop High, mobile
        // and web Low by default), which a game may override at runtime. getAtmosphereQuality
        // is the tier in effect: the override, else this platform's default.
        static void setAtmosphereQualityDefaults(AtmosphereQuality desktop, AtmosphereQuality mobile, AtmosphereQuality web);
        static AtmosphereQuality getAtmosphereQualityDefault(Platform platform);
        static AtmosphereQuality getAtmosphereQualityDefault();
        static void setAtmosphereQuality(AtmosphereQuality quality);
        static AtmosphereQuality getAtmosphereQuality();
        // back to the platform default (the editor calls it when Play stops)
        static void resetAtmosphereQuality();
        static bool isAtmosphereQualityOverridden();

        // Foliage Distance (Unity's QualitySettings.terrainDetailDistanceFactor): multiplies every
        // terrain foliage layer's Draw Distance and Min Draw Distance. The project's scale per
        // platform group (Project Settings > Quality, 1 by default), which a game may override at
        // runtime (0 draws no foliage; clamped to [0, 10], non-finite values ignored).
        // getFoliageDistanceScale is the scale in effect: the override, else this platform's default.
        static void setFoliageDistanceScaleDefaults(float desktop, float mobile, float web);
        static float getFoliageDistanceScaleDefault(Platform platform);
        static float getFoliageDistanceScaleDefault();
        static void setFoliageDistanceScale(float scale);
        static float getFoliageDistanceScale();
        // back to the platform default (the editor calls it when Play stops)
        static void resetFoliageDistanceScale();
        // GPU Instancing (Project Settings > Graphics, spec S55): MeshComponent draws of one mesh,
        // material and pipeline become instanced draws, and constant state is applied once. On by
        // default; off submits every draw as before S55
        static void setGpuInstancingEnabled(bool enabled);
        static bool isGpuInstancingEnabled();
        static bool isOpenGL();
        static float getFramerate();
        static float getDeltatime();
        // the last frame's wall-clock period (from the frame before's start to its start) and
        // the CPU time it took from its start to its submission (the update and the draw,
        // without the wait to present), seconds: dynamic resolution's frame law reads them
        static double getFramePeriod();
        static double getFrameCpuTime();
        // of it, the scenes' draw and the commit: the render submission (benchmark renderCpuMs)
        static double getFrameDrawCpuTime();

        static void beginFrameStats();
        // vertices: the vertex shader runs, indices or vertices drawn times the instances
        static void addDrawStats(uint32_t instances, uint64_t triangles, uint64_t vertices = 0);
        static void addVisibilityStats(uint32_t visible, uint32_t culled);
        static const FrameStats& getFrameStats();
        // the draw calls of the frame being drawn, so far
        static uint32_t getFrameDrawCalls();
        // the draws submitted so far in the frame being drawn (a pass's own count: before and after)
        static uint32_t getPendingDrawCalls();

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

        // The shader clock (frame.time.x of shaders, the waves of library:water): latched once a
        // frame (at the top of systemDraw for a game, by the editor's loop for it), so every fixed
        // step of the frame, its draw and the CPU's waves (Buoyancy, Water.sample) agree. It is the
        // wall clock (getSystemTime) when it latches, or, with a fixed frame delta, advances by
        // exactly that a frame from where the clock was when fixed mode began: captures under
        // `play fixed_fps` are reproducible.
        static void latchShaderTime();
        static float getShaderTime();
        static double getShaderTimeSeconds();
        // Pins the shader clock at `seconds` (golden image tests): from now on every latch keeps
        // it there, whatever the wall clock or the fixed delta. A negative value unpins it: the
        // next latch reads the clock again (a fixed delta then starts from the pinned time).
        static void setPinnedShaderTime(double seconds);
        // whether the shader clock is pinned, and at what (Scene::getSceneTime follows it)
        static bool hasPinnedShaderTime();
        static double getPinnedShaderTime();

        // A shipped game started with --headless (docs/specs/S30-shipping.md R17): no window shown,
        // no audio output, no input; frames still render
        static bool isHeadless();
        // Ends the game after this frame with that process exit code (0: success; R19's codes
        // mean what they say there); the first request wins
        static void quit(int code = 0);
        // the "Engine ready:" line was logged (every scene loaded, no shader build pending)
        static bool isReadySignalled();

        // A development build (Unity's Debug.isDebugBuild): the editor's Play and exports built
        // with the Debug build type (JASPERENGINE_DEVELOPMENT_BUILD, set by the generated init).
        // The debug overlay's "development" mode follows it
        static void setDevelopmentBuild(bool development);
        static bool isDevelopmentBuild();
        // Scaled seconds and frames the update loop has run (since start; not while paused or held)
        static double getGameTime();
        static uint64_t getGameFrames();

        // The frame delta before timeScale (Unity's Time.unscaledDeltaTime): the measured frame
        // time capped by maxDeltatime, or the fixed frame delta when set; 0 while the app is
        // paused, the simulation held or the game events paused (the editor's Play pause). It keeps
        // running at time scale 0 (a pause menu).
        static float getUnscaledDeltatime();
        // Sum of getUnscaledDeltatime() over the frames the update loop has run
        static double getUnscaledTime();
        // Sum of the fixed steps run (game seconds); inside a step, the time at its start
        static double getFixedTime();
        // Wall clock seconds since Engine::systemInit (pauses included)
        static double getRealtimeSinceStartup();
        // Game seconds since the last scene load (SceneManager)
        static double getTimeSinceLevelLoad();
        // A scene stack was loaded now (SceneManager): getTimeSinceLevelLoad() counts from here
        static void markLevelLoad();
        // True inside a fixed step: onFixedUpdate, the scenes' fixed update (physics) and the
        // coroutines waiting for it
        static bool isInFixedStep();
        // Exponential average of getDeltatime() (Unity's Time.smoothDeltaTime)
        static float getSmoothDeltatime();

        // A host (the editor's MCP server) holds the simulation: frames may still draw, but no
        // update runs and no game time passes. Released, the first frame measures at most one
        // fixed step, not the time it was held. Default false.
        static void setSimulationHeld(bool held);
        static bool isSimulationHeld();

        static void setAsyncLoading(bool enable);
        static bool isAsyncLoading();

        // At a frame's start: the shaders and pipelines created off the render thread that are
        // ready, and a step of the shader warm-up list
        static void updateGpuCreation();

        static void startAsyncThread();
        static void commitThreadQueue();
        static void endAsyncThread();
        static bool isAsyncThread();
        static bool isViewLoaded();
        // inside systemDraw(): scenes are updating, a stack must not be torn down
        static bool isFrameRunning();

        // The most workers Background (asset) jobs may occupy at once: the job system's gate,
        // changed live. 0 returns to the reserve-derived maximum.
        static void setMaxResourceLoadingThreads(size_t maxThreads);
        // Asset jobs queued or running, queued reads included: 0 only once loading is idle.
        static size_t getQueuedResourceCount();

        // Starts the job system and the asset reader service with these settings when they
        // are not running yet; once running, the reserve and the pump budget apply live and
        // the thread counts on the next launch. The generated game main calls it first.
        static void setThreadingSettings(const ThreadingSettings& settings);
        static const ThreadingSettings& getThreadingSettings();

        // GpuResourceLimits: set before the engine starts (the render system is set up with
        // them; later calls apply at the next setup). A pool size outside 1 - 65,535, a
        // per-frame buffer outside 64 KB - 1 GB or a bind group cache size that is not a power
        // of 2 in 2 - 32,768 keeps the default.
        static void setGpuResourceLimits(const GpuResourceLimits& limits);
        static const GpuResourceLimits& getGpuResourceLimits();
        // How shaders and pipelines are created (spec S32); the generated game main calls it when
        // the project's differ from the defaults. The mode applies when the view loads
        static void setShaderCreationSettings(const ShaderCreationSettings& settings);
        static const ShaderCreationSettings& getShaderCreationSettings();

        static void setFramebuffer(Framebuffer* framebuffer);
        static Framebuffer* getFramebuffer();
        static void clearPools();
        // Drops pool entries no longer referenced (e.g. after scenes are destroyed on project switch)
        static void clearUnusedPools();

        static void clearAllSubscriptions(bool includeLifecycle);
        // Callbacks scripts left on a scene's components and physics events. Needed before the
        // code behind them goes away, like the editor unloading the project library
        static void clearComponentSubscriptions(Scene* scene);
        // Deprecated (kept one release for C++ glue generated before owner keys): scans every
        // event for tags containing substring. Use SubscriptionOwnerIndex::releaseOwner.
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
            // draws without the volumetric clouds (the Scene view's Clouds toggle)
            bool disableClouds = false;
            // the atmosphere quality the view previews (the Game view's; -1 the project's)
            int atmosphereQuality = -1;
            // casts and receives the scene's shadows (off: an inactive Scene view pane, S26 R20)
            bool shadows = true;
            // the Scene view's Draw Mode (RenderSystem::ViewDrawMode; 0 Shaded)
            uint8_t drawMode = 0;
            // the Scene view's Snap Preview (S41 R21): view pixels per base pixel of its own grid
            // (0: none; a scene without a snapping Pixel Perfect Camera has nothing to preview)
            float snapPreviewScale = 0.0f;
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
        // startProject false leaves the project's main.lua and C++ init() (which load the first
        // scene) to a later systemStartProject(): the web build waits for its saved files first
        static void systemInit(int argc, char* argv[], System* system, bool startProject = true);
        static void systemStartProject();
        static void systemViewLoaded();
        static void systemViewChanged();
        static void systemDraw();
        // the platform presented the frame systemDraw drew (Android: after its buffer swap).
        // With the ready signal on, logs "Engine presented: N frames since ready" once N frames
        // reached the screen after the ready line (tools/android-emulator-test.sh screenshots then)
        static void systemFramePresented();
        // systemDraw() without drawing, for a view that can't be seen (a minimized window). On
        // Metal it first waits for the GPU (SystemRender::waitGpuIdle): a frame without passes
        // frees what earlier frames released without waiting for them
        static void systemUpdate();
        // Nothing when no view is loaded (a launch that failed before systemViewLoaded, a second
        // destroy)
        static void systemViewDestroyed();
        static void systemShutdown();

        // The app goes to the background (pause) and comes back (resume). Idempotent: a
        // second pause or resume in a row does nothing. Leaving, a focused app loses focus
        // first (onFocus(false), then onPause); onResume always comes before onFocus(true).
        // The engine is paused while the OS or a native SDK (setNativePause) holds it: one
        // onPause when the first hold starts, one onResume when the last ends.
        static void systemPause();
        static void systemResume();
        // A native SDK's full-screen view (NativeBridge::pauseEngine/resumeEngine, applied
        // each frame by NativeBridge::update); false at Play stop. Idempotent.
        static void setNativePause(bool held);
        // The app became frontmost and gets input (true), or something covers it: a system
        // dialog, the notification shade, another window (false). Idempotent; gaining focus
        // while the OS holds the pause resumes first. Fires onFocus.
        static void systemFocus(bool hasFocus);
        // The platform's window can (not) be seen: minimized, unmapped or hidden by the window
        // manager, fully covered (macOS), a hidden page (web). Desktop platforms then call
        // systemUpdate at getHiddenUpdateFps() instead of systemDraw; the web pauses as before.
        // Idempotent; logs the change and fires onVisibilityChanged.
        static void systemVisibilityChanged(bool visible);
        // After a rotation or resize: fires onOrientationChanged when Screen::getOrientation()
        // differs from the last one seen. systemViewChanged calls it after onCanvasChanged; a
        // quarter turn reported before the view size flips waits for it (at most
        // ORIENTATION_SIZE_TIMEOUT seconds, checked every frame), a half turn fires at once.
        static void systemOrientationChanged();
        static constexpr double ORIENTATION_SIZE_TIMEOUT = 0.5;
        // a quarter turn is known and waits for its view size (Screen holds the safe-area insets
        // the platform pushes meanwhile: they belong to the new size)
        static bool isOrientationPending();
        // The system is short of memory: onLowMemory, before the platform frees its own caches
        static void systemLowMemory();
        // Play stop / start in the editor: focused, visible, not paused (neither hold), no orientation
        // seen yet, and the default frame pacing again, without any event (nothing listens any
        // more)
        static void resetApplicationState();

        //-----Dynamic Batching (docs/rendering/batching.md)-----
        // Consecutive Sprites, small meshes and UI elements that share pipeline, textures and
        // their unbaked values are drawn as one draw call (the same image: turn it off to compare).
        // The project's setting (Project Settings > Graphics > Dynamic Batching) is the default the
        // application state goes back to; Engine.dynamicBatching changes it while the game runs.
        static void setDynamicBatchingDefault(bool enabled);
        static void setDynamicBatching(bool enabled);
        static bool isDynamicBatching();

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
        // false while the window can't be seen (systemVisibilityChanged); true at start and on
        // Android and iOS, whose pause is the signal there
        static bool isVisible();
        // the update rate of a hidden window: the game's own frame rate (targetFrameRate, or the
        // display's refresh rate over vSyncCount), at most 60 (PlatformRules::hiddenUpdateFps)
        static double getHiddenUpdateFps();

        // The platform window gained or lost the keyboard focus. On a loss what is held comes
        // up (key, mouse button: their events and Input edges) and the fingers are cancelled,
        // so nothing stays pressed; a captured pointer is PENDING until the focus comes back.
        static void systemFocusChanged(bool focused);
        // The browser's pointer lock became active or ended (pointerlockchange)
        static void systemPointerLockChanged(bool locked);

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

        // One UTF-16 unit (Windows WM_CHAR, Android's keyboard) or one whole code point:
        // surrogate pairs are combined here, so documents, Input.inputString, onCharInput and the
        // component UI all get whole code points (a lone surrogate is U+FFFD)
        static void systemCharInput(char32_t unit);
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
        // an element of a document has focus, or a component UI Button is selected, so the d-pad,
        // the left stick and A go to the UI (a game that polls Input can check it)
        static bool wantsGamepadNavigation();
        // a component UI Button is selected: the arrows, Tab, Enter, keypad Enter and Space are the UI's
        static bool wantsKeyboardNavigation();
        // both at once, from one look at the scenes (Input::beginFrame)
        static void getUINavigationWants(bool& gamepad, bool& keyboard);
        // a text field has keyboard focus (a native InputField or a document text control): the
        // keyboard does not drive input actions then (Input::beginFrame)
        static bool isTextInputFocused();

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
        // systemVisibilityChanged: the window can (true) or can't (false) be seen
        static FunctionSubscribe<void(bool)> onVisibilityChanged;
        // the interface rotated: the new ScreenOrientation, after onCanvasChanged
        static FunctionSubscribe<void(int)> onOrientationChanged;
        // the system is short of memory (Android, iOS)
        static FunctionSubscribe<void()> onLowMemory;
        // Screen.safeArea changed (a rotation, the platform's insets, Screen.setExtraInsets): once
        // per change, after onCanvasChanged and onOrientationChanged
        static FunctionSubscribe<void()> onSafeAreaChanged;
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
        // one whole character per call, on every platform (a one-character string in Lua). A C++
        // handler declared with wchar_t still compiles but is deprecated: 16 bits on Windows.
        static FunctionSubscribe<void(char32_t)> onCharInput;
        static FunctionSubscribe<void(int)> onGamepadConnect;
        static FunctionSubscribe<void(int)> onGamepadDisconnect;
        static FunctionSubscribe<void(int,int)> onGamepadButtonDown;
        static FunctionSubscribe<void(int,int)> onGamepadButtonUp;
        static FunctionSubscribe<void(int,int,float)> onGamepadAxisMove;
        // An input action's button went down / up in this frame (Project Settings > Input), with
        // the action's name; fired before onUpdate (Input::beginFrame)
        static FunctionSubscribe<void(const std::string&)> onButtonDown;
        static FunctionSubscribe<void(const std::string&)> onButtonUp;

    };
    
}

#endif /* engine_h */
