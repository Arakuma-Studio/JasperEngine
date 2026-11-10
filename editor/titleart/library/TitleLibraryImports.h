// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S84-title-library.md R29, R30, §6.11.3, §6.12)
// - Purpose: the library's fonts and environments in a project. A look or template names pack
//   fonts by pack id and photo environments by S86's ids; a document names project assets. The
//   first apply that needs a pack file copies it (and a font's OFL.txt) into
//   assets/TitleArt/Fonts/<pack id>/ or assets/TitleArt/Environments/, with a `.meta` whose
//   `provenance` (S36's section) says where it came from, as files of the op's own undo step
//   (TitleOpFile). A second apply finds the copy (same SHA-256) and imports nothing; a file there
//   with other bytes is never overwritten (`font_conflict`: the copy goes to <pack id>-2/).
// - A missing pack (no staged files) is a state: the plan reports `font_unavailable` /
//   `environment_unavailable` and the layer keeps what it had.
// - Previews (R11) never import: `preview` plans resolve the pack file in place (the caller's
//   TransientAssetResolver maps the future asset path to it).
// - Thread-safety: main thread.

#pragma once

#include "titleart/TitleArtOps.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::editor::titlelibrary {

    using Json = jasperengine::titleart::Json;

    // one pack font (fonts.json)
    struct PackFont {
        std::string id;                 // "dela-gothic-one"
        std::string family, style;
        std::vector<std::string> files; // as named upstream
        Json variations = Json::object();
        std::vector<std::string> scripts;
        std::string category;
        std::string url;                // the upstream path at the pinned commit
        std::string fontVersion;
        bool coversJapanese() const;
    };
    // the pack's fonts (packDir()/fonts/fonts.json), read once per pack directory
    const std::vector<PackFont>& packFonts();
    const PackFont* findPackFont(const std::string& id);
    // a photo environment of the staged HDRI set (S86's manifest): id -> {file, slug, url}
    struct PackEnvironment {
        std::string id, file, slug, url, title;
    };
    const PackEnvironment* findPackEnvironment(const std::string& id);

    struct ImportPlan {
        std::filesystem::path projectDir;   // "" plans nothing (previews and tests without a project)
        std::filesystem::path assetsDir;
        bool preview = false;               // R11: resolve in place, write nothing
        std::vector<TitleOpFile> files;
        std::vector<std::string> imported;  // assets-relative paths copied in
        Json warnings = Json::array();      // {code, message, fix?}
        // preview: assets-relative path -> the pack file it stands for
        std::map<std::string, std::string> transient;

        // the assets-relative path of a pack font file in this project, planning its copy when
        // the project lacks it; "" with a `font_unavailable` warning when the pack has no such font
        std::string font(const std::string& packId, const std::string& file = {});
        // the same for a photo environment's HDRI (2D reflection maps, R30)
        std::string environment(const std::string& envId);
        // an assets-relative TitleArt/ path a template names: imported when it is a pack file
        void ensure(const std::string& assetPath);
        Json toJson() const;

    private:
        std::map<std::string, std::string> planned;     // asset path -> planned (dedupe within a batch)
        std::string copyInto(const std::string& assetDir, const std::filesystem::path& source, const std::string& name,
                             const std::vector<std::filesystem::path>& extras, const std::string& provenanceYaml,
                             const std::string& importYaml, const char* conflictCode);
    };

    // the asset folder of the pack's fonts and environments in a project
    constexpr const char* TITLE_FONTS_ASSET_DIR = "TitleArt/Fonts";
    constexpr const char* TITLE_ENVIRONMENTS_ASSET_DIR = "TitleArt/Environments";

}
