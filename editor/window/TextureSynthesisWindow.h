// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > Texture Synthesis: example-based texture synthesis (texsynth) for humans. Preview
// first: Generate runs on a worker thread with a live preview, nothing is written until Save,
// which writes the result, its companion maps and their recipes as one undo step. Opened from the
// Window menu, from the Project window ("Synthesize Texture..." on an image, "Edit Synthesis..."
// on a synthesized PNG) and by the MCP tool synthesize_texture with show_in_editor.
//
// The model (the spec, the run, the result, save) is reachable without the UI (the scripted API
// below), so MCP and the buttons share one code path; the non-UI logic is in
// texsynth/SynthesisSession.

#include "texsynth/SynthesisSession.h"

#include "texture/Texture.h"

#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class TextureSynthesisWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Texture Synthesis";

        explicit TextureSynthesisWindow(Project* project);
        ~TextureSynthesisWindow();

        void show();
        void open();
        void setOpen(bool open);
        bool isOpen() const;
        // Another project is being opened (App::prepareForProjectSwitch): a running synthesis is
        // cancelled and the window forgets the spec, its result and the project's images
        void resetForProjectSwitch();

        // ---- entry points (absolute paths)

        // "Synthesize Texture...": the image as the only example, its companions detected
        void openWithExample(const std::filesystem::path& image);
        // "Edit Synthesis...": the recipe of a synthesized PNG restored (mode, sources, params,
        // seed, output); a notice when sources changed since. False with `error` when the file has
        // no usable recipe.
        bool openRecipe(const std::filesystem::path& png, std::string& error);

        // ---- scripted API (MCP, tests): the same code the buttons run

        void setSpec(const texsynth::SynthesisSpec& spec);
        const texsynth::SynthesisSpec& getSpec() const { return spec; }
        // Generate: builds the request from the spec and starts it (cancelling a running one).
        // `options` callbacks are forwarded (MCP progress and completion). Null with `error`
        // when the spec has problems.
        std::shared_ptr<texsynth::SynthesisRun> generate(std::string& error, texsynth::SynthesisRun::Options options = {},
                                                         texsynth::SynthesisRun::Finished onFinished = nullptr);
        void cancel();
        std::shared_ptr<texsynth::SynthesisRun> currentRun() const { return run; }
        bool isRunning() const;
        // A finished, successful result is shown
        bool hasResult() const;
        // The settings changed since the shown result was generated
        bool isStale() const;
        // Save: the result's files (primary, companions, a painted mask, recipes) as one undo step;
        // existing files are refused unless overwrite. The written project-relative paths go to
        // `written`; the first is selected in the Project window. `outputs` (MCP): the output
        // paths of that spec are written instead of the window's (which the human may have edited
        // during the run), and the window's later setting changes don't block it. Refused when
        // the result was generated in another project.
        bool save(bool overwrite, std::vector<std::string>& written, std::string& error, const texsynth::SynthesisSpec* outputs = nullptr);

    private:
        enum class View { Result, Example, SideBySide };
        enum class SizePreset { S512, S1024, S2048, S4096, SameAsExample, Custom };

        // A GPU copy of an RGBA8 image for ImGui, replaced when the source changes
        struct GpuImage {
            Texture texture;
            int width = 0;
            int height = 0;
            uint64_t serial = 0;       // what it was made from (preview serial, result id...)
            bool valid() const { return width > 0 && !texture.empty(); }
        };

        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;

        texsynth::SynthesisSpec spec;
        std::string generatedKey;              // specKey() of the shown result
        uint64_t maskRevision = 0;             // grows with every brush edit
        uint64_t generatedMaskRevision = 0;
        std::shared_ptr<texsynth::SynthesisRun> run;
        texsynth::SynthesisSpec runSpec;       // the spec `run` was started with
        std::filesystem::path runRoot;         // the project it was started in
        uint64_t runGeneration = 0;            // texsynth::projectGeneration() then
        bool resultSaved = false;
        std::vector<std::string> lastWritten;
        std::string statusMessage;
        bool statusIsError = false;
        std::vector<std::string> notices;      // recipe notes, changed sources
        std::vector<std::string> specWarnings; // companion detection
        std::vector<texsynth::Problem> problems;
        double problemsCheckedAt = -1.0;
        std::string problemsFingerprint;

        // UI state
        SizePreset sizePreset = SizePreset::S1024;
        View view = View::Result;
        bool tile3x3 = false;
        bool showUncertainty = false;
        float zoom = 0.0f;                     // 0 = fit
        ImVec2 pan = ImVec2(0, 0);
        float footerHeight = 0.0f;
        std::string savedOutput;               // the output the shown result was saved to
        // inpaint brush
        float brushSize = 24.0f;               // example pixels (radius)
        bool brushErase = false;
        std::shared_ptr<texsynth::Image> maskCanvas;   // what the brush edits (spec.paintedMask is a copy)
        bool maskDirty = false;
        bool painting = false;
        ImVec2 lastPaint = ImVec2(-1, -1);
        // image picker
        std::vector<std::string> projectImages;
        double projectImagesScannedAt = -1.0;
        char pickerFilter[128] = "";

        GpuImage exampleGpu;
        GpuImage resultGpu;
        GpuImage uncertaintyGpu;
        GpuImage maskGpu;
        uint64_t resultSerial = 0;
        texsynth::Image exampleRgba;           // examples[0] as loaded for display
        std::string exampleLoadedPath;
        std::vector<Texture> retired;          // destroyed next frame (ImGui draws after show())

        void resetForNewSpec();
        // The spec's fingerprint without the painted mask (that is maskRevision)
        std::string specKey() const;
        bool paintsMask() const;
        void loadExampleForDisplay();
        void updateGpuImage(GpuImage& gpu, const texsynth::Image& rgba, uint64_t serial);
        void retire(GpuImage& gpu);
        void releaseRetired();
        void refreshProblems(bool force);
        void ensureMaskCanvas();
        void paintAt(const ImVec2& examplePixel, bool erase);
        void syncSizePreset();

        // UI parts
        void drawSettings();
        void drawModeSection();
        void drawExamplesSection();
        void drawCompanionsSection();
        void drawModeSpecificSection();
        void drawOutputSection();
        void drawActions();
        void drawPreview();
        void drawCanvas(const ImVec2& size);
        void drawMetrics();
        bool imageSlot(const char* id, std::string& path, const char* hint, bool allowClear);
        bool imagePickerPopup(const char* popupId, std::string& path);
        void scanProjectImages(bool force);
        void requestSaveWithConfirm();
        void setStatus(const std::string& message, bool error);
    };

}
