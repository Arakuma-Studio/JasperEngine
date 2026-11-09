// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLibraryRegistration.h"

#include "LibraryChecks.h"
#include "TitleLibrary.h"
#include "TitleLibraryApply.h"
#include "TitleLibraryImports.h"
#include "TitleLibraryProvider.h"
#include "Project.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/designer/TitleDesignerHooks.h"
#include "titleart/fonts/FontCatalog.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;

void titlelibrary::registerTitleLibrary() {
    static bool done = false;
    if (done) return;
    done = true;
    registerTitleLibraryOps();
    registerLibraryChecks();
    TitleArtDocument::setProjectHook([](Project* project) {
        setCurrentProject(project);
        if (project) TitleLibrary::get().setProject(project->getProjectPath(), project->getAssetsPath());
    });
    // the Title Designer's font picker lists the pack (S82's Library section of the picker)
    titlefonts::setLibraryFontProvider([] {
        std::vector<titlefonts::LibraryFont> out;
        for (const PackFont& f : packFonts()) {
            for (const std::string& file : f.files) {
                titlefonts::LibraryFont font;
                font.id = f.id;
                font.family = f.family;
                font.style = f.files.size() > 1 ? std::filesystem::path(file).stem().string() : f.style;
                font.path = (TitleLibrary::packDir() / "fonts" / f.id / file).string();
                font.category = f.category == "serif" ? titlefonts::FontCategory::Serif
                                : f.category == "sans" ? titlefonts::FontCategory::Sans
                                : f.category == "handwriting" ? titlefonts::FontCategory::Handwriting
                                : f.category == "mono" ? titlefonts::FontCategory::Monospace
                                                       : titlefonts::FontCategory::Display;
                out.push_back(std::move(font));
            }
        }
        return out;
    });
    titledesigner::setLibraryProvider(makeTitleLibraryProvider());
}
