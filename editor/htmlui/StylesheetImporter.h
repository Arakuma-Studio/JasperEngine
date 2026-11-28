// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "ContentSniffer.h"
#include "RemoteImporter.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor::htmlui {

    // A URL in a stylesheet, with its byte range (the url token or url()/string function, so a
    // replacement keeps everything around it)
    struct CssUrl {
        enum class Context { Import, FontFace, Other };
        std::string url;                  // unescaped
        uint32_t offset = 0, length = 0;  // the whole url(...) / "..." in the text
        Context context = Context::Other;
        int fontFace = -1;                // index into the sheet's @font-face rules (FontFace)
    };

    // One source of an @font-face src list
    struct CssFontSource {
        std::string url;                  // "" for local()
        std::string format;               // format("…") lowercase, "" when none
        std::string text;                 // the candidate as written ("url(a.ttf) format('truetype')")
        bool usable() const;              // TrueType/OpenType by format or extension
        bool woff() const;                // woff/woff2 by format or extension
    };

    struct CssFontFace {
        std::string family;               // unquoted
        int weight = 400;                 // the first value of font-weight ("bold" -> 700)
        bool italic = false;              // italic or oblique
        std::vector<CssFontSource> sources;
        uint32_t srcOffset = 0, srcLength = 0;   // the src value (to replace the whole list)
        bool hasSrc = false;
    };

    // A text edit; applyCssEdits applies a list in any order (they must not overlap)
    struct CssEdit {
        uint32_t offset = 0, length = 0;
        std::string text;
    };

    // Every URL of a stylesheet (@import, url() in declarations of any rule, nested rules and
    // conditional groups included, @font-face src), parsed with the compat CSS parser
    std::vector<CssUrl> findCssUrls(const std::string& css);
    std::vector<CssFontFace> findFontFaces(const std::string& css);
    std::string applyCssEdits(const std::string& text, std::vector<CssEdit> edits);
    // url("…") with the characters a CSS string needs escaped
    std::string cssUrl(const std::string& path);

    // Remote stylesheets of any host but Google Fonts (cdnjs Font Awesome, Bootstrap Icons,
    // animate.css, …; phase-2 §3.13.3): the sheet goes to css/<host>/<path>.css, every url()
    // it references (images, fonts, @imports, recursively) is downloaded into the same tree and
    // rewritten to a relative path; @font-face keeps its TrueType/OpenType sources when supplied, otherwise its WOFF/WOFF2 sources.
    class StylesheetImporter {
    public:
        static constexpr int kMaxImportDepth = 8;

        explicit StylesheetImporter(RemoteImporter& importer);

        // `body` fetched from `url` (finalUrl: after redirects, the base of relative URLs)
        ImportedResource run(const std::string& url, const std::string& finalUrl, const std::string& body, const ImportLocation& at);

        // css/<host>/<path> for a URL (query hashed into the name, `extension` added when the
        // path has none or another one)
        static std::string localPath(const RemoteImporter& importer, const std::string& url, const std::string& extension);

    private:
        // writes the sheet into staging under `localFile`; false when the sheet itself failed
        void rewriteSheet(const std::string& baseUrl, const std::string& body,
                          const std::string& localFile, int depth, const ImportLocation& at);
        // a resource a sheet references: staged, its asset path returned ("" when it failed)
        std::string importResource(const std::string& url, CssUrl::Context context, const std::string& fromFile,
                                   int depth, const ImportLocation& at);

        RemoteImporter& importer;
        ImportStaging staging;
        std::vector<std::string> nestedFiles;      // files of nested entries (Google Fonts) the sheet relies on
        std::map<std::string, std::string> done;   // normalized URL -> asset path ("" while in progress)
    };

    // A font file URL (.ttf/.otf/.woff/.woff2) imported on its own: fonts/<host>/<sha1-12>-<name>.<ext>
    ImportedResource importFontFile(RemoteImporter& importer, const std::string& url, const std::string& body,
                                    const SniffedContent& sniffed, const ImportLocation& at);

}
