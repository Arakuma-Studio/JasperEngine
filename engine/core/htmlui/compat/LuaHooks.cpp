// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/LuaHooks.h"

#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/css/CssRewriter.h"
#include "htmlui/compat/html/HtmlParser.h"
#include "htmlui/compat/html/HtmlTransform.h"
#include "htmlui/compat/html/RmlSerializer.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>

#include <algorithm>
#include <cstdlib>
#include <map>

using namespace jasperengine::htmlui::compat;

namespace {

    bool startsWith(std::string_view text, std::string_view prefix){
        return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
    }

    void writeElement(Rml::Element* element, std::string& out);

    void writeChildren(Rml::Element* element, std::string& out){
        // an <svg>'s content is its SVG text, not elements
        if (element->GetTagName() == "svg"){
            std::string svg;
            element->GetInnerRML(svg);
            out += svg;
            return;
        }
        const int count = element->GetNumChildren();
        for (int i = 0; i < count; i++)
            writeElement(element->GetChild(i), out);
    }

    void writeElement(Rml::Element* element, std::string& out){
        if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(element)){
            out += RmlSerializer::escapeText(text->GetText());
            return;
        }
        const std::string name = LuaHooks::localName(element);
        out += '<';
        out += name;
        // RmlUi keeps attributes unordered: sorted, so the text is stable
        std::map<std::string, std::string> attributes;
        for (const auto& pair : element->GetAttributes()){
            if (LuaHooks::isHiddenAttribute(pair.first))
                continue;
            std::string value = pair.second.Get<Rml::String>();
            if (pair.first == "class"){
                value = LuaHooks::visibleClassNames(value);
                if (value.empty())
                    continue;
            }
            if (rmlui_dynamic_cast<Rml::ElementDocument*>(element) && pair.first == "id" && value == DocumentLoader::kRootId)
                continue;
            attributes[pair.first] = value;
        }
        if (rmlui_dynamic_cast<Rml::ElementDocument*>(element)){
            // the author id of <html>
            auto authorId = attributes.find("data-html-id");
            if (authorId != attributes.end()){
                attributes["id"] = authorId->second;
                attributes.erase(authorId);
            }
        }
        for (const auto& pair : attributes){
            out += ' ';
            out += pair.first;
            out += "=\"";
            out += RmlSerializer::escapeAttribute(pair.second);
            out += '"';
        }
        out += '>';
        if (HtmlParser::isVoidElement(name))
            return;
        writeChildren(element, out);
        out += "</";
        out += name;
        out += '>';
    }

}

bool LuaHooks::isCompat(const Rml::ElementDocument* document){
    return document && document->GetId() == DocumentLoader::kRootId;
}

std::string LuaHooks::selector(std::string_view css){
    return CssRewriter::rewriteSelector(css, nullptr);
}

std::string LuaHooks::localName(const Rml::Element* element){
    if (!element)
        return std::string();
    if (rmlui_dynamic_cast<const Rml::ElementDocument*>(element))
        return "html";
    const std::string& tag = element->GetTagName();
    if (tag == "htmlbody")
        return "body";
    if (tag == "htmltemplate")
        return "template";
    return tag;
}

std::string LuaHooks::tagName(const Rml::Element* element){
    std::string name = localName(element);
    if (name == "svg" || name == "lottie" || name == "#text")
        return name;
    for (const Rml::Element* ancestor = element ? element->GetParentNode() : nullptr; ancestor; ancestor = ancestor->GetParentNode()){
        if (ancestor->GetTagName() == "svg")
            return name;
    }
    for (char& c : name){
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
    }
    return name;
}

bool LuaHooks::isHiddenAttribute(std::string_view name){
    return startsWith(name, "data-htmlui-");
}

bool LuaHooks::isHiddenClass(std::string_view name){
    return startsWith(name, "htmlui-");
}

std::string LuaHooks::visibleClassNames(const std::string& classNames){
    std::string out;
    for (const std::string& name : html::splitWhitespace(classNames)){
        if (isHiddenClass(name))
            continue;
        if (!out.empty())
            out += ' ';
        out += name;
    }
    return out;
}

std::string LuaHooks::withHiddenClasses(const std::string& visible, const std::string& current){
    std::string out;
    for (const std::string& name : html::splitWhitespace(visible)){
        if (isHiddenClass(name))
            continue;   // scripts cannot set engine classes
        if (!out.empty())
            out += ' ';
        out += name;
    }
    for (const std::string& name : html::splitWhitespace(current)){
        if (!isHiddenClass(name))
            continue;
        if (!out.empty())
            out += ' ';
        out += name;
    }
    return out;
}

std::string LuaHooks::innerHTML(Rml::Element* element){
    std::string out;
    if (element)
        writeChildren(element, out);
    return out;
}

std::string LuaHooks::outerHTML(Rml::Element* element){
    std::string out;
    if (element)
        writeElement(element, out);
    return out;
}

std::string LuaHooks::fragmentToRml(std::string_view html, Rml::ElementDocument* document, const std::string& what){
    FragmentOptions options;
    std::string documentPath;
    if (std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compiledOf(document)){
        documentPath = compiled->sourcePath;
        options.compat = compiled->options;
    }else if (document){
        documentPath = document->GetSourceURL();
    }
    options.documentPath = documentPath;
    std::vector<Diagnostic> diagnostics;
    const std::string chunk = (documentPath.empty() ? std::string("document") : documentPath) + " (" + what + ")";
    std::string rml = DocumentLoader::fragmentToRml(html, chunk, diagnostics, options);
    if (!diagnostics.empty()){
        // only what is lost is worth a log line here (infos stay with the document)
        std::vector<Diagnostic> logged;
        for (const Diagnostic& diagnostic : diagnostics){
            if (diagnostic.severity != Severity::Info)
                logged.push_back(diagnostic);
            DocumentLoader::addRuntimeDiagnostic(document, diagnostic);
        }
        for (const Diagnostic& diagnostic : logged)
            logDiagnostics(chunk, std::vector<Diagnostic>{diagnostic}, false);
    }
    return rml;
}

std::vector<std::pair<std::string, std::string>> LuaHooks::rewriteDeclarations(const std::string& css){
    std::vector<std::pair<std::string, std::string>> declarations;
    const std::string rcss = CssRewriter::rewriteDeclarations(css, nullptr);
    // "name: value; name: value;" (values never hold a bare ';' outside strings and parentheses)
    size_t start = 0;
    int depth = 0;
    char quote = 0;
    for (size_t i = 0; i <= rcss.size(); i++){
        const char c = i < rcss.size() ? rcss[i] : ';';
        if (quote){
            if (c == '\\' && i + 1 < rcss.size())
                i++;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\''){
            quote = c;
        }else if (c == '('){
            depth++;
        }else if (c == ')'){
            depth--;
        }else if (c == ';' && depth <= 0){
            const std::string part = html::trim(rcss.substr(start, i - start));
            start = i + 1;
            const size_t colon = part.find(':');
            if (colon == std::string::npos)
                continue;
            declarations.emplace_back(html::trim(part.substr(0, colon)), html::trim(part.substr(colon + 1)));
        }
    }
    return declarations;
}

std::string LuaHooks::lucideSvg(const Rml::Element* element, const std::string& name, std::string* shapesOut,
                                std::vector<std::pair<std::string, std::string>>* attributesOut){
    LucideIcon icon;
    if (!findLucideIcon(name, icon))
        return std::string();
    std::vector<std::pair<std::string, std::string>> current;
    if (element){
        for (const auto& pair : element->GetAttributes()){
            if (isHiddenAttribute(pair.first) || pair.first == "data-lucide")
                continue;
            std::string value = pair.second.Get<Rml::String>();
            if (pair.first == "class"){
                // the classes of a previous icon go with it
                std::string kept;
                for (const std::string& className : html::splitWhitespace(value)){
                    if (className == "lucide" || startsWith(className, "lucide-") || isHiddenClass(className))
                        continue;
                    kept += (kept.empty() ? "" : " ") + className;
                }
                value = kept;
            }
            current.emplace_back(pair.first, value);
        }
        std::sort(current.begin(), current.end());
    }
    std::vector<std::pair<std::string, std::string>> attributes = lucideAttributes(current, html::trim(name));
    std::string rml = "<svg";
    for (const auto& pair : attributes)
        rml += " " + pair.first + "=\"" + RmlSerializer::escapeAttribute(pair.second) + "\"";
    rml += ">" + icon.shapes + "</svg>";
    if (shapesOut)
        *shapesOut = icon.shapes;
    if (attributesOut)
        *attributesOut = std::move(attributes);
    return rml;
}

std::string LuaHooks::templateContent(const Rml::Element* placeholder){
    if (!placeholder)
        return std::string();
    const Rml::Variant* index = placeholder->GetAttribute(kTemplateAttribute);
    Rml::ElementDocument* document = const_cast<Rml::Element*>(placeholder)->GetOwnerDocument();
    std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compiledOf(document);
    if (!index || !compiled)
        return std::string();
    const size_t position = (size_t)std::strtoul(index->Get<Rml::String>().c_str(), nullptr, 10);
    if (position >= compiled->templates.size())
        return std::string();
    return compiled->templates[position].rml;
}

Rml::Element* LuaHooks::body(Rml::ElementDocument* document){
    if (!isCompat(document))
        return nullptr;
    const int count = document->GetNumChildren();
    for (int i = 0; i < count; i++){
        Rml::Element* child = document->GetChild(i);
        if (child->GetTagName() == "htmlbody")
            return child;
    }
    return nullptr;
}
