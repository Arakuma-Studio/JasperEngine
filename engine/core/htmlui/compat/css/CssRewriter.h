// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CSSREWRITER_H
#define COMPAT_CSS_CSSREWRITER_H

// The CSS -> RCSS rewriter (phase-2 §3.9). This header is the seam between the packages:
// p2-core declares it and ships a pass-through CssRewriter.cpp (parse, flatten nesting
// naively, re-serialize with double quotes, collect URLs); p2-css owns CssRewriter.cpp and
// implements the whole of §3.9 behind the same interface (state in Impl).

#include "Export.h"
#include "htmlui/compat/CompatAtPropertyTypes.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/CompatTypes.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jasperengine::htmlui::compat {

    class DiagnosticSink;
    class ImportManifest;
    class SourceFiles;

    struct StylesheetInput {
        enum class Kind : uint8_t { UserAgent, File, Inline, StyleAttribute };
        Kind kind = Kind::File;
        uint32_t file = 0;                // SourceFiles id (File: the .css; Inline/StyleAttribute: the HTML file)
        std::string_view text;            // the CSS (for Inline/StyleAttribute the slice of the HTML file)
        uint32_t baseOffset = 0;          // inline <style>/style="" position inside the HTML file
        std::string media;                // from the media attribute, "" = all
        uint32_t elementLoc = 0;          // StyleAttribute: data-htmlui-loc of the element
    };

    struct StylesheetOutput {
        StylesheetInput::Kind kind = StylesheetInput::Kind::File;
        std::string rcss;                 // whole sheet, or the declaration list for StyleAttribute
        RcssLineMap lines;
        std::string virtualPath;          // File: "@htmlui/doc/<key>/<asset path>.rcss"; else ""
        std::string sourcePath;           // File: the asset path of the .css; else the HTML file's
        uint32_t elementLoc = 0;          // StyleAttribute: the element
    };

    // Per-document session settings besides CompatOptions.
    struct RewriteContext {
        // p2-css: every attribute of the document's <html> as written (id, class, dir, lang,
        // data-*, ...; rootId/rootDirection below are the same id and dir): selectors whose
        // first compound uses the root's classes or attribute names also get a variant on the
        // root itself (`.dark .x` when <html class="dark">); the loader fills it.
        std::vector<std::pair<std::string, std::string>> rootAttributes;
        FileAccess files;                         // @import and existence checks; FileData when empty
        const ImportManifest* manifest = nullptr; // remote URLs; ImportManifest::get() when null
        std::string virtualDirectory;             // "@htmlui/doc/<key hex>/" (the document compile key);
                                                  // File outputs get virtualDirectory + asset path + ".rcss"
        std::string documentPath;                 // "ui/menu.html": inline sheets, origins
        // p2-html: the author id of the document's <html> (selectors `#id` on it name the root,
        // §3.8) and its dir (logical properties, §3.9.7); "" when none
        std::string rootId;
        std::string rootDirection;
    };

    struct RewriteResult {
        std::vector<StylesheetOutput> sheets;      // same order as the inputs
        // The !important hoisting sheet (§3.9.6 step 7): placed after every author sheet;
        // kind Inline, empty rcss when the document has no !important
        StylesheetOutput importantSheet;
        std::vector<std::string> dependencies;     // every file read (imports, fonts checked), asset-relative
        std::vector<RemoteReference> remoteRefs;   // URLs found (fonts, images, imports)
        // decoded data: URIs found in the CSS: the loader registers them in VirtualFiles under its owner
        std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> virtualFiles;
        bool dependsOnViewport = false;
        std::vector<std::string> animationFillNames;   // keyframes needing AnimationFill
        std::vector<std::string> fontFamilies;         // families named in font-family/font declarations
        std::vector<std::string> declaredFontFamilies; // families an @font-face declares
        std::vector<std::string> bitmapFontFamilies;   // those whose source is a bitmap font (S51)
        // @property rules the fork registers when the document loads (phase 5, CompatAtProperty)
        std::vector<RegisteredProperty> registeredProperties;
    };

    class JASPERENGINE_API CssRewriter {
    public:
        // The sink and files must outlive the rewriter; files gets every file read (imports).
        CssRewriter(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, RewriteContext context = {});
        ~CssRewriter();
        CssRewriter(const CssRewriter&) = delete;
        CssRewriter& operator=(const CssRewriter&) = delete;

        // One session per document: the UA sheet, then the author sheets in cascade order,
        // then the style="" attributes.
        RewriteResult rewrite(Span<const StylesheetInput> inputs);

        // Lua querySelector(All)/matches/closest: names, :focus; no root band (hook H5).
        static std::string rewriteSelector(std::string_view selector, DiagnosticSink* sink,
                                           const CompatOptions& options = CompatOptions());
        // Lua style.cssText / style[prop] = v: an RCSS declaration list (hook H5).
        static std::string rewriteDeclarations(std::string_view css, DiagnosticSink* sink,
                                               const CompatOptions& options = CompatOptions());

        // The declaration memo (phase-2 §4 "p2-perf as implemented"): a rewrite keeps the
        // output of a declaration that depends on its text alone and writes it again for the
        // same text. Process-wide; the default is On, or JASPER_HTMLUI_CSS_MEMO=off|check.
        // Check runs the pipeline for every memo hit as well and counts the hits whose
        // output differs (declarationMemoMismatches: always 0 unless a rewrite reads its
        // rule, its block or the custom properties without counting in Session::memoTaint).
        enum class DeclarationMemo : uint8_t { On, Off, Check };
        static void setDeclarationMemo(DeclarationMemo mode);
        static DeclarationMemo declarationMemo();
        static uint64_t declarationMemoMismatches();
        static uint64_t declarationMemoHits();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

}

#endif //COMPAT_CSS_CSSREWRITER_H
