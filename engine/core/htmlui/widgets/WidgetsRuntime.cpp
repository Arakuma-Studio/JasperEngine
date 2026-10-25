// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/widgets/WidgetsRuntime.h"

#include "htmlui/widgets/ItemsViewElement.h"
#include "htmlui/widgets/MarkdownElement.h"
#include "htmlui/widgets/DragDropSession.h"
#include "htmlui/widgets/TooltipManager.h"
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

#include <algorithm>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::htmlui;

namespace {

    // a data-for that makes more elements than this is reported (R13)
    constexpr int kLargeDataFor = 500;

    class WidgetsPlugin final : public Rml::Plugin {
    public:
        int GetEventClasses() override { return EVT_BASIC | EVT_DOCUMENT | EVT_ELEMENT; }
        void OnElementCreate(Rml::Element* element) override{
            created++;
            if (parsing > 0)
                authored.insert(element);
        }
        void OnElementDestroy(Rml::Element* element) override{
            TooltipManager::elementDestroyed(element);
            if (!authored.empty())
                authored.erase(element);
        }
        // a document's parse: the elements it makes are authored
        void OnDocumentOpen(Rml::Context*, const Rml::String&) override { parsing++; }
        void OnDocumentLoad(Rml::ElementDocument*) override { parsing = std::max(0, parsing - 1); }
        void OnDocumentUnload(Rml::ElementDocument* document) override { TooltipManager::documentUnloaded(document); }
        void OnContextDestroy(Rml::Context* context) override { WidgetsRuntime::contextDestroyed(context); }

        uint64_t created = 0;
        int parsing = 0;
        std::unordered_set<const Rml::Element*> authored;
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
    // RmlUi forgot the instancers and plugins at shutdown (every context, with its sessions, went before)
    TooltipManager::releaseAll();
    DragDropSession::releaseAll();
    r.plugin.authored.clear();
    r.plugin.parsing = 0;
    r.reported.clear();
    r.registered = false;
}

void WidgetsRuntime::update(Rml::Context* context){
    Runtime& r = runtime();
    if (!context || !r.registered)
        return;
    // no parse spans two frames (a document that failed to parse never reported its load)
    r.plugin.parsing = 0;
    DragDropSession::update(context);
    TooltipManager::update(context);
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
    TooltipManager::contextDestroyed(context);
    DragDropSession::contextDestroyed(context);
    for (int i = 0; i < context->GetNumDocuments(); i++)
        r.reported.erase(context->GetDocument(i));
}

uint64_t WidgetsRuntime::elementsCreated(){
    return runtime().plugin.created;
}

bool WidgetsRuntime::isAuthored(const Rml::Element* element){
    return element && runtime().plugin.authored.count(element) > 0;
}

void WidgetsRuntime::markAuthored(Rml::Element* element){
    if (!element)
        return;
    runtime().plugin.authored.insert(element);
    for (int i = 0; i < element->GetNumChildren(); i++)
        markAuthored(element->GetChild(i));
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
