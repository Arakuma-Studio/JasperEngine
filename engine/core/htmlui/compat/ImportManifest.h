// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_IMPORTMANIFEST_H
#define COMPAT_IMPORTMANIFEST_H

#include "Export.h"
#include "htmlui/compat/CompatOptions.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    // One remote resource the editor imported into assets/htmlui-imports/ (phase-2 §3.7.4,
    // §3.13.2).
    struct ImportEntry {
        std::string url;          // normalized: lowercase scheme/host, https, no fragment, query kept
        std::string kind;         // "google-fonts-css", "stylesheet", "font", "image", "lottie", "svg"
        std::string path;         // asset-relative local file that replaces the URL
        std::vector<std::string> files;   // every file written for the entry (fonts of a css, lottie images)
        std::string license;      // asset-relative license file; "" when unknown
        std::string licenseName;  // "SIL Open Font License 1.1", "unknown"
        std::string source;       // "Google Fonts", "LottieFiles", host name
        std::string fetched;      // ISO-8601 UTC
        std::string sha256;       // of path's content
    };

    // The runtime reader of assets/htmlui-imports/manifest.json (the editor's
    // ImportManifestWriter, p2-importers, writes it): remote URLs resolve to local files, the
    // runtime never touches the network.
    class JASPERENGINE_API ImportManifest {
    public:
        static constexpr const char* kPath = "htmlui-imports/manifest.json";   // asset-relative
        static constexpr int kVersion = 1;

        // The project's manifest through FileData, loaded once and kept until reload() (hot
        // reload of the manifest file: notifyFileChanged). The reference stays valid until
        // the next reload; across threads or reloads use current().
        static const ImportManifest& get();
        static std::shared_ptr<const ImportManifest> current();
        static void reload();                                   // the next get() reads the file again
        static void notifyFileChanged(std::string_view assetPath);   // reload() when it is the manifest
        // A manifest read through `files` (checks outside a loaded project), not cached
        static ImportManifest load(const FileAccess& files);
        // Parses manifest JSON; false (and error set) when it is not a version-1 manifest
        static bool parse(std::string_view json, ImportManifest& manifest, std::string* error = nullptr);

        const ImportEntry* find(std::string_view url) const;   // the URL is normalized first
        const std::vector<ImportEntry>& entries() const { return list; }
        bool empty() const { return list.empty(); }
        bool loaded() const { return fileFound; }               // the file existed and parsed

        void add(ImportEntry entry);                            // tests, writer: keeps entries sorted by url

        // "HTTP://Fonts.Example.com:443/a?b#c" -> "https://fonts.example.com/a?b"; "//host/x" -> "https://host/x"
        static std::string normalizeUrl(std::string_view url);

    private:
        std::vector<ImportEntry> list;   // sorted by url
        bool fileFound = false;
    };

}

#endif //COMPAT_IMPORTMANIFEST_H
