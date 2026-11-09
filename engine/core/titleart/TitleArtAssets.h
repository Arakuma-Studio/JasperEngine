// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S80-title-art-core.md §6.3 RenderRequest::assets, §8 "Contract with S81")
// - Purpose: the seam between Title Art code and where its assets live. The renderer, text
//   layout and geometry reach fonts, images, SVGs and reflection maps only through an
//   AssetResolver: the editor's (its AssetDatabase, GUID first), S82's TransientAssetResolver
//   (fonts being previewed before import), and S81's player resolver (exported files). So the
//   module holds no editor include and a player runs the same code.
// - Thread-safety: resolve() and the loads are called from the render job's CPU stages, which may
//   run on job threads (S06); an implementation is safe to call from several threads at once.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    class FontFace;
}

namespace jasperengine::titleart {

    enum class TitleAssetKind : uint8_t { Font, Image, Vector, Environment, Sound };

    struct ResolvedTitleAsset {
        bool found = false;
        std::string path;           // what the loads below take (absolute, or a player's virtual path)
        std::string guid;
        std::string assetPath;      // assets-relative (problems and dependency edges name it)
        // the content revision (TitleArtHash feeds it into every hash that names the asset, so an
        // edited font re-renders its titles); 0 when the resolver cannot tell
        uint64_t revision = 0;
    };

    // straight-alpha RGBA8, rows top first
    struct TitleImageData {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    class JASPERENGINE_API AssetResolver {
    public:
        virtual ~AssetResolver() = default;

        // the asset a reference names, GUID first, then the path hint
        virtual ResolvedTitleAsset resolve(const TitleAssetRef& ref, TitleAssetKind kind) const = 0;
        // a face of a font file (`faceIndex` for .ttc members), shared: one FontFace per file and
        // index for the resolver's lifetime; null with `error`. Loaded with
        // FontFaceRasterizer::Optional, so CFF2 variable fonts open (Title Art draws outlines)
        virtual std::shared_ptr<FontFace> loadFont(const ResolvedTitleAsset& asset, int faceIndex, std::string* error) const = 0;
        // a file's bytes (SVG text, image files); false with `error`
        virtual bool readBytes(const ResolvedTitleAsset& asset, std::vector<uint8_t>& out, std::string* error) const = 0;
        // a decoded image (images, pattern images, reflection maps); false with `error`
        virtual bool loadImage(const ResolvedTitleAsset& asset, TitleImageData& out, std::string* error) const = 0;
        // the engine's built-in face (engine/core/util/DefaultFont.h): `font: null` and the end of
        // every fallback chain
        virtual std::shared_ptr<FontFace> builtinFont() const = 0;
        // every font the project has (missing-glyph problems name the ones that cover a code
        // point, S80 R23); none by default
        virtual std::vector<ResolvedTitleAsset> projectFonts() const { return {}; }
    };

}
