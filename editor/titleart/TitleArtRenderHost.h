// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md R10, R36, §6.3, §6.9, §6.11)
// - Purpose: the editor's side of the one Title Art renderer. It owns the editor's
//   TitleArtRenderer and the project's AssetResolver (EditorTitleAssetResolver: the asset
//   database, GUID first), and turns a finished TitleRenderJob into pixels on the CPU (the
//   bakes, render_title, check_title, view_asset, thumbnails). No editor-only rendering path:
//   every request goes through TitleArtRenderer::begin.
// - Readback (§6.9): an output image (straight sRGB) is read as RGBA8 (4 bytes per pixel) or, for
//   RGBA16F, with FrameReadback's 8-byte float path and kept 16-bit; a premultiplied linear
//   image (an intermediate the renderer handed back) is unpremultiplied and sRGB-encoded first.
//   Rows top first, the frame space of the renderer (memory rows on every backend).
// - Thread-safety: main thread. The resolver is safe from job threads (a mutex).

#pragma once

#include "texsynth/SynthImage.h"
#include "titleart/TitleArtAssets.h"
#include "titleart/render/TitleArtRenderer.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class AssetDatabase;
    class Project;

    // Fonts, images, SVGs and reflection maps of a project, by AssetRef (GUID first, then the
    // assets-relative path hint)
    class EditorTitleAssetResolver : public titleart::AssetResolver {
    public:
        EditorTitleAssetResolver(const AssetDatabase* database, std::filesystem::path projectPath, std::filesystem::path assetsPath);

        titleart::ResolvedTitleAsset resolve(const titleart::TitleAssetRef& ref, titleart::TitleAssetKind kind) const override;
        std::shared_ptr<FontFace> loadFont(const titleart::ResolvedTitleAsset& asset, int faceIndex, std::string* error) const override;
        bool readBytes(const titleart::ResolvedTitleAsset& asset, std::vector<uint8_t>& out, std::string* error) const override;
        bool loadImage(const titleart::ResolvedTitleAsset& asset, titleart::TitleImageData& out, std::string* error) const override;
        std::shared_ptr<FontFace> builtinFont() const override;
        std::vector<titleart::ResolvedTitleAsset> projectFonts() const override;
        // every project asset a reference of `kind` may name (fonts; images and SVGs for the others)
        std::vector<titleart::ResolvedTitleAsset> projectAssets(titleart::TitleAssetKind kind) const;

        const std::filesystem::path& projectPath() const { return project; }
        const std::filesystem::path& assetsPath() const { return assets; }
        // the project-relative path of an assets-relative one
        std::string projectRelative(const std::string& assetsRelative) const;

    private:
        const AssetDatabase* database = nullptr;
        std::filesystem::path project;
        std::filesystem::path assets;
        std::string assetsPrefix;           // the assets directory, project-relative ("" or "assets/")
        mutable std::mutex lock;
        mutable std::map<std::pair<std::string, int>, std::shared_ptr<FontFace>> fonts;
        mutable std::shared_ptr<FontFace> builtin;
    };

    // a finished render on the CPU
    struct TitleRenderedImage {
        texsynth::Image image;              // RGBA, straight alpha, sRGB-encoded, rows top first; 8- or 16-bit
        titleart::TitleRectI frame;         // canvas pixels the image covers (image space)
        titleart::TitleRectI alphaBounds;   // pixels of the image with alpha >= 1/255 (empty when none)
        float pxScale = 1.0f;
        std::string pathUsed;               // "rgba16f" or "rgba8"
        double renderMs = 0.0;
        uint32_t passes = 0;
        std::vector<titleart::TitleProblem> problems;
    };

    class TitleArtRenderHost {
    public:
        // the editor's host (main thread)
        static TitleArtRenderHost& editor();

        // the resolver of `project` (made again when the project or its paths change)
        const EditorTitleAssetResolver* resolver(Project* project);
        // a job for `request` (its `assets` filled with the project's resolver when null); null
        // with `error` when the request has no document or the renderer refuses it
        std::unique_ptr<titleart::TitleRenderJob> begin(Project* project, titleart::RenderRequest request, std::string& error);
        // runs `request`'s job to its end (or its failure: check failed()); null with `error` when it
        // cannot start or waits too long for a program (S85 reads the job's glyph cells)
        std::unique_ptr<titleart::TitleRenderJob> runToEnd(Project* project, titleart::RenderRequest request, std::string& error);
        // runs `request` to the end and reads it back (`request.outputBitDepth` chooses 8 or 16 bits)
        bool render(Project* project, titleart::RenderRequest request, TitleRenderedImage& out, std::string& error);

        // the pixels of a finished job: RGBA 8-bit (bitDepth 8) or 16-bit (bitDepth 16, needs an
        // RGBA16F image); false with `error`
        static bool readBack(const titleart::TitleRenderJob& job, int bitDepth, TitleRenderedImage& out, std::string& error);
        // the same for any image of a finished job (S85: its glyph cells, TitleSplits)
        static bool readImage(const titleart::TitleImage& image, int bitDepth, TitleRenderedImage& out, std::string& error);
        // the alpha bounds of an RGBA image (alpha >= 1/255), empty when it has none
        static titleart::TitleRectI alphaBoundsOf(const texsynth::Image& image);

        // the renderer's counters (null before the first render)
        const titleart::RendererStats* stats() const;
        void trim();
        // the project closed: drop the resolver and the renderer's caches
        void reset();

    private:
        TitleArtRenderHost();
        ~TitleArtRenderHost();
        // the renderer, made on first use (an incomplete type here: only the .cpp names it)
        struct Backend;
        std::unique_ptr<Backend> backend;
        std::unique_ptr<EditorTitleAssetResolver> projectResolver;
        Project* resolverProject = nullptr;
        std::filesystem::path resolverPath;

        titleart::TitleArtRenderer& rendererInstance();
    };

}
