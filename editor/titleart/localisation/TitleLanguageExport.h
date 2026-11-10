// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S87-title-localisation-interchange.md R24, R25)
// - Purpose: which languages of each title a build ships. With Project Settings > Localization >
//   Languages set, a title ships its source and those of its languages, and every other language's
//   set (its sidecar and PNGs) stays out of the build with W-TITLE-LANGUAGE-DROPPED naming the
//   document and the code; the build's copy of the source sidecar lists only the shipped languages
//   (the project's file is untouched). An empty list ships every language.
// - Thread-safety: main thread (reads the project's documents and sidecars).

#pragma once

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct TitleLanguageShipping {
        std::set<std::string> droppedFiles;     // assets-relative: language sidecars, PNGs and their metas
        // assets-relative source sidecar -> the text the build ships
        std::vector<std::pair<std::string, std::string>> sidecars;
        std::vector<std::string> warnings;      // W-TITLE-LANGUAGE-DROPPED ...
    };

    TitleLanguageShipping planTitleLanguageShipping(Project* project);

}
