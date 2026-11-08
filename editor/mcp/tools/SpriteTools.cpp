// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The `sprites` group (docs/specs/S44-aseprite-import.md §6.1, shared with S45 and S47):
// set_sprite_import_settings, and the .aseprite parts of get_asset_info and view_asset.

#include "McpTools.h"
#include "SpriteTools.h"

#include <chrono>

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "EditorHost.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/SetAssetMetaCmd.h"
#include "pixelart/aseprite/AsepriteContactSheet.h"
#include "pixelart/aseprite/AsepriteImporter.h"

#include "jobs/JobSystem.h"
#include "sprite/SpriteSheetPool.h"
#include "util/Base64.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

namespace ase = pixelart::aseprite;

Json problemJson(const pixelart::PixelProblem& p) {
    Json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix.empty() ? std::string("see jasper://docs/sprites (Import)") : p.fix}};
    if (!p.field.empty()) j["field"] = p.field;
    return j;
}

void failTyped(const CallPtr& call, const std::vector<pixelart::PixelProblem>& problems) {
    Json list = Json::array();
    std::string text;
    for (const pixelart::PixelProblem& p : problems) {
        list.push_back(problemJson(p));
        text += (text.empty() ? "" : "\n") + p.kind + ": " + p.message + (p.fix.empty() ? "" : " Fix: " + p.fix);
    }
    call->fail(text, Json{{"errors", list}, {"error", list.empty() ? Json() : list[0]}});
}

// "<project>/assets/sprites/hero.aseprite" from a project-relative path, checked
bool asepritePathArg(Project* project, const std::string& given, fs::path& absolute, std::string& assetPath, pixelart::PixelProblem& problem) {
    absolute = (project->getProjectPath() / fs::path(given)).lexically_normal();
    std::error_code ec;
    if (!fs::is_regular_file(absolute, ec)) {
        problem = {"import_failed", "paths", "no file " + given, "pass project-relative paths of .aseprite files (list_assets finds them)"};
        return false;
    }
    if (!SpriteSheetPool::isAsepritePath(given)) {
        problem = {"not_aseprite", "paths", given + " is not an .aseprite/.ase file", "set_texture_import_settings is for images"};
        return false;
    }
    const fs::path relative = absolute.lexically_relative(project->getAssetsPath().lexically_normal());
    assetPath = relative.generic_string();
    if (assetPath.empty() || assetPath.rfind("..", 0) == 0) {
        problem = {"import_failed", "paths", given + " is outside the assets directory", "move it under the assets directory"};
        return false;
    }
    return true;
}

// what one file imports to with `settings`, and its contact sheet (any thread)
Json previewOf(const fs::path& absolute, const std::string& assetPath, const ase::AsepriteImportSettings& settings, bool image,
               std::string* pngBase64, std::vector<pixelart::PixelProblem>& problems) {
    ase::ReadOptions options;
    options.provenance = false;
    ase::ReadResult read = ase::readFile(absolute, options);
    if (!read.ok()) {
        problems.push_back(read.error);
        return Json();
    }
    ase::AsepriteImportContext ctx;
    ctx.sourcePath = assetPath;
    ase::AsepriteImport import = ase::AsepriteImport::run(*read.document, settings, ctx);
    if (!import.ok()) {
        problems.push_back(import.error);
        return Json();
    }
    Json out = import.toJson();
    if (image && pngBase64) {
        const pixelart::PixelImage sheet = ase::contactSheet(import.sheet, import.pages, 768);
        const std::string png = sheet.encodePng();
        *pngBase64 = Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size());
    }
    return out;
}

// polls `done` on the main thread each loop until it holds, then runs `then` (an import's swap
// happens there)
// `then(timedOut)` on the main thread once `done()` holds, or after `seconds`
void onMainWhen(std::function<bool()> done, std::function<void(bool)> then, double seconds,
                std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now()) {
    getEditorHost().enqueueMainThreadTask([done, then, seconds, started]() {
        const bool late = std::chrono::steady_clock::now() - started > std::chrono::duration<double>(seconds);
        if (done() || late) {
            then(!done());
            return;
        }
        onMainWhen(done, then, seconds, started);
    });
}

void setSpriteImportSettingsTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    if (!args.contains("paths") || !args["paths"].is_array() || args["paths"].empty()) {
        failTyped(call, {{"invalid_settings", "paths", "paths is a non-empty list of project-relative .aseprite files", "pass [\"assets/sprites/hero.aseprite\"]"}});
        return;
    }
    const Json patch = args.value("settings", Json::object());
    const bool output = args.value("output", false);
    const bool includePreview = args.value("include_preview", false);
    ase::AsepriteImporter& importer = ase::AsepriteImporter::get();

    struct Item {
        std::string given, assetPath;
        fs::path absolute;
        ase::AsepriteImportSettings settings;
    };
    std::vector<Item> items;
    std::vector<pixelart::PixelProblem> problems;
    for (const Json& p : args["paths"]) {
        Item item;
        pixelart::PixelProblem problem;
        if (!p.is_string() || !asepritePathArg(project, p.get<std::string>(), item.absolute, item.assetPath, problem)) {
            if (!p.is_string()) problem = {"invalid_settings", "paths", "paths holds strings", ""};
            problems.push_back(problem);
            continue;
        }
        item.given = p.get<std::string>();
        item.settings = importer.settingsOf(item.assetPath);
        if (item.settings.readOnly()) {
            problems.push_back({"invalid_settings", "version", item.given + "'s import settings are version " + std::to_string(item.settings.version) +
                                ", written by a newer editor (read only here)", "edit them with that editor"});
            continue;
        }
        std::vector<pixelart::PixelProblem> local;
        item.settings.applyJson(patch, local);
        for (pixelart::PixelProblem& l : local) problems.push_back(std::move(l));
        // a layer filter naming a missing layer is refused here (a warning in a plain import)
        ase::ReadOptions quick;
        quick.pixels = false;
        quick.provenance = false;
        ase::ReadResult read = ase::readFile(item.absolute, quick);
        if (!read.ok()) {
            problems.push_back(read.error);
            continue;
        }
        std::vector<pixelart::PixelProblem> warnings;
        ase::selectLayers(*read.document, item.settings, warnings);
        for (pixelart::PixelProblem& w : warnings) {
            if (w.kind == "unknown_layer") problems.push_back(std::move(w));
        }
        items.push_back(std::move(item));
    }
    if (!problems.empty()) {
        failTyped(call, problems);
        return;
    }

    JobPtr job = call->ctx().jobs().create("set_sprite_import_settings", output ? "Import Aseprite files" : "Preview Aseprite imports");
    if (!output) {
        // the preview: nothing written; the import runs off the main thread
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "aseprite-import-preview";
        jobs::submit([job, items, includePreview]() {
            struct Wake {
                ~Wake() { getEditorHost().enqueueMainThreadTask([]() {}); }
            } wake;
            Json files = Json::array();
            std::vector<pixelart::PixelProblem> failures;
            Json images = Json::array();
            for (const Item& item : items) {
                std::string png;
                Json preview = previewOf(item.absolute, item.assetPath, item.settings, includePreview, &png, failures);
                if (preview.is_null()) continue;
                files.push_back({{"path", item.given}, {"settings", item.settings.toJson()}, {"outputs", preview}});
                if (!png.empty()) images.push_back(png);
            }
            if (!failures.empty()) {
                Json list = Json::array();
                for (const pixelart::PixelProblem& p : failures) list.push_back(problemJson(p));
                job->fail(failures[0].kind + ": " + failures[0].message, Json{{"errors", list}, {"error", list[0]}});
                return;
            }
            job->succeed("Preview of " + std::to_string(files.size()) + " file(s); nothing written. The same arguments with output: true write it.",
                         Json{{"files", files}, {"images", images}, {"written", false}});
        }, options);
    } else {
        // the metas, one undo step; the importer imports each file again (its meta listener)
        // each file's own import of these settings is the one after this count (the previous
        // good import stays in record() when it fails)
        std::vector<uint64_t> attemptsBefore;
        for (const Item& item : items) attemptsBefore.push_back(importer.attemptCount(item.assetPath));
        CommandHistory* history = project->getProjectCommandHistory();
        InteractiveEdit edit = history->beginInteractive("Set Sprite Import Settings");
        for (const Item& item : items) {
            Command* command = new SetAssetMetaCmd(project, project->normalizeToProjectRelative(item.absolute), ase::AsepriteImporter::kSettingsKey,
                                                   item.settings.toYaml());
            if (command->isNoOp()) {
                delete command;
                importer.reimport(item.assetPath);   // unchanged settings: imported again all the same
                continue;
            }
            if (!history->addCommand(command)) {
                edit.cancel();
                failTyped(call, {{"import_failed", "paths", "couldn't write the import settings of " + item.given, "check the .meta file is writable"}});
                return;
            }
        }
        edit.commit();
        std::vector<std::string> paths;
        for (const Item& item : items) paths.push_back(item.assetPath);
        onMainWhen([paths, attemptsBefore, &importer]() {
            for (size_t i = 0; i < paths.size(); i++) {
                if (importer.isPending(paths[i]) || importer.attemptCount(paths[i]) <= attemptsBefore[i]) return false;
            }
            return true;
        }, [job, items, paths, &importer, includePreview](bool timedOut) {
            if (timedOut) {
                const Json problem = {{"kind", "import_failed"}, {"field", "paths"}, {"message", "the import did not finish within 5 minutes"},
                                      {"fix", "wait, then read get_asset_info's import_state"}};
                job->fail("import_failed: the import did not finish within 5 minutes", Json{{"errors", Json::array({problem})}, {"error", problem}});
                return;
            }
            Json files = Json::array();
            Json images = Json::array();
            Json failures = Json::array();
            for (size_t i = 0; i < items.size(); i++) {
                std::shared_ptr<const ase::ImportRecord> record = importer.lastAttempt(paths[i]);
                if (!record || !record->ok) {
                    failures.push_back(problemJson(record ? record->error : pixelart::PixelProblem{"import_failed", "", "no import of " + items[i].given, ""}));
                    continue;
                }
                files.push_back({{"path", items[i].given}, {"settings", items[i].settings.toJson()}, {"outputs", record->toJson()}});
                if (includePreview) {
                    std::vector<pixelart::PixelImage> pages;
                    for (size_t p = 0; p < record->pages.size(); p++) {
                        std::optional<pixelart::PixelImage> page = pixelart::PixelImage::loadPng(record->pageFile(p), nullptr);
                        pages.push_back(page ? *page : pixelart::PixelImage());
                    }
                    std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(paths[i]);
                    if (sheet) {
                        const std::string png = ase::contactSheet(*sheet, pages, 768).encodePng();
                        images.push_back(Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()));
                    }
                }
            }
            if (!failures.empty()) {
                job->fail(failures[0].value("kind", std::string()) + ": " + failures[0].value("message", std::string()),
                          Json{{"errors", failures}, {"error", failures[0]}});
                return;
            }
            job->succeed("Wrote the import settings of " + std::to_string(files.size()) + " file(s) and imported them. Undo with scope project.",
                         Json{{"files", files}, {"images", images}, {"written", true}});
        }, 300.0);
    }
    replyWhenJobDone(call, job, true, 1.0, [](const McpJob& finished) {
        Json result = finished.result();
        ToolResult reply = finished.status() == JobStatus::Succeeded ? ToolResult::success(finished.message()) : ToolResult::error(finished.message());
        if (result.contains("images")) {
            for (const Json& png : result["images"]) reply.images.push_back({png.get<std::string>(), "image/png"});
            result.erase("images");
        }
        reply.structured = result;
        return reply;
    });
}

}

Json asepriteAssetInfo(Project* project, const fs::path& file, std::string& text) {
    ase::AsepriteImporter& importer = ase::AsepriteImporter::get();
    const std::string assetPath = file.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
    Json data = Json::object();
    Json warnings = Json::array();
    ase::ReadOptions options;
    options.pixels = false;
    options.provenance = false;
    ase::ReadResult read = ase::readFile(file, options);
    if (read.ok()) {
        data["sprite"] = ase::describe(*read.document, ase::Detail::Summary);
    } else {
        // a file being written (or broken): the import of the last version that read stays
        data["read_error"] = problemJson(read.error);
        text += "; it does not read: " + read.error.message;
    }
    std::vector<pixelart::PixelProblem> settingsProblems;
    const ase::AsepriteImportSettings settings = importer.settingsOf(assetPath, &settingsProblems);
    data["import_settings"] = {{"stored", SetAssetMetaCmd::read(file, ase::AsepriteImporter::kSettingsKey) ? "meta" : "defaults"},
                               {"resolved", settings.toJson()}, {"read_only", settings.readOnly()}};
    for (const pixelart::PixelProblem& p : settingsProblems) warnings.push_back(problemJson(p));
    std::shared_ptr<const ase::ImportRecord> record = importer.record(assetPath);
    if (record) {
        data["import"] = record->toJson();
        for (const pixelart::PixelProblem& p : record->warnings) warnings.push_back(problemJson(p));
        text += "; imported: " + record->summary;
    } else {
        text += importer.isPending(assetPath) ? "; importing" : "; not imported yet";
    }
    data["import_state"] = importer.isPending(assetPath) ? "importing" : record ? (record->ok ? "imported" : "failed") : "none";
    data["warnings"] = warnings;
    return data;
}

bool replyAsepriteView(const CallPtr& call, Project* project, const fs::path& file, int maxSize) {
    const std::string assetPath = file.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
    std::shared_ptr<const ase::ImportRecord> record = ase::AsepriteImporter::get().record(assetPath);
    std::shared_ptr<const SpriteSheetData> sheet = SpriteSheetPool::get(assetPath);
    if (!record || !record->ok || !sheet) {
        call->fail(assetPath + " has no import yet (get_asset_info says import_state); try again once it is imported.",
                   Json{{"error", {{"kind", "import_failed"}, {"message", "no import of " + assetPath}, {"fix", "wait for the import, or fix its warnings"}}}});
        return true;
    }
    std::vector<pixelart::PixelImage> pages;
    for (size_t p = 0; p < record->pages.size(); p++) {
        std::optional<pixelart::PixelImage> page = pixelart::PixelImage::loadPng(record->pageFile(p), nullptr);
        pages.push_back(page ? *page : pixelart::PixelImage());
    }
    const pixelart::PixelImage contact = ase::contactSheet(*sheet, pages, maxSize);
    const std::string png = contact.encodePng();
    ToolResult result = ToolResult::success(assetPath + ": " + std::to_string(sheet->frames.size()) + " frames as the game draws them, " +
                                                "labelled by index, tags as bands.",
                                            Json{{"frames", sheet->frames.size()}, {"tags", record->tags}, {"width", contact.width()}, {"height", contact.height()}});
    result.images.push_back({Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size()), "image/png"});
    call->reply(result);
    return true;
}

void registerSpriteTools(McpRegistry& registry) {
    using namespace schema;
    ToolDef tool;
    tool.name = "set_sprite_import_settings";
    tool.title = "Set Sprite Import Settings";
    tool.description = "Partial update of .aseprite files' import settings (keys: jasper://docs/sprites). Without output: a preview of "
                       "the frames, pages, clips and warnings, nothing written; output: true writes the metas (one undo step) and reimports.";
    tool.inputSchema = object({
        {"paths", array(string(""), "Project-relative .aseprite files.")},
        {"settings", anyObject("")},
        {"output", withDefault(boolean(""), false)},
        {"include_preview", withDefault(boolean("A contact sheet image."), false)},
    }, {"paths"});
    tool.allowedWhilePlaying = false;
    tool.group = "sprites";
    tool.handler = setSpriteImportSettingsTool;
    registry.addTool(std::move(tool));
    // S45: edit_sprite_atlas, get_sprite_atlas
    registerSpriteAtlasTools(registry);
}

}
