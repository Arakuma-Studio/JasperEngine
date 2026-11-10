// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLanguageExport.h"

#include "Project.h"
#include "asset/AssetFileWrite.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleArtDoc.h"
#include "util/ProjectChecks.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
namespace fs = std::filesystem;

namespace {

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

}

TitleLanguageShipping editor::planTitleLanguageShipping(Project* project) {
    TitleLanguageShipping out;
    if (!project || project->getProjectPath().empty()) return out;
    const std::vector<std::string>& shipped = project->getLocalizationSettings().languages;
    if (shipped.empty()) return out;
    const fs::path assets = project->getAssetsPath();
    for (const fs::path& document : ProjectChecks::findProjectFiles(assets, {titleart::TITLE_ART_EXTENSION})) {
        const fs::path sidecar = titleArtSidecarPath(document);
        std::string text, error;
        TitleBakeInfo info;
        if (!assetio::readFile(sidecar, text) || !TitleBakeInfo::parse(text, info, error)) continue;
        if (!info.extra.contains("languages") || !info.extra["languages"].is_object()) continue;
        Json& table = info.extra["languages"];
        if (!table.contains("sets") || !table["sets"].is_object()) continue;
        Json kept = Json::object();
        const std::string relativeDocument = project->normalizeToProjectRelative(document).generic_string();
        for (auto it = table["sets"].begin(); it != table["sets"].end(); ++it) {
            const std::string code = it.key();
            const bool ships = std::any_of(shipped.begin(), shipped.end(), [&](const std::string& s) {
                return lower(TitleArtLanguage::canonicalize(s)) == lower(TitleArtLanguage::canonicalize(code));
            });
            if (ships) {
                kept[code] = it.value();
                continue;
            }
            out.warnings.push_back("W-TITLE-LANGUAGE-DROPPED: " + relativeDocument + " " + code + " is not in localization.languages: it is left out");
            if (!it.value().is_string()) continue;
            // the language's sidecar and every output it lists
            const fs::path languageSidecar = document.parent_path() / it.value().get<std::string>();
            out.droppedFiles.insert(fs::relative(languageSidecar, assets).generic_string());
            std::string languageText;
            TitleBakeInfo language;
            if (!assetio::readFile(languageSidecar, languageText) || !TitleBakeInfo::parse(languageText, language, error)) continue;
            for (const TitleBakeInfoOutput& o : language.outputs) {
                const fs::path png = (document.parent_path() / o.file).lexically_normal();
                out.droppedFiles.insert(fs::relative(png, assets).generic_string());
                out.droppedFiles.insert(fs::relative(fs::path(png.string() + ".meta"), assets).generic_string());
            }
        }
        if (kept.size() == table["sets"].size()) continue;
        table["sets"] = kept;
        out.sidecars.emplace_back(fs::relative(sidecar, assets).generic_string(), info.toText());
    }
    return out;
}
