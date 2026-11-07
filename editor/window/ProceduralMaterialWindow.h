// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S90-procedural-material.md R7, R24, R26, R27, AC15; docs/editor/procedural-material.md)
// - Purpose: the Procedural Material window, the Graph Editor (GraphEditorWindow) of `.procmat`
//   files with what a material graph adds. It opens on a double-click of a .procmat in the Project
//   window, and Assets > Create > Procedural Material makes one (a Material Output fed by a noise).
// - The canvas: every node shows its image and what drawing it cost (ProcMatThumbnails, R24's
//   budget: one shared evaluator, an LRU of 128² images, at most one finished per frame). On tier 1
//   every visible node gets one; on tier 2 only the selected node and the Material Output; on tier
//   3 only the selected node, as a job its deselection cancels.
// - The preview pane follows the selection: the selected node's first output, or the material when
//   none is selected. Pin A and Pin B keep an output in a slot whatever is selected; a pinned slot
//   shows until it is clicked again. It shows the lit ball of the material (MaterialRender), the
//   shown output as a 2×2 tile, and one image per output of the Material Output. An image that is
//   out of date (R7: a rebuild waiting for its 0.2 s debounce, or drawing) stays, marked.
// - Live edits (R7): a value drag reaches the passes as uniforms; a structural edit rebuilds after
//   the debounce (ProceduralSession).
// - The graph's seed (metadata "seed") with Lock (metadata "seed_locked": the seed field, Reroll and
//   Variations are off) and Reroll; Variations shows the material at 9 seeds, a click adopts one.
// - Exposed Properties: parameters the Inspector row's menu exposes (metadata "exposed": [{node,
//   param}]), edited in the pane; Presets: named values of them (metadata "presets": [{name,
//   values: {"<node>/<param>": value}}]), applied as one undo step.
// - The Problems strip adds the material's own rows: why it has no image, and the rows of the
//   check report preview_procedural_material makes (Services::checks; a fix that is one edit gets
//   Fix). The tier and device always show; on tiers 2 and 3 the Bake button shows an estimate and
//   the window says why it is slower.
// - Every edit is one named undo step (GraphDocument::apply, the setMetadata op for the window's
//   own fields). Esc during a canvas drag puts it back (the base window).
// - Its GPU work runs in App::engineRender (processFrame), where offscreen passes may be drawn; the
//   window's draw only uploads finished pixels and draws ImGui.
// - Thread-safety: main thread.

#pragma once

#include "window/GraphEditorWindow.h"

#include "procmat/ProcMatThumbnails.h"
#include "procmat/ProceduralSession.h"
#include "texture/Texture.h"

#include "json.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine{
    struct Material;
}

namespace jasperengine::editor{

    class MaterialRender;

    class ProceduralMaterialWindow : public GraphEditorWindow{
    public:
        static constexpr const char* WINDOW_NAME = "Procedural Material";
        static constexpr int PREVIEW_SIZE = 256;
        static constexpr int VARIATIONS = 9;
        static constexpr int PIN_SLOTS = 2;
        // what a frame of the window's GPU work may take: the frame's other views come first
        static constexpr double FRAME_BUDGET_MS = 4.0;

        struct BakeRequest{
            std::string path;       // the .procmat, project-relative
            int size = 1024;
            int seed = 0;
        };
        struct BakeState{
            bool running = false;
            float progress = 0.0f;  // 0..1
            std::string message;    // the last bake's outcome ("" none yet)
        };

        // What the window takes from the rest of S90, so each part stays where it is made
        struct Services{
            // an evaluator for one view of a graph, on the editor's device in R8's order
            std::function<std::unique_ptr<procmat::ProceduralEvaluator>()> makeEvaluator;
            // the device image of an output its evaluation left there (no read back); null: the
            // evaluator has none, its result's pixels are the image
            procmat::ProcMatThumbnails::TakeImage takeImage;
            // preview_procedural_material's report on the graph ({checks: [rows], fixes: [{check,
            // node, op}]}), run as the window's image changes; null: no check rows
            std::function<nlohmann::json(Project*, const std::string& path, const graph::Graph&, const procmat::EvaluationResult& images)> checks;
            // bake_procedural_material's bake: starts it (false and why when it cannot), its state, cancel
            std::function<bool(Project*, const BakeRequest&, std::string& error)> startBake;
            std::function<BakeState()> bakeState;
            std::function<void()> cancelBake;
        };
        static Services defaultServices();

        explicit ProceduralMaterialWindow(Project* project, Services services = defaultServices());
        ~ProceduralMaterialWindow() override;

        // App::engineRender, once a frame: every Procedural Material window's work advances. True
        // while some is left, so an idle editor keeps drawing frames until it is done.
        static bool processFrame();
        // the graphics view goes (App::engineViewDestroyed): every window drops its GPU objects
        static void releaseGpu();
        // called when work waits, so an idle editor draws the frames it needs (App's requestRedraw)
        static void setFrameRequester(std::function<void()> requester);
        // one frame of this window's work at `now` seconds (processFrame; tests pass their clock)
        bool update(double now);

        // ---- the seed (R27)
        int seed() const;
        bool seedLocked() const;
        bool setSeed(int value);
        bool setSeedLocked(bool locked);
        // a new seed, one undo step; refused while locked
        bool rerollSeed();
        // the 9 seeds the Variations grid shows: the same until the seed changes
        std::vector<int> variationSeeds() const;
        void setVariationsOpen(bool open) { variationsOpen = open; }
        bool isVariationsOpen() const { return variationsOpen; }
        // the seed of cell `index` becomes the graph's (refused while locked)
        bool adoptVariation(size_t index);

        // ---- what the preview shows (R27)
        struct Target{
            std::string node;       // "" for the material (the Material Output's ports)
            std::string port;
            bool operator==(const Target&) const = default;
        };
        // the selection's (its first node's first output), a pinned slot's when one is shown
        Target shownTarget() const;
        const Target& pinnedTarget(int slot) const { return pins[(size_t)slot]; }
        int shownSlot() const { return activePin; }
        // the slot takes what is shown now and shows it; again on the slot shown follows the selection
        void clickPin(int slot);
        void clearPin(int slot);

        // ---- Exposed Properties and Presets (R27)
        struct Exposed{
            std::string node;
            std::string param;
        };
        std::vector<Exposed> exposedProperties() const;
        bool isExposed(const std::string& node, const std::string& param) const;
        bool setExposed(const std::string& node, const std::string& param, bool exposed);
        std::vector<std::string> presetNames() const;
        // the exposed values as they are now, under `name` (replaced if it exists)
        bool savePreset(const std::string& name);
        bool applyPreset(const std::string& name);
        bool deletePreset(const std::string& name);

        // ---- device and bake (R27)
        procmat::EvaluatorTier tier() const;
        std::string deviceName() const;
        // "No GPU available, baking on the CPU." on tier 3, the software renderer's on tier 2, else ""
        std::string tierReason() const;
        // the estimated seconds of a bake at `size`, from the latest preview (-1: none yet)
        double bakeEstimateSeconds(int size) const;
        int bakeSize() const { return bakeSizeValue; }
        void setBakeSize(int size) { bakeSizeValue = size; }
        bool canBake(std::string* why = nullptr) const;
        bool bake();

        // the report rows as Problems strip rows: a FAIL is an error, a WARN a warning; a fix of
        // one op on the row's node is its Fix
        static std::vector<Problem> problemsFromReport(const nlohmann::json& report);

        // (tests) the sessions and the images
        const procmat::ProceduralSession* materialSession() const { return material.get(); }
        const procmat::ProceduralSession* focusSession() const { return focus.get(); }
        const procmat::ProcMatThumbnails& thumbnails() const { return *thumbs; }
        // the thumbnail requests of the last frame drawn, most wanted first
        const std::vector<procmat::ProcMatThumbnails::Request>& thumbnailRequests() const { return requests; }

    protected:
        bool nodeHasThumbnail(const graph::Node& node) const override;
        void drawNodeThumbnail(const graph::Node& node, ImDrawList* list, ImVec2 min, ImVec2 max) override;
        std::string nodeCost(const graph::Node& node) const override;
        bool hasPreviewPane() const override { return true; }
        void drawPreviewPane() override;
        void drawToolbarExtras() override;
        void collectProblems(std::vector<Problem>& out) const override;
        void documentChanged() override;
        void selectionChanged() override;
        void frameStarted() override;
        void documentClosed() override;
        void drawParamMenuExtras(const graph::Node& node, const std::string& param) override;

    private:
        // a pixel image on the GPU for ImGui and the ball, replaced as its source changes
        struct Upload{
            Texture texture;
            uint64_t serial = 0;
            bool valid() const { return serial != 0 && !texture.empty(); }
        };

        Services services;
        std::shared_ptr<const procmat::NodeLibrary> library;
        std::unique_ptr<procmat::ProceduralEvaluator> materialDevice;
        std::unique_ptr<procmat::ProceduralEvaluator> focusDevice;
        std::unique_ptr<procmat::ProceduralSession> material;
        std::unique_ptr<procmat::ProceduralSession> focus;
        std::unique_ptr<procmat::ProcMatThumbnails> thumbs;
        double now = 0.0;
        double sessionSeed = 0.0;
        // the last update left work (a thumbnail, a session drawing)
        bool workLeft = false;
        // the graph each node's image depends on, per revision (what feeds it, its values, the seed)
        std::map<std::string, uint64_t> nodeKeys;
        std::vector<procmat::ProcMatThumbnails::Request> requests;
        std::vector<std::string> visible;    // the nodes the canvas drew a thumbnail slot for this frame
        std::vector<std::string> nextVisible;

        std::array<Target, PIN_SLOTS> pins;
        int activePin = -1;
        bool variationsOpen = false;
        int bakeSizeValue = 1024;
        std::string bakeError;

        // the latest preview's images on the GPU, and how long it took
        std::map<std::string, Upload> outputUploads;    // per output of the material
        Upload focusUpload;
        uint64_t materialSerial = 0;
        uint64_t focusSerial = 0;
        std::map<std::string, Upload> thumbUploads;     // per thumbnail id (pixels from the CPU tier)
        std::vector<Texture> retired;
        double evaluationStarted = -1.0;
        double lastEvaluationSeconds = -1.0;

        std::unique_ptr<MaterialRender> ball;
        std::unique_ptr<Material> ballMaterial;

        // the check report of the latest image, and the rows it gives
        nlohmann::json report;
        std::vector<Problem> reportProblems;

        void ensureSessions();
        void dropSessions();
        double graphSeed() const;
        bool setMetadata(const std::string& key, const graph::Json& value, const std::string& undoName);
        const graph::Json& metadata(const std::string& key) const;
        void computeKeys();
        std::vector<procmat::PlanRoot> rootsOf(const Target& target) const;
        std::optional<procmat::PlanRoot> thumbnailRoot(const graph::Node& node) const;
        void buildRequests();
        void syncFocusRoots();
        void uploadImages();
        void upload(Upload& target, const std::vector<uint8_t>& rgba, int size, uint64_t serial);
        void retire(Upload& target);
        void updateBall();
        ImTextureID thumbnailTexture(const std::string& id);
        void drawImage(ImTextureID texture, const ImVec2& size, bool outOfDate, int tiles, const char* probe);
        void drawVariations();
        void drawExposed();
        void drawPresets();
        void drawDevice();

        static std::vector<ProceduralMaterialWindow*>& windows();
    };

}
