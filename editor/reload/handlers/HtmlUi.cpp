// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// HTML UI documents (S05 R11): every document of every open scene, prefab stage and playing
// scene that reads a changed file reloads through UIDocumentSystem::reloadChangedFiles, whose
// stamps make a change reload once whichever path sees it first. This replaces the 0.25 s
// HtmlUIWatcher poll.

#include "ReloadHandlers.h"

#include "Out.h"
#include "Project.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/HtmlUIDependencies.h"
#include "mcp/McpRefs.h"
#include "subsystem/UIDocumentSystem.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/Util.h"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

bool reload::isHtmlUiFile(const std::string& path) {
    if (ProjectChecks::isUiFile(path) || Util::isImageFile(path) || getHtmlUIReferenceKind(path) == HtmlUIReference::FONT) return true;
    const std::string ext = watch::lowerExtension(FileUtils::pathFromUtf8(path));
    // .frag and .glsl: a project UI effect (@effect src) and its includes, which the documents
    // declaring it read (phase 5)
    return ext == ".json" || ext == ".lottie" || ext == ".frag" || ext == ".glsl";
}

Json reload::syncHtmlUi(Project* project, const std::vector<fs::path>& changed, const std::vector<fs::path>& changedSince) {
    Json reloadedOut = Json::array();
    Json problems = Json::array();
    int current = 0;
    if (!project || project->getProjectPath().empty()) {
        return {{"reloaded", reloadedOut}, {"current", current}, {"problems", problems}};
    }
    const fs::path projectRoot = project->getProjectPath().lexically_normal();
    const fs::path assets = project->getAssetsPath().lexically_normal();

    // asset-relative, as UIDocumentSystem names files
    std::vector<std::string> assetPaths;
    std::set<std::string> changedAssets;
    for (const fs::path& path : changed) {
        const fs::path rel = path.lexically_normal().lexically_relative(assets);
        if (rel.empty() || rel == "." || *rel.begin() == ".." || rel.is_absolute()) continue;
        const std::string assetPath = jasperengine::normalizeHtmlUIPath(FileUtils::pathToGenericUtf8(rel));
        if (!assetPath.empty() && changedAssets.insert(assetPath).second) assetPaths.push_back(assetPath);
    }
    if (assetPaths.empty()) return {{"reloaded", reloadedOut}, {"current", current}, {"problems", problems}};
    std::vector<std::string> forgotten;
    for (const fs::path& path : changedSince) {
        const fs::path rel = path.lexically_normal().lexically_relative(assets);
        if (rel.empty() || rel == "." || *rel.begin() == ".." || rel.is_absolute()) continue;
        forgotten.push_back(jasperengine::normalizeHtmlUIPath(FileUtils::pathToGenericUtf8(rel)));
    }

    std::set<std::string> problemKeys;
    auto addProblem = [&](const ProjectChecks::Finding& finding) {
        const std::string key = finding.path + "\n" + std::to_string(finding.line) + "\n" + std::to_string(finding.col) + "\n" +
                                finding.severity + "\n" + finding.code + "\n" + finding.message;
        if (!problemKeys.insert(key).second) return;
        problems.push_back({{"path", finding.path}, {"line", finding.line}, {"col", finding.col}, {"severity", finding.severity},
                            {"code", finding.code}, {"message", finding.message}, {"suggestion", finding.suggestion}});
    };
    auto shown = [&](const std::string& assetPath) {
        return rel(*project, (assets / FileUtils::pathFromUtf8(assetPath)).lexically_normal());
    };

    std::set<std::string> covered;   // changed files a live document reads
    for (SceneProject* sceneProject : project->getHtmlUIScenes()) {
        Scene* scene = sceneProject->scene;
        if (!scene || !scene->isComponentRegistered<UIDocumentComponent>()) continue;
        auto components = scene->getComponentArray<UIDocumentComponent>();
        if (components->size() == 0) continue;
        UIDocumentSystem* system = scene->getSystem<UIDocumentSystem>().get();
        if (!system) continue;

        // the documents reading a changed file, found before the reload changes their files
        struct Document {
            Entity entity;
            std::string filename;
        };
        std::vector<Document> documents;
        for (size_t i = 0; i < components->size(); i++) {
            const UIDocumentComponent& component = components->getComponentFromIndex(i);
            if (component.filename.empty()) continue;
            const Entity entity = components->getEntity(i);
            Document document{entity, jasperengine::normalizeHtmlUIPath(component.filename)};
            bool reads = false;
            std::vector<std::string> files = system->getDocumentFiles(entity);
            files.push_back(document.filename);
            for (const std::string& file : files) {
                const std::string normal = jasperengine::normalizeHtmlUIPath(file);
                if (changedAssets.count(normal)) {
                    covered.insert(normal);
                    reads = true;
                }
            }
            if (reads) documents.push_back(document);
        }
        if (documents.empty()) continue;

        // a save in the same timestamp tick as the one a document read keeps its time and size
        if (!forgotten.empty()) system->forgetFileStamps(forgotten);
        const std::vector<UIDocumentSystem::Reloaded> reloaded = system->reloadChangedFiles(assetPaths);
        if (!reloaded.empty()) sceneProject->needUpdateRender = true;
        for (const UIDocumentSystem::Reloaded& entry : reloaded) {
            const UIDocumentComponent* component = scene->findComponent<UIDocumentComponent>(entry.entity);
            const std::string filename = component ? jasperengine::normalizeHtmlUIPath(component->filename) : std::string();
            reloadedOut.push_back({{"scene", sceneProject->id}, {"entity", mcp::McpRefs::entityJson(sceneProject, entry.entity)},
                                   {"html", shown(filename)}, {"stylesheet_only", entry.stylesheetOnly}});
            Out::info("[HTML UI] %s %s in scene '%s'", entry.stylesheetOnly ? "Restyled" : "Reloaded", filename.c_str(),
                      sceneProject->name.c_str());
        }
        for (const Document& document : documents) {
            const bool wasReloaded = std::any_of(reloaded.begin(), reloaded.end(),
                                                 [&](const UIDocumentSystem::Reloaded& entry) { return entry.entity == document.entity; });
            // the editor's poller saw the change first: reloaded already, nothing to do but report
            if (!wasReloaded) current++;
            for (const jasperengine::htmlui::compat::Diagnostic& diagnostic : system->getDiagnostics(document.entity)) {
                if (diagnostic.severity == jasperengine::htmlui::compat::Severity::Info) continue;
                if (!changedAssets.count(jasperengine::normalizeHtmlUIPath(diagnostic.file))) continue;
                addProblem(ProjectChecks::uiFinding(projectRoot, assets, diagnostic));
            }
            std::error_code ec;
            if (changedAssets.count(document.filename) && !fs::is_regular_file(assets / FileUtils::pathFromUtf8(document.filename), ec)) {
                ProjectChecks::Finding missing;
                missing.tool = "ui";
                missing.path = shown(document.filename);
                missing.severity = "error";
                missing.code = jasperengine::htmlui::compat::codes::FileNotFound.id();
                missing.message = "the document file is missing: '" + scene->getEntityName(document.entity) + "' shows nothing";
                missing.suggestion = "restore the file or set the UI Document's filename to an existing .html";
                addProblem(missing);
            }
        }
    }

    // changed .html/.htm/.css no live document reads: checked on their own
    std::vector<fs::path> unused;
    for (const std::string& assetPath : assetPaths) {
        std::error_code ec;
        const fs::path file = (assets / FileUtils::pathFromUtf8(assetPath)).lexically_normal();
        if (!covered.count(assetPath) && ProjectChecks::isUiFile(assetPath) && fs::is_regular_file(file, ec)) unused.push_back(file);
    }
    if (!unused.empty()) {
        std::set<std::string> changedShown;
        for (const std::string& assetPath : assetPaths) changedShown.insert(shown(assetPath));
        for (const ProjectChecks::Finding& finding : ProjectChecks::checkUiFiles(projectRoot, unused).findings) {
            if (changedShown.count(finding.path)) addProblem(finding);
        }
    }
    return {{"reloaded", reloadedOut}, {"current", current}, {"problems", problems}};
}

void reload::reloadHtmlUi(Context& ctx, const std::vector<fs::path>& files, const std::vector<fs::path>& changedSince) {
    if (files.empty()) return;
    const auto start = std::chrono::steady_clock::now();
    const Json ui = syncHtmlUi(&ctx.project, files, changedSince);
    for (const Json& item : ui["reloaded"]) ctx.result.uiReloaded.push_back(item);
    ctx.result.uiCurrent += ui.value("current", 0);
    for (const Json& problem : ui["problems"]) ctx.result.problems.push_back(problem);
    if (ui["reloaded"].empty()) return;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    for (const fs::path& path : files) ctx.service.recordDone("ui", path, ms);
}
