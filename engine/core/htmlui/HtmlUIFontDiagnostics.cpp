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

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    std::string describe(const std::vector<uint32_t>& codepoints){
        // the characters and their code points: "'Ó' (U+00D3), 'Ñ' (U+00D1)"
        std::string text;
        for (size_t i = 0; i < codepoints.size(); i++){
            const uint32_t c = codepoints[i];
            std::string utf8;
            if (c < 0x80){
                utf8 += (char)c;
            }else if (c < 0x800){
                utf8 += (char)(0xC0 | (c >> 6));
                utf8 += (char)(0x80 | (c & 0x3F));
            }else if (c < 0x10000){
                utf8 += (char)(0xE0 | (c >> 12));
                utf8 += (char)(0x80 | ((c >> 6) & 0x3F));
                utf8 += (char)(0x80 | (c & 0x3F));
            }else{
                utf8 += (char)(0xF0 | (c >> 18));
                utf8 += (char)(0x80 | ((c >> 12) & 0x3F));
                utf8 += (char)(0x80 | ((c >> 6) & 0x3F));
                utf8 += (char)(0x80 | (c & 0x3F));
            }
            char code[16];
            std::snprintf(code, sizeof(code), "U+%04X", (unsigned int)c);
            text += (i ? ", '" : "'") + utf8 + "' (" + code + ")";
        }
        return text;
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
        diagnostic.code = codes::FallbackGlyph;
        diagnostic.severity = Severity::Info;
        diagnostic.message = describe(glyphs.codepoints) + (glyphs.codepoints.size() == 1 ? " is" : " are") + " not in '" + glyphs.primary +
                             "', the first face of font-family '" + glyphs.family + "': a fallback face draws " +
                             (glyphs.codepoints.size() == 1 ? "it" : "them") + " (font-effect and synthetic bold and italic apply to it)";
        diagnostic.suggestion = "use a font with these characters, or name one that has them after it in font-family";
        diagnostic.subject = glyphs.family;
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

std::vector<Diagnostic> HtmlUIFontDiagnostics::check(Rml::ElementDocument* document, const CompatDocument& compiled){
    std::vector<Diagnostic> diagnostics;
    if (!document)
        return diagnostics;
    // every text element of the laid-out document (a stand-in compile pointer: locateElement reads it, nothing owns it)
    const std::shared_ptr<const CompatDocument> located(&compiled, [](const CompatDocument*){});
    std::vector<Rml::Element*> stack{document};
    while (!stack.empty()){
        Rml::Element* element = stack.back();
        stack.pop_back();
        for (int i = element->GetNumChildren() - 1; i >= 0; i--)
            stack.push_back(element->GetChild(i));
        Rml::ElementText* text = rmlui_dynamic_cast<Rml::ElementText*>(element);
        if (!text)
            continue;
        const RmlFontEngine::FallbackGlyphs glyphs = HtmlUI::getFontEngine().fallbackGlyphsOf(element->GetFontFaceHandle(), text->GetText());
        if (glyphs.codepoints.empty())
            continue;
        Diagnostic diagnostic = makeDiagnostic(glyphs, located, element);
        // (the check level shows warnings and errors: what a fallback face will draw is worth one there)
        diagnostic.severity = Severity::Warning;
        if (std::none_of(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d){ return sameDiagnostic(d, diagnostic); }))
            diagnostics.push_back(diagnostic);
    }
    return diagnostics;
}
