// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HtmlUIFontDiagnostics.h"

#include "HtmlUI.h"
#include "HtmlUIContext.h"
#include "Log.h"
#include "RmlFontEngine.h"
#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/VirtualFiles.h"
#include "util/FontFace.h"
#include "util/FontRegistry.h"
#include "util/TextShaper.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    void appendUtf8(std::string& out, uint32_t c){
        if (c < 0x80){
            out += (char)c;
        }else if (c < 0x800){
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }else if (c < 0x10000){
            out += (char)(0xE0 | (c >> 12));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }else{
            out += (char)(0xF0 | (c >> 18));
            out += (char)(0x80 | ((c >> 12) & 0x3F));
            out += (char)(0x80 | ((c >> 6) & 0x3F));
            out += (char)(0x80 | (c & 0x3F));
        }
    }

    std::string describe(const std::vector<uint32_t>& codepoints){
        // the characters and their code points: "'Ó' (U+00D3), 'Ñ' (U+00D1)"
        std::string text;
        for (size_t i = 0; i < codepoints.size(); i++){
            const uint32_t c = codepoints[i];
            std::string utf8;
            appendUtf8(utf8, c);
            char code[16];
            std::snprintf(code, sizeof(code), "U+%04X", (unsigned int)c);
            text += (i ? ", '" : "'") + utf8 + "' (" + code + ")";
        }
        return text;
    }

    // the report's code, text and subject (the runtime's and the check's say the same, so a document reports it once)
    void describeFallback(Diagnostic& diagnostic, const std::vector<uint32_t>& codepoints, const std::string& primary, const std::string& familyList){
        diagnostic.code = codes::FallbackGlyph;
        diagnostic.severity = Severity::Info;
        // (RmlUi passes the list normalized, "a,b": written back as CSS lists it)
        std::string family;
        for (char c : familyList)
            family += (c == ',') ? std::string(", ") : std::string(1, c);
        diagnostic.message = describe(codepoints) + (codepoints.size() == 1 ? " is" : " are") + " not in '" + primary +
                             "', the first face of font-family '" + family + "': a fallback face draws " +
                             (codepoints.size() == 1 ? "it" : "them") + " (font-effect and synthetic bold and italic apply to it)";
        diagnostic.suggestion = "use a font with these characters, or name one that has them after it in font-family";
        diagnostic.subject = familyList;
    }

    Diagnostic makeDiagnostic(const RmlFontEngine::FallbackGlyphs& glyphs, const std::shared_ptr<const CompatDocument>& compiled,
                              Rml::Element* element){
        Diagnostic diagnostic;
        // the text's element carries the source location (a text node has none of its own)
        Rml::Element* located = element;
        while (compiled && located && !located->GetAttribute(DocumentLoader::kLocationAttribute))
            located = located->GetParentNode();
        if (compiled && located){
            if (std::optional<Diagnostic> location = locateElement(*compiled, located))
                diagnostic = *location;
        }else if (compiled){
            diagnostic.file = compiled->sourcePath;
        }
        describeFallback(diagnostic, glyphs.codepoints, glyphs.primary, glyphs.family);
        return diagnostic;
    }

    // the text elements of a subtree drawn with the handle, and their own fallback code points
    void findTexts(Rml::Element* element, Rml::FontFaceHandle handle, const std::set<uint32_t>& drawn,
                   std::vector<std::pair<Rml::Element*, RmlFontEngine::FallbackGlyphs>>& found){
        if (Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(element)){
            if (element->GetFontFaceHandle() == handle){
                RmlFontEngine::FallbackGlyphs glyphs = HtmlUI::getFontEngine().fallbackGlyphsOf(handle, text->GetText());
                const bool drawnHere = std::any_of(glyphs.codepoints.begin(), glyphs.codepoints.end(),
                                                   [&drawn](uint32_t c){ return drawn.count(c) > 0; });
                if (drawnHere)
                    found.push_back({element, std::move(glyphs)});
            }
        }
        for (int i = 0; i < element->GetNumChildren(); i++)
            findTexts(element->GetChild(i), handle, drawn, found);
    }

    bool sameDiagnostic(const Diagnostic& a, const Diagnostic& b){
        return a.code == b.code && a.subject == b.subject && a.message == b.message;
    }

}

void HtmlUIFontDiagnostics::update(){
    RmlFontEngine& fonts = HtmlUI::getFontEngine();
    for (HtmlUIContext* context : HtmlUI::getContexts()){
        Rml::Context* rml = context->getRmlContext();
        if (!rml)
            continue;
        const std::vector<RmlFontEngine::FallbackGlyphs> events = fonts.takeFallbackGlyphs(rml->GetRenderManager());
        for (const RmlFontEngine::FallbackGlyphs& event : events){
            const std::set<uint32_t> drawn(event.codepoints.begin(), event.codepoints.end());
            for (int d = 0; d < rml->GetNumDocuments(); d++){
                Rml::ElementDocument* document = rml->GetDocument(d);
                std::vector<std::pair<Rml::Element*, RmlFontEngine::FallbackGlyphs>> found;
                findTexts(document, event.handle, drawn, found);
                const std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compiledOf(document);
                for (const auto& text : found){
                    const Diagnostic diagnostic = makeDiagnostic(text.second, compiled, text.first);
                    if (compiled){
                        // once per (document, family, code point set)
                        const std::vector<Diagnostic>& known = DocumentLoader::diagnosticsOf(document);
                        if (std::any_of(known.begin(), known.end(), [&](const Diagnostic& k){ return sameDiagnostic(k, diagnostic); }))
                            continue;
                        DocumentLoader::addRuntimeDiagnostic(document, diagnostic);
                    }else{
                        static std::set<std::string> logged;
                        if (!logged.insert(diagnostic.subject + "\n" + diagnostic.message).second)
                            continue;
                    }
                    Log::verbose("[HTML UI] %s", toText(diagnostic).c_str());
                }
            }
        }
    }
}

// ---- the check without RmlUi (check --ui, inspect_ui of a file): the compile's RML and RCSS ----

namespace {

    // the font properties a text inherits, as the cascade below resolves them
    struct GlyphFontStyle {
        std::string family;   // normalized: lowercase, unquoted, "a,b"
        int weight = 400;
        bool italic = false;
    };

    struct GlyphFontDeclarations {
        bool hasFamily = false, hasWeight = false, hasStyle = false;
        GlyphFontStyle style;
    };

    std::string glyphLowered(std::string text){
        for (char& c : text)
            c = (char)std::tolower((unsigned char)c);
        return text;
    }

    std::string glyphTrimmed(const std::string& text){
        const size_t begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
            return std::string();
        return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
    }

    // "'A B', sans-serif" -> "a b,sans-serif" (RmlUi's form of the list)
    std::string glyphFamilyList(const std::string& value){
        std::string out;
        for (const std::string& name : RmlFontEngine::splitFamilyList(value))
            out += (out.empty() ? "" : ",") + name;
        return out;
    }

    int glyphWeight(const std::string& value){
        const std::string v = glyphLowered(glyphTrimmed(value));
        if (v == "bold" || v == "bolder")
            return 700;
        if (v == "normal" || v == "lighter")
            return 400;
        const int number = std::atoi(v.c_str());
        return number > 0 ? std::min(number, 1000) : 400;
    }

    // the font declarations of an RCSS declaration list
    GlyphFontDeclarations glyphDeclarations(const std::string& block){
        GlyphFontDeclarations out;
        size_t at = 0;
        while (at < block.size()){
            const size_t end = std::min(block.find(';', at), block.size());
            const std::string declaration = block.substr(at, end - at);
            at = end + 1;
            const size_t colon = declaration.find(':');
            if (colon == std::string::npos)
                continue;
            const std::string name = glyphLowered(glyphTrimmed(declaration.substr(0, colon)));
            std::string value = glyphTrimmed(declaration.substr(colon + 1));
            const size_t important = glyphLowered(value).find("!important");
            if (important != std::string::npos)
                value = glyphTrimmed(value.substr(0, important));
            // (the inherited value stays; the font properties inherit)
            const std::string keyword = glyphLowered(value);
            if (keyword == "inherit" || keyword == "unset" || keyword == "revert" || keyword.find("var(") != std::string::npos)
                continue;
            if (name == "font-family"){
                out.hasFamily = true;
                out.style.family = glyphFamilyList(value);
            }else if (name == "font-weight"){
                out.hasWeight = true;
                out.style.weight = glyphWeight(value);
            }else if (name == "font-style"){
                out.hasStyle = true;
                out.style.italic = glyphLowered(value) != "normal";
            }
        }
        return out;
    }

    // one compound selector: type, id, classes; anything else (attributes, pseudo-classes) makes it match nothing
    struct GlyphSelectorCompound {
        std::string tag;
        std::string id;
        std::vector<std::string> classes;
        bool unsupported = false;
        char combinator = ' ';   // with the compound to its left: ' ' descendant, '>' child
    };

    struct GlyphSelector {
        std::vector<GlyphSelectorCompound> compounds;   // left to right
        int specificity = 0;
    };

    struct GlyphStyleRule {
        std::vector<GlyphSelector> selectors;
        GlyphFontDeclarations declarations;
        int layer = 0;   // 0 user agent, 1 author, 2 !important
        int order = 0;
    };

    struct GlyphFontFaceRule {
        std::string family;   // normalized
        std::string path;
        int weight = 400;
        bool italic = false;
        std::shared_ptr<FontFace> face;
        bool loaded = false;
    };

    GlyphSelector glyphSelector(const std::string& text){
        GlyphSelector selector;
        GlyphSelectorCompound current;
        bool pending = false;
        char combinator = ' ';
        size_t i = 0;
        auto flush = [&](){
            if (!pending)
                return;
            current.combinator = combinator;
            selector.compounds.push_back(current);
            current = GlyphSelectorCompound();
            pending = false;
            combinator = ' ';
        };
        auto identifier = [&](){
            std::string name;
            while (i < text.size() && (std::isalnum((unsigned char)text[i]) || text[i] == '-' || text[i] == '_' || (unsigned char)text[i] >= 0x80))
                name += text[i++];
            return name;
        };
        while (i < text.size()){
            const char c = text[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r'){
                flush();
                i++;
            }else if (c == '>'){
                flush();
                combinator = '>';
                i++;
            }else if (c == '+' || c == '~'){
                current.unsupported = true;
                flush();
                i++;
            }else if (c == '#'){
                i++;
                current.id = identifier();
                selector.specificity += 10000;
                pending = true;
            }else if (c == '.'){
                i++;
                current.classes.push_back(identifier());
                selector.specificity += 100;
                pending = true;
            }else if (c == '*'){
                i++;
                pending = true;
            }else if (c == '[' || c == ':'){
                // attribute selectors and pseudo-classes: states and conditions a static check cannot know
                current.unsupported = true;
                pending = true;
                const char close = (c == '[') ? ']' : ' ';
                int depth = 0;
                i++;
                while (i < text.size()){
                    if (text[i] == '(')
                        depth++;
                    else if (text[i] == ')')
                        depth--;
                    else if (depth == 0 && (text[i] == close || (close == ' ' && (text[i] == '.' || text[i] == '#' || text[i] == '>' ||
                                                                                   text[i] == ' ' || text[i] == '['))))
                        break;
                    i++;
                }
                if (close == ']' && i < text.size())
                    i++;
            }else{
                current.tag = glyphLowered(identifier());
                if (current.tag.empty()){
                    current.unsupported = true;
                    i++;
                }else{
                    selector.specificity += 1;
                }
                pending = true;
            }
        }
        flush();
        return selector;
    }

    // the rules and @font-face rules of an RCSS sheet (@media blocks: their rules apply, as at the canvas size)
    void glyphSheet(const std::string& rcss, int layer, int& order, std::vector<GlyphStyleRule>& rules, std::vector<GlyphFontFaceRule>& faces){
        std::string text;
        text.reserve(rcss.size());
        for (size_t i = 0; i < rcss.size(); i++){
            if (rcss[i] == '/' && i + 1 < rcss.size() && rcss[i + 1] == '*'){
                const size_t end = rcss.find("*/", i + 2);
                i = (end == std::string::npos) ? rcss.size() : end + 1;
                continue;
            }
            text += rcss[i];
        }
        size_t at = 0;
        while (at < text.size()){
            const size_t open = text.find('{', at);
            if (open == std::string::npos)
                break;
            const std::string prelude = glyphTrimmed(text.substr(at, open - at));
            if (!prelude.empty() && prelude[0] == '@' && glyphLowered(prelude).rfind("@font-face", 0) != 0){
                // @media and other blocks: their rules are read as the sheet's (nested braces)
                at = open + 1;
                continue;
            }
            const size_t close = text.find('}', open);
            if (close == std::string::npos)
                break;
            const std::string block = text.substr(open + 1, close - open - 1);
            at = close + 1;
            if (prelude.empty())
                continue;
            if (glyphLowered(prelude).rfind("@font-face", 0) == 0){
                GlyphFontFaceRule face;
                size_t d = 0;
                while (d < block.size()){
                    const size_t end = std::min(block.find(';', d), block.size());
                    const std::string declaration = block.substr(d, end - d);
                    d = end + 1;
                    const size_t colon = declaration.find(':');
                    if (colon == std::string::npos)
                        continue;
                    const std::string name = glyphLowered(glyphTrimmed(declaration.substr(0, colon)));
                    const std::string value = glyphTrimmed(declaration.substr(colon + 1));
                    if (name == "font-family")
                        face.family = glyphFamilyList(value);
                    else if (name == "src"){
                        // compat writes "asset://<asset path>" (what the font engine opens); FileAccess takes the asset path
                        face.path = value;
                        if (face.path.rfind("asset://", 0) == 0)
                            face.path = face.path.substr(8);
                        while (!face.path.empty() && face.path[0] == '/')
                            face.path.erase(0, 1);
                    }
                    else if (name == "font-weight")
                        face.weight = glyphWeight(value);
                    else if (name == "font-style")
                        face.italic = glyphLowered(value) != "normal";
                }
                if (!face.family.empty() && !face.path.empty())
                    faces.push_back(std::move(face));
                continue;
            }
            GlyphStyleRule rule;
            rule.declarations = glyphDeclarations(block);
            if (!rule.declarations.hasFamily && !rule.declarations.hasWeight && !rule.declarations.hasStyle)
                continue;
            size_t s = 0;
            while (s <= prelude.size()){
                const size_t comma = std::min(prelude.find(',', s), prelude.size());
                const GlyphSelector selector = glyphSelector(glyphTrimmed(prelude.substr(s, comma - s)));
                if (!selector.compounds.empty())
                    rule.selectors.push_back(selector);
                s = comma + 1;
            }
            rule.layer = layer;
            rule.order = order++;
            rules.push_back(std::move(rule));
        }
    }

    // the RML's element tree: what a selector and the text need
    struct GlyphRmlNode {
        int parent = -1;
        std::string tag, id, style;
        std::vector<std::string> classes;
        int location = -1;   // data-htmlui-loc
        std::string text;    // the element's own text
    };

    std::string glyphDecodedText(const std::string& text){
        std::string out;
        for (size_t i = 0; i < text.size(); i++){
            if (text[i] != '&'){
                out += text[i];
                continue;
            }
            const size_t semi = text.find(';', i);
            if (semi == std::string::npos){
                out += text[i];
                continue;
            }
            const std::string entity = text.substr(i + 1, semi - i - 1);
            uint32_t codepoint = 0;
            if (entity == "amp") codepoint = '&';
            else if (entity == "lt") codepoint = '<';
            else if (entity == "gt") codepoint = '>';
            else if (entity == "quot") codepoint = '"';
            else if (entity == "nbsp") codepoint = 0xA0;
            else if (!entity.empty() && entity[0] == '#')
                codepoint = (uint32_t)std::strtoul(entity.c_str() + (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X') ? 2 : 1), nullptr,
                                                   entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X') ? 16 : 10);
            if (codepoint == 0){
                out += text[i];
                continue;
            }
            appendUtf8(out, codepoint);
            i = semi;
        }
        return out;
    }

    std::vector<GlyphRmlNode> glyphRmlNodes(const std::string& rml){
        std::vector<GlyphRmlNode> nodes;
        int current = -1;
        int skipped = 0;   // inside head, style, script, template, svg: no text of the page
        size_t i = 0;
        auto skippedTag = [](const std::string& tag){
            return tag == "head" || tag == "style" || tag == "script" || tag == "template" || tag == "svg" || tag == "title";
        };
        while (i < rml.size()){
            if (rml[i] != '<'){
                const size_t next = std::min(rml.find('<', i), rml.size());
                if (current >= 0 && skipped == 0)
                    nodes[(size_t)current].text += glyphDecodedText(rml.substr(i, next - i));
                i = next;
                continue;
            }
            if (rml.compare(i, 4, "<!--") == 0){
                const size_t end = rml.find("-->", i);
                i = (end == std::string::npos) ? rml.size() : end + 3;
                continue;
            }
            if (rml.compare(i, 2, "<!") == 0 || rml.compare(i, 2, "<?") == 0){
                i = std::min(rml.find('>', i), rml.size()) + 1;
                continue;
            }
            const bool closing = i + 1 < rml.size() && rml[i + 1] == '/';
            size_t j = i + (closing ? 2 : 1);
            std::string tag;
            while (j < rml.size() && !std::isspace((unsigned char)rml[j]) && rml[j] != '>' && rml[j] != '/')
                tag += rml[j++];
            tag = glyphLowered(tag);
            if (closing){
                i = std::min(rml.find('>', j), rml.size()) + 1;
                if (skipped > 0){
                    if (skippedTag(tag))
                        skipped--;
                    continue;
                }
                if (current >= 0)
                    current = nodes[(size_t)current].parent;
                continue;
            }
            GlyphRmlNode node;
            node.tag = tag;
            node.parent = current;
            bool selfClosing = false;
            while (j < rml.size() && rml[j] != '>'){
                if (rml[j] == '/'){
                    selfClosing = true;
                    j++;
                    continue;
                }
                if (std::isspace((unsigned char)rml[j])){
                    j++;
                    continue;
                }
                std::string name;
                while (j < rml.size() && rml[j] != '=' && rml[j] != '>' && !std::isspace((unsigned char)rml[j]) && rml[j] != '/')
                    name += rml[j++];
                std::string value;
                if (j < rml.size() && rml[j] == '='){
                    j++;
                    const char quote = (j < rml.size() && (rml[j] == '"' || rml[j] == '\'')) ? rml[j++] : 0;
                    while (j < rml.size() && (quote ? rml[j] != quote : (!std::isspace((unsigned char)rml[j]) && rml[j] != '>')))
                        value += rml[j++];
                    if (quote && j < rml.size())
                        j++;
                }
                name = glyphLowered(name);
                value = glyphDecodedText(value);
                if (name == "id")
                    node.id = value;
                else if (name == "class"){
                    std::string word;
                    for (char c : value + " "){
                        if (std::isspace((unsigned char)c)){
                            if (!word.empty())
                                node.classes.push_back(word);
                            word.clear();
                        }else{
                            word += c;
                        }
                    }
                }else if (name == "style")
                    node.style = value;
                else if (name == DocumentLoader::kLocationAttribute)
                    node.location = std::atoi(value.c_str());
            }
            i = j + 1;
            if (skipped > 0 || skippedTag(tag)){
                if (!selfClosing && skippedTag(tag))
                    skipped++;
                continue;
            }
            nodes.push_back(node);
            if (!selfClosing)
                current = (int)nodes.size() - 1;
        }
        return nodes;
    }

    bool glyphMatchesCompound(const GlyphSelectorCompound& compound, const GlyphRmlNode& node){
        if (compound.unsupported)
            return false;
        if (!compound.tag.empty() && compound.tag != node.tag)
            return false;
        if (!compound.id.empty() && compound.id != node.id)
            return false;
        for (const std::string& name : compound.classes){
            if (std::find(node.classes.begin(), node.classes.end(), name) == node.classes.end())
                return false;
        }
        return true;
    }

    bool glyphMatches(const GlyphSelector& selector, const std::vector<GlyphRmlNode>& nodes, int index, int compound){
        if (compound < 0)
            return true;
        if (index < 0 || !glyphMatchesCompound(selector.compounds[(size_t)compound], nodes[(size_t)index]))
            return false;
        if (compound == 0)
            return true;
        if (selector.compounds[(size_t)compound].combinator == '>')
            return glyphMatches(selector, nodes, nodes[(size_t)index].parent, compound - 1);
        for (int ancestor = nodes[(size_t)index].parent; ancestor >= 0; ancestor = nodes[(size_t)ancestor].parent){
            if (glyphMatches(selector, nodes, ancestor, compound - 1))
                return true;
        }
        return false;
    }

    // the face a family of the list gives (the runtime's chain): the document's @font-face of the nearest weight in the
    // style, a generic family or Roboto the built-in Roboto; null when nothing provides it
    std::shared_ptr<FontFace> glyphFaceOf(const std::string& family, const GlyphFontStyle& style, std::vector<GlyphFontFaceRule>& faces, const FileAccess& files){
        if (FontRegistry::isGenericFamily(family) || family == "roboto")
            return FontRegistry::getBuiltInLatin();
        GlyphFontFaceRule* best = nullptr;
        auto distance = [&style](const GlyphFontFaceRule& face){
            return (face.italic != style.italic ? 10000 : 0) + std::abs(face.weight - style.weight);
        };
        for (GlyphFontFaceRule& face : faces){
            if (face.family == family && (!best || distance(face) < distance(*best)))
                best = &face;
        }
        if (!best)
            return nullptr;
        if (!best->loaded){
            best->loaded = true;
            std::string bytes;
            std::string error;
            if (readAssetFile(files, best->path, bytes) && !bytes.empty())
                best->face = FontFace::loadMemory(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), 0, true, best->path, &error);
        }
        return best->face;
    }

}

std::vector<Diagnostic> HtmlUIFontDiagnostics::checkStatic(const CompatDocument& compiled, const FileAccess& files){
    std::vector<Diagnostic> diagnostics;
    if (!compiled.compiled || compiled.declaredFontFamilies.empty())
        return diagnostics;

    // the sheets in cascade order: the user agent's, the hints and the author's, then !important
    std::vector<GlyphStyleRule> rules;
    std::vector<GlyphFontFaceRule> faces;
    int order = 0;
    if (std::shared_ptr<const std::string> ua = VirtualFiles::find(compiled.userAgentPath))
        glyphSheet(*ua, 0, order, rules, faces);
    glyphSheet(compiled.hints.rcss, 1, order, rules, faces);
    for (const StylesheetOutput& sheet : compiled.stylesheets)
        glyphSheet(sheet.rcss, 1, order, rules, faces);
    glyphSheet(compiled.important.rcss, 2, order, rules, faces);
    if (faces.empty())
        return diagnostics;

    std::vector<GlyphRmlNode> nodes = glyphRmlNodes(compiled.rml);
    std::vector<GlyphFontStyle> computed(nodes.size());
    const std::shared_ptr<FontFace> latin = FontRegistry::getBuiltInLatin();
    const std::shared_ptr<FontFace> arabic = FontRegistry::getBuiltInArabic();
    DiagnosticSink sink(compiled.files);
    for (size_t n = 0; n < nodes.size(); n++){
        GlyphFontStyle style = nodes[n].parent >= 0 ? computed[(size_t)nodes[n].parent] : GlyphFontStyle();
        // each property from the matching rule of the highest layer, then specificity, then order; the style attribute
        // above the normal rules
        struct GlyphCascadeWinner { int layer = -1, specificity = -1, order = -1; };
        GlyphCascadeWinner family, weight, italic;
        auto better = [](const GlyphCascadeWinner& w, int layer, int specificity, int ruleOrder){
            return layer != w.layer ? layer > w.layer : (specificity != w.specificity ? specificity > w.specificity : ruleOrder > w.order);
        };
        for (const GlyphStyleRule& rule : rules){
            int specificity = -1;
            for (const GlyphSelector& selector : rule.selectors){
                if (selector.specificity > specificity && glyphMatches(selector, nodes, (int)n, (int)selector.compounds.size() - 1))
                    specificity = selector.specificity;
            }
            if (specificity < 0)
                continue;
            const GlyphFontDeclarations& d = rule.declarations;
            if (d.hasFamily && better(family, rule.layer, specificity, rule.order)){
                family = GlyphCascadeWinner{rule.layer, specificity, rule.order};
                style.family = d.style.family;
            }
            if (d.hasWeight && better(weight, rule.layer, specificity, rule.order)){
                weight = GlyphCascadeWinner{rule.layer, specificity, rule.order};
                style.weight = d.style.weight;
            }
            if (d.hasStyle && better(italic, rule.layer, specificity, rule.order)){
                italic = GlyphCascadeWinner{rule.layer, specificity, rule.order};
                style.italic = d.style.italic;
            }
        }
        if (!nodes[n].style.empty()){
            const GlyphFontDeclarations inline_ = glyphDeclarations(nodes[n].style);
            if (inline_.hasFamily && family.layer < 2)
                style.family = inline_.style.family;
            if (inline_.hasWeight && weight.layer < 2)
                style.weight = inline_.style.weight;
            if (inline_.hasStyle && italic.layer < 2)
                style.italic = inline_.style.italic;
        }
        computed[n] = style;

        const std::string text = glyphTrimmed(nodes[n].text);
        if (text.empty() || style.family.empty())
            continue;
        // the runtime's chain: the list's faces, then the built-in Roboto and Noto Sans Arabic; reported when the first
        // face of the list is one of the document's own and lacks characters a later face has
        std::vector<std::shared_ptr<FontFace>> chain;
        for (const std::string& name : RmlFontEngine::splitFamilyList(style.family)){
            std::shared_ptr<FontFace> face = glyphFaceOf(name, style, faces, files);
            if (face && std::find(chain.begin(), chain.end(), face) == chain.end())
                chain.push_back(face);
        }
        if (chain.empty() || chain[0] == latin || chain[0] == arabic)
            continue;
        for (const std::shared_ptr<FontFace>& face : {latin, arabic}){
            if (face && std::find(chain.begin(), chain.end(), face) == chain.end())
                chain.push_back(face);
        }
        std::vector<uint32_t> codepoints;
        TextShaper::decodeUtf8(text.data(), text.size(), codepoints);
        std::set<uint32_t> fallback;
        for (uint32_t codepoint : codepoints){
            const bool ignorable = codepoint < 0x20 || (codepoint >= 0x200B && codepoint <= 0x200F) || (codepoint >= 0x2060 && codepoint <= 0x206F) ||
                                   (codepoint >= 0xFE00 && codepoint <= 0xFE0F) || codepoint == 0xFEFF || codepoint >= 0xE0000;
            if (ignorable || chain[0]->hasCodepoint(codepoint))
                continue;
            for (size_t f = 1; f < chain.size(); f++){
                if (chain[f]->hasCodepoint(codepoint)){
                    fallback.insert(codepoint);
                    break;
                }
            }
        }
        if (fallback.empty())
            continue;

        // at the element's source (the nearest element the compile located)
        Diagnostic diagnostic;
        for (int located = (int)n; located >= 0; located = nodes[(size_t)located].parent){
            const int location = nodes[(size_t)located].location;
            if (location >= 0 && (size_t)location < compiled.sourceMap.elementRefs.size() && compiled.sourceMap.elementRefs[(size_t)location].valid()){
                sink.locate(diagnostic, compiled.sourceMap.elementRefs[(size_t)location]);
                break;
            }
        }
        if (diagnostic.file.empty())
            diagnostic.file = compiled.sourcePath;
        describeFallback(diagnostic, std::vector<uint32_t>(fallback.begin(), fallback.end()), chain[0]->getLabel(), style.family);
        // (check --ui shows warnings and errors: what a fallback face will draw is one there)
        diagnostic.severity = Severity::Warning;
        if (std::none_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d){ return sameDiagnostic(d, diagnostic); }))
            diagnostics.push_back(diagnostic);
    }
    return diagnostics;
}
