// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The files of a graph kind (GraphRegistry): a change is checked and its problems reported, and
// open documents (the Graph Editor's) reload. S23's load keeps what it does not know, so a file
// another program broke would otherwise say nothing until something reads it.

#include "ReloadHandlers.h"

#include "Project.h"
#include "graph/Graph.h"
#include "graph/GraphDocument.h"
#include "util/ProjectChecks.h"

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::addGraphFileProblems(Project& project, const fs::path& path, Json& problems) {
    std::error_code ec;
    std::string text;
    if (!fs::is_regular_file(path, ec) || !ProjectChecks::readFile(path, text)) return;
    const std::string relative = reload::rel(project, path);
    graph::LoadReport report;
    const graph::Graph loaded = graph::load(text, report);
    const std::string problem = graph::fileProblem(relative, loaded, report);
    if (!problem.empty()) {
        problems.push_back({{"path", relative}, {"severity", "error"}, {"message", "Not a graph file: " + problem}});
        return;
    }
    for (const std::string& warning : graph::fileWarnings(report)) {
        problems.push_back({{"path", relative}, {"severity", "warning"}, {"message", warning}});
    }
    for (const graph::Issue& issue : graph::validate(loaded)) {
        if (issue.severity == graph::Severity::Info) continue;
        Json problem = {{"path", relative}, {"severity", graph::severityName(issue.severity)}, {"code", issue.code}, {"message", issue.message}};
        if (!issue.suggestion.empty()) problem["suggestion"] = issue.suggestion;
        problems.push_back(std::move(problem));
    }
}

void reload::reloadGraphs(Context& ctx, const std::vector<fs::path>& files) {
    if (!files.empty()) GraphDocument::filesChanged(files);
    for (const fs::path& path : files) {
        addGraphFileProblems(ctx.project, path, ctx.result.problems);
        ctx.service.recordDone("graph", path, 0.0);
    }
}
