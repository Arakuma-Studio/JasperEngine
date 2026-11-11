// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R54, R57-R59)
// - Purpose: bringing a font into the project as one named project undo step ("Import Font Dela
//   Gothic One"): a system, library or any font file copied (a .ttc whole), a Google Fonts family
//   through GoogleFontsImporter's fonts-only mode (its styles and licence), and a .woff / .woff2
//   converted to <stem>.ttf / .otf next to where it was asked (the web font is not copied).
// - Design: plan, then commit. A plan reads and checks everything (decoding, the licence, names,
//   a free file name: "<stem> 1.ttf" when taken, the existing file reused when its bytes are the
//   same) and holds the bytes to write; commitFontImport writes them through one WriteFilesCmd on
//   the project history (undo removes them). The Restricted licence question is the caller's: a
//   system font with fsType Restricted plans with needsLicenceConfirmation until the request says
//   the user chose Copy Anyway.
// - Thread-safety: the plan functions are pure file reads (planGoogleFontImport downloads: run it
//   on a job); commitFontImport on the main thread.

#pragma once

#include "FontLicence.h"
#include "command/type/WriteFilesCmd.h"

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::titlefonts {

    // the program's font folders (S80 §6.15), assets-relative
    constexpr const char* FONTS_FOLDER = "TitleArt/Fonts";
    constexpr const char* SYSTEM_FONTS_FOLDER = "TitleArt/Fonts/System";
    constexpr const char* GOOGLE_FONTS_FOLDER = "TitleArt/Fonts/Google";
    constexpr const char* LIBRARY_FONTS_FOLDER = "TitleArt/Fonts/Library";

    enum class FontImportSource : uint8_t { File, System, Library, GoogleCache };

    struct FontImportPlan {
        bool ok = false;
        std::string error;
        // a Restricted system font: ask restrictedLicenceQuestion(), then plan again with
        // licenceConfirmed when the user chose Copy Anyway
        bool needsLicenceConfirmation = false;
        FontLicenceInfo licence;
        std::string licenceNote;                    // system fonts: systemFontLicenceNote()
        bool alreadyPresent = false;                // the same bytes are in the project: nothing to write
        std::string assetPath;                      // the font file, assets-relative ('/' separators)
        int faceIndex = 0;                          // the face the document names
        std::string family;                         // that face's family name
        std::string style;
        std::string undoName;                       // "Import Font Dela Gothic One"
        std::vector<WriteFilesCmd::Item> items;     // the font first, then licence files
    };

    struct FontCopyRequest {
        std::filesystem::path source;               // absolute
        int faceIndex = 0;
        std::filesystem::path assetsDir;            // absolute
        std::string folder = SYSTEM_FONTS_FOLDER;   // assets-relative
        FontImportSource from = FontImportSource::System;
        bool licenceConfirmed = false;
        // licence files copied beside the font when not there (a library font's OFL.txt)
        std::vector<std::filesystem::path> companions;
    };
    FontImportPlan planFontCopy(const FontCopyRequest& request);

    struct WebFontConversionRequest {
        std::filesystem::path source;               // absolute .woff / .woff2 (inside the assets or not)
        std::filesystem::path assetsDir;            // absolute
        std::string folder = FONTS_FOLDER;          // assets-relative: the picker's, or a drop's folder
    };
    FontImportPlan planWebFontConversion(const WebFontConversionRequest& request);

    struct GoogleFontImportRequest {
        std::string family;                         // "Dela Gothic One"
        std::vector<int> weights = {400, 700};      // the styles ticked (normal); a missing one is skipped
        std::vector<int> italicWeights;
        int rangeMin = 0, rangeMax = 0;             // a variable family's whole range (rangeMax > 0)
        std::filesystem::path projectRoot;          // absolute
        std::filesystem::path assetsDir;            // absolute
        std::string folder = GOOGLE_FONTS_FOLDER;   // the files go to <folder>/<family-slug>/
    };
    // blocking (HTTP): run it on a job; the plan's first item is the Regular (or first) style
    FontImportPlan planGoogleFontImport(const GoogleFontImportRequest& request);

    // writes the plan as one project undo step named plan.undoName (nothing for alreadyPresent);
    // false with `error` when the write failed (no step then)
    bool commitFontImport(Project* project, const FontImportPlan& plan, std::string& error);

    // a free file name in `directory`: `fileName`, else "<stem> 1<ext>", "<stem> 2<ext>" ...;
    // when a file there already holds `content`, that file's name with `identical` set
    std::string freeFontFileName(const std::filesystem::path& directory, const std::string& fileName, const std::string& content,
                                 bool& identical);
    // "Import Font <family>"
    std::string importFontUndoName(const std::string& family);

}
