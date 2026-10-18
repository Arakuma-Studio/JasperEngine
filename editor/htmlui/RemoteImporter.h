// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "HttpClient.h"

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/CompatTypes.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ImportManifest.h"

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace jasperengine::editor::htmlui {

    namespace compat = jasperengine::htmlui::compat;

    // Where imported files go (phase-2 §3.13.2). The manifest is always
    // <assets>/htmlui-imports/manifest.json (the runtime reads it there); the files go under
    // `root` (asset-relative, "htmlui-imports" unless a caller names another directory inside
    // the assets) in the layout fonts/, css/<host>/, images/<host>/, lottie/.
    struct ImportTarget {
        std::filesystem::path projectRoot;   // absolute
        std::filesystem::path assetsDir;     // absolute
        std::string root = "htmlui-imports"; // asset-relative, '/' separators

        // projectRoot's assets directory from project.yaml (assetsDir, default the project
        // root). destDir: project-relative or absolute directory inside the assets ("" =
        // htmlui-imports). False with error set when it is outside the assets directory.
        static bool forProject(const std::filesystem::path& projectRoot, const std::string& destDir,
                               ImportTarget& target, std::string& error);

        // asset-relative <-> project-relative ("ui/a.png" <-> "assets/ui/a.png") and absolute
        std::string toProjectRelative(const std::string& assetPath) const;
        std::filesystem::path toAbsolute(const std::string& assetPath) const;
        // asset-relative path of a file or directory inside the assets ("" when outside)
        std::string toAssetPath(const std::filesystem::path& path) const;
        // FileAccess over the assets directory (compat checks and discovery)
        compat::FileAccess fileAccess() const;
    };

    // Where a URL is referenced (diagnostics point there)
    struct ImportLocation {
        std::string file;                  // asset-relative document or stylesheet, "" for a direct import
        uint32_t line = 0, column = 0;
    };

    // One remote URL a document references (§3.13.3 discovery), located in the file that has it
    struct DiscoveredReference {
        std::string url;                   // as written
        std::string kind;                  // RemoteReference::kind
        ImportLocation location;
    };

    // What one import produced
    struct ImportedResource {
        std::string url;                                   // as requested
        bool ok = false;
        std::string error;                                 // why it failed (also a diagnostic)
        std::vector<compat::ImportEntry> entries;          // manifest entries written, the requested URL's first
        std::vector<std::string> written;                  // asset-relative files written

        const compat::ImportEntry* entry() const { return entries.empty() ? nullptr : &entries.front(); }
    };

    // Files an import keeps in memory until it has everything: a failed download writes
    // nothing (§3.13.6)
    class ImportStaging {
    public:
        // asset-relative path; a second add of the same path replaces the content
        void add(const std::string& assetPath, std::string content);
        bool has(const std::string& assetPath) const;
        const std::map<std::string, std::string>& files() const { return pending; }
        // writes every file (temporary file + rename each); false with error on the first failure
        bool commit(const ImportTarget& target, std::vector<std::string>& written, std::string& error) const;

    private:
        std::map<std::string, std::string> pending;
    };

    // The engine of every import (phase-2 §3.13.3): fetches a URL once, decides what it is
    // (Google Fonts CSS by URL; stylesheet, font, image, SVG, Lottie JSON, dotLottie or a
    // LottieFiles page by content type and magic bytes), hands it to the importer of that kind,
    // writes the files and the manifest entry, and reports HTMLUI-IMP-* diagnostics. Blocking;
    // one instance per thread.
    class RemoteImporter {
    public:
        explicit RemoteImporter(ImportTarget target);

        // kindHint: RemoteReference::kind ("google-fonts-css", "stylesheet", "font", "image",
        // "lottie", "svg", "video") or "" (detected)
        ImportedResource import(const std::string& url, const std::string& kindHint = {}, const ImportLocation& at = {});

        const ImportTarget& target() const { return importTarget; }
        HttpClient& http() { return client; }
        const std::vector<compat::Diagnostic>& diagnostics() const { return reported; }
        std::vector<compat::Diagnostic> takeDiagnostics();
        // "[HTML UI] importing <url>" lines and the like (the editor's Console); none by default
        void setProgress(std::function<void(const std::string&)> progress);

        // ---- for the importers of each kind

        // the diagnostic, located at `at`, subject `url`
        void report(compat::DiagCode code, const ImportLocation& at, const std::string& url, std::string message,
                    std::string suggestion = {});
        void progress(const std::string& line) const;
        // GET with the body read into memory; on failure reports HTMLUI-IMP-0002 (or -0005 when
        // curl is missing) unless quiet, and returns false
        bool fetch(const std::string& url, const ImportLocation& at, HttpResponse& response, std::string& body,
                   std::string& error, bool quiet = false);
        // writes staged files and the entries (manifest); fills `result`
        bool finish(ImportStaging& staging, std::vector<compat::ImportEntry> entries, ImportedResource& result);
        // a nested import (a stylesheet's @import of Google Fonts) done once per session
        ImportedResource importNested(const std::string& url, const std::string& kindHint, const ImportLocation& at);

        // asset-relative path under the import root
        std::string importPath(const std::string& relative) const;

    private:
        ImportTarget importTarget;
        HttpClient client;
        std::vector<compat::Diagnostic> reported;
        std::function<void(const std::string&)> progressLine;
        std::map<std::string, ImportedResource> nested;   // by normalized URL
        bool curlMissingReported = false;
    };

    // ---- helpers shared by the importers (RemoteImporter.cpp)

    // RFC 3986 reference resolution of `reference` against an absolute http(s) `base`
    std::string resolveUrl(const std::string& base, const std::string& reference);
    // "https://host/a/b.css?x" -> host "host", path "/a/b.css", query "x" (false when not http(s))
    bool splitUrl(const std::string& url, std::string& host, std::string& path, std::string& query);
    // a file-name-safe form of a URL path segment (percent-decoded; reserved characters -> '_')
    std::string safeFileName(const std::string& segment);
    // lowercase ASCII
    std::string lowerAscii(std::string text);
    // asset-relative `target` written relative to the directory of asset-relative `fromFile`
    std::string relativeAssetPath(const std::string& fromFile, const std::string& target);
    // first `digits` hex digits of the SHA-1 of the normalized URL
    std::string urlHash(const std::string& url, size_t digits);
    // 1-based line/column (code points) of the first occurrence of `needle` in `text`
    bool locateText(const std::string& text, const std::string& needle, uint32_t& line, uint32_t& column);

    // Remote references of an .html/.htm/.css (asset-relative), located (§3.13.3): the compat
    // pipeline's collection (compat::collectRemoteReferences), so the importers and the runtime
    // agree on what is remote. Unique by normalized URL, in order of appearance.
    using ReferenceCollector = std::function<std::vector<compat::RemoteReference>(const std::string& assetPath,
                                                                                 const compat::FileAccess& files)>;
    std::vector<DiscoveredReference> discoverRemoteReferences(const ImportTarget& target, const std::string& assetPath);
    // Tests: the collector discovery uses (an empty function restores compat::collectRemoteReferences)
    void setReferenceCollector(ReferenceCollector collector);

}
