// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/widgets/WidgetsRuntime.h"

#include "htmlui/widgets/ItemsViewElement.h"
#include "htmlui/widgets/MarkdownElement.h"
#include "htmlui/compat/CompatApi.h"
#include "htmlui/compat/DiagnosticCodes.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/DocumentLoader.h"

#include "Log.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Plugin.h>

#include <memory>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    // a data-for that makes more elements than this is reported (R13)
    constexpr int kLargeDataFor = 500;

    class WidgetsPlugin final : public Rml::Plugin {
    public:
        int GetEventClasses() override { return EVT_BASIC | EVT_ELEMENT; }
        void OnElementCreate(Rml::Element*) override { created++; }
        void OnContextDestroy(Rml::Context* context) override { WidgetsRuntime::contextDestroyed(context); }

        uint64_t created = 0;
    };

    struct Runtime {
        bool registered = false;
        ItemsViewInstancer views;
        WidgetsPlugin plugin;
        uint64_t scannedAt = 0;   // elementsCreated() at the last data-for scan
        // the (element, code) pairs reported, per document
        std::unordered_map<const Rml::ElementDocument*, std::set<std::pair<const Rml::Element*, int>>> reported;
    };

    Runtime& runtime(){
        static Runtime value;
        return value;
    }

    compat::DiagCode diagCode(WidgetsRuntime::Code code){
        switch (code){
            case WidgetsRuntime::Code::VirtualViewHeight: return compat::codes::VirtualViewHeight;
            case WidgetsRuntime::Code::VirtualViewTemplate: return compat::codes::VirtualViewTemplate;
            case WidgetsRuntime::Code::LargeDataFor: return compat::codes::LargeDataFor;
            case WidgetsRuntime::Code::MarkdownRawHtml: return compat::codes::MarkdownRawHtml;
        }
        return compat::codes::VirtualViewHeight;
    }

    // HTMLUI-HTML-0031: the data-for templates whose generated siblings passed the limit
    void scanDataFor(Rml::Context* context){
        for (int i = 0; i < context->GetNumDocuments(); i++){
            Rml::ElementDocument* document = context->GetDocument(i);
            if (!document || !compat::DocumentLoader::compiledOf(document))
                continue;
            Rml::ElementList templates;
            document->QuerySelectorAll(templates, "[data-for]");
            for (Rml::Element* element : templates){
                Rml::Element* parent = element->GetParentNode();
                if (!parent)
                    continue;
                int rows = 0;
                const Rml::String& tag = element->GetTagName();
                for (int k = 0; k < parent->GetNumChildren(); k++){
                    Rml::Element* sibling = parent->GetChild(k);
                    if (sibling != element && sibling->GetTagName() == tag && !sibling->HasAttribute("data-for"))
                        rows++;
                }
                if (rows > kLargeDataFor){
                    WidgetsRuntime::reportElement(element, WidgetsRuntime::Code::LargeDataFor,
                                                  "data-for made " + std::to_string(rows) + " elements: every one is laid out and drawn",
                                                  "use <list-view items=\"...\"> (or grid-view, table-view): it makes only the rows on screen");
                }
            }
        }
    }

}

void WidgetsRuntime::registerAll(){
    Runtime& r = runtime();
    if (r.registered)
        return;
    for (const char* tag : {"list-view", "grid-view", "tree-view", "table-view"})
        Rml::Factory::RegisterElementInstancer(tag, &r.views);
    MarkdownElement::registerInstancers();
    Rml::RegisterPlugin(&r.plugin);
    r.scannedAt = r.plugin.created;
    r.registered = true;
}

void WidgetsRuntime::releaseAll(){
    Runtime& r = runtime();
    // RmlUi forgot the instancers and plugins at shutdown
    r.reported.clear();
    r.registered = false;
}

void WidgetsRuntime::update(Rml::Context* context){
    Runtime& r = runtime();
    if (!context || !r.registered)
        return;
    // a scan costs a query per document: only after enough elements were made to pass the limit
    if (r.plugin.created - r.scannedAt >= (uint64_t)kLargeDataFor){
        r.scannedAt = r.plugin.created;
        scanDataFor(context);
    }
}

void WidgetsRuntime::contextDestroyed(Rml::Context* context){
    Runtime& r = runtime();
    if (!context)
        return;
    for (int i = 0; i < context->GetNumDocuments(); i++)
        r.reported.erase(context->GetDocument(i));
}

uint64_t WidgetsRuntime::elementsCreated(){
    return runtime().plugin.created;
}

void WidgetsRuntime::reportElement(Rml::Element* element, Code code, const std::string& message, const std::string& suggestion){
    if (!element)
        return;
    Rml::ElementDocument* document = element->GetOwnerDocument();
    std::shared_ptr<const compat::CompatDocument> compiled = compat::DocumentLoader::compiledOf(document);
    std::set<std::pair<const Rml::Element*, int>>& reported = runtime().reported[document];
    if (!reported.insert({element, (int)code}).second)
        return;

    compat::Diagnostic diagnostic;
    if (compiled){
        if (std::optional<compat::Diagnostic> located = compat::locateElement(*compiled, element))
            diagnostic = *located;
    }else if (document){
        diagnostic.file = document->GetSourceURL();
    }
    diagnostic.code = diagCode(code);
    const compat::DiagCodeInfo* info = compat::findDiagnosticCode(diagnostic.code);
    diagnostic.severity = info ? info->severity : compat::Severity::Warning;
    diagnostic.message = message;
    diagnostic.suggestion = suggestion;
    diagnostic.subject = element->GetTagName();
    compat::DocumentLoader::addRuntimeDiagnostic(document, diagnostic);
    const std::string text = compat::toText(diagnostic);
    if (diagnostic.severity == compat::Severity::Error)
        Log::error("[HTML UI] %s", text.c_str());
    else if (diagnostic.severity == compat::Severity::Warning)
        Log::warn("[HTML UI] %s", text.c_str());
    else
        Log::verbose("[HTML UI] %s", text.c_str());
}
