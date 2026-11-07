// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "RemoteImporter.h"

#include <string>
#include <vector>

namespace jasperengine::editor::htmlui {

    // Google Fonts (phase-2 §3.13.3): fonts.googleapis.com/css2, /css and /icon requested with
    // the editor's non-browser User-Agent answer one @font-face per weight/style with a static
    // TrueType file. Each is saved as fonts/<family-slug>/<FamilySlug>-<weight>[-italic].ttf,
    // a local CSS with the same rules pointing at them (relative URLs) as
    // fonts/<slugs>-<hash>.css, and each family's license from
    // fonts.google.com/download/list?family=<name>. HTTP 400: the family list of
    // fonts.google.com/metadata/fonts (once per session) tells an unknown family
    // (HTMLUI-IMP-0002 with the closest name) from a weight or style the family does not have
    // (HTMLUI-IMP-0006, imported without it).
    //
    // Fonts-only mode (docs/specs/S82-title-designer.md R57: the Title Designer's picker):
    // Options{writeCss = false, writeManifest = false, dir = "TitleArt/Fonts/Google"} writes the
    // TrueType files and each family's licence file to <assets>/<dir>/<family-slug>/ with the
    // same file names, and no CSS, no combined licence file and no manifest entry; `staging`
    // set: the files are added to it instead of being written (a caller that writes them as one
    // undo step).
    class GoogleFontsImporter {
    public:
        struct Options {
            bool writeCss = true;
            bool writeManifest = true;
            std::string dir;                    // asset-relative font folder ("" = <import root>/fonts)
            ImportStaging* staging = nullptr;   // fonts-only: collect instead of writing
        };

        struct Variant {
            int weight = 400;
            bool italic = false;
            bool operator<(const Variant& other) const { return italic != other.italic ? !italic : weight < other.weight; }
            bool operator==(const Variant& other) const { return weight == other.weight && italic == other.italic; }
        };

        struct FamilyRequest {
            std::string family;              // "Roboto Mono"
            std::vector<Variant> variants;   // explicit variants; empty = the family's default (400 normal)
            int rangeMin = 0, rangeMax = 0;  // wght@100..900 (rangeMax > 0)
            bool rangeItalic = false;        // ital,wght@1,100..900
        };

        explicit GoogleFontsImporter(RemoteImporter& importer);
        GoogleFontsImporter(RemoteImporter& importer, Options options);

        ImportedResource run(const std::string& url, const ImportLocation& at);

        // fonts.googleapis.com /css, /css2 or /icon
        static bool isGoogleFontsUrl(const std::string& url);
        // the families and variants a css/css2/icon URL asks for
        static std::vector<FamilyRequest> parseRequest(const std::string& url);
        // facade specs: "Inter", "Inter:400,700", "Roboto Mono:400,400i,700italic", "Inter:100..900"
        static bool parseSpec(const std::string& spec, FamilyRequest& request, std::string& error);
        // the css2 request for families: https://fonts.googleapis.com/css2?family=Inter:wght@400;700
        // (spaces as '+', variants sorted as Google requires; ital,wght@0,400;1,700 with italics)
        static std::string css2Url(const std::vector<FamilyRequest>& families);

        static std::string familySlug(const std::string& family);       // "Press Start 2P" -> "press-start-2p"
        static std::string familyFileStem(const std::string& family);   // "Press Start 2P" -> "PressStart2P"
        static std::string licenseUrl(const std::string& family);       // fonts.google.com/download/list?family=...
        static std::string metadataUrl();                               // fonts.google.com/metadata/fonts
        // "OFL.txt" -> "SIL Open Font License 1.1", "LICENSE.txt" -> "Apache License 2.0", "UFL.txt" -> "Ubuntu Font License 1.0"
        static std::string licenseNameForFile(const std::string& fileName);
        // the family whose name is closest to `family` (edit distance, case-insensitive)
        static std::string closestFamily(const std::string& family, const std::vector<std::string>& families);

        // drops the session's metadata cache (tests)
        static void clearSessionCache();

    private:
        struct Metadata {
            bool loaded = false;
            std::vector<std::string> families;
            std::map<std::string, std::vector<Variant>> variants;   // by lowercase family name
        };
        // fonts.google.com/metadata/fonts, fetched once per session (per resolved URL)
        const Metadata* metadata(const ImportLocation& at);

        // asset-relative path of a font folder file ("<slug>/<file>")
        std::string fontPath(const std::string& relative) const;

        RemoteImporter& importer;
        Options options;
    };

}
