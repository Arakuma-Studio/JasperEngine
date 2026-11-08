// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleDesignerHooks.h"

#include <algorithm>
#include <map>

using namespace jasperengine::editor::titledesigner;

namespace {

    struct Hooks {
        std::vector<PanelDef> panels;
        std::vector<ToolbarItemDef> toolbar;
        std::vector<PreviewModeDef> previewModes;
        std::vector<std::pair<std::string, CheckSource>> checks;
        EditRouter router;
        TimeSource time;
        Gizmo3DProvider gizmo3d;
        std::shared_ptr<TitleLibraryProvider> library;
    };

    Hooks& hooks() {
        static Hooks h;
        return h;
    }

    template<class T>
    void eraseId(std::vector<T>& list, const std::string& id) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const T& item) { return item.id == id; }), list.end());
    }

}

void jasperengine::editor::titledesigner::addPanel(PanelDef panel) {
    eraseId(hooks().panels, panel.id);
    hooks().panels.push_back(std::move(panel));
}

void jasperengine::editor::titledesigner::removePanel(const std::string& id) {
    eraseId(hooks().panels, id);
}

const std::vector<PanelDef>& jasperengine::editor::titledesigner::panels() {
    return hooks().panels;
}

void jasperengine::editor::titledesigner::addToolbarItem(ToolbarItemDef item) {
    eraseId(hooks().toolbar, item.id);
    hooks().toolbar.push_back(std::move(item));
}

void jasperengine::editor::titledesigner::removeToolbarItem(const std::string& id) {
    eraseId(hooks().toolbar, id);
}

const std::vector<ToolbarItemDef>& jasperengine::editor::titledesigner::toolbarItems() {
    return hooks().toolbar;
}

void jasperengine::editor::titledesigner::setEditRouter(EditRouter router) {
    hooks().router = std::move(router);
}

const EditRouter& jasperengine::editor::titledesigner::editRouter() {
    return hooks().router;
}

void jasperengine::editor::titledesigner::setTimeSource(TimeSource source) {
    hooks().time = std::move(source);
}

const TimeSource& jasperengine::editor::titledesigner::timeSource() {
    return hooks().time;
}

void jasperengine::editor::titledesigner::setGizmo3DProvider(Gizmo3DProvider provider) {
    hooks().gizmo3d = std::move(provider);
}

const Gizmo3DProvider& jasperengine::editor::titledesigner::gizmo3DProvider() {
    return hooks().gizmo3d;
}

namespace {
    std::vector<std::function<void()>>& engineFrameRequests() {
        static std::vector<std::function<void()>> list;
        return list;
    }
}

void jasperengine::editor::titledesigner::requestEngineFrame(std::function<void()> work) {
    if (work) engineFrameRequests().push_back(std::move(work));
}

void jasperengine::editor::titledesigner::runEngineFrameRequests() {
    std::vector<std::function<void()>> work;
    work.swap(engineFrameRequests());
    for (const std::function<void()>& w : work) w();
}

void jasperengine::editor::titledesigner::addPreviewMode(PreviewModeDef mode) {
    eraseId(hooks().previewModes, mode.id);
    hooks().previewModes.push_back(std::move(mode));
}

void jasperengine::editor::titledesigner::removePreviewMode(const std::string& id) {
    eraseId(hooks().previewModes, id);
}

const std::vector<PreviewModeDef>& jasperengine::editor::titledesigner::previewModes() {
    return hooks().previewModes;
}

void jasperengine::editor::titledesigner::addCheckSource(const std::string& id, CheckSource source) {
    removeCheckSource(id);
    hooks().checks.emplace_back(id, std::move(source));
}

void jasperengine::editor::titledesigner::removeCheckSource(const std::string& id) {
    auto& list = hooks().checks;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& entry) { return entry.first == id; }), list.end());
}

std::vector<CheckFinding> jasperengine::editor::titledesigner::runCheckSources(const TitleDesignerState& state) {
    std::vector<CheckFinding> out;
    for (const auto& entry : hooks().checks) {
        if (!entry.second) continue;
        std::vector<CheckFinding> found = entry.second(state);
        out.insert(out.end(), found.begin(), found.end());
    }
    return out;
}

const char* jasperengine::editor::titledesigner::libraryTabName(LibraryTab tab) {
    switch (tab) {
        case LibraryTab::Looks: return "looks";
        case LibraryTab::ColorSchemes: return "color_schemes";
        case LibraryTab::Templates: return "templates";
    }
    return "";
}

bool jasperengine::editor::titledesigner::libraryTabFromName(const std::string& name, LibraryTab& out) {
    for (LibraryTab tab : {LibraryTab::Looks, LibraryTab::ColorSchemes, LibraryTab::Templates}) {
        if (name == libraryTabName(tab)) {
            out = tab;
            return true;
        }
    }
    return false;
}

void jasperengine::editor::titledesigner::setLibraryProvider(std::shared_ptr<TitleLibraryProvider> provider) {
    if (hooks().library) hooks().library->cancelAll();
    hooks().library = std::move(provider);
}

TitleLibraryProvider* jasperengine::editor::titledesigner::libraryProvider() {
    return hooks().library.get();
}
