// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The HTML UI importers for phase 4 (MCP import_files, create_ui_document) and the editor
// (phase-2 §3.13.7): thin wrappers over editor/htmlui/RemoteImporter that also write
// assets/htmlui-imports/manifest.json, so the runtime resolves the remote URLs locally.
// Blocking (network): call them from a worker thread or an MCP job.
//
// destDir: project-relative (or absolute) directory inside the project's assets where the
// files go, "" = assets/htmlui-imports. The manifest is always assets/htmlui-imports/manifest.json.
// Paths in ImportResult are project-relative ("assets/htmlui-imports/fonts/inter/Inter-400.ttf").
//
// URLs each importer fetches (JASPERENGINE_IMPORT_MIRROR=<dir> serves
// https://<host><path-and-query> from <dir>/<host>/<urlquote(path-and-query, safe="")>;
// Python: urllib.parse.quote(path_and_query, safe="")):
//   importGoogleFonts({"Karla:400"})
//       https://fonts.googleapis.com/css2?family=Karla:wght@400
//           family names with '+' for spaces; "Inter:400,700" -> family=Inter:wght@400;700;
//           italics -> family=Roboto+Mono:ital,wght@0,400;1,400; no variants -> family=Inter;
//           several specs -> one request with one family= parameter each, in spec order
//       every src url() of the answer (e.g. https://fonts.gstatic.com/s/karla/v1/karla.ttf)
//       https://fonts.google.com/download/list?family=Karla (spaces as %20; optional: when it
//           is missing the license is recorded as unknown with an HTMLUI-IMP-0008 note)
//       https://fonts.google.com/metadata/fonts (only after an HTTP 400 from css2)
//   importUrl(url) / importDocumentReferences
//       the URL itself; a stylesheet's url()s and @imports resolved against it; a Lottie
//       JSON's image assets (u + p resolved against it); a LottieFiles page's .json/.lottie
//   importLucideIcons: nothing (the icons are built into the engine)

#include "htmlui/compat/Diagnostics.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor::htmlui {

    struct ImportResult {
        bool ok = false;                                           // everything asked for was imported
        std::string error;                                         // the failures, one per line
        std::vector<std::string> written;                          // project-relative files written
        std::vector<std::pair<std::string, std::string>> replaced; // remote URL -> project-relative local path
        std::string fontFaceCss;                                   // project-relative .css with @font-face rules (fonts only)
        // additions for the callers' reports (phase 4 import_files "failed"):
        std::vector<std::pair<std::string, std::string>> failed;   // what failed -> why
        std::vector<jasperengine::htmlui::compat::Diagnostic> diagnostics;   // HTMLUI-IMP-* (file: asset-relative)
    };

    // specs "Inter:400,700", "Roboto Mono:400,400i": the Google Fonts css2 request built from them
    ImportResult importGoogleFonts(const std::filesystem::path& projectRoot, const std::vector<std::string>& specs, const std::string& destDir);
    // writes the built-in Lucide icons (phase-2 §3.11.8) as standalone .svg files (currentColor kept); no network.
    // names: "play", "lucide:play" or an alias; files <destDir>/<name>.svg (destDir "" = assets/ui/icons)
    ImportResult importLucideIcons(const std::filesystem::path& projectRoot, const std::vector<std::string>& names, const std::string& destDir);
    // one remote image, svg, font, stylesheet, LottieFiles page or .json/.lottie
    ImportResult importUrl(const std::filesystem::path& projectRoot, const std::string& url, const std::string& destDir);
    // every remote reference of an .html/.css (collectRemoteReferences); `replaced` lists what a caller
    // may substitute in the file (the importer never edits the file; the runtime resolves through the manifest anyway).
    // URLs already in the manifest (with their file present) are not downloaded again.
    // htmlOrCss: project-relative or absolute path inside the assets
    ImportResult importDocumentReferences(const std::filesystem::path& projectRoot, const std::string& htmlOrCss, const std::string& destDir);

    // The standalone SVG of a built-in Lucide icon (Lucide's root attributes, currentColor
    // kept); false when the name is not in the pack. suggestion: the closest names.
    bool lucideIconSvg(const std::string& name, std::string& svg, std::string* suggestion = nullptr);
    // The engine has the Lucide pack (phase-2 p2-runtime)
    bool lucideIconsAvailable();

}
