// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtExport.h"

#include "Project.h"
#include "TitleArtBaker.h"
#include "titleart/TitleArtDoc.h"
#include "util/ProjectChecks.h"

#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    // the export's documents read at runtime (assets-relative), by project (S81's plan)
    std::map<const Project*, std::set<std::string>>& runtimeTitles() {
        static std::map<const Project*, std::set<std::string>> titles;
        return titles;
    }

}

bool TitleArtExport::isNeeded(Project* project, const std::string& assetsRelativePath) {
    // S81 answers from the shipped scenes' Title Art Renderers (Render Mode Realtime or Glyph
    // Atlas): their plan, set before the assets are copied
    auto it = runtimeTitles().find(project);
    return it != runtimeTitles().end() && it->second.count(fs::path(assetsRelativePath).generic_string()) > 0;
}

void TitleArtExport::setRuntimeTitles(Project* project, const std::vector<std::string>& projectRelativePaths) {
    std::set<std::string>& titles = runtimeTitles()[project];
    titles.clear();
    for (const std::string& path : projectRelativePaths) {
        titles.insert(project ? project->normalizeToAssetsRelative(project->getProjectPath() / fs::path(path)).generic_string() : path);
    }
}

TitleArtExport::Prepared TitleArtExport::prepare(Project* project, TitleStaleBakesPolicy policy) {
    Prepared prepared;
    if (!project || project->getProjectPath().empty()) return prepared;
    std::vector<std::string> failing;
    for (const fs::path& document : ProjectChecks::findProjectFiles(project->getAssetsPath(), {titleart::TITLE_ART_EXTENSION})) {
        prepared.documents++;
        const std::string relative = project->normalizeToProjectRelative(document).generic_string();
        titleart::TitleArtDoc doc;
        std::string code, error;
        if (!loadTitleArtForBake(project, document, doc, code, error)) {
            prepared.warnings.push_back(relative + ": not baked (" + error + ")");
            continue;
        }
        const TitleBakeStatus status = titleArtStatusOf(project, document, doc);
        const std::vector<std::string> notFresh = status.notFresh();
        if (notFresh.empty()) continue;
        std::string outputs;
        for (const TitleOutputState& o : status.outputs) {
            if (o.status == TitleOutputStatus::Fresh) continue;
            outputs += (outputs.empty() ? "" : ", ") + o.id + " " + titleOutputStatusName(o.status);
        }
        const std::string line = relative + " (" + outputs + ")";
        if (policy == TitleStaleBakesPolicy::Fail) {
            failing.push_back(line);
            continue;
        }
        if (policy == TitleStaleBakesPolicy::Warn) {
            prepared.warnings.push_back("stale Title Art bake: " + line);
            continue;
        }
        TitleBakeOptions options;
        options.outputs = notFresh;
        const TitleBakeResult result = bakeTitleArt(project, document, doc, options);
        if (result.ok) {
            prepared.rebaked.push_back(line);
        } else {
            prepared.warnings.push_back("could not rebake " + line + ": " + result.error);
        }
    }
    if (!failing.empty()) {
        prepared.ok = false;
        prepared.error = "Title Art bakes are stale or missing (title_art.stale_bakes_on_export is fail): ";
        for (size_t i = 0; i < failing.size(); i++) prepared.error += (i ? "; " : "") + failing[i];
        prepared.error += ". Bake them (export_title, or Bake in the Inspector) or change the setting.";
    }
    return prepared;
}
