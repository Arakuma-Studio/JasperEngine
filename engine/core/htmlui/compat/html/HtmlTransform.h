// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_HTML_HTMLTRANSFORM_H
#define COMPAT_HTML_HTMLTRANSFORM_H

// The HTML transforms (phase-2 §3.7.3): HTML5 as a browser builds it -> the tree the RML
// serializer writes. Each pass is a function in html/transforms/*.cpp taking the
// TransformContext; runTransforms() calls them in the order of §3.7.3.

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/CompatTypes.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/SupportTables.h"
#include "htmlui/compat/html/HtmlTree.h"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace jasperengine::htmlui::compat {

    class DiagnosticSink;
    class SourceFiles;
    class UrlResolver;
    struct ResolvedUrl;

    // A stylesheet of the document, in tree order (head and body, as browsers apply them)
    struct StylesheetSource {
        enum class Kind : uint8_t { File, Inline };
        Kind kind = Kind::Inline;
        std::string path;            // File: the asset path of the .css (local, or a remote URL's import)
        std::string text;            // Inline: the CSS (the source slice, CRLF kept, so offsets match)
        uint32_t baseOffset = 0;     // Inline: where `text` starts in the HTML file
        std::string media;           // the media attribute ("" = all)
        SourceRef ref;               // the <link>/<style> element
    };

    // A style="" of an element, rewritten in the document's CSS session
    struct StyleAttributeSource {
        HtmlNode* element = nullptr;
        std::string text;            // the decoded value
        uint32_t baseOffset = 0;     // where the value starts in the HTML file (0 when unknown)
    };

    // A <template>: its content, transformed like the document, serialized as an RML fragment
    struct TemplateSource {
        std::string id;
        std::unique_ptr<HtmlNode> content;   // "#content" element
        SourceRef ref;
    };

    // Presentational hints of one element (§3.7.3 step 10): RCSS declarations ranked above the
    // user-agent sheet and below every author rule, written as `.htmlui-h<n> { ... }`
    struct HintDeclarations {
        std::string own;             // "text-align: center; background-color: #ff0000ff;"
        std::string cells;           // for `.htmlui-h<n> td, .htmlui-h<n> th` (table cellpadding/border)
        SourceRef ref;               // the attribute that produced the first one
        bool empty() const { return own.empty() && cells.empty(); }
    };

    struct HintRule {
        std::string className;       // "htmlui-h3"
        HintDeclarations declarations;
    };

    struct TransformContext {
        TransformContext(const CompatOptions& options, SourceFiles& files, DiagnosticSink& sink, const UrlResolver& urls);

        const CompatOptions& options;
        SourceFiles& files;
        DiagnosticSink& sink;
        const UrlResolver& urls;
        uint32_t file = 0;           // the HTML file (0 for fragments without a file)
        std::string path;            // its asset path (URLs resolve against it)
        HtmlNode* root = nullptr;    // the html element (fragments: "#fragment")
        bool fragment = false;       // innerHTML & co: no head, root, scripts or hint sheet
        FileAccess fileAccess;       // reads (Lua src); FileData when empty
        HtmlNode* body = nullptr;    // the body element (documents; set by transformRoot)
        std::string rootId;          // the author id of <html> (kept as data-html-id)
        std::string rootDirection;   // the dir of <html> ("" = ltr)

        // --- results ---
        std::string title;
        std::vector<StylesheetSource> stylesheets;
        std::vector<StyleAttributeSource> styleAttributes;
        std::vector<DocumentScript> scripts;
        std::vector<TemplateSource> templates;
        std::vector<RemoteReference> remoteRefs;
        std::vector<std::string> dependencies;      // files read or referenced (Lua src, linked sheets, SVG)
        std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> virtualFiles;   // data: URIs
        std::vector<HintRule> hintRules;
        std::unordered_map<std::string, size_t> hintIndex;   // declarations -> hintRules index
        bool dependsOnViewport = false;   // an image choice read the viewport (sizes, media)
        bool reportedDialog = false;
        bool reportedRuby = false;

        // the source text of a ref ("" when invalid)
        std::string_view slice(SourceRef ref) const;
        // the hint class for an element's declarations (fragments: prepended to its style="")
        void addHints(HtmlNode& element, const HintDeclarations& hints);
        // a URL of an attribute: resolved (diagnostics reported at ref), data: URIs registered,
        // remote ones recorded; the URL to write into RML ("" = drop the attribute)
        std::string resolveUrl(std::string_view url, std::string_view kind, SourceRef ref, ResolvedUrl* resolved = nullptr);
        void addDependency(const std::string& assetPath);
        void addRemote(const std::string& url, const std::string& kind, SourceRef ref);
    };

    // --- the passes, in the order runTransforms() calls them ---
    // html/transforms/HeadTransform.cpp: steps 1-5 (head collection, scripts, reserved names,
    // root mapping, templates); only for documents (not fragments, except scripts/styles,
    // which a fragment drops with a diagnostic)
    void transformHead(TransformContext& context);
    void transformScripts(TransformContext& context, HtmlNode& tree);
    void transformReservedNames(TransformContext& context, HtmlNode& tree);
    void transformRoot(TransformContext& context);
    void transformTemplates(TransformContext& context, HtmlNode& tree);
    // html/transforms/ElementTransform.cpp: steps 6 and 8 (elements, inline SVG)
    void transformElements(TransformContext& context, HtmlNode& tree);
    // html/transforms/LucideTransform.cpp: step 7
    void transformLucide(TransformContext& context, HtmlNode& tree);
    // html/transforms/FormTransform.cpp: step 9
    void transformForms(TransformContext& context, HtmlNode& tree);
    // html/transforms/AttributeTransform.cpp: steps 10-11 (attributes, hints, URLs), duplicate
    // ids, data-binding names outside a model
    void transformAttributes(TransformContext& context, HtmlNode& tree);

    // Lucide icons (§3.7.3 step 7), also used at runtime by the Lua hook (setAttribute("data-lucide"))
    struct LucideIcon {
        std::string name;      // the pack's (kebab-case) name
        std::string shapes;    // the icon's inner SVG markup
    };
    // false when the build has no icon pack (engine/libs/lucide, p2-runtime)
    bool lucideAvailable();
    // by kebab-case name, alias, camelCase or PascalCase name
    bool findLucideIcon(std::string_view name, LucideIcon& icon);
    // the closest names (for HTMLUI-HTML-0021)
    std::vector<std::string> closestLucideIcons(std::string_view name, size_t count = 3);
    // the attributes lucide.createIcons() gives the <svg> that replaces an element with
    // `attributes` (its replaceElement: defaults, data-lucide, the element's attributes over
    // them, class "lucide lucide-<name> <classes>", aria-hidden unless aria-* or role is set)
    std::vector<std::pair<std::string, std::string>> lucideAttributes(const std::vector<std::pair<std::string, std::string>>& attributes,
                                                                      const std::string& iconName);

    // Every pass over the document (and over each template's content).
    void runTransforms(TransformContext& context);

    // Helpers shared by the passes
    namespace html {
        // pre-order walk of the element descendants of `node` (the callback may modify the
        // element's attributes and children, not its position); templates' content is not entered
        template<typename F>
        void forEachElement(HtmlNode& node, F&& callback){
            for (size_t i = 0; i < node.children.size(); i++){
                HtmlNode* child = node.children[i].get();
                if (child->kind != HtmlNode::Kind::Element)
                    continue;
                callback(*child);
                // the callback may have replaced the child
                if (i < node.children.size() && node.children[i].get() == child)
                    forEachElement(*child, callback);
            }
        }
        std::string toLower(std::string_view text);
        std::string trim(std::string_view text);
        bool startsWith(std::string_view text, std::string_view prefix);
        bool endsWith(std::string_view text, std::string_view suffix);
        bool equalsIgnoreCase(std::string_view a, std::string_view b);
        std::vector<std::string> splitWhitespace(std::string_view text);
        // the class list without htmlui-* names
        bool hasClass(const HtmlNode& element, std::string_view name);
        void addClass(HtmlNode& element, std::string_view name);
        // HTML's "rules for parsing a legacy colour value" -> "#rrggbbff" ("" for none)
        std::string legacyColor(std::string_view value);
        // an HTML non-negative integer / dimension ("120", "50%"); false when invalid
        bool parseDimension(std::string_view value, double& number, bool& percent);
        // a Lucide name written in camelCase/PascalCase -> kebab-case
        std::string kebabCase(std::string_view name);
        // findElement / findAttribute of the support tables, memoized (the tables are scanned
        // linearly; a document asks for the same names thousands of times)
        const ElementSupport* elementRow(const std::string& tag);
        const AttributeSupport* attributeRow(const std::string& tag, const std::string& name);
    }

}

#endif //COMPAT_HTML_HTMLTRANSFORM_H
