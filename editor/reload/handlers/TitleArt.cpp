// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// .titleart documents (S80 R6): a changed document reloads its open TitleArtDocument (the Title
// Designer, the Inspector and MCP see it), its previews and thumbnails are keyed by its content
// so they redraw, and with Auto Bake its stale outputs rebake. A changed font, image or SVG does
// the same for every document that names it (the dependency index). Load errors are problems
// {path, line, message}; normalize warnings are warnings.

#include "ReloadHandlers.h"

#include "Project.h"
#include "asset/AssetFileWrite.h"
#include "asset/AssetIndex.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/TitleArtDocument.h"
#include "titleart/TitleArtIO.h"
#include "util/Util.h"

#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

void reload::reloadTitleArt(Context& ctx, const std::vector<fs::path>& documents, const std::vector<fs::path>& assets) {
    std::vector<fs::path> changed;
    for (const fs::path& path : documents) {
        std::string text;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || !assetio::readFile(path, text)) {
            ctx.service.recordDone("titleart", path, 0.0);
            continue;
        }
        const titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        const std::string relative = rel(ctx.project, path);
        if (!parsed.ok) {
            Json problem = {{"path", relative}, {"message", "Not a valid Title Art: " + parsed.error.message + " (the last good version stays loaded)"}};
            if (parsed.error.line > 0) problem["line"] = parsed.error.line;
            ctx.result.problems.push_back(problem);
        } else {
            for (const titleart::TitleWarning& warning : parsed.warnings) {
                ctx.result.problems.push_back(Json{{"path", relative}, {"severity", "warning"}, {"message", warning.toString()}});
            }
        }
        changed.push_back(path);
        ctx.service.recordDone("titleart", path, 0.0, parsed.ok, parsed.ok ? std::string() : parsed.error.message);
    }
    if (!changed.empty()) {
        TitleArtDocument::filesChanged(changed, TitleArtChangeOrigin::Disk);
        TitleArtDocument::notifyChanged(changed);
    }

    // the documents reading a changed asset re-render and rebake
    std::set<std::string> readers;
    for (const fs::path& asset : assets) {
        const std::string relative = rel(ctx.project, asset);
        const AssetDatabase::Resolution resolution = ctx.project.getAssetDatabase().lookupPath(relative);
        for (const DepEdge& edge : ctx.project.getAssetIndex().referencesTo(resolution.found() ? resolution.guid : Guid(), relative, false, 4096)) {
            if (edge.kind != DepEdgeKind::TitleArtAsset) continue;
            const std::string referrer = ctx.project.getAssetIndex().referrerPath(edge.referrer);
            if (Util::isTitleArtFile(referrer)) readers.insert(referrer);
        }
    }
    std::vector<fs::path> readerPaths;
    for (const std::string& referrer : readers) readerPaths.push_back((ctx.project.getProjectPath() / fs::path(referrer)).lexically_normal());
    TitleArtDocument::notifyChanged(readerPaths);
}
