// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S82-title-designer.md R54, R55, R57, R58, R60)
// - Purpose: the font picker's model. Sections: Project (every Font asset; a .ttc's faces one by
//   one; .woff / .woff2 files inside the assets with a Convert badge), Library (fonts a provider
//   registers: S84's bundled OFL fonts), Google Fonts (Google's metadata, fetched once per
//   session and cached 7 days under the user cache; hovering a family downloads its Regular into
//   <user cache>/jasperengine/google-fonts/ for previews, at most 2 downloads at once; offline the
//   section says "Google Fonts needs a connection" and the rest works) and System
//   (SystemFontList). Search over family, style and file names; filters Covers My Text (with the
//   layer's Fallback Fonts) and Category (from OS/2 sFamilyClass and PANOSE, and Google's
//   metadata); the missing code points of a text per entry; a font's variable axes and named
//   instances, and R60's rule for changing font (keep the axes the new font has).
// - Asynchronous: the project scan and the font details (names, OS/2, fvar, cmap: only those
//   tables are read, so a 20 MB system font costs a few hundred KB) run on the jobs' Background
//   lane, Google's metadata and downloads on the net channel; poll() takes their results on the
//   main thread. Nothing here blocks the main thread; a detail not read yet is unknown (null /
//   false) and its read is started by the first ask.
// - Test seams: Services (the project's fonts, a system list, an HTTP getter, the cache folder,
//   the clock) and the Library provider.
// - Thread-safety: main thread (the jobs hand their results over under a mutex).

#pragma once

#include "FontLicence.h"
#include "FontPreviewRaster.h"
#include "FontTables.h"
#include "SystemFontList.h"

#include "titleart/TitleArtDoc.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::titlefonts {

    enum class FontSection : uint8_t { Project, Library, Google, System };
    const char* fontSectionLabel(FontSection section);      // "Project", "Library", "Google Fonts", "System"

    enum class FontCategory : uint8_t { Any, Sans, Serif, Display, Handwriting, Monospace };
    const char* fontCategoryLabel(FontCategory category);   // "Any", "Sans", "Serif", ...
    // OS/2: monospaced (post.isFixedPitch or PANOSE proportion), handwriting and display
    // (PANOSE family kind, sFamilyClass scripts and ornamentals), serif and sans (sFamilyClass,
    // else PANOSE serif style); Any when the font says nothing
    FontCategory categoryOfFont(const Os2Info& os2);
    // Google's metadata ("Sans Serif", "Serif", "Display", "Handwriting", "Monospace")
    FontCategory categoryOfGoogle(const std::string& category);

    constexpr const char* GOOGLE_OFFLINE_MESSAGE = "Google Fonts needs a connection";
    constexpr const char* DEFAULT_PREVIEW_TEXT = "Title タイトル 0123";

    struct FontEntry {
        FontSection section = FontSection::Project;
        std::string key;                // unique in the catalog
        std::string family, style;
        std::string fileName;           // search; "" for a Google family before its download
        std::string path;               // absolute file; a Google family's cached Regular once downloaded
        int faceIndex = 0;
        bool collectionFace = false;    // one face of a .ttc
        std::string assetPath, guid;    // Project: assets-relative, the asset's GUID
        bool webFont = false;           // Project .woff / .woff2: the Convert badge (choosing it converts)
        std::string libraryId;          // Library
        FontCategory category = FontCategory::Any;
        // Google
        std::vector<std::string> googleSubsets;
        std::vector<int> googleWeights;     // normal styles offered
        bool googleVariable = false;

        FontFileRef file() const { return {path, faceIndex}; }
    };

    // the facts the picker needs from a face
    struct FontDetails {
        FontNames names;
        Os2Info os2;
        FontCategory category = FontCategory::Any;
        FontLicenceInfo licence;
        std::vector<FontAxisInfo> axes;
        std::vector<FontInstanceInfo> instances;
        CharMap cmap;
        OutlineKind outlines = OutlineKind::None;
    };
    // reads only the tables FontDetails needs (blocking); false with `error`
    bool loadFontDetails(const std::string& path, int faceIndex, FontDetails& out, std::string& error);

    // S84's bundled fonts (a provider registered once; none until then)
    struct LibraryFont {
        std::string id;
        std::string family, style;
        std::string path;               // absolute
        int faceIndex = 0;
        FontCategory category = FontCategory::Any;
    };
    using LibraryFontProvider = std::function<std::vector<LibraryFont>()>;
    void setLibraryFontProvider(LibraryFontProvider provider);

    // a Google Fonts family's coverage before its download, from its subsets (latin, japanese ...)
    bool googleSubsetsCover(const std::vector<std::string>& subsets, uint32_t codepoint);

    // The picker's preview text: the layer's first line, its first 24 code points, else
    // DEFAULT_PREVIEW_TEXT
    std::string previewTextFor(const std::string& layerText);
    // Covers My Text is on by default when the text has a character outside Basic Latin
    bool coversMyTextByDefault(const std::string& text);

    // R60: the new font's axes keep their values (clamped to its range), the others are dropped
    struct AxesAfterFontChange {
        jasperengine::titleart::Json variations = jasperengine::titleart::Json::object();
        std::vector<std::string> dropped;
        bool reset() const { return !dropped.empty(); }
    };
    AxesAfterFontChange keepAxesOfNewFont(const jasperengine::titleart::Json& variations, const std::vector<FontAxisInfo>& newAxes);
    // "Change Font to Dela Gothic One", with " (Axes Reset)" when axes were dropped
    std::string changeFontUndoName(const std::string& family, bool axesReset);

    class FontCatalog {
    public:
        struct ProjectFontFile {
            std::string path;           // absolute
            std::string assetPath;      // assets-relative
            std::string guid;
        };
        struct HttpResult {
            bool ok = false;
            std::string body;
            std::string finalUrl;       // after redirects ("" = the URL asked)
            std::string error;
        };

        struct Services {
            // main thread: the project's Font assets (.ttf, .otf, .ttc)
            std::function<std::vector<ProjectFontFile>()> projectFonts;
            // a job: the .woff / .woff2 files inside the assets
            std::function<std::vector<ProjectFontFile>()> projectWebFonts;
            // a job: the system's faces (empty function: SystemFontList::instance())
            std::function<std::vector<SystemFontFace>()> systemFonts;
            // a job on the net channel: one GET (empty function: HttpClient, curl)
            std::function<HttpResult(const std::string& url)> http;
            std::filesystem::path cacheDir;             // "<user cache>/jasperengine"
            std::function<int64_t()> now;               // seconds (the metadata cache's age)
        };
        // the editor's: the project's resolver and assets folder, SystemFontList, curl, App's cache
        static Services editorServices(Project* project);

        explicit FontCatalog(Services services);
        ~FontCatalog();
        FontCatalog(const FontCatalog&) = delete;
        FontCatalog& operator=(const FontCatalog&) = delete;

        // the picker opened: starts the project scan, the system listing and Google's metadata
        // (each once; refreshProject scans again)
        void open();
        // takes the finished background work (main thread, every frame the picker is open)
        void poll();
        // the project's fonts changed
        void refreshProject();
        // tests: waits for every job started so far, then polls
        void waitIdle();

        // every entry, sections in order (Project, Library, Google, System), each by family and style
        const std::vector<FontEntry>& entries() const { return all; }
        const FontEntry* find(const std::string& key) const;
        bool projectReady() const { return projectScanned; }
        bool systemReady() const { return systemListed; }

        enum class GoogleState { Idle, Loading, Ready, Offline };
        GoogleState googleState() const { return google; }
        // GOOGLE_OFFLINE_MESSAGE when Offline, else ""
        std::string googleMessage() const;

        // a local face's details (null until read; the first ask starts the read)
        const FontDetails* details(const FontFileRef& file);
        const FontDetails* details(const FontEntry& entry);

        struct Query {
            std::string search;                     // words, all of which match family, style or file name
            bool coversText = false;                // Covers My Text
            std::vector<uint32_t> text;             // the layer's text (code points)
            std::vector<FontFileRef> fallbacks;     // the layer's Fallback Fonts
            FontCategory category = FontCategory::Any;
        };
        // the entries the query keeps, in entries() order; an entry whose coverage or category is
        // not known yet stays (it is hidden once known not to match). The pointers (and those of
        // entries() and find()) are valid until the next poll(), open() or refreshProject().
        std::vector<const FontEntry*> filter(const Query& query);

        // the code points of `text` the entry and the fallbacks do not map (distinct, in text
        // order, ignorables skipped); false while a needed detail is not read yet
        bool missingCodepoints(const FontEntry& entry, const std::vector<uint32_t>& text, const std::vector<FontFileRef>& fallbacks,
                               std::vector<uint32_t>& out);

        // R55's Add Fallback Font: the Project fonts (web fonts aside) that map every one of
        // `missing` first, then those that map some, each group in entries() order; false while a
        // font's details are still being read (call again next frame)
        bool fallbackCandidates(const std::vector<uint32_t>& missing, std::vector<const FontEntry*>& out);

        enum class DownloadState { None, Queued, Downloading, Ready, Failed };
        // hover: downloads a Google family's Regular for previews (at most 2 at once)
        void requestGoogleDownload(const std::string& key);
        DownloadState googleDownloadState(const std::string& key) const;
        std::string googleDownloadError(const std::string& key) const;
        // where a family's Regular is cached
        std::filesystem::path googleCachePath(const std::string& family) const;

        static constexpr int MAX_GOOGLE_DOWNLOADS = 2;
        static constexpr int64_t GOOGLE_METADATA_MAX_AGE_SECONDS = 7 * 24 * 3600;

    private:
        struct Inbox;
        struct DetailSlot {
            bool loading = false;
            bool failed = false;
            std::shared_ptr<const FontDetails> details;
        };
        struct Download {
            DownloadState state = DownloadState::None;
            std::string family;
            std::string error;
        };

        Services services;
        std::shared_ptr<Inbox> inbox;
        std::vector<FontEntry> all;
        std::vector<FontEntry> projectEntries, libraryEntries, googleEntries, systemEntries;
        bool opened = false;
        bool projectScanned = false, projectScanning = false;
        bool systemListed = false, systemListing = false;
        GoogleState google = GoogleState::Idle;
        std::map<std::string, DetailSlot> detailSlots;      // "<path>#<face>"
        std::map<std::string, Download> downloads;          // by entry key
        std::vector<std::string> downloadQueue;
        int downloadsRunning = 0;

        void startProjectScan();
        void startSystemListing();
        void startGoogleMetadata();
        void startDownloads();
        void rebuild();
        bool entryCovers(const FontEntry& entry, uint32_t codepoint, bool& known);
    };

}
