// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem and the HTML/CSS compatibility layer (phase 2, p2-html, spec §3.12, hooks
// H1b, H2, H4): compiled documents and their diagnostics, color scheme and reduced motion,
// <a href> navigation, autofocus, <script type="text/lua"> and stylesheet-only restyles.

#include "UIDocumentSystem.h"

#include "Scene.h"
#include "Log.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "object/ui/UIDocument.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/UrlResolver.h"

#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cstdlib>

namespace jasperengine{

    struct HtmlUICompatState {
        bool scriptsEnabled = false;          // the scene's Lua scripts run (play)
        std::vector<Entity> pendingScripts;   // documents whose scripts run at the next flush
        bool runningScripts = false;
        // what setColorScheme/setReducedMotion asked for, applied to contexts made later
        std::string colorScheme;
        bool reducedMotionSet = false;
        bool reducedMotion = false;
        // diagnostics of documents that did not load (a missing file has no loaded document)
        std::unordered_map<Entity, std::vector<htmlui::compat::Diagnostic>> failed;
    };

}

using namespace jasperengine;
using namespace jasperengine::htmlui::compat;

namespace {

    std::string documentPathOf(const UIDocumentComponent* component){
        if (!component)
            return std::string();
        std::string path = component->filename;
        for (char& c : path){
            if (c == '\\')
                c = '/';
        }
        if (path.compare(0, 8, "asset://") == 0){
            path.erase(0, 8);
            while (!path.empty() && path[0] == '/')
                path.erase(0, 1);
        }
        return path;   // an absolute path (tests, tools) stays absolute
    }

    // "ui/menu.html:45: attempt to index a nil value" -> file, line, message
    Diagnostic scriptError(const DocumentScript& script, const std::string& error){
        Diagnostic diagnostic;
        diagnostic.code = codes::LuaScriptError;
        diagnostic.severity = Severity::Error;
        diagnostic.file = script.sourceFile;
        diagnostic.line = script.firstLine;
        std::string first = error.substr(0, error.find('\n'));
        const std::string prefix = script.sourceFile + ":";
        if (!script.sourceFile.empty() && first.compare(0, prefix.size(), prefix) == 0){
            const size_t colon = first.find(':', prefix.size());
            if (colon != std::string::npos){
                const int line = std::atoi(first.substr(prefix.size(), colon - prefix.size()).c_str());
                if (line > 0)
                    diagnostic.line = (uint32_t)line;
                first = first.substr(colon + 1);
                while (!first.empty() && first[0] == ' ')
                    first.erase(0, 1);
            }
        }
        diagnostic.message = "Lua script error: " + first;
        diagnostic.suggestion = "fix the script (the line is the HTML file's)";
        diagnostic.subject = script.chunkName;
        return diagnostic;
    }

}

HtmlUICompatState& UIDocumentSystem::getCompatState(){
    if (!compatState)
        compatState = std::make_shared<HtmlUICompatState>();
    return *compatState;
}

std::shared_ptr<const CompatDocument> UIDocumentSystem::getCompiledDocument(Entity entity) const{
    return DocumentLoader::compiledOf(getDocument(entity));
}

const std::vector<Diagnostic>& UIDocumentSystem::getDiagnostics(Entity entity) const{
    static const std::vector<Diagnostic> none;
    if (Rml::ElementDocument* document = getDocument(entity)){
        if (DocumentLoader::compiledOf(document))
            return DocumentLoader::diagnosticsOf(document);
        return none;
    }
    // not loaded: why (a missing .html, ...)
    const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
    const std::string path = documentPathOf(component);
    if (!component || path.empty() || !HtmlUIContext::isCompatPath(path) || !compatState)
        return none;
    auto it = compatState->failed.find(entity);
    return (it != compatState->failed.end()) ? it->second : none;
}

void UIDocumentSystem::setColorScheme(Entity entity, const std::string& scheme){
    HtmlUICompatState& state = getCompatState();
    state.colorScheme = (scheme == "dark") ? "dark" : "light";
    HtmlUIContext* target = getDocument(entity) ? documentContext(entity) : getContext();
    if (target)
        target->setColorScheme(state.colorScheme);
}

void UIDocumentSystem::setReducedMotion(Entity entity, bool reduced){
    HtmlUICompatState& state = getCompatState();
    state.reducedMotionSet = true;
    state.reducedMotion = reduced;
    HtmlUIContext* target = getDocument(entity) ? documentContext(entity) : getContext();
    if (target)
        target->setReducedMotion(reduced);
}

void UIDocumentSystem::compatDocumentLoaded(Entity entity){
    Rml::ElementDocument* document = getDocument(entity);
    HtmlUIContext* target = document ? documentContext(entity) : nullptr;
    if (!document || !target){
        // a failed load: keep why for getDiagnostics
        const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
        const std::string path = documentPathOf(component);
        if (!path.empty() && HtmlUIContext::isCompatPath(path)){
            CompatOptions options;
            getCompatState().failed[entity] = DocumentLoader::compile(path, options)->diagnostics;
        }
        return;
    }
    if (compatState)
        compatState->failed.erase(entity);

    if (!DocumentLoader::compiledOf(document))
        return;   // an .rml document

    HtmlUICompatState& state = getCompatState();
    if (!state.colorScheme.empty() && target->getColorScheme() != state.colorScheme)
        target->setColorScheme(state.colorScheme);
    if (state.reducedMotionSet && target->getReducedMotion() != state.reducedMotion)
        target->setReducedMotion(state.reducedMotion);

    // <a href="other.html">: the htmlnavigate default action loads it in the same component
    target->setNavigateHandler([this](Rml::ElementDocument* from, const std::string& href, const std::string& url){
        for (const auto& pair : documents){
            if (pair.second != from)
                continue;
            UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(pair.first);
            if (!component)
                return;
            std::string path = href;
            const size_t cut = path.find_first_of("?#");
            if (cut != std::string::npos)
                path = path.substr(0, cut);
            const UrlKind kind = UrlResolver::classify(path);
            bool outside = false;
            std::string target;
            if (!url.empty())
                target = url;   // resolved by the anchor (HtmlAnchorElement)
            else if (kind == UrlKind::Relative)
                target = UrlResolver::joinPath(documentPathOf(component), UrlResolver::percentDecode(path), &outside);
            else if (kind == UrlKind::RootRelative)
                target = UrlResolver::normalizePath(path.compare(0, 8, "asset://") == 0 ? path.substr(8) : path, &outside);
            if (target.empty() || outside || !(HtmlUIContext::isCompatPath(target) ||
                (target.size() > 4 && target.compare(target.size() - 4, 4, ".rml") == 0)))
                return;   // fragments scroll (the anchor), other links only dispatch the event
            Log::verbose("[HTML UI] %s: navigating to %s", documentPathOf(component).c_str(), target.c_str());
            component->filename = target;
            component->needReload = true;
            return;
        }
    });

    if (state.scriptsEnabled){
        if (std::find(state.pendingScripts.begin(), state.pendingScripts.end(), entity) == state.pendingScripts.end())
            state.pendingScripts.push_back(entity);
    }
}

void UIDocumentSystem::compatDocumentShown(Rml::ElementDocument* document){
    if (DocumentLoader::compiledOf(document))
        DocumentLoader::applyAutofocus(document);
}

void UIDocumentSystem::startDocumentScripts(){
    HtmlUICompatState& state = getCompatState();
    state.scriptsEnabled = true;
    for (const auto& pair : documents){
        if (DocumentLoader::compiledOf(pair.second) &&
            std::find(state.pendingScripts.begin(), state.pendingScripts.end(), pair.first) == state.pendingScripts.end())
            state.pendingScripts.push_back(pair.first);
    }
    compatRunScripts();
}

void UIDocumentSystem::compatScriptsStopped(){
    if (!compatState)
        return;
    compatState->scriptsEnabled = false;
    compatState->pendingScripts.clear();
}

void UIDocumentSystem::compatRunScripts(){
    if (!compatState || compatState->pendingScripts.empty() || compatState->runningScripts)
        return;
    // a script can load documents (UIDocument:load), which queues theirs: they run in this loop
    compatState->runningScripts = true;
    while (compatState && !compatState->pendingScripts.empty()){
        const Entity entity = compatState->pendingScripts.front();
        compatState->pendingScripts.erase(compatState->pendingScripts.begin());
        Rml::ElementDocument* document = getDocument(entity);
        std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compiledOf(document);
        if (!compiled || compiled->scripts.empty())
            continue;
        for (const DocumentScript& script : compiled->scripts){
            // the document may have been reloaded or unloaded by the previous script
            if (getDocument(entity) != document)
                break;
            UIDocument handle(scene, entity);
            std::string error;
            if (!HtmlUILua::runDocumentScript(handle, script, &error)){
                const Diagnostic diagnostic = scriptError(script, error);
                Log::error("[HTML UI] %s", toText(diagnostic).c_str());
                DocumentLoader::addRuntimeDiagnostic(document, diagnostic);
            }
        }
    }
    if (compatState)
        compatState->runningScripts = false;
}

bool UIDocumentSystem::compatRestyle(Entity entity){
    Rml::ElementDocument* document = getDocument(entity);
    std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compiledOf(document);
    if (!compiled)
        return false;
    std::shared_ptr<const CompatDocument> fresh = DocumentLoader::compile(compiled->sourcePath, compiled->options);
    if (!fresh->compiled || !DocumentLoader::sameRml(*fresh, *compiled))
        return false;
    // scripts and templates are part of the document: a change reloads it (scripts run again)
    if (fresh->scripts.size() != compiled->scripts.size() || fresh->templates.size() != compiled->templates.size())
        return false;
    for (size_t i = 0; i < fresh->scripts.size(); i++){
        if (fresh->scripts[i].code != compiled->scripts[i].code)
            return false;
    }
    for (size_t i = 0; i < fresh->templates.size(); i++){
        if (fresh->templates[i].rml != compiled->templates[i].rml)
            return false;
    }
    if (fresh != compiled){
        logDiagnostics(fresh->sourcePath, fresh->diagnostics, fresh->options.verboseDiagnostics);
        DocumentLoader::restyle(document, fresh, fresh->options);
    }
    return true;
}

std::vector<std::string> UIDocumentSystem::compatDependencies(Rml::ElementDocument* document) const{
    std::shared_ptr<const CompatDocument> compiled = DocumentLoader::compiledOf(document);
    return compiled ? compiled->dependencies : std::vector<std::string>();
}
