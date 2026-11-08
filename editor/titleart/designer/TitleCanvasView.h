// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R28, R29, R30, R64, R66, R68, R71, D2, D12)
// - Purpose: the Title Designer's canvas pixels. The window asks for a render (a document
//   snapshot, a scale, the visible region, Draft or Full) and for a view (its size, where the
//   canvas sits, the backdrop); the view runs S80's TitleArtRenderer jobs (through the editor's
//   TitleArtRenderHost: one renderer and its caches for the whole editor), time-sliced within
//   the slice budget of the request, and shows the finished TitleImage in a private scene (never
//   added to the Engine: an orthographic front camera, a backdrop sprite and a quad textured with
//   the image's target), drawn as the secondary view TITLE_DESIGNER_VIEW_ID into its own
//   framebuffer, which the window shows and registers for the eyedropper.
// - Double buffered: the previous finished image stays on screen (scaled by its own frame and
//   scale) until the next job is complete; a partial or failed render is never shown.
// - Draws only on change (R66): view 5 is drawn when the image, the view or the backdrop changed,
//   or while settle frames remain; an unchanged canvas issues no draw.
// - Resource rebuild (R64): a new SystemRender::getContextId(), a job failing with device_lost or
//   out_of_target_memory, or the shown image's target reading null (the pool was cleared) drop
//   the quad's texture at once (it pointed into the cleared target), set rebuilding(), and render
//   the last request again.
// - Thread-safety: main thread. processFrame() runs from App::engineRender.

#pragma once

#include "titleart/TitleArtAssets.h"
#include "titleart/TitleArtDoc.h"
#include "titleart/render/TitleRenderJob.h"

#include "texture/Framebuffer.h"
#include "texture/Texture.h"
#include "math/Vector4.h"
#include "ecs/Entity.h"

#include <functional>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class Scene;
    class Camera;
    class Sprite;
    class TextureRender;
}

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::titledesigner {

    namespace ta = jasperengine::titleart;

    // what to render
    struct CanvasRenderRequest {
        std::shared_ptr<const ta::TitleArtDoc> doc;
        uint64_t docChange = 0;                 // the state's change counter of `doc`
        float pxScale = 1.0f;
        ta::TitleRectI region;                  // canvas px, y down from the canvas top-left; empty = canvas
        bool draft = false;
        bool includeGuides = true;
        double sliceBudgetMs = 12.0;            // per frame (Draft 8, Full 12: R68)
        const ta::AssetResolver* assets = nullptr;      // null: the project's
        std::vector<ta::FieldOverride> overrides;       // S85's sampled time (§6.8)
        uint32_t testFlags = 0;
    };

    // the backdrop behind the title (R30)
    struct CanvasBackdrop {
        enum class Kind : uint8_t { Checker, Black, White, Gray, Color, Image };
        Kind kind = Kind::Checker;
        Vector4 color = Vector4(0.5f, 0.5f, 0.5f, 1.0f);        // sRGB
        // Image (a Texture asset, a Game Scene or In Context snapshot): RGBA8 rows top first
        std::string imageKey;                   // identity of the pixels (a path, a snapshot id)
        std::vector<uint8_t> imageRgba;
        int imageWidth = 0, imageHeight = 0;
        bool operator==(const CanvasBackdrop& other) const {
            return kind == other.kind && color == other.color && imageKey == other.imageKey && imageWidth == other.imageWidth &&
                   imageHeight == other.imageHeight;
        }
    };

    // where things are in the view
    struct CanvasViewSetup {
        int width = 0, height = 0;              // the view's framebuffer, pixels
        float zoom = 1.0f;                      // framebuffer pixels per document unit
        double centerX = 0.0, centerY = 0.0;    // the document point at the view's centre (y up)
        // the stage the backdrop covers, document space (the canvas, or a device's screen)
        ta::TitleRect stage;
        // the title's placement on the stage (a device's Anchor and Display Height): document
        // point (x, y) of the title goes to stageOffset + (x, y) * stageScale
        double stageScale = 1.0;
        double stageOffsetX = 0.0, stageOffsetY = 0.0;
        float checkerPixels = 8.0f;             // screen pixels per checker square
        float dim = 1.0f;                       // the title's opacity (text editing dims the rest: R36)
        bool operator==(const CanvasViewSetup& o) const {
            return width == o.width && height == o.height && zoom == o.zoom && centerX == o.centerX && centerY == o.centerY &&
                   stage.x == o.stage.x && stage.y == o.stage.y && stage.width == o.stage.width && stage.height == o.stage.height &&
                   stageScale == o.stageScale && stageOffsetX == o.stageOffsetX && stageOffsetY == o.stageOffsetY &&
                   checkerPixels == o.checkerPixels && dim == o.dim;
        }
    };

    struct CanvasRenderInfo {
        bool hasImage = false;
        bool busy = false;                      // a job is running
        bool rebuilding = false;                // R64's overlay
        bool draft = false;                     // the shown image
        float progress = 0.0f;                  // the running job, 0..1
        uint32_t passesDone = 0, passesTotal = 0;
        double runningMs = 0.0;                 // since the running job began
        double lastRenderMs = 0.0;              // the shown image's wall time
        uint32_t lastPasses = 0;
        float shownPxScale = 0.0f;
        ta::TitleRectI shownFrame;              // canvas px of the shown image
        int imageWidth = 0, imageHeight = 0;
        std::string error;                      // the last failed job's
        std::vector<ta::TitleProblem> problems; // the shown image's
        std::vector<ta::NodeCost> nodeCosts;    // the shown image's (Stats)
        size_t cachedLayers = 0, totalLayers = 0;
        bool titleDrawn = false;                // the last drawn view showed the image
    };

    class TitleCanvasView {
    public:
        static constexpr int SETTLE_FRAMES = 6;

        explicit TitleCanvasView(Project* project);
        ~TitleCanvasView();
        TitleCanvasView(const TitleCanvasView&) = delete;
        TitleCanvasView& operator=(const TitleCanvasView&) = delete;

        // a render wanted: starts a job when it differs from the shown and the running one
        void requestRender(const CanvasRenderRequest& request);
        // the view's size, placement and backdrop this frame; draws view 5 when changed
        void requestDraw(const CanvasViewSetup& setup, const CanvasBackdrop& backdrop);
        // drops every job and the shown image (a document switch shows the checker and a spinner)
        void clear();
        // renders `scene` once through `camera` at width x height, as the stopped Game view does,
        // and hands the pixels (RGBA8, rows top first) to `done` on the main thread (R30's Game
        // Scene snapshot: explicit, never by itself)
        void requestSceneSnapshot(Scene* scene, Entity camera, int width, int height,
                                  std::function<void(std::vector<uint8_t> rgba, int width, int height, std::string error)> done);
        bool snapshotPending() const { return snapshotScene != nullptr; }

        // the composited view (null before the first draw)
        TextureRender* texture();
        Framebuffer* framebuffer();
        const CanvasRenderInfo& info() const { return status; }
        // the shown image's job (tests and Stats read its target)
        const ta::TitleRenderJob* shownJob() const { return shown.get(); }
        int drawCount() const { return draws; }

        // App::engineRender, once a frame after its views: steps the jobs, draws the views that asked
        static void processFrame();
        // the graphics view is going away: GPU objects freed
        static void releaseGpu();
        // every live view (tests)
        static size_t liveCount();

        // test hooks: the next frame sees a new context id (R64)
        void invalidateForTesting();

    private:
        Project* project = nullptr;
        Scene* scene = nullptr;
        Camera* camera = nullptr;
        Sprite* backdropSprite = nullptr;
        Sprite* titleSprite = nullptr;
        bool sceneLoaded = false;
        Framebuffer target;
        Texture backdropTexture;
        Texture titleTexture;
        std::string backdropTextureKey;

        CanvasRenderRequest wanted;
        bool hasWanted = false;
        CanvasRenderRequest runningRequest;
        std::unique_ptr<ta::TitleRenderJob> running;
        std::chrono::steady_clock::time_point runningStart;
        CanvasRenderRequest shownRequest;
        std::unique_ptr<ta::TitleRenderJob> shown;
        Framebuffer* shownFramebuffer = nullptr;
        double shownMs = 0.0;

        CanvasViewSetup setup;
        CanvasBackdrop backdrop;
        bool drawRequested = false;
        bool sceneDirty = true;
        int settle = SETTLE_FRAMES;
        int draws = 0;
        uint64_t contextId = 0;
        bool forceInvalid = false;
        CanvasRenderInfo status;
        Scene* snapshotScene = nullptr;
        Entity snapshotCamera = NULL_ENTITY;
        int snapshotWidth = 0, snapshotHeight = 0;
        std::function<void(std::vector<uint8_t>, int, int, std::string)> snapshotDone;
        void drawSnapshot();

        void createScene();
        void destroyScene();
        void step();
        void startJob(const CanvasRenderRequest& request);
        void presentFinished();
        void rebuildResources();
        void updateScene();
        void draw();
        void releaseGpuResources();
        static bool sameRender(const CanvasRenderRequest& a, const CanvasRenderRequest& b);
    };

}
