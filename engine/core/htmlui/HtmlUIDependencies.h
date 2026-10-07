// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUIDEPENDENCIES_H
#define HTMLUIDEPENDENCIES_H

#include "Export.h"

#include <functional>
#include <string>
#include <vector>

namespace jasperengine {

    // One file a document refers to, found by the static scanner below (no RmlUi): the hot
    // reload dependencies of a document (UIDocumentSystem::getDocumentFiles) and the export
    // check (Exporter::checkHtmlUIReferences).
    struct JASPERENGINE_API HtmlUIReference {
        enum Kind { STYLESHEET, IMAGE, FONT, SVG, LOTTIE, TEMPLATE, OTHER };

        std::string path;            // asset-relative (FileData rules), normalized; the URL itself when remote
        std::string raw;             // as written in the file
        Kind kind = OTHER;
        std::string fromFile;        // asset-relative file holding the reference
        int line = 0;                // 1-based position of the reference's first character in fromFile
        int column = 0;
        bool remote = false;         // http://, https:// or protocol-relative //host
        bool missing = false;        // the file could not be read
        bool outsideAssets = false;  // a relative path climbing above the assets directory
        bool absolute = false;       // an absolute filesystem path (works on this machine only)
    };

    // Reads a file given by its asset-relative (or absolute) path; false when it does not
    // exist. content = nullptr only asks whether it exists.
    using HtmlUIFileReader = std::function<bool(const std::string& path, std::string* content)>;

    // Every reference of an HTML/RML document, recursively through the stylesheets
    // (<link>, @import), <style> blocks, style="" attributes and templates it loads, in the
    // order they are found:
    // - HTML: link[href], img[src], svg[src], lottie[src], template[src]; @import and url(...)
    //   in <style> and style="";
    // - CSS: @import, url(...), @font-face src, @spritesheet src, decorator image(...)/svg(...).
    // data: URIs, fragments (#id) and data:// / lua:// paths are not files and are skipped.
    // reader = nullptr reads through Data (the engine's asset rules: asset path, packs).
    JASPERENGINE_API std::vector<HtmlUIReference> collectHtmlUIReferences(const std::string& documentPath,
                                                                          const HtmlUIFileReader& reader = nullptr);

    // References of one stylesheet's / document's text, not recursive (tests, tools)
    JASPERENGINE_API std::vector<HtmlUIReference> scanHtmlUIDocumentText(const std::string& text, const std::string& fromFile);
    JASPERENGINE_API std::vector<HtmlUIReference> scanHtmlUIStyleSheetText(const std::string& text, const std::string& fromFile);

    // A path as the engine resolves it from a document: `path` relative to the directory of
    // `fromFile`, "." and ".." resolved, asset:// dropped. `outside` is set when ".." climbs
    // above the assets directory.
    JASPERENGINE_API std::string resolveHtmlUIPath(const std::string& fromFile, const std::string& path, bool* outside = nullptr);
    // asset-relative form used for comparisons: '/' separators, no asset:// or "./", no ".."
    JASPERENGINE_API std::string normalizeHtmlUIPath(const std::string& path);

    JASPERENGINE_API const char* getHtmlUIReferenceKindName(HtmlUIReference::Kind kind);
    // kind of a path by its extension (.css -> STYLESHEET, .png -> IMAGE, .ttf -> FONT, ...)
    JASPERENGINE_API HtmlUIReference::Kind getHtmlUIReferenceKind(const std::string& path);

    // Runtime record of what a document load does, next to the static scanner: the files RmlUi
    // opens (RmlFile, RmlFontEngine::LoadFontFace) and the warnings/errors it logs while a Scope
    // is active (UIDocumentSystem's loads and stylesheet reloads), the textures each renderer
    // loads lazily at draw time (RmlRenderer::LoadTexture), and the messages logged outside any
    // scope (handed to the observer, which attributes them by the paths they name).
    class JASPERENGINE_API HtmlUIFileLog {
    public:
        enum class Level { Info, Warning, Error };
        using MessageSink = std::function<void(Level level, const std::string& text)>;

        class JASPERENGINE_API Scope {
        public:
            // files and messages may be null; scopes nest (the innermost one records)
            Scope(std::vector<std::string>* files, MessageSink messages);
            ~Scope();
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;
        private:
            Scope* previous;
            std::vector<std::string>* files;
            MessageSink messages;
            friend class HtmlUIFileLog;
        };

        static void recordFile(const std::string& path);
        static void recordMessage(Level level, const std::string& text);
        // messages outside any scope (one observer, set by UIDocumentSystem)
        static void setMessageObserver(MessageSink observer);

        static void recordLazyFile(const void* renderer, const std::string& path);
        static std::vector<std::string> getLazyFiles(const void* renderer);
        static void forgetRenderer(const void* renderer);
    };

    // "ui/menu.css:12: ..." / "line 12 of ui/menu.html" -> file and line (false if none)
    JASPERENGINE_API bool parseHtmlUIMessageLocation(const std::string& text, std::string& file, int& line);

}

#endif //HTMLUIDEPENDENCIES_H
