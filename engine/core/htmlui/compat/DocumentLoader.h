// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_DOCUMENTLOADER_H
#define COMPAT_DOCUMENTLOADER_H

// HTML -> RML, the document half of the compatibility layer (phase-2 §3.12): compile (parse,
// HTML transforms, CSS rewrite of every stylesheet in one session, RML serialization), the
// in-memory cache, loading into an RmlUi context, stylesheet-only restyles (hot reload,
// viewport changes) and HTML fragments for the Lua API.

#include "Export.h"
#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/CompatTypes.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/css/CssRewriter.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
}

namespace jasperengine::htmlui::compat {

    // <script type="text/lua"> (inline, or src="x.lua"): run per document after every load in
    // play mode (§3.12.4, HtmlUILua::runDocumentScript)
    struct DocumentScript {
        std::string code;
        std::string chunkName;    // "ui/menu.html:42": Lua errors point at the HTML line
        std::string srcPath;      // asset path of src="" ("" for inline)
        SourceRef ref;            // the <script> element
        // as implemented: the file and line `code` starts at, so Lua reports errors at
        // "ui/menu.html:45" (the code runs with as many leading line breaks)
        std::string sourceFile;   // the .html (inline) or the .lua (src)
        uint32_t firstLine = 1;
    };

    // <template id>: its content as an RML fragment (template.content in Lua)
    struct HtmlTemplate {
        std::string id;
        std::string rml;
        SourceRef ref;
    };

    // One compiled document (phase-2 §3.12.1).
    struct JASPERENGINE_API CompatDocument {
        CompatDocument() = default;
        // The virtual files of its sheets go with the compile, cached or not (a check, the
        // importers' discovery and compileString are not cached). One owner: not copied.
        ~CompatDocument();
        CompatDocument(const CompatDocument&) = delete;
        CompatDocument& operator=(const CompatDocument&) = delete;

        std::string sourcePath;                       // "ui/menu.html"
        std::string title;
        std::string rml;
        RmlSourceMap sourceMap;
        // The author sheets in head order (File: a virtual file the RML links; Inline: a
        // <style> in the RML head); then the presentational hints and the !important sheet are
        // kept apart below. StyleAttribute outputs are not listed (they are in the RML).
        std::vector<StylesheetOutput> stylesheets;
        std::vector<DocumentScript> scripts;
        std::vector<HtmlTemplate> templates;
        std::vector<RemoteReference> remoteRefs;
        std::vector<std::string> dependencies;        // every file read (asset paths): the HTML first
        std::vector<Diagnostic> diagnostics;          // compile-time, sorted
        SourceFiles files;
        bool dependsOnViewport = false;
        uint64_t key = 0;                             // cache key (path, contents of every dependency, options)
        std::string rootSignature;                    // the <html> class and attribute names its selectors were banded for

        // --- as implemented ---
        bool compiled = false;                        // false: the HTML could not be read (diagnostics say why)
        CompatOptions options;                        // the options it was compiled with
        uint64_t owner = 0;                           // VirtualFiles owner of its sheets (unique per compile)
        std::string virtualDirectory;                 // "@htmlui/doc/<owner hex>/"
        std::string userAgentPath;                    // the UA sheet the RML links ("@htmlui/ua.rcss")
        StylesheetOutput hints;                       // presentational hints (inline <style> after the UA link)
        StylesheetOutput important;                   // the !important sheet (inline <style> after the author sheets)
        // the RML line of the first line of every inline <style> of the head, as RmlUi numbers it
        // (index: 0 hints, 1.. stylesheets[i] when Inline, last important); 0 = not written
        uint32_t hintsLine = 0, importantLine = 0;
        std::vector<uint32_t> stylesheetLines;        // per stylesheets[i] (Inline only)
        // the rewritten style="" of every element that has one: (data-htmlui-loc, RCSS)
        std::vector<std::pair<uint32_t, std::string>> styleAttributes;
        // what each dependency was at compile time: the FNV-1a 64 of the files read (content),
        // existence of the others (1/0); a cache hit needs every stamp to be current
        struct DependencyStamp {
            std::string path;
            uint64_t hash = 0;
            bool content = false;
        };
        std::vector<DependencyStamp> dependencyStamps;
        std::vector<std::string> fontFamilies;        // named by font-family declarations
        std::vector<std::string> declaredFontFamilies;   // declared by @font-face
        std::vector<std::string> animationFillNames;
        std::vector<std::string> virtualPaths;        // the VirtualFiles this compile registered (owner)
        // the @property rules the fork registers before RmlUi parses the sheets (phase 5, CompatAtProperty::registerFor, per loaded document)
        std::vector<RegisteredProperty> registeredProperties;
    };

    // HTML fragments (innerHTML, insertAdjacentHTML, outerHTML)
    struct FragmentOptions {
        std::string documentPath;                     // URLs resolve against it
        CompatOptions compat;
        std::string contextTag = "body";              // the element the fragment is parsed in
    };

    class JASPERENGINE_API DocumentLoader {
    public:
        // bumped whenever the compile output changes for the same input (part of the cache key)
        static constexpr uint32_t kVersion = 1;

        // Compiles (or returns the cached result of) an HTML file. Thread-safe. Never null;
        // `compiled` is false when the file could not be read.
        static std::shared_ptr<const CompatDocument> compile(const std::string& htmlPath, const CompatOptions& options);
        // Same, reading every file through `files` (checks outside a loaded project); not cached.
        static std::shared_ptr<const CompatDocument> compile(const std::string& htmlPath, const CompatOptions& options,
                                                             const FileAccess& files);
        // An HTML text that is not a file (tests, tools); not cached. virtualPath names it.
        static std::shared_ptr<const CompatDocument> compileString(std::string_view html, const std::string& virtualPath,
                                                                   const CompatOptions& options);

        // Compiles, loads into the context (hidden), logs the diagnostics. The caller keeps
        // `compiled` with the document: RmlUi's messages about it are mapped while it is
        // registered (registerDocument). Engine-internal (RmlUi types).
        static Rml::ElementDocument* load(Rml::Context* context, const std::string& htmlPath, const CompatOptions& options,
                                          std::shared_ptr<const CompatDocument>* compiled = nullptr);

        // A file changed: drops the cached compiles that depend on it (and their virtual
        // files) and clears RmlUi's style sheet cache.
        static void invalidate(const std::string& assetPath);
        // Canvas resized: for a compile with dependsOnViewport, compiles again for the new
        // viewport and swaps the document's style sheets (restyle) and changed style=""
        // attributes in place. Returns the compile now in use (the given one when nothing
        // depends on the viewport). Never calls ElementDocument::ReloadStyleSheet (it would
        // read the .html as RML).
        static std::shared_ptr<const CompatDocument> restyleForViewport(Rml::ElementDocument* document,
                                                                       const std::shared_ptr<const CompatDocument>& compiled,
                                                                       const CompatOptions& options);
        // The <html> element's class names or attribute names changed (a script): selectors whose
        // first compound may match the root are banded for its attributes as they are now
        // (`.dark .x` after documentElement.classList.add("dark")), so the document is compiled
        // again with them (CompatOptions::rootAttributes) and restyled as for the viewport. Returns
        // the compile now in use (the given one when nothing the selectors read changed).
        static std::shared_ptr<const CompatDocument> restyleForRoot(Rml::ElementDocument* document,
                                                                  const std::shared_ptr<const CompatDocument>& compiled);
        // The document's style sheet container rebuilt from a compile of the same RML (UA from
        // the cache, hints, author sheets, !important, in head order): DOM, element handles,
        // listeners, scroll and focus stay (stylesheet-only hot reload).
        static void restyle(Rml::ElementDocument* document, const CompatDocument& compiled, const CompatOptions& options);
        // The documents whose registered custom properties (phase 5) waited for a name another
        // document held with another definition, and hold it now, are restyled (called before
        // the contexts update: compat::updateRuntime).
        static void updateRegisteredProperties();
        // the same, and the document is bound to the new compile from now on (compiledOf,
        // RmlUi's messages about its sheets)
        static void restyle(Rml::ElementDocument* document, const std::shared_ptr<const CompatDocument>& compiled,
                            const CompatOptions& options);

        // HTML fragment for Lua innerHTML/insertAdjacentHTML: parsed in body context,
        // transformed, serialized; style attributes rewritten; returns RML and appends diagnostics.
        static std::string fragmentToRml(std::string_view html, const std::string& chunkName, std::vector<Diagnostic>& diagnostics);
        static std::string fragmentToRml(std::string_view html, const std::string& chunkName, std::vector<Diagnostic>& diagnostics,
                                         const FragmentOptions& options);

        // the same RML (the per-compile virtual directory of the sheets aside): a restyle can
        // replace one by the other
        static bool sameRml(const CompatDocument& a, const CompatDocument& b);

        static void clearCache();
        // the document's style sheet container rebuilt from the compile's sheets (restyle, without
        // the registration of its custom properties)
        static void restyleSheets(Rml::ElementDocument* document, const CompatDocument& compiled);
        static size_t cacheSize();
        // compiles the document's source again with `options` and restyles it (restyleForViewport,
        // restyleForRoot)
        static std::shared_ptr<const CompatDocument> restyleWith(Rml::ElementDocument* document,
                                                                const std::shared_ptr<const CompatDocument>& compiled,
                                                                const CompatOptions& options);

        // --- loaded documents (engine-internal) ---

        // Maps RmlUi's messages about the compile's texts (virtual sheets, inline styles, the RML)
        // back to the user's files while registered; `sink` gets each mapped diagnostic.
        // Returns a handle for unregisterDocument (RmlLogTranslator).
        static uint64_t registerDocument(const std::shared_ptr<const CompatDocument>& compiled,
                                         std::function<void(const Diagnostic&)> sink);
        static void unregisterDocument(uint64_t handle);
        // the compile of a loaded document (load() binds them until the document is
        // destroyed); null for documents not loaded through load()
        static std::shared_ptr<const CompatDocument> compiledOf(const Rml::ElementDocument* document);
        // compile-time + runtime diagnostics (RmlUi messages mapped back, Lua script errors, the
        // font check) of a loaded document; empty for others
        static const std::vector<Diagnostic>& diagnosticsOf(const Rml::ElementDocument* document);
        static void addRuntimeDiagnostic(const Rml::ElementDocument* document, const Diagnostic& diagnostic);
        // focuses the first element with `autofocus` (after the document is shown)
        static void applyAutofocus(Rml::ElementDocument* document);
        // HTMLUI-CSS-0023: families the document names that no @font-face declares and the font
        // engine does not know (needs the engine's font registry)
        static std::vector<Diagnostic> checkFontFamilies(const CompatDocument& compiled);
        // the RML <body> of a compat document is "body#htmlui-root"
        static bool isCompatDocument(const Rml::Element* element);
        static constexpr const char* kRootId = "htmlui-root";
        static constexpr const char* kLocationAttribute = "data-htmlui-loc";
    };

}

#endif //COMPAT_DOCUMENTLOADER_H
