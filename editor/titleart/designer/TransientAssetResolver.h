// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md D10, §6.6 "S80 renderer")
// - Purpose: the resolver of the Title Designer's previews. It wraps the project's resolver and
//   maps assets-relative paths that are not project assets yet (a system font, a Google Fonts
//   download, a library font being hovered in the font picker) to files outside the project, for
//   the renders that carry it in RenderRequest::assets only. A preview names a transient font by
//   the path it will have once imported, so committing the import changes nothing but the file's
//   existence.
// - Thread-safety: resolve() and the loads may run on render job threads (a mutex guards the map).

#pragma once

#include "titleart/TitleArtAssets.h"

#include <map>
#include <mutex>
#include <string>

namespace jasperengine::editor::titledesigner {

    class TransientAssetResolver : public jasperengine::titleart::AssetResolver {
    public:
        // `base` (the project's resolver) must outlive this object; null resolves nothing
        void setBase(const jasperengine::titleart::AssetResolver* base);
        const jasperengine::titleart::AssetResolver* base() const { return inner; }

        // `assetPath` (assets-relative) resolves to `absoluteFile` until removed
        void add(const std::string& assetPath, const std::string& absoluteFile);
        void remove(const std::string& assetPath);
        void clear();
        bool empty() const;
        // a revision that rises with every change of the map (renders key their requests by it)
        uint64_t revision() const;

        jasperengine::titleart::ResolvedTitleAsset resolve(const jasperengine::titleart::TitleAssetRef& ref,
                                                           jasperengine::titleart::TitleAssetKind kind) const override;
        std::shared_ptr<FontFace> loadFont(const jasperengine::titleart::ResolvedTitleAsset& asset, int faceIndex, std::string* error) const override;
        bool readBytes(const jasperengine::titleart::ResolvedTitleAsset& asset, std::vector<uint8_t>& out, std::string* error) const override;
        bool loadImage(const jasperengine::titleart::ResolvedTitleAsset& asset, jasperengine::titleart::TitleImageData& out, std::string* error) const override;
        std::shared_ptr<FontFace> builtinFont() const override;
        std::vector<jasperengine::titleart::ResolvedTitleAsset> projectFonts() const override;

    private:
        const jasperengine::titleart::AssetResolver* inner = nullptr;
        mutable std::mutex lock;
        std::map<std::string, std::string> files;
        uint64_t changes = 0;
    };

}
