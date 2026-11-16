// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Unity's PSD Importer for JasperEngine (docs/specs/S94-rendering-anim-gaps.md F48): a PSD or PSB
// whose texture Import Mode is Layers becomes sprites.
// - Every visible pixel layer (a hidden group hides its layers; adjustment, fill and text layers
//   without pixels are skipped, each with a warning) is a frame of one sprite sheet packed into
//   pages (SpritePacker, trimmed), named after the layer (duplicates "<name> 2"), keyed
//   "layer:<id>" by its Photoshop layer id, so a reimport keeps each sprite's identity when
//   layers are renamed or reordered.
// - "<psd>#sheet" and "<psd>#image#<n>" resolve to the import (the sheet provider), so a Sprite
//   whose sheet is the PSD draws its layers; an export ships both.
// - `<file>.prefab` beside the source ("art/hero.psd.prefab"): one Sprite GameObject per layer at
//   its document position (the document's centre at the root's origin, Pixels Per Unit applied),
//   layer groups as parent GameObjects, stack order as Order in Layer (the bottom layer 0).
//   GameObject ids come from layer ids, so prefab instances keep their overrides across
//   reimports. With Character Rig, the layers' skins' bones (by name) become one skeleton of
//   GameObjects under the root and each skinned layer's Sprite Skin is bound to it.
// - Sprite skins (SpriteSkinEdit.h) of the PSD's meta are attached by frame name.
//
//   .jasperengine/cache/imported/v1/psd/<k[0:2]>/<k>/sheet.json, page<n>.png
//
// k = SHA-256 of (the file's bytes, its asset path, the Pixels Per Unit, the skins, the importer
// version). Thread model: importPsdLayers and resolve are any-thread (resolve memoizes under its
// mutex); writePrefab runs on the main thread.

#pragma once

#include "pixelart/PixelImage.h"
#include "sprite/SpriteSkinEdit.h"

#include "asset/Guid.h"
#include "sprite/SpriteSheet.h"

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    constexpr int kPsdLayerImporterVersion = 1;

    // a layer or group of the document, top of the stack first among siblings
    struct PsdLayerNode {
        bool group = false;
        uint32_t layerId = 0;
        std::string name;
        int frame = -1;             // a layer's frame in the sheet
        int order = 0;              // stack order, the bottom layer 0
        float x = 0.0f, y = 0.0f;   // a layer's centre in units from the document's centre, y up
        std::vector<PsdLayerNode> children;
    };

    struct PsdLayerImportResult {
        bool ok = false;
        std::string error;
        std::vector<std::string> warnings;
        int width = 0, height = 0;          // the document
        int pixelsPerUnit = 1;
        SpriteSheetData sheet;              // pages' textures empty: ImportedAssets names them
        std::vector<pixelart::PixelImage> pages;
        std::vector<PsdLayerNode> roots;
    };

    // the layers of a PSD/PSB as sprites (no files written)
    PsdLayerImportResult importPsdLayers(const std::vector<uint8_t>& bytes, const std::string& assetPath, int pixelsPerUnit,
                                         const sprite::SpriteSkinSet& skins);

    // the prefab file of an import (its `type: Prefab` document)
    YAML::Node psdLayerPrefab(const PsdLayerImportResult& import, const std::string& assetPath, const Guid& sourceGuid,
                              const Guid& prefabGuid, bool characterRig);

    // ".psd" / ".psb", any case
    bool isPsdPath(const std::string& path);

    class PsdLayerImporter {
    public:
        static PsdLayerImporter& get();
        void attach(Project* project);

        // the source is a PSD/PSB whose Import Mode is Layers (its meta; any thread)
        bool isLayered(const std::string& assetPath) const;
        // "<psd>#sheet" ("sheet") and "<psd>#image#<n>": the import's file, made when missing
        bool resolve(const std::string& assetPath, const std::string& key, std::string& fileOut);
        // the import of a layered source (made or read from the cache)
        bool import(const std::string& assetPath, PsdLayerImportResult& out);
        // the source changed (bytes, meta): the memo goes, the pool's sheet with it
        void invalidate(const std::string& assetPath);
        // "art/hero.psd" -> "art/hero.psd.prefab" (asset-relative)
        static std::string prefabPathOf(const std::string& assetPath);
        // main thread: imports and writes the prefab (only when its content changes); false with `error`
        bool writePrefab(const std::string& assetPath, std::string& error);
        // what an export ships: each layered source's cache files ("sheet", "image#<n>")
        std::vector<std::pair<std::string, std::filesystem::path>> exportedFiles(const std::string& assetPath);

        static constexpr const char* kCacheDir = "psd";

    private:
        PsdLayerImporter() = default;

        struct Memo {
            std::filesystem::file_time_type sourceTime, metaTime;
            uintmax_t sourceSize = 0, metaSize = 0;
            std::filesystem::path dir;   // the cache directory of the import ("" when it failed)
            size_t pages = 0;
        };

        Project* project = nullptr;
        std::filesystem::path assetsRoot, cacheRoot;
        int projectPixelsPerUnit = 1;
        mutable std::mutex mutex;
        std::map<std::string, Memo> memos;

        bool ensure(const std::string& assetPath, Memo& out);
    };

}
