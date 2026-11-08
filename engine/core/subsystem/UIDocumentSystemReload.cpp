// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// UIDocumentSystem hot reload and editor support (phase 1, p1-editor, spec §3.8.6-§3.8.7):
// HtmlUIReloadState (file stamps, runtime file log, messages), reloads with state
// preservation.

#include "UIDocumentSystem.h"

#include "Scene.h"
#include "Log.h"
#include "io/FileData.h"
#include "htmlui/HtmlUI.h"
#include "htmlui/HtmlUIContext.h"
#include "htmlui/HtmlUIDependencies.h"
#include "htmlui/RmlFontEngine.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ImportManifest.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "htmlui/effects/EffectRegistry.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Property.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace jasperengine{

    struct HtmlUIReloadState {
        // what a file was at a document's last (re)load
        struct Stamp {
            bool exists = false;
            long long time = 0;
            unsigned long long size = 0;
            // forgetFileStamps: the editor knows the file changed since (a save in the same
            // timestamp tick has the same time and size), so this stamp never says "unchanged"
            bool stale = false;

            bool operator==(const Stamp& other) const{
                return exists == other.exists && time == other.time && size == other.size && !stale && !other.stale;
            }
            bool operator!=(const Stamp& other) const{
                return !(*this == other);
            }
        };

        struct Document {
            std::string filename;                 // normalized document path of the last (re)load
            std::vector<std::string> files;       // the document first, then the scanner's and the runtime log's
            std::map<std::string, Stamp> stamps;
            std::vector<HtmlUIMessage> messages;  // the last maxMessages
            uint32_t loadCount = 0;
        };

        static constexpr size_t maxMessages = 50;

        UIDocumentSystem* system;
        std::unordered_map<Entity, Document> documents;

        explicit HtmlUIReloadState(UIDocumentSystem* system);
        ~HtmlUIReloadState();

        static std::vector<HtmlUIReloadState*>& all();
        static Stamp stampOf(const std::string& path);
        static void addMessage(Document& document, HtmlUIFileLog::Level level, const std::string& text);
        // messages logged outside a load: to the documents whose files they name
        static void onUnscopedMessage(HtmlUIFileLog::Level level, const std::string& text);

        // the stamps of the document's files; `before` holds what the files the load expected were
        // before it read them: a file that changed while it was reading keeps its old stamp, so
        // the next reloadChangedFiles reloads it (a new file the load found gets its stamp now)
        void recordStamps(Document& document, const std::map<std::string, Stamp>& before = {});
        static std::map<std::string, Stamp> stampsOf(const std::vector<std::string>& files);
    };

}

using namespace jasperengine;

namespace {

    const std::vector<HtmlUIMessage> noMessages;

    // form and scroll state of an element with an id, kept across a full reload
    struct ElementState {
        float scrollLeft = 0.0f;
        float scrollTop = 0.0f;
        bool hasValue = false;
        std::string value;
        int checked = -1;          // checkbox/radio: 0/1; -1 not a toggle
        int selection = -1;        // <select>
    };

    struct DocumentSnapshot {
        std::map<std::string, ElementState> elements;
        std::string focusedId;
        int selectionStart = -1;
        int selectionEnd = -1;
    };

    // every element with an id, document order, the first of a duplicated id only
    void collectIdElements(Rml::Element* element, std::map<std::string, Rml::Element*>& out){
        if (!element)
            return;
        const std::string& id = element->GetId();
        if (!id.empty())
            out.emplace(id, element);
        const int count = element->GetNumChildren();
        for (int i = 0; i < count; i++){
            collectIdElements(element->GetChild(i), out);
        }
    }

    bool isToggle(Rml::Element* element){
        if (element->GetTagName() != "input")
            return false;
        const std::string type = element->GetAttribute<Rml::String>("type", "text");
        return type == "checkbox" || type == "radio";
    }

    DocumentSnapshot captureState(Rml::ElementDocument* document, Rml::Context* context){
        DocumentSnapshot snapshot;

        std::map<std::string, Rml::Element*> elements;
        collectIdElements(document, elements);
        for (const auto& pair : elements){
            Rml::Element* element = pair.second;
            ElementState state;
            state.scrollLeft = element->GetScrollLeft();
            state.scrollTop = element->GetScrollTop();

            if (auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(element)){
                state.selection = select->GetSelection();
            }else if (isToggle(element)){
                state.checked = element->HasAttribute("checked") ? 1 : 0;
            }else if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element)){
                state.hasValue = true;
                state.value = control->GetValue();
            }
            snapshot.elements[pair.first] = state;
        }

        Rml::Element* focus = context ? context->GetFocusElement() : nullptr;
        if (focus && focus != document && focus->GetOwnerDocument() == document && !focus->GetId().empty()){
            snapshot.focusedId = focus->GetId();
            if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(focus)){
                input->GetSelection(&snapshot.selectionStart, &snapshot.selectionEnd, nullptr);
            }else if (auto* area = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(focus)){
                area->GetSelection(&snapshot.selectionStart, &snapshot.selectionEnd, nullptr);
            }
        }

        return snapshot;
    }

    // a control whose value a data model holds (data-value, data-checked): the binding gives the
    // new element its value; restoring the old one would write it back into the model
    bool isBoundControl(Rml::Element* element){
        return element->HasAttribute("data-value") || element->HasAttribute("data-checked");
    }

    // form values first (they can change the layout), scroll once laid out, then focus
    void restoreValues(Rml::ElementDocument* document, const DocumentSnapshot& snapshot){
        for (const auto& pair : snapshot.elements){
            Rml::Element* element = document->GetElementById(pair.first);
            if (!element || isBoundControl(element))
                continue;
            const ElementState& state = pair.second;

            if (state.selection >= 0){
                if (auto* select = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(element))
                    select->SetSelection(state.selection);
            }else if (state.checked >= 0){
                if (isToggle(element)){
                    if (state.checked)
                        element->SetAttribute("checked", "");
                    else
                        element->RemoveAttribute("checked");
                }
            }else if (state.hasValue){
                auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(element);
                if (control && control->GetValue() != state.value)
                    control->SetValue(state.value);
            }
        }
    }

    void restoreScrollAndFocus(Rml::ElementDocument* document, const DocumentSnapshot& snapshot){
        for (const auto& pair : snapshot.elements){
            if (pair.second.scrollLeft == 0.0f && pair.second.scrollTop == 0.0f)
                continue;
            if (Rml::Element* element = document->GetElementById(pair.first)){
                element->SetScrollLeft(pair.second.scrollLeft);
                element->SetScrollTop(pair.second.scrollTop);
            }
        }

        if (!snapshot.focusedId.empty()){
            if (Rml::Element* focus = document->GetElementById(snapshot.focusedId)){
                focus->Focus();
                if (snapshot.selectionStart >= 0){
                    if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(focus)){
                        input->SetSelectionRange(snapshot.selectionStart, snapshot.selectionEnd);
                    }else if (auto* area = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(focus)){
                        area->SetSelectionRange(snapshot.selectionStart, snapshot.selectionEnd);
                    }
                }
            }
        }
    }

    // the files of a document as reloadChangedFiles decides by them
    enum class ChangeKind { STYLESHEET, IMAGE, FONT, FULL };

    ChangeKind changeKindOf(const std::string& path, const std::string& documentPath){
        if (path == documentPath)
            return ChangeKind::FULL;
        switch (getHtmlUIReferenceKind(path)){
            case HtmlUIReference::STYLESHEET: return ChangeKind::STYLESHEET;
            case HtmlUIReference::IMAGE: return ChangeKind::IMAGE;
            case HtmlUIReference::FONT: return ChangeKind::FONT;
            default: return ChangeKind::FULL;
        }
    }

    void appendUnique(std::vector<std::string>& list, const std::string& value){
        if (!value.empty() && std::find(list.begin(), list.end(), value) == list.end())
            list.push_back(value);
    }

}

// ---- HtmlUIReloadState ----

HtmlUIReloadState::HtmlUIReloadState(UIDocumentSystem* system): system(system){
    all().push_back(this);
    HtmlUIFileLog::setMessageObserver(&HtmlUIReloadState::onUnscopedMessage);
}

HtmlUIReloadState::~HtmlUIReloadState(){
    std::vector<HtmlUIReloadState*>& states = all();
    states.erase(std::remove(states.begin(), states.end(), this), states.end());
}

std::vector<HtmlUIReloadState*>& HtmlUIReloadState::all(){
    static std::vector<HtmlUIReloadState*> states;
    return states;
}

HtmlUIReloadState::Stamp HtmlUIReloadState::stampOf(const std::string& path){
    Stamp stamp;
    std::error_code error;
    const std::filesystem::path systemPath(FileData::getSystemPath(path));
    if (!std::filesystem::is_regular_file(systemPath, error) || error)
        return stamp;

    const auto size = std::filesystem::file_size(systemPath, error);
    if (error)
        return stamp;
    const auto time = std::filesystem::last_write_time(systemPath, error);
    if (error)
        return stamp;

    stamp.exists = true;
    stamp.size = (unsigned long long)size;
    stamp.time = (long long)time.time_since_epoch().count();
    return stamp;
}

void HtmlUIReloadState::addMessage(Document& document, HtmlUIFileLog::Level level, const std::string& text){
    if (level == HtmlUIFileLog::Level::Info)
        return;

    HtmlUIMessage message;
    message.level = (level == HtmlUIFileLog::Level::Error) ? HtmlUIMessageLevel::Error : HtmlUIMessageLevel::Warning;
    message.text = text;
    parseHtmlUIMessageLocation(text, message.file, message.line);

    document.messages.push_back(message);
    if (document.messages.size() > maxMessages)
        document.messages.erase(document.messages.begin(), document.messages.begin() + (document.messages.size() - maxMessages));
}

void HtmlUIReloadState::onUnscopedMessage(HtmlUIFileLog::Level level, const std::string& text){
    std::string file;
    int line = 0;
    parseHtmlUIMessageLocation(text, file, line);

    for (HtmlUIReloadState* state : all()){
        for (auto& pair : state->documents){
            Document& document = pair.second;
            bool named = false;
            for (const std::string& path : document.files){
                if (path == file || text.find(path) != std::string::npos){
                    named = true;
                    break;
                }
            }
            if (named)
                addMessage(document, level, text);
        }
    }
}

void HtmlUIReloadState::recordStamps(Document& document, const std::map<std::string, Stamp>& before){
    document.stamps.clear();
    for (const std::string& path : document.files){
        auto known = before.find(path);
        document.stamps[path] = (known != before.end()) ? known->second : stampOf(path);
    }
}

std::map<std::string, HtmlUIReloadState::Stamp> HtmlUIReloadState::stampsOf(const std::vector<std::string>& files){
    std::map<std::string, Stamp> stamps;
    for (const std::string& path : files)
        stamps[path] = stampOf(path);
    return stamps;
}

namespace {
    std::function<void(Entity)>& loadedHook(){
        static std::function<void(Entity)> hook;
        return hook;
    }
}

void UIDocumentSystem::setLoadedHookForTesting(std::function<void(Entity)> hook){
    loadedHook() = std::move(hook);
}

// ---- UIDocumentSystem, p1-editor ----

HtmlUIReloadState& UIDocumentSystem::getReloadState(){
    if (!reloadState)
        reloadState = std::make_shared<HtmlUIReloadState>(this);
    return *reloadState;
}

Rml::ElementDocument* UIDocumentSystem::getDocument(Entity entity) const{
    // p1-rtt keeps texture-mode documents in `documents` too, or extends this
    auto it = documents.find(entity);
    return (it != documents.end()) ? it->second : nullptr;
}

Rml::ElementDocument* UIDocumentSystem::loadEntityDocument(Entity entity, const std::string& filename){
    HtmlUIReloadState& state = getReloadState();
    HtmlUIReloadState::Document& record = state.documents[entity];
    // what the files the load is about to read are now (the last load's, the scanner's)
    std::vector<std::string> expected;
    if (record.filename == normalizeHtmlUIPath(filename))
        expected = record.files;
    appendUnique(expected, normalizeHtmlUIPath(filename));
    for (const HtmlUIReference& reference : collectHtmlUIReferences(normalizeHtmlUIPath(filename))){
        if (!reference.remote && !reference.outsideAssets)
            appendUnique(expected, reference.path);
    }
    const std::map<std::string, HtmlUIReloadState::Stamp> before = HtmlUIReloadState::stampsOf(expected);
    record.filename = normalizeHtmlUIPath(filename);
    record.messages.clear();

    std::vector<std::string> opened;
    Rml::ElementDocument* document = nullptr;
    {
        HtmlUIFileLog::Scope scope(&opened, [&record](HtmlUIFileLog::Level level, const std::string& text){
            HtmlUIReloadState::addMessage(record, level, text);
        });
        // p1-rtt: a TEXTURE-mode document loads into its own context
        UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
        if (component && component->renderMode == UIDocumentRenderMode::TEXTURE)
            document = textureLoadDocument(entity, *component);
        else
            document = getContext()->loadDocument(filename);
    }
    if (!document){
        HtmlUIReloadState::addMessage(record, HtmlUIFileLog::Level::Error, "could not load document '" + filename + "'");
    }

    // the document, what the scanner finds (missing files too: creating one reloads), what
    // RmlUi opened
    record.files.clear();
    appendUnique(record.files, record.filename);
    for (const HtmlUIReference& reference : collectHtmlUIReferences(record.filename)){
        if (!reference.remote && !reference.outsideAssets)
            appendUnique(record.files, reference.path);
    }
    for (const std::string& path : opened){
        appendUnique(record.files, path);
    }
    // p2-html (hook H2): an .html document's stylesheets, imports, Lua src files, the manifest
    for (const std::string& path : compatDependencies(document)){
        appendUnique(record.files, path);
    }
    if (loadedHook())
        loadedHook()(entity);
    state.recordStamps(record, before);
    record.loadCount++;

    return document;
}

void UIDocumentSystem::reloadContextDestroyed(){
    if (context){
        HtmlUIFileLog::forgetRenderer(&getContext()->getRenderer());
    }
}

std::vector<std::string> UIDocumentSystem::getDocumentFiles(Entity entity) const{
    std::vector<std::string> files;

    const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
    if (!component || component->filename.empty())
        return files;
    const std::string filename = normalizeHtmlUIPath(component->filename);

    const HtmlUIReloadState::Document* record = nullptr;
    if (reloadState){
        auto it = reloadState->documents.find(entity);
        if (it != reloadState->documents.end() && it->second.filename == filename)
            record = &it->second;
    }

    if (record){
        files = record->files;
    }else{
        // not loaded yet (or since its filename changed): what the scanner sees now
        appendUnique(files, filename);
        for (const HtmlUIReference& reference : collectHtmlUIReferences(filename)){
            if (!reference.remote && !reference.outsideAssets)
                appendUnique(files, reference.path);
        }
    }

    // images are loaded at draw time, by the context's renderer
    if (context && isDocumentLoaded(entity)){
        for (const std::string& path : HtmlUIFileLog::getLazyFiles(&documentContext(entity)->getRenderer())){
            appendUnique(files, path);
        }
    }

    return files;
}

namespace {
    UIReloadMode defaultReloadMode = UIReloadMode::PATCH;
}

void UIDocumentSystem::reloadDocument(Entity entity, bool preserveState){
    reloadDocument(entity, preserveState, getReloadMode());
}

void UIDocumentSystem::reloadDocument(Entity entity, bool preserveState, UIReloadMode mode){
    UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entity);
    if (!component)
        return;

    auto it = documents.find(entity);
    if (!preserveState || it == documents.end() || !context || component->filename.empty()){
        // a plain (re)load; without a view the next update loads it
        component->needReload = true;
        loadDocumentNow(entity);
        return;
    }

    // S17: an .html document is patched in place when it can be
    UIReloadReport report;
    report.mode = "rebuild";
    report.file = normalizeHtmlUIPath(component->filename);
    if (mode == UIReloadMode::PATCH && getCompiledDocument(entity)){
        if (patchDocument(entity, report))
            return;
        report.mode = "rebuild";
    }
    const auto rebuildStart = std::chrono::steady_clock::now();

    Rml::ElementDocument* oldDocument = it->second;
    HtmlUIContext* oldContext = documentContext(entity);   // p1-rtt: the screen's or its own
    Rml::Context* rmlContext = oldContext->getRmlContext();

    // 1. capture, by id
    const DocumentSnapshot snapshot = captureState(oldDocument, rmlContext);

    // 2. load the new one next to the old one, which RmlUi only destroys at the next update
    if (component->renderMode != UIDocumentRenderMode::TEXTURE) scriptDocumentLoading(component->filename);
    Rml::ElementDocument* newDocument = loadEntityDocument(entity, component->filename);
    if (!newDocument){
        // the old document goes too, as with a plain load of a broken file
        scriptDocumentUnloading(entity, oldDocument);
        oldContext->unloadDocument(oldDocument);
        documents.erase(it);
        component->loaded = false;
        component->loadFailed = true;
        component->needReload = false;
        return;
    }

    // 4. the script's listeners on id-ed elements move to their new counterparts
    std::map<std::string, Rml::Element*> oldElements;
    collectIdElements(oldDocument, oldElements);
    std::unordered_map<Rml::Element*, Rml::Element*> oldToNew;
    oldToNew[oldDocument] = newDocument;
    for (const auto& pair : oldElements){
        if (pair.second == oldDocument)
            continue;
        if (Rml::Element* counterpart = newDocument->GetElementById(pair.first))
            oldToNew[pair.second] = counterpart;
    }
    scriptDocumentReloaded(entity, oldToNew);

    // the old document's handles, detached elements and onUnload go with it
    scriptDocumentUnloading(entity, oldDocument);
    oldContext->unloadDocument(oldDocument);
    documents[entity] = newDocument;
    // p1-rtt: back to SCREEN, the texture context goes (after the old document left it)
    if (component->renderMode != UIDocumentRenderMode::TEXTURE) textureRemoveEntity(entity);
    HtmlUIContext* newContext = documentContext(entity);
    component->loaded = true;
    component->loadFailed = false;
    component->needReload = false;

    // placed and shown as applyComponent does
    newDocument->SetProperty(Rml::PropertyId::ZIndex, Rml::Property((float)component->sortOrder, Rml::Unit::NUMBER));
    component->appliedSortOrder = component->sortOrder;
    const bool visible = component->enabled && scene->isEntityActiveInHierarchy(entity);
    if (visible){
        newDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    }else{
        newDocument->Hide();
    }
    component->appliedVisible = visible;
    // the live safe-area insets (env(safe-area-inset-*)) before the first layout
    updateSafeAreaProperties();

    // 3. restore: values, then layout, then scroll and focus
    restoreValues(newDocument, snapshot);
    newContext->update();
    restoreScrollAndFocus(newDocument, snapshot);
    newContext->update();

    // 5. notify: onUnload then onLoad, fired at the end of the next update
    scriptDocumentLoaded(entity);
    compatRunScripts();   // p2-html: the document's <script type="text/lua"> run again

    report.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - rebuildStart).count();
    finishReload(entity, std::move(report));
}

void UIDocumentSystem::reloadStyleSheets(Entity entity){
    auto it = documents.find(entity);
    if (it == documents.end() || !context)
        return;

    HtmlUIReloadState& state = getReloadState();
    HtmlUIReloadState::Document& record = state.documents[entity];
    // what the files the restyle is about to read are now
    const std::map<std::string, HtmlUIReloadState::Stamp> before = HtmlUIReloadState::stampsOf(record.files);

    std::vector<std::string> opened;
    const bool compat = (bool)getCompiledDocument(entity);
    bool restyled = true;
    {
        HtmlUIFileLog::Scope scope(&opened, [&record](HtmlUIFileLog::Level level, const std::string& text){
            HtmlUIReloadState::addMessage(record, level, text);
        });
        // re-reads the document's sheets (RmlUi clears its sheet cache first); the DOM, element
        // handles, listeners, scroll and focus stay. An .html document is recompiled instead
        // (ReloadStyleSheet would read the .html as RML; p2-html hook H2)
        if (compat)
            restyled = compatRestyle(entity);
        else
            it->second->ReloadStyleSheet();
    }
    if (!restyled){
        // the change reached the document itself (its RML, scripts or templates)
        reloadDocument(entity, true);
        return;
    }
    for (const std::string& path : compatDependencies(it->second)){
        appendUnique(record.files, path);
    }

    // a sheet may @import another now
    for (const HtmlUIReference& reference : collectHtmlUIReferences(record.filename)){
        if (!reference.remote && !reference.outsideAssets)
            appendUnique(record.files, reference.path);
    }
    for (const std::string& path : opened){
        appendUnique(record.files, path);
    }
    if (loadedHook())
        loadedHook()(entity);
    state.recordStamps(record, before);
    record.loadCount++;

    documentContext(entity)->update();
}

bool UIDocumentSystem::reloadInPlace(Entity entity, const std::function<bool()>& work){
    auto it = documents.find(entity);
    if (it == documents.end())
        return false;
    HtmlUIReloadState& state = getReloadState();
    HtmlUIReloadState::Document& record = state.documents[entity];
    const std::map<std::string, HtmlUIReloadState::Stamp> before = HtmlUIReloadState::stampsOf(record.files);
    std::vector<std::string> opened;
    bool done = false;
    {
        HtmlUIFileLog::Scope scope(&opened, [&record](HtmlUIFileLog::Level level, const std::string& text){
            HtmlUIReloadState::addMessage(record, level, text);
        });
        done = work();
    }
    if (!done)
        return false;
    for (const std::string& path : compatDependencies(it->second))
        appendUnique(record.files, path);
    for (const HtmlUIReference& reference : collectHtmlUIReferences(record.filename)){
        if (!reference.remote && !reference.outsideAssets)
            appendUnique(record.files, reference.path);
    }
    for (const std::string& path : opened)
        appendUnique(record.files, path);
    if (loadedHook())
        loadedHook()(entity);
    state.recordStamps(record, before);
    record.loadCount++;
    return true;
}

void UIDocumentSystem::finishReload(Entity entity, UIReloadReport report){
    // R37: one Console line per reload
    std::string line;
    char ms[32];
    std::snprintf(ms, sizeof(ms), "%.1f", report.ms);
    if (report.mode == "patch"){
        line = "Patched " + report.file + ": " + std::to_string(report.updated) + " updated, " + std::to_string(report.inserted) + " inserted, " +
               std::to_string(report.removed) + " removed, " + std::to_string(report.replaced) + " replaced, " +
               std::to_string(report.runtimeKept) + " runtime nodes kept (" + ms + " ms)";
    }else{
        line = "Rebuilt " + report.file + " (" + ms + " ms)";
        if (!report.fallbackReason.empty())
            line += ": " + report.fallbackReason;
    }
    // the full report on the lines after the first: the Console lists the first line and shows
    // the rest in its details pane when the line is clicked
    std::string full = line;
    full += "\n  mode: " + report.mode;
    if (report.mode == "patch"){
        full += "\n  kept: " + std::to_string(report.kept) + ", updated: " + std::to_string(report.updated) +
                ", inserted: " + std::to_string(report.inserted) + ", removed: " + std::to_string(report.removed) +
                ", replaced: " + std::to_string(report.replaced);
        full += "\n  runtime nodes kept: " + std::to_string(report.runtimeKept) +
                ", document scripts run again: " + std::to_string(report.scriptsRerun);
    }
    if (!report.fallbackReason.empty())
        full += "\n  why not a patch: " + report.fallbackReason;
    for (const std::string& cause : report.causes)
        full += "\n  replaced " + cause;
    Log::print("[HTML UI] %s", full.c_str());
    if (reloadState){
        auto it = reloadState->documents.find(entity);
        if (it != reloadState->documents.end())
            HtmlUIReloadState::addMessage(it->second, HtmlUIFileLog::Level::Info, line);
    }
    if (report.mode == "patch")
        getScriptState().reloads.push_back({entity, report});
    reloadReports[entity] = std::move(report);
}

void UIDocumentSystem::setReloadMode(UIReloadMode mode){
    reloadMode = mode;
}

UIReloadMode UIDocumentSystem::getReloadMode() const{
    return reloadMode.value_or(defaultReloadMode);
}

void UIDocumentSystem::setDefaultReloadMode(UIReloadMode mode){
    defaultReloadMode = mode;
}

UIReloadMode UIDocumentSystem::getDefaultReloadMode(){
    return defaultReloadMode;
}

const UIReloadReport& UIDocumentSystem::getLastReloadReport(Entity entity) const{
    static const UIReloadReport none;
    auto it = reloadReports.find(entity);
    return it != reloadReports.end() ? it->second : none;
}

void UIDocumentSystem::reloadTexture(const std::string& path){
    // RmlUi loads it again the next time something draws with it. Global: the texture
    // database of every context (the other scenes too) drops the image
    if (!HtmlUI::isInitialized())
        return;
    // RmlUi knows a texture by the source it was asked for, as written: the path of an .rml
    // document's image, an asset:// URL from the compat layer, either with a query (the
    // sampling filter of image-rendering, an SVG scale). Every spelling of the file goes.
    const std::string file = normalizeHtmlUIPath(path);
    for (const Rml::String& source : Rml::GetTextureSourceList()){
        if (normalizeHtmlUIPath(source.substr(0, source.find('?'))) == file)
            Rml::ReleaseTexture(source);
    }
}

const std::vector<HtmlUIMessage>& UIDocumentSystem::getDocumentMessages(Entity entity) const{
    if (!reloadState)
        return noMessages;
    auto it = reloadState->documents.find(entity);
    return (it != reloadState->documents.end()) ? it->second.messages : noMessages;
}

uint32_t UIDocumentSystem::getLoadCount(Entity entity) const{
    if (!reloadState)
        return 0;
    auto it = reloadState->documents.find(entity);
    return (it != reloadState->documents.end()) ? it->second.loadCount : 0;
}

void UIDocumentSystem::forgetFileStamps(const std::vector<std::string>& files){
    if (!reloadState)
        return;
    std::set<std::string> forgotten;
    for (const std::string& file : files)
        forgotten.insert(normalizeHtmlUIPath(file));
    for (auto& [entity, record] : reloadState->documents){
        for (auto& [path, stamp] : record.stamps){
            if (forgotten.count(path))
                stamp.stale = true;
        }
    }
}

std::vector<UIDocumentSystem::Reloaded> UIDocumentSystem::reloadChangedFiles(const std::vector<std::string>& files, double settleSeconds){
    std::vector<Reloaded> reloaded;
    if (files.empty() || !reloadState || !context)
        return reloaded;

    std::set<std::string> asked;
    for (const std::string& file : files){
        asked.insert(normalizeHtmlUIPath(file));
    }
    // phase 5 (p5-tooling): a declared project effect whose .frag is gone is forgotten first, so
    // the documents compiled next declare theirs again
    const uint64_t effectsBefore = htmlui::EffectRegistry::get().projectGeneration();
    htmlui::EffectRegistry::get().forgetMissingProjectEffects();
    // p2-html (hook H2): compiles that read a changed file are stale (the manifest reloads too)
    for (const std::string& path : asked){
        htmlui::compat::DocumentLoader::invalidate(path);
        htmlui::compat::ImportManifest::notifyFileChanged(path);
    }

    // what to do per document, decided before anything reloads (a reload rewrites the stamps)
    struct Action {
        Entity entity;
        bool full = false;
        bool rebuild = false;      // S17: a file the live patch does not read again changed (a font, a Lua file, an SVG ...)
        bool styleSheets = false;
        std::vector<std::string> images;
        std::vector<std::string> fonts;
    };
    std::vector<Action> actions;
    // the changed files, for the project effects (phase 5: an @effect .frag or one of its includes)
    std::set<std::string> changedEffects;

    auto components = scene->getComponentArray<UIDocumentComponent>();
    for (size_t i = 0; i < components->size(); i++){
        const Entity entity = components->getEntity(i);
        const UIDocumentComponent& component = components->getComponentFromIndex(i);
        if (component.filename.empty() || component.needReload)
            continue;

        auto stateIt = reloadState->documents.find(entity);
        if (stateIt == reloadState->documents.end())
            continue;
        HtmlUIReloadState::Document& record = stateIt->second;
        if (record.filename != normalizeHtmlUIPath(component.filename))
            continue;

        Action action;
        action.entity = entity;
        bool changed = false;
        // the document's changed files, decided before any stamp moves: a file still being
        // written (settleSeconds) leaves the whole document for a later call
        std::vector<std::pair<std::string, HtmlUIReloadState::Stamp>> changes;
        bool settling = false;
        for (const std::string& path : getDocumentFiles(entity)){
            if (asked.find(path) == asked.end())
                continue;

            const HtmlUIReloadState::Stamp now = HtmlUIReloadState::stampOf(path);
            auto stampIt = record.stamps.find(path);
            if (stampIt == record.stamps.end()){
                // first seen (an image the renderer loaded after the document): from now on
                record.stamps[path] = now;
                continue;
            }
            if (stampIt->second == now)
                continue;
            if (settleSeconds > 0.0 && now.exists){
                const long long fileNow = (long long)std::filesystem::file_time_type::clock::now().time_since_epoch().count();
                const long long window = (long long)std::chrono::duration_cast<std::filesystem::file_time_type::duration>(
                                                        std::chrono::duration<double>(settleSeconds)).count();
                if (fileNow - now.time < window)
                    settling = true;
            }
            changes.emplace_back(path, now);
        }
        if (settling)
            continue;
        for (const auto& [path, now] : changes){
            record.stamps[path] = now;
            changed = true;
            changedEffects.insert(path);

            ChangeKind kind = changeKindOf(path, record.filename);
            // p2-html: an import may only change CSS; compatRestyle falls back to a full reload
            if (path == htmlui::compat::ImportManifest::kPath && getCompiledDocument(entity))
                kind = ChangeKind::STYLESHEET;
            switch (kind){
                case ChangeKind::STYLESHEET: action.styleSheets = true; break;
                case ChangeKind::IMAGE: action.images.push_back(path); break;
                case ChangeKind::FONT: action.fonts.push_back(path); action.full = true; action.rebuild = true; break;
                case ChangeKind::FULL:{
                    action.full = true;
                    // the document's own file and a <markdown src> (compiled into its RML) can be patched
                    const std::string extension = std::filesystem::path(path).extension().string();
                    if (path != record.filename && extension != ".md" && extension != ".markdown")
                        action.rebuild = true;
                    break;
                }
            }
        }
        if (changed)
            actions.push_back(action);
    }

    // phase 5 (p5-tooling): a project effect's manifest is read again and its variants dropped
    // before the documents that declared it reload (a .frag or include a document reads changes it
    // in full); the registry ignores the other files
    for (const std::string& path : changedEffects)
        htmlui::EffectRegistry::get().onProjectFileChanged(path);

    std::set<std::string> fontsReloaded;
    std::set<std::string> imagesReleased;
    for (const Action& action : actions){
        for (const std::string& font : action.fonts){
            if (fontsReloaded.insert(font).second)
                HtmlUI::getFontEngine().reloadFontFile(font);
        }
        for (const std::string& image : action.images){
            if (imagesReleased.insert(image).second){
                reloadTexture(image);
                // a bitmap face's sheet or page (S51): the face loads again
                if (RmlFontEngine::isBitmapDependency(image))
                    HtmlUI::getFontEngine().reloadFontFile(image);
            }
        }

        if (action.full){
            Log::verbose("[HTML UI] reloading document of entity %u (its files changed)", (unsigned int)action.entity);
            reloadDocument(action.entity, true, action.rebuild ? UIReloadMode::REBUILD : getReloadMode());
        }else if (action.styleSheets){
            Rml::ElementDocument* before = getDocument(action.entity);
            reloadStyleSheets(action.entity);
            // p2-html: an .html document whose RML changed was reloaded in full
            if (getDocument(action.entity) != before){
                reloaded.push_back({action.entity, false});
                continue;
            }
            // phase 5 (p5-tooling): a project effect changed (declared from another file, read
            // again, forgotten): the elements whose effect value reads the same keep the
            // decorators of the old one, so the document loads again in full
            if (htmlui::EffectRegistry::get().projectGeneration() != effectsBefore){
                Log::verbose("[HTML UI] reloading document of entity %u (a project effect changed)", (unsigned int)action.entity);
                reloadDocument(action.entity, true);
                reloaded.push_back({action.entity, false});
                continue;
            }
        }
        reloaded.push_back({action.entity, !action.full});
    }

    return reloaded;
}

double UIDocumentSystem::getNextUpdateDelay() const{
    double delay = std::numeric_limits<double>::infinity();
    if (!context || documents.empty())
        return delay;
    // the screen context and every texture-mode document's own context (p1-rtt)
    std::vector<HtmlUIContext*> contexts = getTextureContexts();
    contexts.push_back(getContext());
    for (HtmlUIContext* each : contexts){
        if (each && each->getRmlContext())
            delay = std::min(delay, (double)each->getRmlContext()->GetNextUpdateDelay());
    }
    return delay;
}
