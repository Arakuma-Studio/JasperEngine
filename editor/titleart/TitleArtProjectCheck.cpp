// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtProjectCheck.h"

#include "TitleArtBake.h"
#include "TitleArtBuiltinChecks.h"
#include "TitleArtRenderHost.h"
#include "asset/AssetFileWrite.h"
#include "titleart/TitleArtIO.h"
#include "util/AssetDatabase.h"
#include "util/Util.h"

#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace fs = std::filesystem;

TitleArtProjectCheck editor::checkTitleArtFiles(const fs::path& projectRoot, const fs::path& assetsDir, const std::vector<fs::path>& files) {
    TitleArtProjectCheck check;
    // the documents: those named, the documents of baked PNGs named, or all
    std::set<fs::path> documents;
    AssetDatabase database;
    database.setReadOnly(true);
    database.setBackgroundHashing(false);
    database.open(projectRoot, assetsDir);
    if (files.empty()) {
        for (const fs::path& file : ProjectChecks::findProjectFiles(projectRoot, {titleart::TITLE_ART_EXTENSION})) documents.insert(file);
    } else {
        for (const fs::path& file : files) {
            if (Util::isTitleArtFile(file.string())) {
                documents.insert(file);
                continue;
            }
            if (!Util::isImageFile(file.string())) continue;
            std::string error;
            const std::optional<TitleBakeRecipe> recipe = readTitleBakeRecipe(file, &error);
            if (!recipe) continue;
            const fs::path document = database.getPath(recipe->sourceGuid);
            if (!document.empty()) documents.insert((projectRoot / document).lexically_normal());
        }
    }
    const fs::path cache = projectRoot / ".jasperengine" / "titleart-check-cache.json";
    titleArtHashCacheLoad(cache);
    EditorTitleAssetResolver resolver(&database, projectRoot, (projectRoot / assetsDir).lexically_normal());
    for (const fs::path& document : documents) {
        check.documents++;
        const std::string relative = ProjectChecks::projectRelative(projectRoot, document);
        auto add = [&](const std::string& severity, const std::string& code, const std::string& message, const std::string& suggestion, int line = 0) {
            ProjectChecks::Finding f;
            f.tool = "title";
            f.path = relative;
            f.line = line;
            f.severity = severity;
            f.code = code;
            f.message = message;
            f.suggestion = suggestion;
            check.findings.push_back(std::move(f));
        };
        const std::string regenerate = "jasperengine-editor call export_title --project " + projectRoot.generic_string() + " path=" + relative;
        std::string text;
        if (!assetio::readFile(document, text)) {
            add("error", "title-parse-failed", "cannot read the file", "");
            continue;
        }
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        if (!parsed.ok) {
            if (parsed.error.code == "version_newer") add("error", "title-version-newer", parsed.error.message, "open it with the editor that wrote it");
            else add("error", "title-parse-failed", parsed.error.message, "fix the JSON at the line given", parsed.error.line);
            continue;
        }
        for (const titleart::TitleProblem& p : titleArtMissingAssets(parsed.doc, resolver)) {
            add("error", "title-asset-missing", p.message, "check_title assets_present gives the nearest asset as a fix");
        }
        for (const titleart::TitleProblem& p : titleArtMissingGlyphs(parsed.doc, resolver)) {
            add("warning", "title-glyph-missing", p.message, "add a font that has them to the layer's fallback_fonts");
        }
        const TitleBakeStatus status = titleArtBakeStatus(document, parsed.doc, resolver);
        for (const TitleOutputState& o : status.outputs) {
            if (o.status == TitleOutputStatus::Missing) add("warning", "title-bake-missing", "output '" + o.id + "' is not baked (" + o.fileName + ")", regenerate);
            if (o.status == TitleOutputStatus::Stale) add("warning", "title-bake-stale", "output '" + o.id + "' is " + o.reason, regenerate);
            if (o.status == TitleOutputStatus::Invalid) add("warning", "title-recipe-invalid", "output '" + o.id + "': " + o.reason, regenerate);
        }
    }
    titleArtHashCacheSave(cache);
    database.close();
    return check;
}
