// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Style sheets a script adds to an .html document (S93 R29, doc:addStyleSheet): kept per UI
// Document component and compiled by the compat layer with the document's own sheets
// (CompatOptions::injectedSheets), so they get its rewrites, diagnostics and source map, cascade
// after its sheets and survive reloads and live patches (both compile with the loaded options, and
// a rebuild comes back through compatDocumentLoaded).

#include "UIDocumentSystem.h"

#include "Log.h"
#include "Scene.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/DocumentLoader.h"

#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>

namespace jasperengine{

    struct HtmlUIStyleSheetState {
        struct Entry {
            uint64_t id = 0;
            htmlui::compat::InjectedStyleSheet sheet;
            bool enabled = true;
        };
        std::unordered_map<Entity, std::vector<Entry>> sheets;
        uint64_t nextId = 1;
    };

}

using namespace jasperengine;

namespace {

    HtmlUIStyleSheetState::Entry* findInjectedSheet(HtmlUIStyleSheetState* state, Entity entity, uint64_t id){
        if (!state)
            return nullptr;
        auto it = state->sheets.find(entity);
        if (it == state->sheets.end())
            return nullptr;
        for (HtmlUIStyleSheetState::Entry& entry : it->second){
            if (entry.id == id)
                return &entry;
        }
        return nullptr;
    }

    // a diagnostic of one of the injected sheets (their sources are "injected:<name>" or their files)
    bool isInjectedDiagnostic(const htmlui::compat::Diagnostic& diagnostic, const std::vector<htmlui::compat::InjectedStyleSheet>& sheets){
        for (const htmlui::compat::InjectedStyleSheet& sheet : sheets){
            if (diagnostic.file == (sheet.path.empty() ? "injected:" + sheet.name : sheet.path))
                return true;
        }
        return false;
    }

}

uint64_t UIDocumentSystem::addStyleSheet(Entity entity, const std::string& css, const std::string& name, const std::string& path){
    if (!styleSheetState)
        styleSheetState = std::make_shared<HtmlUIStyleSheetState>();
    std::vector<HtmlUIStyleSheetState::Entry>& list = styleSheetState->sheets[entity];
    HtmlUIStyleSheetState::Entry entry;
    entry.id = styleSheetState->nextId++;
    entry.sheet.path = path;
    entry.sheet.css = path.empty() ? css : std::string();
    // an unnamed text sheet is numbered in its document, so its source reads "injected:sheet1:3"
    entry.sheet.name = !name.empty() ? name : (!path.empty() ? path : "sheet" + std::to_string(list.size() + 1));
    list.push_back(entry);

    const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
    if (component && !component->filename.empty() && !HtmlUIContext::isCompatPath(component->filename))
        Log::warn("[HTML UI] addStyleSheet: '%s' is an .rml document; injected style sheets apply to .html documents",
                  component->filename.c_str());
    applyStyleSheets(entity);
    return entry.id;
}

bool UIDocumentSystem::removeStyleSheet(Entity entity, uint64_t id){
    if (!findInjectedSheet(styleSheetState.get(), entity, id))
        return false;
    std::vector<HtmlUIStyleSheetState::Entry>& list = styleSheetState->sheets[entity];
    list.erase(std::remove_if(list.begin(), list.end(), [id](const HtmlUIStyleSheetState::Entry& entry){ return entry.id == id; }), list.end());
    applyStyleSheets(entity);
    if (list.empty())
        styleSheetState->sheets.erase(entity);
    return true;
}

bool UIDocumentSystem::setStyleSheetEnabled(Entity entity, uint64_t id, bool enabled){
    HtmlUIStyleSheetState::Entry* entry = findInjectedSheet(styleSheetState.get(), entity, id);
    if (!entry)
        return false;
    if (entry->enabled != enabled){
        entry->enabled = enabled;
        applyStyleSheets(entity);
    }
    return true;
}

bool UIDocumentSystem::getStyleSheet(Entity entity, uint64_t id, bool* enabled, std::string* name) const{
    const HtmlUIStyleSheetState::Entry* entry = findInjectedSheet(styleSheetState.get(), entity, id);
    if (!entry)
        return false;
    if (enabled)
        *enabled = entry->enabled;
    if (name)
        *name = entry->sheet.name;
    return true;
}

void UIDocumentSystem::applyStyleSheets(Entity entity){
    Rml::ElementDocument* document = getDocument(entity);
    std::shared_ptr<const htmlui::compat::CompatDocument> compiled = htmlui::compat::DocumentLoader::compiledOf(document);
    if (!compiled)
        return;   // not loaded yet (compatDocumentLoaded applies them), or an .rml document

    htmlui::compat::CompatOptions options = compiled->options;
    options.injectedSheets.clear();
    if (styleSheetState){
        auto it = styleSheetState->sheets.find(entity);
        if (it != styleSheetState->sheets.end()){
            for (const HtmlUIStyleSheetState::Entry& entry : it->second){
                if (entry.enabled)
                    options.injectedSheets.push_back(entry.sheet);
            }
        }
    }
    if (options.injectedSheets == compiled->options.injectedSheets)
        return;

    std::shared_ptr<const htmlui::compat::CompatDocument> fresh = htmlui::compat::DocumentLoader::restyleWith(document, compiled, options);
    if (fresh == compiled)
        return;   // the document's own file does not compile any more: it keeps its sheets
    // the injected sheets' own problems reach the Console as the document's did at its load
    std::vector<htmlui::compat::Diagnostic> own;
    for (const htmlui::compat::Diagnostic& diagnostic : fresh->diagnostics){
        if (isInjectedDiagnostic(diagnostic, options.injectedSheets))
            own.push_back(diagnostic);
    }
    if (!own.empty())
        htmlui::compat::logDiagnostics(fresh->sourcePath, own, options.verboseDiagnostics);
}

void UIDocumentSystem::styleSheetsComponentRemoved(Entity entity){
    if (styleSheetState)
        styleSheetState->sheets.erase(entity);
}
