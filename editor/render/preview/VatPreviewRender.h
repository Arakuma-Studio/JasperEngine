// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (vertex animation spec 5.1 item 5)
// - Purpose: the live views of the Vertex Animation Baker's Preview, source vs VAT. Each view is a
//   private scene (never added to the Engine: the open scenes, the Scene view and the Game view are
//   left alone) with an orthographic three-quarter camera, a key and a fill light and one subject:
//   * the source: a model file loaded with an Animation Player on its root (the node hierarchy the
//     clips address, as the baker's own scratch scene) or a prefab instantiated as the prefab
//     previews do, posed by an AnimationClip at a time through ActionSystem::previewPlayer (the
//     baker's posing call);
//   * the VAT: the bake's preview files (VatBakeSession::writePreviewFiles: a .vat.glb Model and a
//     Vertex Animator on the in-memory asset), one clip held at a time (Play, SetSpeed 0, SetTime
//     requests), drawn by the engine's VAT shader path.
//   Each frame the window asks for an image (requestDraw); App::engineRender calls processFrame
//   after the frame's own views, which updates the scenes that changed and draws them as secondary
//   views (Engine::drawView, VAT_PREVIEW_VIEW_ID) into each view's framebuffer, shown by ImGui as
//   it is (no read-back). A change is drawn for a few frames (meshes and pipelines load over them).
// - Thread-safety: main thread.

#pragma once

#include "animation/AnimationClip.h"
#include "animation/AnimationPlayerRuntime.h"
#include "ecs/Entity.h"
#include "math/Vector3.h"
#include "texture/Framebuffer.h"

#include <memory>
#include <string>

namespace jasperengine{
    class Scene;
    class Camera;
    class Light;
    class Model;
    class TextureRender;
}

namespace jasperengine::editor{

    class Project;

    class VatPreviewView{
    public:
        // frames a change is drawn for (loads and pipelines settle over them)
        static constexpr int SETTLE_FRAMES = 6;

        VatPreviewView();
        ~VatPreviewView();
        VatPreviewView(const VatPreviewView&) = delete;
        VatPreviewView& operator=(const VatPreviewView&) = delete;

        // an empty scene
        void clear();
        // the source: a model (.gltf/.glb/.fbx) or .prefab file, project-relative or absolute
        bool loadSource(Project* project, const std::string& file, std::string& error);
        // the baked asset: absolute .vat and .vat.glb paths (the .vat may be an in-memory pool entry);
        // `unlit`: drawn with its vertex colours only (the heatmap's mesh, no lighting)
        bool loadVat(const std::string& vatFile, const std::string& meshFile, std::string& error, bool unlit = false);
        bool hasSubject() const{ return root != NULL_ENTITY; }
        bool isVat() const{ return vat; }
        bool isUnlit() const{ return unlit; }
        const std::string& getFile() const{ return file; }

        // the source posed by `clip` at `time` (seconds in the clip); null: the rest pose
        void poseSource(const std::shared_ptr<const AnimationClip>& clip, float time);
        // the VAT holding `clip` at `time`
        void poseVat(const std::string& clip, float time);
        // the VAT's animator state as the engine sees it (tests): loaded and drawing animated
        bool vatActive() const;
        std::string vatError() const;

        // three-quarter front view framing the box (the subject's root sits at the origin)
        void setFraming(const Vector3& min, const Vector3& max);

        // an image of width x height is wanted this frame
        void requestDraw(int width, int height);
        // the last image (null before the first draw)
        TextureRender* texture();
        int getDrawCount() const{ return drawCount; }

        // App::engineRender, once a frame after its views: draws the views that asked
        static void processFrame();
        // the graphics view is going away (App::engineViewDestroyed): GPU objects freed
        static void releaseGpu();

        // the scene (tests read the posed hierarchy)
        Scene* getScene() const{ return scene; }
        Entity getRoot() const{ return root; }

    private:
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Light* keyLight = nullptr;
        Light* fillLight = nullptr;
        Model* model = nullptr;
        Entity root = NULL_ENTITY;
        bool vat = false;
        bool unlit = false;
        std::string file;
        std::string vatFile;

        anim::ClipPreviewSession session;
        const AnimationClip* posedClip = nullptr;
        std::string heldClip;
        float heldTime = -1.0f;

        Vector3 frameMin = Vector3(-1, 0, -1);
        Vector3 frameMax = Vector3(1, 2, 1);
        Framebuffer target;
        bool sceneLoaded = false;
        int settle = 0;
        int requestedWidth = 0;
        int requestedHeight = 0;
        bool requested = false;
        int drawCount = 0;

        void createScene();
        void destroyScene();
        void applyCamera(int width, int height);
        void draw();
        void releaseGpuResources();
    };

}
