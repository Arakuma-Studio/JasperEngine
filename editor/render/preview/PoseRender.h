// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Offscreen pose rendering (spec: pose animation phase 3, section 2): a private scene with an
// orthographic camera, a key and a fill light, a floor and one model instance, drawn with any
// bone transforms from any direction into an offscreen framebuffer and read back to a
// PoseImage. The scene is never added to the Engine, so the open scenes, the Scene view and
// the Game view are left alone; it is drawn as a secondary view (Engine::drawView) on the main
// thread, from the editor's frame (processFrame) or, when everything is loaded, synchronously.

#pragma once

#include "Scene.h"
#include "math/AABB.h"
#include "math/Quaternion.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "object/Camera.h"
#include "object/Light.h"
#include "object/LineRenderer.h"
#include "object/Model.h"
#include "object/Primitive.h"
#include "texture/Framebuffer.h"

#include "pose/PoseImage.h"

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor{

    class PoseRender{
    public:
        struct Style{
            // the scene's clear colour (sRGB)
            Vector4 background = Vector4(0.16f, 0.16f, 0.17f, 1.0f);
            // a dark grid quad, horizontal (world Y up) at floorHeight
            bool floorVisible = true;
            float floorHeight = 0.0f;
            // lit by the key and fill lights; false draws the model's colours unlit
            bool shaded = true;
            bool modelVisible = true;
            // the read-back image gets alpha 255 everywhere (as the views' screenshots)
            bool opaque = true;
        };

        // One skeleton bone of the loaded model: bones are listed in glTF node order and the
        // parent is the nearest ancestor that is a bone (-1 for a root bone)
        struct BoneInfo{
            std::string name;
            int parent = -1;
            Entity entity = NULL_ENTITY;
            // the local transform from the file (relative to the bone's parent node)
            Vector3 restPosition;
            Quaternion restRotation;
            Vector3 restScale = Vector3(1.0f, 1.0f, 1.0f);
        };

        // ok false: the render failed (no graphics view, a load that never finished, a
        // readback error) or the request was cancelled; the image is then empty. A callback
        // getting ok false may be running inside the renderer's destructor (or releaseGpu), so
        // it must not touch the renderer.
        using RenderCallback = std::function<void(bool ok, PoseImage&& image)>;

        // frames a queued render waits for the scene to load (meshes, shaders) before it fails
        static constexpr int RENDER_TIMEOUT_FRAMES = 600;
        // frame time processFrame spends on queued renders once the first one ran
        static constexpr double FRAME_BUDGET_SECONDS = 0.025;
        // margin of the default framing (fraction of the framed box's half extents)
        static constexpr float DEFAULT_MARGIN = 0.08f;

        PoseRender();
        ~PoseRender();

        PoseRender(const PoseRender&) = delete;
        PoseRender& operator=(const PoseRender&) = delete;

        // Loads a model file (glTF, GLB or OBJ: an absolute path or one the engine resolves,
        // project-relative in the editor) into a fresh private scene, at rest pose, framed
        // from the front (looking down -Z) with the floor under its rest bounds. On failure
        // the renderer is left without a model and error (when given) says why.
        bool setModel(const std::string& modelPath, std::string* error = nullptr);
        void clearModel();
        bool hasModel() const { return model != nullptr; }
        const std::string& getModelPath() const { return modelPath; }

        // the private scene and the model's entity in it (for rig builders reading the hierarchy)
        Scene* getScene() const { return scene; }
        Entity getModelEntity() const;

        // ---- skeleton ----
        int getBoneCount() const { return (int)bones.size(); }
        const BoneInfo& getBone(int index) const { return bones[index]; }
        const std::vector<BoneInfo>& getBones() const { return bones; }
        int findBone(const std::string& name) const;
        // local TRS of a bone (relative to its parent node, as in the file); false for a bad index
        bool setBoneLocal(int index, const Vector3& position, const Quaternion& rotation, const Vector3& scale);
        bool setBoneRotation(int index, const Quaternion& rotation);
        bool getBoneLocal(int index, Vector3& position, Quaternion& rotation, Vector3& scale) const;
        // every bone back to its rest transform
        void resetToRest();

        // world position of a bone in the current pose (zero for a bad index or no graphics view).
        // The scene is updated only when the pose changed since the last update.
        Vector3 boneWorldPosition(int index);
        // every bone's world position, in bone order, with one update; false without a graphics view
        bool boneWorldPositions(std::vector<Vector3>& out);
        // world bounds of the posed model: its (skinned) meshes, or its bone positions when it
        // has no mesh bounds. False when there is nothing to bound.
        bool computeBounds(AABB& out);

        // ---- camera ----
        // Orthographic camera looking along viewDirection with screenUp up the image (made
        // perpendicular to it), centred on frame and fitting the box with margin (a fraction
        // of its half extents) at every image aspect.
        void setView(const Vector3& viewDirection, const Vector3& screenUp, const AABB& frame, float margin = DEFAULT_MARGIN);
        // the image size project() maps to (requestRender and renderNow set it too)
        void setViewSize(int width, int height);
        int getViewWidth() const { return viewWidth; }
        int getViewHeight() const { return viewHeight; }
        // a world point in image pixels of the current view and size: origin at the top-left
        // corner, y down, pixel (i, j) covering [i, i+1) x [j, j+1)
        Vector2 project(const Vector3& world) const;
        // image pixels per world unit at the current view and size
        float getPixelsPerUnit() const;
        Vector3 getViewDirection() const { return view.direction; }
        Vector3 getViewUp() const { return view.up; }

        // ---- look ----
        // style and lighting are applied to the scene when a render draws (each queued render
        // keeps the ones it was requested with)
        void setStyle(const Style& style);
        const Style& getStyle() const { return style; }
        // directions the lights shine along (world). The defaults light a character facing +Z
        // from its front-left-above (key) and front-right (fill).
        void setLighting(const Vector3& keyDirection, const Vector3& fillDirection);

        // ---- rendering ----
        // Queues a width x height render of the current pose, view, style and lighting (captured now:
        // later changes do not affect it). done runs on the main thread from a later call of
        // processFrame (the editor's frame loop), never inside this call; queued renders of
        // one PoseRender finish in order. Main thread only.
        void requestRender(int width, int height, RenderCallback done);
        // Renders now when the scene is ready (loaded meshes and shaders, a graphics view) and the
        // editor is inside a drawing frame (App::engineRender; see setInEditorFrame). False,
        // leaving out empty, otherwise; requestRender waits for that instead. Main thread.
        bool renderNow(int width, int height, PoseImage& out);
        size_t getPendingRenderCount() const { return pending.size(); }
        // the queued renders end now with ok false
        void cancelRenders();

        // The editor calls this once a frame (App::engineRender): serves the queued renders of
        // every PoseRender. Returns true while renders are still queued.
        static bool processFrame();
        static bool hasPendingRenders();
        // called when a render is queued, so an idle editor loop draws the frames it needs
        static void setFrameRequester(std::function<void()> requester);
        // App::engineRender marks the span of a frame that began drawing (renderNow refuses to
        // draw outside it: the loop also runs main-thread tasks in frames that do not draw)
        static void setInEditorFrame(bool inFrame);
        // The graphics view is going away (App::engineViewDestroyed): every queue is cancelled
        // and every renderer frees its GPU objects; the next use loads its scene again.
        static void releaseGpu();

    private:
        struct BoneLocal{
            Vector3 position;
            Quaternion rotation;
            Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
        };

        struct ViewState{
            Vector3 direction = Vector3(0.0f, 0.0f, -1.0f);
            Vector3 up = Vector3(0.0f, 1.0f, 0.0f);
            Vector3 right = Vector3(1.0f, 0.0f, 0.0f);
            Vector3 center = Vector3(0.0f, 0.0f, 0.0f);
            // half extents of the framed box along right and up (margin included) and its radius
            float halfRight = 1.0f;
            float halfUp = 1.0f;
            float radius = 1.0f;
        };

        struct RenderState{
            std::vector<BoneLocal> pose;
            ViewState view;
            Style style;
            Vector3 keyDirection;
            Vector3 fillDirection;
            int width = 0;
            int height = 0;
        };

        struct Request{
            RenderState state;
            RenderCallback done;
            int frames = 0;
        };

        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Light* keyLight = nullptr;
        Light* fillLight = nullptr;
        Primitive* floorMesh = nullptr;
        LineRenderer* floorGrid = nullptr;
        Model* model = nullptr;

        std::string modelPath;
        std::vector<BoneInfo> bones;
        std::vector<Entity> modelMeshes;

        ViewState view;
        Style style;
        // what the scene has (style and floor are applied when a render draws)
        bool appliedShaded = true;
        bool appliedModelVisible = true;
        bool floorBuilt = false;
        Vector3 appliedFloorCenter;
        float appliedFloorRadius = 0.0f;
        float appliedFloorHeight = 0.0f;
        Vector3 keyDirection = Vector3(-0.45f, -0.6f, -0.65f);
        Vector3 fillDirection = Vector3(0.6f, -0.25f, -0.75f);
        int viewWidth = 160;
        int viewHeight = 200;

        Framebuffer target;
        bool sceneLoaded = false;
        // drawn once: the view's pipelines are baked into the meshes on the next update
        bool viewWarmed = false;
        // a bone moved (or the scene is new) since the last scene update
        bool poseDirty = true;
        std::deque<Request> pending;

        void createScene();
        void destroyScene();
        void collectModel();
        // the floor quad and grid for the current view and style, rebuilt only when they moved
        void applyFloor();
        // style, lighting, floor and camera of a state into the scene (inside tryRender only)
        void applyToScene(const RenderState& state);
        void applyCamera(const ViewState& state, int width, int height);
        void halfExtents(const ViewState& state, int width, int height, float& halfWidth, float& halfHeight) const;
        bool ensureLoaded();
        void updateScene();
        // updateScene when the pose changed since the last one
        void updatePose();
        RenderState captureState(int width, int height) const;
        // the pose, view, style, lighting and image size of a state, as the caller's values
        // (nothing is applied to the scene's look here: applyToScene)
        void setState(const RenderState& state);
        bool isSceneReady();
        void draw(int width, int height);
        bool readBack(int width, int height, bool opaque, PoseImage& out);
        void releaseGpuResources();
        bool tryRender(const RenderState& state, PoseImage& out);
        // serves this renderer's queue within the frame budget; false once it was destroyed
        // by a callback
        bool serveQueue(double frameStart, bool& ranOne);
    };

}
