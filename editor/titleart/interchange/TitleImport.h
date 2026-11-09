// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R28-R34, R57, §6.2)
// - Purpose: what every import into Title Art shares (Pachislop projects, layered PSDs, SVGs,
//   Photoshop styles): the report notes (R31), the asset folder `<stem>_assets/` beside the
//   document with sanitised, unique names and SHA-256 reuse (R29), the undo step names (R57), the
//   `import` member of edit_title's result, and the registration of the importers
//   (TitleArtOps::registerCreateImporter, the import ops).
// - Files are never written here: an import returns them (TitleOpFile) and the document's
//   commit writes them with the document in one undo step, so one undo removes every file.
// - Thread-safety: main thread (the registration); the helpers are values.

#pragma once

#include "titleart/TitleArtOps.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // one note of an import report (R31): `layer` the new layer path ("" for the document)
    struct TitleImportNote {
        std::string layer;
        std::string field;
        std::string what;
        std::string action = "dropped";    // dropped | approximated | kept_unknown | note
        std::string code;                  // font_unavailable, image_format_unsupported ... ("" for none)

        TitleJson toJson() const;
        // the line kept in the document's `import_notes` ("Title: style.face.bevel: chisel soft drawn as chamfer (approximated)")
        std::string toLine() const;
    };

    // "Import Pachislop Project (logo.pslot.json)", "Import PSD (logo.psd)", "Import SVG (badge.svg)"
    std::string titleImportUndoName(const std::string& kind, const std::string& sourceFileName);
    // "Pachislop project", "PSD", "SVG"
    std::string titleImportKindLabel(const std::string& kind);
    // edit_title's `import` member: {kind, files (project-relative), notes}
    TitleJson titleImportJson(const TitleCreateResult& result, const std::filesystem::path& projectPath);

    // Pachislop's safeFileName (src/io/project.ts): trimmed, path and space characters as '_',
    // leading and trailing '_' dropped, "untitled" when nothing is left
    std::string titleSafeFileName(const std::string& name);

    // The files an import adds beside its document, in `<stem>_assets/` (R29): a name is
    // sanitised and suffixed _2, _3 on a clash; a file whose SHA-256 equals one already in the
    // folder (on disk or added before) is reused, so importing again writes nothing new.
    class TitleImportAssets {
    public:
        // `document` the .titleart (absolute), `assetsPath` the project's assets directory
        TitleImportAssets(const std::filesystem::path& document, const std::filesystem::path& assetsPath);

        // the folder (absolute) and its name relative to the document's directory ("logo_assets")
        const std::filesystem::path& folder() const { return directory; }
        // adds `bytes` as `name` (sanitised) in `subfolder` ("" or "patterns"); the assets-relative
        // path the document references
        std::string add(const std::string& name, const std::string& bytes, const std::string& subfolder = {});
        // the files to write (only the new ones)
        const std::vector<TitleOpFile>& files() const { return staged; }
        std::vector<TitleOpFile> take() { return std::move(staged); }

    private:
        std::filesystem::path directory;
        std::filesystem::path assets;
        std::vector<TitleOpFile> staged;
        std::map<std::string, std::string> byHash;      // sha256 -> assets-relative path
        std::set<std::string> taken;                    // lowercase absolute paths in use
        bool scanned = false;
        void scan(const std::filesystem::path& dir);
    };

    // the asset folder of a document ("logo.titleart" -> "logo_assets")
    std::filesystem::path titleImportFolder(const std::filesystem::path& document);

    // every importer and import op of S87 (Pachislop, Photoshop, SVG, strings); from
    // registerTitleArtTools
    void registerTitleInterchange();

}
