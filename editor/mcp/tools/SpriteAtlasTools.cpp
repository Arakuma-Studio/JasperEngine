// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S45's tools of the `sprites` group (docs/specs/S45-sprite-atlas.md R33-R35, §6.1):
// edit_sprite_atlas, get_sprite_atlas, and the sheet parts of get_asset_info (a texture's Sprite
// Mode and frames, packed_in for any sheet source, an atlas's build).

#include "McpTools.h"
#include "SpriteTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "EditorHost.h"
#include "Project.h"
#include "sprite/SpriteAtlasBuilder.h"
#include "sprite/SpriteSheetBuilder.h"
#include "util/AssetDatabase.h"
#include "util/ProjectFileWrites.h"

#include "jobs/JobSystem.h"
#include "sprite/SpriteSheetPool.h"
#include "sprite/SpriteSheetSources.h"
#include "util/Base64.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace jasperengine::editor::mcp {

namespace {

using pixelart::PixelProblem;
using namespace sprite;

Json atlasProblemJson(const PixelProblem& p) {
    Json j = {{"kind", p.kind}, {"message", p.message}, {"fix", p.fix.empty() ? std::string("see jasper://docs/sprites (Atlases)") : p.fix}};
    if (!p.field.empty()) j["field"] = p.field;
    return j;
}

void atlasFail(const CallPtr& call, const std::vector<PixelProblem>& problems) {
    Json list = Json::array();
    std::string text;
    for (const PixelProblem& p : problems) {
        list.push_back(atlasProblemJson(p));
        text += (text.empty() ? "" : "\n") + p.kind + ": " + p.message + (p.fix.empty() ? "" : " Fix: " + p.fix);
    }
    call->fail(text, Json{{"errors", list}, {"error", list.empty() ? Json() : list[0]}});
}

// a project-relative path under the assets directory -> asset-relative
bool assetArg(Project* project, const std::string& given, std::string& assetPath, fs::path& absolute) {
    absolute = (project->getProjectPath() / fs::path(given)).lexically_normal();
    assetPath = absolute.lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
    return !assetPath.empty() && assetPath != "." && assetPath.rfind("..", 0) != 0;
}

std::string projectRelativeOf(Project* project, const std::string& assetPath) {
    return (project->getAssetsPath() / assetPath).lexically_normal().lexically_relative(project->getProjectPath()).generic_string();
}

// one page as PNG (base64)
std::string pagePng(const pixelart::PixelImage& image) {
    const std::string png = image.encodePng();
    return Base64::encode(reinterpret_cast<const unsigned char*>(png.data()), png.size());
}

void atlasWhenMain(std::function<bool()> done, std::function<void(bool)> then, double seconds,
                   std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now()) {
    getEditorHost().enqueueMainThreadTask([done, then, seconds, started]() {
        const bool late = std::chrono::steady_clock::now() - started > std::chrono::duration<double>(seconds);
        if (done() || late) {
            then(!done());
            return;
        }
        atlasWhenMain(done, then, seconds, started);
    });
}

ToolResult jobReply(const McpJob& finished) {
    Json result = finished.result();
    ToolResult reply = finished.status() == JobStatus::Succeeded ? ToolResult::success(finished.message()) : ToolResult::error(finished.message());
    if (result.contains("images")) {
        for (const Json& png : result["images"]) reply.images.push_back({png.get<std::string>(), "image/png"});
        result.erase("images");
    }
    reply.structured = result;
    return reply;
}

// the members a list of paths adds: files by GUID, folders by path
bool addMembers(Project* project, const Json& list, SpriteAtlasDocument& doc, std::vector<PixelProblem>& problems) {
    for (const Json& item : list) {
        std::string assetPath;
        fs::path absolute;
        if (!item.is_string() || !assetArg(project, item.get<std::string>(), assetPath, absolute)) {
            problems.push_back({"missing_member", "add", "add takes project-relative paths under the assets directory", "pass [\"assets/sprites/hero.aseprite\", \"assets/ui/icons\"]"});
            continue;
        }
        std::error_code ec;
        while (!assetPath.empty() && assetPath.back() == '/') assetPath.pop_back();
        // an excluded path added again is no longer excluded
        doc.exclude.erase(std::remove(doc.exclude.begin(), doc.exclude.end(), assetPath), doc.exclude.end());
        if (fs::is_directory(absolute, ec)) {
            bool present = false;
            for (const SpriteAtlasMember& m : doc.members) present = present || (m.isFolder() && m.folder == assetPath);
            if (!present) {
                SpriteAtlasMember m;
                m.folder = assetPath;
                doc.members.push_back(m);
            }
            continue;
        }
        if (!fs::is_regular_file(absolute, ec)) {
            problems.push_back({"missing_member", item.get<std::string>(), "no file or folder " + item.get<std::string>(), "list_assets finds textures and .aseprite files"});
            continue;
        }
        if (!SpriteSheetSources::isTexturePath(assetPath) && !SpriteSheetPool::isAsepritePath(assetPath)) {
            problems.push_back({"missing_member", item.get<std::string>(), item.get<std::string>() + " is not a texture or an .aseprite file", "an atlas packs textures and .aseprite files"});
            continue;
        }
        bool present = false;
        for (const SpriteAtlasMember& m : doc.members) present = present || (!m.isFolder() && m.path == assetPath);
        if (present) continue;
        SpriteAtlasMember m;
        m.path = assetPath;
        m.guid = project->getAssetDatabase().getAssetGuid(project->normalizeToProjectRelative(absolute));
        doc.members.push_back(m);
    }
    return problems.empty();
}

// remove: a member is dropped; a file inside a folder member is excluded
void removeMembers(Project* project, const Json& list, SpriteAtlasDocument& doc, std::vector<PixelProblem>& problems) {
    for (const Json& item : list) {
        std::string assetPath;
        fs::path absolute;
        if (!item.is_string() || !assetArg(project, item.get<std::string>(), assetPath, absolute)) {
            problems.push_back({"missing_member", "remove", "remove takes project-relative paths", "pass the paths get_sprite_atlas lists"});
            continue;
        }
        while (!assetPath.empty() && assetPath.back() == '/') assetPath.pop_back();
        const size_t before = doc.members.size();
        doc.members.erase(std::remove_if(doc.members.begin(), doc.members.end(), [&](const SpriteAtlasMember& m) {
            return (m.isFolder() ? m.folder : m.path) == assetPath;
        }), doc.members.end());
        if (doc.members.size() != before) continue;
        bool inFolder = false;
        for (const SpriteAtlasMember& m : doc.members) inFolder = inFolder || (m.isFolder() && assetPath.rfind(m.folder + "/", 0) == 0);
        if (inFolder) {
            if (std::find(doc.exclude.begin(), doc.exclude.end(), assetPath) == doc.exclude.end()) doc.exclude.push_back(assetPath);
            continue;
        }
        problems.push_back({"missing_member", item.get<std::string>(), item.get<std::string>() + " is not a member of the atlas", "get_sprite_atlas lists its members"});
    }
}

Json buildSummary(Project* project, const AtlasBuildResult& r, bool images, Json& pngs) {
    Json j = r.toJson("", 0, -1, false);
    Json warnings = Json::array();
    for (const PixelProblem& w : r.warnings) warnings.push_back(atlasProblemJson(w));
    for (const PixelProblem& w : SpriteAtlasBuilder::get().crossWarnings(r.atlasPath)) warnings.push_back(atlasProblemJson(w));
    j["warnings"] = warnings;
    j["path"] = projectRelativeOf(project, r.atlasPath);
    if (images) {
        for (size_t p = 0; p < r.pages.size() && p < 4; p++) {
            if (p < r.pageImages.size()) {
                pngs.push_back(pagePng(r.pageImages[p]));
            } else {
                std::optional<pixelart::PixelImage> page = pixelart::PixelImage::loadPng(r.pageFile(p), nullptr);
                if (page) pngs.push_back(pagePng(*page));
            }
        }
        if (r.pages.size() > 4) j["pages_shown"] = 4;
    }
    return j;
}

void editSpriteAtlasTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    std::string atlasPath;
    fs::path absolute;
    if (!args.contains("path") || !args["path"].is_string() || !assetArg(project, args["path"].get<std::string>(), atlasPath, absolute) ||
        !SpriteSheetSources::isAtlasPath(atlasPath)) {
        atlasFail(call, {{"invalid_settings", "path", "path is a project-relative .spriteatlas under the assets directory", "pass \"assets/ui/icons.spriteatlas\""}});
        return;
    }
    const bool create = args.value("create", false);
    const bool output = args.value("output", false);
    const bool includePages = args.value("include_pages", true);
    const std::string platform = args.value("platform", std::string());
    std::error_code ec;
    const bool exists = fs::exists(absolute, ec);
    SpriteAtlasDocument doc;
    std::string before;
    if (exists) {
        if (create) {
            atlasFail(call, {{"invalid_settings", "create", args["path"].get<std::string>() + " exists", "leave create out to edit it"}});
            return;
        }
        std::ifstream in(absolute, std::ios::binary);
        before.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SpriteAtlasParseResult parsed = parseSpriteAtlas(before);
        if (!parsed.ok()) {
            atlasFail(call, {parsed.error});
            return;
        }
        doc = std::move(*parsed.document);
    } else if (!create) {
        atlasFail(call, {{"missing_member", "path", "no atlas " + args["path"].get<std::string>(), "pass create: true to make it"}});
        return;
    } else {
        doc.guid = Guid::parse(AssetDatabase::generateGuid());
    }
    std::vector<PixelProblem> problems;
    if (args.contains("add")) addMembers(project, args["add"], doc, problems);
    if (args.contains("remove")) removeMembers(project, args["remove"], doc, problems);
    if (args.contains("settings")) {
        Json settings = args["settings"];
        if (settings.is_object() && settings.contains("include_in_build")) {
            if (!settings["include_in_build"].is_boolean()) {
                problems.push_back({"invalid_settings", "include_in_build", "include_in_build is a boolean", ""});
            } else {
                doc.includeInBuild = settings["include_in_build"].get<bool>();
            }
            settings.erase("include_in_build");
        }
        applyAtlasSettings(settings, platform, doc.settings, problems);
    } else if (!platform.empty() && !isAtlasPlatform(platform) && platform != "default") {
        problems.push_back({"invalid_settings", "platform", "platform is desktop, mobile, web or android_etc2", ""});
    }
    if (!problems.empty()) {
        atlasFail(call, problems);
        return;
    }
    AtlasBuildInput input;
    SpriteAtlasBuilder::gather(*project, atlasPath, doc, isAtlasPlatform(platform) ? platform : "desktop", input);
    for (const PixelProblem& p : input.problems) {
        if (p.kind == "importing") {
            // the members' imports first: the call waits (the editor keeps running)
            call->waitUntil([project, atlasPath, doc]() {
                AtlasBuildInput again;
                SpriteAtlasBuilder::gather(*project, atlasPath, doc, "desktop", again);
                for (const PixelProblem& q : again.problems) {
                    if (q.kind == "importing") return false;
                }
                return true;
            }, 120000, [call](bool timedOut) {
                if (timedOut) {
                    atlasFail(call, {{"missing_member", "add", "a member's import did not finish within 120 s", "call again once get_asset_info's import_state is imported"}});
                    return;
                }
                editSpriteAtlasTool(call);
            });
            return;
        }
    }
    const std::string given = args["path"].get<std::string>();
    if (!output) {
        // the preview: packed in memory, nothing written
        input.outDir.clear();
        JobPtr job = call->ctx().jobs().create("edit_sprite_atlas", "Preview Sprite Atlas " + fs::path(atlasPath).stem().string());
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "sprite-atlas-preview";
        jobs::submit([job, input, project, includePages, given]() {
            struct Wake {
                ~Wake() { getEditorHost().enqueueMainThreadTask([]() {}); }
            } wake;
            std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
            if (!r->ok) {
                job->fail(r->error.kind + ": " + r->error.message + (r->error.fix.empty() ? "" : " Fix: " + r->error.fix),
                          Json{{"error", atlasProblemJson(r->error)}, {"errors", Json::array({atlasProblemJson(r->error)})}});
                return;
            }
            Json pngs = Json::array();
            Json data = buildSummary(project, *r, includePages, pngs);
            data["written"] = false;
            data["next"] = "Nothing written. The same arguments plus output: true write " + given + " and build it.";
            if (!pngs.empty()) data["images"] = pngs;
            job->succeed("Preview of " + given + ": " + r->statsLine() + ". Nothing written.", data);
        }, options);
        replyWhenJobDone(call, job, true, 1.0, jobReply);
        return;
    }

    const std::string text = writeSpriteAtlas(doc);
    if (exists && text == before) {
        SpriteAtlasBuilder::get().rebuild(atlasPath);
    } else {
        std::string error;
        if (!ProjectFileWrites::writeUndoable(project, {{absolute, text}}, error, nullptr, nullptr, {}, false,
                                              (create ? "Create Sprite Atlas " : "Edit Sprite Atlas ") + absolute.filename().string())) {
            atlasFail(call, {{"invalid_settings", "path", "couldn't write " + given + ": " + error, "check the folder is writable"}});
            return;
        }
        SpriteAtlasBuilder::get().rebuild(atlasPath);
    }
    const uint64_t buildsBefore = SpriteAtlasBuilder::get().buildCount(atlasPath);
    JobPtr job = call->ctx().jobs().create("edit_sprite_atlas", "Build Sprite Atlas " + fs::path(atlasPath).stem().string());
    job->setCancelHandler([]() { SpriteAtlasBuilder::get().cancelAll(); });
    atlasWhenMain([atlasPath, buildsBefore]() {
        SpriteAtlasBuilder& b = SpriteAtlasBuilder::get();
        return b.buildCount(atlasPath) > buildsBefore && !b.isBuilding(atlasPath);
    }, [job, atlasPath, project, includePages, given, create](bool timedOut) {
        std::shared_ptr<const AtlasBuildResult> r = SpriteAtlasBuilder::get().lastAttempt(atlasPath);
        if (timedOut || !r) {
            job->fail("the build did not finish within 5 minutes", Json{{"error", {{"kind", "missing_member"}, {"message", "the build is still running"},
                                                                                 {"fix", "read get_sprite_atlas's build_state later"}}}});
            return;
        }
        if (!r->ok) {
            job->fail(r->error.kind + ": " + r->error.message + (r->error.fix.empty() ? "" : " Fix: " + r->error.fix),
                      Json{{"error", atlasProblemJson(r->error)}, {"errors", Json::array({atlasProblemJson(r->error)})}, {"written", true}});
            return;
        }
        Json pngs = Json::array();
        Json data = buildSummary(project, *r, includePages, pngs);
        data["written"] = true;
        data["next"] = std::string(create ? "Created" : "Wrote") + " and built " + given +
                       " (undo with scope project). Sprites keep referencing their sources; they now draw from its pages.";
        if (!pngs.empty()) data["images"] = pngs;
        job->succeed((create ? "Created " : "Wrote ") + given + ": " + r->statsLine() + ".", data);
    }, 300.0);
    replyWhenJobDone(call, job, true, 1.0, jobReply);
}

void getSpriteAtlasTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    const Json& args = call->args();
    std::string assetPath;
    fs::path absolute;
    if (!args.contains("path") || !args["path"].is_string() || !assetArg(project, args["path"].get<std::string>(), assetPath, absolute)) {
        atlasFail(call, {{"invalid_settings", "path", "path is a project-relative .spriteatlas, or a texture or .aseprite file to find its atlas", ""}});
        return;
    }
    const std::string given = args["path"].get<std::string>();
    SpriteAtlasBuilder& builder = SpriteAtlasBuilder::get();
    const std::string search = args.value("search", std::string());
    const int limit = args.value("limit", 50);
    const int page = args.value("page", -1);
    const bool includePages = args.value("include_pages", false);
    const std::string platform = args.value("platform", std::string());

    if (!SpriteSheetSources::isAtlasPath(assetPath)) {
        // a member: the atlas that packs it and its placements
        const std::vector<std::string> atlases = builder.atlasesPacking(assetPath);
        int memberPage = 0;
        const std::string atlas = builder.packedIn(assetPath, &memberPage);
        Json data = {{"path", given}, {"packed_in", atlas.empty() ? Json() : Json(projectRelativeOf(project, atlas))}};
        Json all = Json::array();
        for (const std::string& a : atlases) all.push_back(projectRelativeOf(project, a));
        data["atlases"] = all;
        std::shared_ptr<const AtlasBuildResult> r = atlas.empty() ? nullptr : builder.result(atlas);
        if (r) {
            Json placements = r->toJson("member:" + assetPath, limit, -1, true)["frames"];
            Json mine = Json::array();
            for (const Json& f : placements) {
                if (f.value("member", std::string()) == assetPath) mine.push_back(f);
            }
            data["placements"] = mine;
            data["in_use"] = !atlases.empty();
        }
        call->ok(atlas.empty() ? given + " is not packed by any atlas." : given + " is packed in " + projectRelativeOf(project, atlas) + " (page " +
                                                                              std::to_string(memberPage) + ").", data);
        return;
    }
    std::error_code ec;
    if (!fs::exists(absolute, ec)) {
        atlasFail(call, {{"missing_member", "path", "no atlas " + given, "edit_sprite_atlas with create: true makes one"}});
        return;
    }
    PixelProblem problem;
    std::optional<SpriteAtlasDocument> doc = SpriteAtlasBuilder::readDocument(absolute, &problem);
    if (!doc) {
        atlasFail(call, {problem});
        return;
    }
    Json members = Json::array();
    for (const SpriteAtlasMember& m : doc->members) {
        if (m.isFolder()) members.push_back({{"folder", projectRelativeOf(project, m.folder)}, {"types", m.types}});
        else members.push_back({{"path", projectRelativeOf(project, m.path)}, {"guid", m.guid.str()}});
    }
    Json exclude = Json::array();
    for (const std::string& e : doc->exclude) exclude.push_back(projectRelativeOf(project, e));
    Json base = {{"path", given}, {"guid", doc->guid.str()}, {"include_in_build", doc->includeInBuild}, {"members", members}, {"exclude", exclude},
                 {"settings", atlasSettingsJson(doc->settings)}, {"mode", atlasModeName(builder.mode())}};

    if (!platform.empty() && platform != "desktop") {
        if (!isAtlasPlatform(platform)) {
            atlasFail(call, {{"invalid_settings", "platform", "platform is desktop, mobile, web or android_etc2", ""}});
            return;
        }
        // another family's layout: packed in memory
        AtlasBuildInput input;
        SpriteAtlasBuilder::gather(*project, assetPath, *doc, platform, input);
        input.outDir.clear();
        JobPtr job = call->ctx().jobs().create("get_sprite_atlas", "Pack Sprite Atlas for " + platform);
        jobs::SubmitOptions options;
        options.lane = jobs::Lane::Background;
        options.name = "sprite-atlas-preview";
        jobs::submit([job, input, base, search, limit, page, includePages, project]() {
            struct Wake {
                ~Wake() { getEditorHost().enqueueMainThreadTask([]() {}); }
            } wake;
            std::shared_ptr<AtlasBuildResult> r = SpriteAtlasBuilder::get().build(input);
            if (!r->ok) {
                job->fail(r->error.kind + ": " + r->error.message, Json{{"error", atlasProblemJson(r->error)}});
                return;
            }
            Json data = base;
            Json built = r->toJson(search, limit, page, true);
            for (auto it = built.begin(); it != built.end(); ++it) data[it.key()] = it.value();
            data["build_state"] = "preview";
            if (includePages) {
                Json pngs = Json::array();
                for (size_t p = 0; p < r->pageImages.size() && p < 4; p++) pngs.push_back(pagePng(r->pageImages[p]));
                data["images"] = pngs;
            }
            job->succeed(data["path"].get<std::string>() + " on " + r->platform + ": " + r->statsLine() + ".", data);
        }, options);
        replyWhenJobDone(call, job, true, 1.0, jobReply);
        return;
    }

    Json data = base;
    std::shared_ptr<const AtlasBuildResult> r = builder.result(assetPath);
    std::shared_ptr<const AtlasBuildResult> attempt = builder.lastAttempt(assetPath);
    std::string state = builder.isBuilding(assetPath) ? "building" : r ? "built" : builder.mode() == AtlasMode::Disabled ? "disabled" : "not_built";
    if (attempt && !attempt->ok && attempt->error.kind != "cancelled" && !builder.isBuilding(assetPath)) {
        state = "failed";
        data["error"] = atlasProblemJson(attempt->error);
    }
    data["build_state"] = state;
    Json warnings = Json::array();
    if (r) {
        Json built = r->toJson(search, limit, page, true);
        for (auto it = built.begin(); it != built.end(); ++it) data[it.key()] = it.value();
        for (const PixelProblem& w : r->warnings) warnings.push_back(atlasProblemJson(w));
    }
    for (const PixelProblem& w : builder.crossWarnings(assetPath)) warnings.push_back(atlasProblemJson(w));
    data["warnings"] = warnings;
    ToolResult reply = ToolResult::success(given + ": " + (r ? r->statsLine() : std::string("not built (") + state + ")") + ".", data);
    if (r && includePages) {
        for (size_t p = 0; p < r->pages.size() && p < 4; p++) {
            std::optional<pixelart::PixelImage> image = pixelart::PixelImage::loadPng(r->pageFile(p), nullptr);
            if (image) reply.images.push_back({pagePng(*image), "image/png"});
        }
    }
    call->reply(reply);
}

}

Json spriteSheetAssetInfo(Project* project, const fs::path& file, const std::string& kind, std::string& text) {
    Json data = Json::object();
    const std::string assetPath = file.lexically_normal().lexically_relative(project->getAssetsPath().lexically_normal()).generic_string();
    SpriteAtlasBuilder& builder = SpriteAtlasBuilder::get();
    if (kind == "texture") {
        const TextureSheetInfo info = textureSheetSettings(file);
        Json mode = sheetSettingsToJson(info.settings);
        mode["stored"] = info.sourceName();
        if (info.readOnly) mode["read_only"] = true;
        Json frames = Json::array();
        if (info.isSheet()) {
            TextureSheetBuild built = buildTextureSheet(file, assetPath, info.settings);
            for (size_t i = 0; i < built.sheet.frames.size() && i < 64; i++) {
                const SpriteFrame& f = built.sheet.frames[i];
                Rect r = f.rect;
                frames.push_back({{"index", i}, {"name", f.name}, {"rect", {static_cast<int>(r.getX()), static_cast<int>(r.getY()), static_cast<int>(r.getWidth()), static_cast<int>(r.getHeight())}}});
            }
            mode["frame_count"] = built.sheet.frames.size();
            Json warnings = Json::array();
            for (const std::string& w : built.warnings) warnings.push_back(w);
            for (const PixelProblem& p : info.problems) warnings.push_back(p.message);
            if (!warnings.empty()) mode["warnings"] = warnings;
            text += "; Sprite Mode " + std::string(spriteModeName(info.settings.mode)) + ", " + std::to_string(built.sheet.frames.size()) + " frame(s)";
        }
        mode["frames"] = frames;
        data["sprite_mode"] = mode;
    }
    if (kind == "sprite_atlas") {
        std::shared_ptr<const AtlasBuildResult> r = builder.result(assetPath);
        data["atlas"] = r ? r->toJson("", 0, -1, false) : Json{{"build_state", builder.isBuilding(assetPath) ? "building" : "not_built"}};
        if (r) text += "; " + r->statsLine();
        return data;
    }
    int page = 0;
    const std::string atlas = builder.packedIn(assetPath, &page);
    if (!atlas.empty()) {
        data["packed_in"] = {{"atlas", projectRelativeOf(project, atlas)}, {"page", page}, {"in_use", !builder.atlasesPacking(assetPath).empty()}};
        text += "; packed in " + projectRelativeOf(project, atlas) + " (page " + std::to_string(page) + ")";
    }
    return data;
}

void registerSpriteAtlasTools(McpRegistry& registry) {
    using namespace schema;
    {
        ToolDef tool;
        tool.name = "edit_sprite_atlas";
        tool.title = "Edit Sprite Atlas";
        tool.description = "Create a .spriteatlas, add or remove members (textures, .aseprite files, folders), change settings (keys: "
                           "jasper://docs/sprites; platform: one family's max_size). Without output: an in-memory pack (pages, usage, "
                           "duplicates, warnings), nothing written; output: true writes it (one undo step) and builds it.";
        tool.inputSchema = object({
            {"path", string("")},
            {"create", withDefault(boolean(""), false)},
            {"add", array(string(""), "")},
            {"remove", array(string(""), "")},
            {"settings", anyObject("")},
            {"platform", string("")},
            {"include_pages", withDefault(boolean(""), true)},
            {"output", withDefault(boolean(""), false)},
        }, {"path"});
        tool.allowedWhilePlaying = false;
        tool.group = "sprites";
        tool.handler = editSpriteAtlasTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_sprite_atlas";
        tool.title = "Get Sprite Atlas";
        tool.description = "A .spriteatlas's settings, members, pages, frames (search: name, member:, page:) and warnings; a texture or "
                           ".aseprite path gives the atlas packing it and its placements.";
        tool.inputSchema = object({
            {"path", string("")},
            {"search", string("")},
            {"limit", withDefault(integer("", 0, 10000), 50)},
            {"page", integer("", 0, 1000)},
            {"platform", string("")},
            {"include_pages", withDefault(boolean(""), false)},
        }, {"path"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.group = "sprites";
        tool.handler = getSpriteAtlasTool;
        registry.addTool(std::move(tool));
    }
}

}
