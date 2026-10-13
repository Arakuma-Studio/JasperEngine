// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// §3.7.3 steps 1-5: head collection (title, meta, base, stylesheets in tree order), scripts
// (Lua kept, JavaScript reported), noscript, reserved names, the root mapping and templates.

#include "htmlui/compat/html/HtmlTransform.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/LuaHooks.h"
#include "htmlui/compat/SourceFiles.h"
#include "htmlui/compat/UrlResolver.h"
#include "htmlui/compat/VirtualFiles.h"


using namespace jasperengine::htmlui::compat;
using namespace jasperengine::htmlui::compat::html;

namespace {

    // the text of a raw-text element (style, script): the source slice (CRLF kept, offsets
    // exact) when its text node has a range, else the decoded text
    std::string rawText(TransformContext& context, const HtmlNode& element, uint32_t& baseOffset){
        baseOffset = 0;
        if (element.children.size() == 1 && element.children[0]->kind == HtmlNode::Kind::Text){
            const HtmlNode& text = *element.children[0];
            const std::string_view slice = context.slice(text.ref);
            if (text.ref.valid() && text.ref.file == context.file && !slice.empty()){
                baseOffset = text.ref.offset;
                return std::string(slice);
            }
        }
        return element.textContent();
    }

    bool isRelSkippedSilently(const std::string& rel){
        static const char* const silent[] = {"preload", "preconnect", "dns-prefetch", "icon", "manifest", "modulepreload",
                                             "prefetch", "apple-touch-icon", "apple-touch-icon-precomposed", "mask-icon",
                                             "shortcut", "canonical", "author", "license", "help", "search", "next", "prev",
                                             "prerender", "me", "pingback", "alternate"};
        for (const char* name : silent){
            if (rel == name)
                return true;
        }
        return false;
    }

    void addLinkedSheet(TransformContext& context, HtmlNode& link, const std::string& href){
        ResolvedUrl resolved;
        const HtmlAttr* hrefAttr = link.findAttr("href");
        context.resolveUrl(href, "stylesheet", (hrefAttr && hrefAttr->valueRef.valid()) ? hrefAttr->valueRef : link.ref, &resolved);
        if (resolved.dropped())
            return;   // remote without an import (HTMLUI-IMP-0001) or another scheme: skipped
        if (resolved.kind == UrlKind::Remote || resolved.kind == UrlKind::Relative || resolved.kind == UrlKind::RootRelative ||
            resolved.kind == UrlKind::Data || resolved.kind == UrlKind::Virtual || resolved.kind == UrlKind::EnginePath){
            if (resolved.outsideAssets)
                return;
            StylesheetSource source;
            source.kind = StylesheetSource::Kind::File;
            source.path = resolved.assetPath;
            source.media = trim(link.attr("media"));
            source.ref = link.ref;
            context.addDependency(resolved.assetPath);
            context.stylesheets.push_back(std::move(source));
        }
    }

    void handleLink(TransformContext& context, HtmlNode& link){
        const std::vector<std::string> rels = splitWhitespace(toLower(link.attr("rel")));
        bool stylesheet = false;
        bool alternate = false;
        for (const std::string& rel : rels){
            if (rel == "stylesheet")
                stylesheet = true;
            else if (rel == "alternate")
                alternate = true;
        }
        const std::string type = toLower(trim(link.attr("type")));
        const std::string href = trim(link.attr("href"));

        if (!stylesheet){
            // RmlUi's own form (and the phase-1 guide's): <link type="text/css" href>, no rel
            if (rels.empty() && (type == "text/css" || type == "text/rcss") && !href.empty()){
                context.sink.report(codes::LinkSkipped, link.ref,
                                    "<link type=\"" + type + "\"> has no rel=\"stylesheet\": browsers ignore it; the engine loads it as a stylesheet",
                                    "add rel=\"stylesheet\"", "", "link");
                addLinkedSheet(context, link, href);
                return;
            }
            bool silent = !rels.empty();
            for (const std::string& rel : rels){
                if (!isRelSkippedSilently(rel))
                    silent = false;
            }
            if (!silent){
                context.sink.report(codes::LinkSkipped, link.ref,
                                    "<link rel=\"" + link.attr("rel") + "\"> has no effect in the engine and is skipped", "", "", "link");
            }
            return;
        }

        if (alternate){
            context.sink.report(codes::LinkSkipped, link.ref, "alternate stylesheet skipped (browsers do not apply it either)", "", "", "link");
            return;
        }
        if (link.hasAttr("disabled")){
            context.sink.report(codes::LinkSkipped, link.ref, "disabled stylesheet skipped", "", "", "link");
            return;
        }
        if (!type.empty() && type != "text/css" && type != "text/rcss"){
            context.sink.report(codes::LinkSkipped, link.ref, "stylesheet of type '" + type + "' skipped (browsers only apply text/css)", "", "", "link");
            return;
        }
        if (href.empty()){
            context.sink.report(codes::LinkSkipped, link.ref, "stylesheet link without href skipped", "", "", "link");
            return;
        }
        addLinkedSheet(context, link, href);
    }

    void handleStyle(TransformContext& context, HtmlNode& style){
        const std::string type = toLower(trim(style.attr("type")));
        if (!type.empty() && type != "text/css" && type != "text/rcss"){
            context.sink.report(codes::LinkSkipped, style.ref, "<style type=\"" + type + "\"> skipped (browsers only apply text/css)", "", "", "style");
            return;
        }
        StylesheetSource source;
        source.kind = StylesheetSource::Kind::Inline;
        source.text = rawText(context, style, source.baseOffset);
        source.media = trim(style.attr("media"));
        source.ref = style.ref;
        context.stylesheets.push_back(std::move(source));
    }

    void handleMeta(TransformContext& context, const HtmlNode& meta){
        std::string charset = toLower(trim(meta.attr("charset")));
        if (charset.empty() && equalsIgnoreCase(trim(meta.attr("http-equiv")), "content-type")){
            const std::string content = toLower(meta.attr("content"));
            const size_t at = content.find("charset=");
            if (at != std::string::npos)
                charset = trim(content.substr(at + 8));
        }
        if (!charset.empty() && charset != "utf-8" && charset != "utf8"){
            context.sink.report(codes::NonUtf8Charset, meta.ref, "the document declares charset '" + charset + "'; it is decoded as UTF-8",
                                "save the file as UTF-8 and declare <meta charset=\"utf-8\">", "", "meta");
        }
    }

    // link, style, meta, base and title anywhere in the document (head and body, in tree order,
    // as browsers apply them); not inside SVG (its <style>/<title> are SVG's) or templates
    void collectHead(TransformContext& context, HtmlNode& node){
        for (size_t i = 0; i < node.children.size();){
            HtmlNode& child = *node.children[i];
            if (child.kind != HtmlNode::Kind::Element || child.ns != Namespace::Html){
                i++;
                continue;
            }
            // (the head's element names are all 4 or 5 letters long)
            const size_t length = child.tag.size();
            if (length != 4 && length != 5){
                collectHead(context, child);
                i++;
                continue;
            }
            if (child.tag == "link"){
                handleLink(context, child);
            }else if (child.tag == "style"){
                handleStyle(context, child);
            }else if (child.tag == "meta"){
                handleMeta(context, child);
            }else if (child.tag == "base"){
                context.sink.report(codes::BaseIgnored, child.ref, "<base href> is ignored: URLs resolve against the document's own path",
                                    "write URLs relative to the document", "", "base");
            }else if (child.tag == "title"){
                if (context.title.empty()){
                    // document.title: whitespace stripped and collapsed
                    std::string title;
                    for (const std::string& word : splitWhitespace(child.textContent())){
                        if (!title.empty())
                            title += ' ';
                        title += word;
                    }
                    context.title = title;
                }
            }else{
                collectHead(context, child);
                i++;
                continue;
            }
            node.removeChild(&child);
        }
    }

    struct KnownLibrary {
        const char* pattern;      // in the src URL (or the inline text for Lucide)
        const char* name;
        Severity severity;
        const char* message;
        const char* suggestion;
    };

    const KnownLibrary knownLibraries[] = {
        {"unpkg.com/lucide", "Lucide", Severity::Info, "Lucide icons are built into the engine: data-lucide elements are expanded at load", "remove the script"},
        {"cdn.jsdelivr.net/npm/lucide", "Lucide", Severity::Info, "Lucide icons are built into the engine: data-lucide elements are expanded at load", "remove the script"},
        {"@lottiefiles/lottie-player", "lottie-player", Severity::Info, "lottie-player elements are mapped to the engine's <lottie>", "remove the script"},
        {"@lottiefiles/dotlottie-wc", "dotlottie-wc", Severity::Info, "dotlottie-wc elements are mapped to the engine's <lottie>", "remove the script"},
        {"dotlottie-player", "dotlottie-player", Severity::Info, "dotlottie-player elements are mapped to the engine's <lottie>", "remove the script"},
        {"cdn.tailwindcss.com", "Tailwind Play CDN", Severity::Warning, "Tailwind's Play CDN generates CSS in the browser with JavaScript: its classes have no styles here",
         "run the Tailwind CLI and <link rel=\"stylesheet\"> its output"},
        {"animejs", "Anime.js", Severity::Warning, "Anime.js is not run", "use the Lua anime API (same names: anime.animate, createTimeline, stagger)"},
        {"gsap", "GSAP", Severity::Warning, "GSAP is not run", "use the Lua anime API (animate, createTimeline, stagger)"},
        {"alpinejs", "Alpine.js", Severity::Warning, "Alpine.js is not run: its x- attributes do nothing", "port the logic to Lua (<script type=\"text/lua\"> with the element API)"},
        {"jquery", "jQuery", Severity::Warning, "jQuery is not run", "port the logic to Lua (<script type=\"text/lua\"> with the element API)"},
        {"react", "React", Severity::Warning, "React is not run", "write the markup as HTML and port the logic to Lua"},
        {"vue", "Vue", Severity::Warning, "Vue is not run", "write the markup as HTML and port the logic to Lua (or data binding: data-model)"},
    };

    bool isJavaScriptType(const std::string& type){
        static const char* const types[] = {"", "text/javascript", "application/javascript", "application/x-javascript",
                                            "text/ecmascript", "application/ecmascript", "text/jscript", "text/livescript",
                                            "text/x-javascript", "text/x-ecmascript", "module", "importmap", "text/babel",
                                            "text/jsx", "speculationrules"};
        for (const char* name : types){
            if (type == name)
                return true;
        }
        return false;
    }

    std::string lineOf(const TransformContext& context, SourceRef ref){
        if (!ref.valid())
            return "0";
        return std::to_string(context.files.line(ref.file, ref.offset));
    }

    void handleScript(TransformContext& context, HtmlNode& script, bool inert){
        std::string type = toLower(trim(script.attr("type")));
        const size_t parameters = type.find(';');
        if (parameters != std::string::npos)
            type = trim(type.substr(0, parameters));
        const std::string src = trim(script.attr("src"));

        if (type == "text/lua" || type == "lua" || type == "application/lua" || type == "text/x-lua"){
            if (inert){
                context.sink.reportAs(Severity::Info, codes::KnownScript, script.ref,
                                      "<script type=\"text/lua\"> in a template or an HTML fragment is not run (scripts run once per document load)",
                                      "move the script to the document", "", "script");
                return;
            }
            DocumentScript lua;
            lua.ref = script.ref;
            lua.chunkName = context.path + ":" + lineOf(context, script.ref);
            if (!src.empty()){
                ResolvedUrl resolved;
                context.resolveUrl(src, "script", script.ref, &resolved);
                if (resolved.dropped() || !resolved.local() || !resolved.exists)
                    return;   // reported by the resolver
                std::string code;
                if (!readAssetFile(context.fileAccess, resolved.assetPath, code)){
                    context.sink.report(codes::FileNotFound, script.ref, "Lua script '" + resolved.assetPath + "' could not be read", "", "", resolved.assetPath);
                    return;
                }
                stripUtf8Bom(code);
                lua.code = std::move(code);
                lua.srcPath = resolved.assetPath;
                lua.sourceFile = resolved.assetPath;
                lua.firstLine = 1;
                lua.chunkName = resolved.assetPath;
                context.addDependency(resolved.assetPath);
            }else{
                uint32_t offset = 0;
                lua.code = rawText(context, script, offset);
                lua.sourceFile = context.path;
                const bool fromSource = script.children.size() == 1 && script.children[0]->ref.valid();
                const SourcePos position = context.files.position(context.file, fromSource ? offset : script.ref.offset);
                lua.firstLine = position.line > 0 ? position.line : 1;
            }
            context.scripts.push_back(std::move(lua));
            return;
        }

        // a recognized library: tailored diagnostic
        // inline: only Lucide's createIcons() call is recognized (the first row)
        const bool lucideInline = src.empty() && script.textContent().find("lucide.createIcons(") != std::string::npos;
        for (const KnownLibrary& library : knownLibraries){
            if ((!src.empty() && toLower(src).find(library.pattern) != std::string::npos) || (lucideInline && &library == &knownLibraries[0])){
                context.sink.reportAs(library.severity, codes::KnownScript, script.ref,
                                      std::string(library.name) + " script not run: " + library.message, library.suggestion, "", library.name);
                return;
            }
        }

        if (isJavaScriptType(type)){
            context.sink.report(codes::Javascript, script.ref, "JavaScript is not run (the engine has no JavaScript engine)",
                                "use <script type=\"text/lua\"> with the element API (document:querySelector, addEventListener, ...)", "", "script");
        }else{
            context.sink.reportAs(Severity::Info, codes::KnownScript, script.ref,
                                  "script data block (type=\"" + type + "\") removed: no script reads it", "keep the data in a Lua table", "", "script");
        }
    }

    void collectScripts(TransformContext& context, HtmlNode& node, bool inert){
        for (size_t i = 0; i < node.children.size();){
            HtmlNode& child = *node.children[i];
            if (child.kind != HtmlNode::Kind::Element){
                i++;
                continue;
            }
            if (child.tag == "script" && (child.ns == Namespace::Html || child.ns == Namespace::Svg)){
                handleScript(context, child, inert);
                node.removeChild(&child);
                continue;
            }
            if (child.is("noscript")){
                context.sink.report(codes::NoscriptRendered, child.ref,
                                    "<noscript> content is shown (no JavaScript runs, so the browser would show it too)", "", "", "noscript");
                collectScripts(context, child, inert);
                child.unwrap();
                continue;   // its children now sit at i
            }
            collectScripts(context, child, inert);
            i++;
        }
    }

    // link/style/script in a fragment or template content: inert (browsers apply styles inserted
    // later, but compat rewrites CSS per document)
    void dropInertResources(TransformContext& context, HtmlNode& node){
        for (size_t i = 0; i < node.children.size();){
            HtmlNode& child = *node.children[i];
            if (child.kind == HtmlNode::Kind::Element && child.ns == Namespace::Html && (child.tag == "link" || child.tag == "style")){
                context.sink.report(codes::LinkSkipped, child.ref,
                                    "<" + child.tag + "> inside a template or an HTML fragment is not applied (stylesheets are compiled with the document)",
                                    "move it to the document's <head>", "", child.tag);
                node.removeChild(&child);
                continue;
            }
            if (child.kind == HtmlNode::Kind::Element)
                dropInertResources(context, child);
            i++;
        }
    }

    void renameReserved(TransformContext& context, HtmlNode& node){
        for (std::unique_ptr<HtmlNode>& owned : node.children){
            HtmlNode& child = *owned;
            if (child.kind != HtmlNode::Kind::Element)
                continue;
            if (child.ns == Namespace::Html && (child.tag == "htmlbody" || child.tag == "htmltemplate" || child.tag == "marker")){
                context.sink.report(codes::ReservedName, child.ref,
                                    "<" + child.tag + "> is a reserved element name; renamed <html-reserved-" + child.tag + ">",
                                    "use another element name", "", child.tag);
                child.tag = "html-reserved-" + child.tag;
            }
            for (HtmlAttr& attribute : child.attrs){
                if (attribute.name == "id" && attribute.value == DocumentLoader::kRootId){
                    context.sink.report(codes::ReservedName, attribute.nameRef.valid() ? attribute.nameRef : child.ref,
                                        "id=\"htmlui-root\" is reserved for the document root; renamed data-html-reserved-id",
                                        "use another id", "", "htmlui-root");
                    attribute.name = "data-html-reserved-id";
                }else if (startsWith(attribute.name, "data-htmlui-")){
                    const std::string renamed = "data-html-reserved-" + attribute.name.substr(12);
                    context.sink.report(codes::ReservedName, attribute.nameRef.valid() ? attribute.nameRef : child.ref,
                                        "'" + attribute.name + "' is a reserved attribute name; renamed " + renamed,
                                        "use another data- name", "", attribute.name);
                    attribute.name = renamed;
                }else if (attribute.name == "class" && child.ns == Namespace::Html && attribute.value.find("htmlui-") != std::string::npos){
                    std::string rewritten;
                    bool changed = false;
                    for (const std::string& name : splitWhitespace(attribute.value)){
                        std::string kept = name;
                        if (startsWith(name, "htmlui-")){
                            kept = "html-reserved-" + name.substr(7);
                            context.sink.report(codes::ReservedName, attribute.valueRef.valid() ? attribute.valueRef : child.ref,
                                                "class '" + name + "' is reserved for the engine; renamed " + kept, "use another class name", "", name);
                            changed = true;
                        }
                        if (!rewritten.empty())
                            rewritten += ' ';
                        rewritten += kept;
                    }
                    if (changed)
                        attribute.value = rewritten;
                }
            }
            renameReserved(context, child);
            if (child.templateContent)
                renameReserved(context, *child.templateContent);
        }
    }

    void collectTemplates(TransformContext& context, HtmlNode& node){
        for (size_t i = 0; i < node.children.size(); i++){
            HtmlNode& child = *node.children[i];
            if (child.kind != HtmlNode::Kind::Element)
                continue;
            if (child.is("template")){
                TemplateSource source;
                source.id = child.attr("id");
                source.ref = child.ref;
                source.content = std::move(child.templateContent);
                if (!source.content)
                    source.content = HtmlNode::makeElement("#content");
                source.content->parent = nullptr;
                collectScripts(context, *source.content, true);
                dropInertResources(context, *source.content);
                // nested templates become their own entries
                collectTemplates(context, *source.content);

                // the placeholder keeps getElementById and querySelector working (UA: display none)
                std::unique_ptr<HtmlNode> placeholder = HtmlNode::makeElement("htmltemplate");
                placeholder->implied = false;
                placeholder->ref = child.ref;
                if (child.hasAttr("id"))
                    placeholder->setAttr("id", child.attr("id")).nameRef = child.findAttr("id")->nameRef;
                if (child.hasAttr("class"))
                    placeholder->setAttr("class", child.attr("class"));
                // template.content finds the content by this index (an id is optional)
                placeholder->setAttr(LuaHooks::kTemplateAttribute, std::to_string(context.templates.size()));
                context.templates.push_back(std::move(source));
                placeholder->parent = &node;
                node.children[i] = std::move(placeholder);
                continue;
            }
            if (child.ns == Namespace::Html)
                collectTemplates(context, child);
        }
    }

}

void jasperengine::htmlui::compat::transformHead(TransformContext& context){
    HtmlNode& root = *context.root;
    collectHead(context, root);
    // the head disappears; what was left in it (unknown elements) goes to the body, as the
    // parser would have put them there
    for (size_t i = 0; i < root.children.size(); i++){
        HtmlNode& child = *root.children[i];
        if (!child.is("head"))
            continue;
        std::unique_ptr<HtmlNode> head = root.removeChild(&child);
        HtmlNode* body = nullptr;
        for (std::unique_ptr<HtmlNode>& sibling : root.children){
            if (sibling->is("body"))
                body = sibling.get();
        }
        size_t at = 0;
        for (std::unique_ptr<HtmlNode>& leftover : head->children){
            if (leftover->kind == HtmlNode::Kind::Element && body)
                body->insertChild(at++, std::move(leftover));
        }
        break;
    }
}

void jasperengine::htmlui::compat::transformScripts(TransformContext& context, HtmlNode& tree){
    collectScripts(context, tree, context.fragment);
    if (context.fragment)
        dropInertResources(context, tree);
}

void jasperengine::htmlui::compat::transformReservedNames(TransformContext& context, HtmlNode& tree){
    renameReserved(context, tree);
}

void jasperengine::htmlui::compat::transformRoot(TransformContext& context){
    HtmlNode& root = *context.root;

    // <html id="x"> keeps its author id as data-html-id; the RML body is #htmlui-root
    if (HtmlAttr* id = root.findAttr("id")){
        context.rootId = id->value;
        id->name = "data-html-id";
    }
    HtmlAttr& rootId = root.setAttr("id", DocumentLoader::kRootId);
    rootId.nameRef = SourceRef{};
    context.rootDirection = toLower(trim(root.attr("dir")));
    // phase-3 spec 2.2: the text runs right to left, the layout is not mirrored
    if (context.rootDirection == "rtl"){
        const HtmlAttr* dir = root.findAttr("dir");
        context.sink.reportAs(Severity::Info, codes::ApproximatedMinor, dir ? dir->nameRef : SourceRef{},
                              "<html dir=\"rtl\"> sets the text direction; block, flex and grid layouts are not mirrored", hints::kMirroredLayout);
    }

    for (std::unique_ptr<HtmlNode>& child : root.children){
        if (child->is("body")){
            context.body = child.get();
            break;
        }
    }
    if (!context.body){
        // a frameset document has no body: the RML shape keeps one
        std::unique_ptr<HtmlNode> body = HtmlNode::makeElement("body");
        context.body = root.appendChild(std::move(body));
        // what the parser put next to the frameset goes into it
        for (size_t i = 0; i < root.children.size();){
            HtmlNode* child = root.children[i].get();
            if (child == context.body || child->kind != HtmlNode::Kind::Element){
                i++;
                continue;
            }
            context.body->appendChild(root.removeChild(child));
        }
    }
}

void jasperengine::htmlui::compat::transformTemplates(TransformContext& context, HtmlNode& tree){
    collectTemplates(context, tree);
}
