// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Inspector's Aseprite Importer (docs/specs/S44-aseprite-import.md R30), shown while
// .aseprite/.ase files are selected in the Project window (Unity's Aseprite Importer):
//   - General (Import Mode, Pixels Per Unit, Pivot, Border, Trim), Layers (Include Hidden
//     Layers, Ignore Prefixes, the layer tree with its checkboxes, blend modes and opacity),
//     Animation (Generate Animation Clips, the tags, Events Layer), Slices (what each feeds),
//     Texture (Layout, Padding, Extrude, Max Size, Compression);
//   - the import's summary line and warnings, Open in Aseprite, Open Sprite Preview, Reimport
//     and Extract Clips...;
//   - edits are kept until Apply (one undo step for every selected file; the importer imports
//     each written file again) or Revert. With several files the fields show mixed values and
//     the layer and tag lists, which belong to one file, are read only.

#pragma once

#include "pixelart/PixelProblem.h"
#include "pixelart/aseprite/AsepriteDocument.h"
#include "pixelart/aseprite/AsepriteImport.h"

#include "json.hpp"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class AsepriteImporterView {
    public:
        explicit AsepriteImporterView(Project* project);

        // Draws the importer of the selected files (project-relative); false when none of them
        // is an .aseprite file
        bool draw(const std::vector<std::filesystem::path>& selection);
        // the selection's edits not applied yet
        bool hasPendingEdits() const { return !touched.empty(); }

    private:
        struct File {
            std::string assetPath;                      // asset-relative
            std::filesystem::path absolute;
            pixelart::aseprite::AsepriteImportSettings settings;   // as stored (defaults when absent)
            std::vector<pixelart::PixelProblem> problems;          // reading them
            bool metaExists = false;
            std::filesystem::file_time_type metaTime;
        };
        // the first file's structure, read without pixels (layers, tags, slices)
        struct Structure {
            std::string assetPath;
            std::filesystem::file_time_type time;
            std::optional<pixelart::aseprite::Document> document;
            std::string error;
        };

        Project* project;
        std::vector<File> files;
        std::string selectionKey;
        nlohmann::json working;                         // the first file's settings (MCP form) with the edits
        std::set<std::string> touched;                  // JSON pointers ("/texture/padding")
        std::set<std::string> mixed;                    // pointers whose values differ in the selection
        std::vector<pixelart::PixelProblem> applyProblems;
        Structure structure;
        std::string extractStatus;
        bool openExtract = false;

        void select(const std::vector<std::filesystem::path>& selection);
        bool storedChanged() const;
        void readStructure();
        nlohmann::json workingValue(const std::string& pointer) const;
        void setWorking(const std::string& pointer, nlohmann::json value);
        pixelart::aseprite::AsepriteImportSettings workingSettings() const;
        void apply();
        void revert();
        void extractClips();

        bool comboRow(const char* label, const std::string& pointer, const std::vector<std::string>& names,
                      const std::vector<std::string>& labels, const char* tooltip);
        bool boolRow(const char* label, const std::string& pointer, const char* tooltip);
        bool intRow(const char* label, const std::string& pointer, int lo, int hi, const char* tooltip);
        bool textRow(const char* label, const std::string& pointer, const char* tooltip);
        bool listRow(const char* label, const std::string& pointer, const char* hint, const char* tooltip);

        void drawGeneral();
        void drawLayers();
        void drawLayerRows(const pixelart::aseprite::Document& document, int group, const std::set<int>& imported);
        void toggleLayer(const pixelart::aseprite::Document& document, int layer, bool on);
        void drawAnimation();
        void drawSlices();
        void drawTexture();
        void drawStatus();
        void drawButtons();
    };

}
