// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The shipping group (docs/specs/S30-shipping.md R32): player runtimes and releases. Every tool
// here runs the same code as `jasperengine-editor players|publish` (editor/publish/Publishing.h,
// editor/players/PlayerRuntimes.h). publish_steam_upload is deliberately absent: a person runs it,
// with SteamCMD's own login.

#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"

#include "Project.h"
#include "jobs/JobChannel.h"
#include "players/PlayerRuntimes.h"
#include "publish/Publishing.h"
#include "util/FileUtils.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::mcp {

namespace {

namespace fs = std::filesystem;
using namespace schema;

// one release or runtime step at a time, off the main thread
jobs::JobChannel& shippingChannel() {
    static jobs::JobChannel* channel = new jobs::JobChannel("shipping", 1);
    return *channel;
}

void submit(std::function<void()> work) {
    jobs::SubmitOptions options;
    options.name = "shipping";
    shippingChannel().submit(std::move(work), options);
}

bool loadPublishing(Project* project, publish::PublishingConfig& config, std::string& error) {
    return publish::PublishingConfig::load(publish::PublishingConfig::pathOf(project->getProjectPath()), config, error);
}

// a release name under publishing.json's releasesDir, or an absolute release directory
fs::path releaseDir(Project* project, const publish::PublishingConfig& config, const std::string& release) {
    const fs::path asPath = FileUtils::pathFromUtf8(release);
    std::error_code ec;
    if (asPath.is_absolute()) return asPath;
    const fs::path root = FileUtils::pathFromUtf8(config.releasesDir);
    return (root.is_absolute() ? root : project->getProjectPath() / root) / asPath;
}

Json problemsJson(const std::vector<std::string>& problems) {
    Json list = Json::array();
    for (const std::string& problem : problems) list.push_back(problem);
    return list;
}

std::string firstProblems(const std::vector<std::string>& problems) {
    std::string text;
    for (size_t i = 0; i < problems.size() && i < 8; i++) text += "\n  " + problems[i];
    if (problems.size() > 8) text += "\n  ... " + std::to_string(problems.size() - 8) + " more (structured problems)";
    return text;
}

publish::PublishTools jobTools(const JobPtr& job) {
    publish::PublishTools tools;
    tools.log = [job](const std::string& line) { job->log(line); };
    tools.cancelled = [job]() { return job->cancelRequested(); };
    return tools;
}

// ------------------------------------------------------------------ install_player_runtime

void installPlayerRuntimeTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    const Json& args = call->args();
    const std::string id = args.value("id", "");
    const std::string archive = args.value("archive", "");
    if (id.empty() == archive.empty()) {
        call->fail("Give id (download it from this editor's release) or archive (an absolute .zip path), not both.");
        return;
    }
    if (!archive.empty() && !FileUtils::pathFromUtf8(archive).is_absolute()) {
        call->fail("archive must be an absolute path to a jasperengine-player-<id>-<version>.zip.");
        return;
    }
    JobPtr job = ctx.jobs().create("player-runtime", archive.empty() ? "Download player " + id : "Install player runtime");
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    job->setCancelHandler([cancel]() { cancel->store(true); });
    submit([job, id, archive, cancel]() {
        std::string installed, error;
        std::vector<std::string> warnings;
        const bool ok = archive.empty() ? PlayerRuntimes::download(id, {}, cancel.get(), installed, warnings, error)
                                        : PlayerRuntimes::install(FileUtils::pathFromUtf8(archive), {}, installed, warnings, error);
        Json result = {{"id", installed}, {"warnings", problemsJson(warnings)}};
        if (ok) {
            for (const PlayerInfo& info : PlayerRuntimes::list())
                if (info.id == installed) result["dir"] = FileUtils::pathToGenericUtf8(info.dir);
            job->succeed("Installed player runtime " + installed + ".", result);
        } else if (cancel->load()) {
            job->markCancelled("Download cancelled.");
        } else {
            job->fail(error, result);
        }
    });
    replyWhenJobDone(call, job, args.value("wait", true), args.value("timeout_seconds", 300.0));
}

// ------------------------------------------------------------------ publish_prepare / publish_verify

void publishPrepareTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    publish::PublishingConfig config;
    std::string error;
    if (!loadPublishing(project, config, error)) {
        call->fail(error + " (its targets name each build: docs/shipping.md, or `jasperengine-editor publish init`).");
        return;
    }
    publish::PrepareRequest request;
    request.projectDir = project->getProjectPath();
    request.projectName = project->getName();
    request.config = config;
    request.release = args.value("release", "");
    if (args.contains("targets") && args["targets"].is_array())
        for (const Json& target : args["targets"])
            if (target.is_string()) request.targets.push_back(target.get<std::string>());
    JobPtr job = ctx.jobs().create("publish", "Prepare release " + request.release);
    submit([job, request]() {
        publish::PrepareResult result;
        std::vector<std::string> problems;
        if (publish::prepareRelease(request, jobTools(job), result, problems)) {
            job->succeed("Prepared " + FileUtils::pathToGenericUtf8(result.releaseDir) + ". Check it with publish_verify.",
                         {{"release_dir", FileUtils::pathToGenericUtf8(result.releaseDir)}, {"warnings", problemsJson(result.warnings)}});
        } else {
            job->fail("The release was not prepared:" + firstProblems(problems), {{"problems", problemsJson(problems)}});
        }
    });
    replyWhenJobDone(call, job, args.value("wait", true), args.value("timeout_seconds", 300.0));
}

void publishVerifyTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    publish::PublishingConfig config;
    std::string error;
    loadPublishing(project, config, error);   // a release path works without one
    const fs::path dir = releaseDir(project, config, args.value("release", ""));
    JobPtr job = ctx.jobs().create("publish", "Verify release " + FileUtils::pathToGenericUtf8(dir.filename()));
    submit([job, dir]() {
        std::vector<std::string> problems;
        publish::ReleaseInfo info;
        const bool ok = publish::verifyRelease(dir, problems, &info);
        Json targets = Json::object();
        for (const auto& [name, target] : info.targets) {
            Json item = {{"platform", target.platform}, {"arch", target.arch}, {"player_id", target.playerId}, {"method", target.method},
                         {"files", target.files.size()}};
            if (!target.glibcFloor.empty()) item["glibc_floor"] = target.glibcFloor;
            targets[name] = item;
        }
        const Json result = {{"release", info.release}, {"release_dir", FileUtils::pathToGenericUtf8(dir)}, {"targets", targets},
                             {"problems", problemsJson(problems)}};
        if (ok) job->succeed("Release " + info.release + " verified: unchanged since prepare.", result);
        else job->fail("Release verify failed:" + firstProblems(problems), result);
    });
    replyWhenJobDone(call, job, args.value("wait", true), args.value("timeout_seconds", 300.0));
}

// ------------------------------------------------------------------ publish_steam_vdf

void publishSteamVdfTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    publish::PublishingConfig config;
    std::string error;
    loadPublishing(project, config, error);
    const fs::path dir = releaseDir(project, config, args.value("release", ""));
    fs::path app;
    std::vector<fs::path> depots;
    if (!publish::writeSteamScripts(dir, args.value("preview", true), args.value("branch", ""), app, depots, error)) {
        call->fail(error);
        return;
    }
    Json list = Json::array();
    for (const fs::path& depot : depots) list.push_back(FileUtils::pathToGenericUtf8(depot));
    call->ok("Wrote " + FileUtils::pathToGenericUtf8(app) + " and " + std::to_string(depots.size()) +
                 " depot script(s). A person uploads it: jasperengine-editor publish steam-upload (SteamCMD's own login).",
             {{"app_build", FileUtils::pathToGenericUtf8(app)}, {"depot_builds", list}});
}

// ------------------------------------------------------------------ publish_deploy_deck

void publishDeployDeckTool(const CallPtr& call) {
    McpContext& ctx = call->ctx();
    Project* project = ctx.project();
    const Json& args = call->args();
    publish::PublishingConfig config;
    std::string error;
    if (!loadPublishing(project, config, error)) {
        call->fail(error + " (the Deck's host, user and remoteDir are its deck section).");
        return;
    }
    const fs::path dir = releaseDir(project, config, args.value("release", ""));
    std::string target = args.value("target", "");
    if (target.empty())
        for (const auto& [name, t] : config.targets)
            if (t.deviceProfile == "steamdeck") target = name;
    if (target.empty()) {
        call->fail("target: the release's linux x64 target (publishing.json gives none with deviceProfile steamdeck).");
        return;
    }
    const publish::DeckSettings deck = config.deck;
    const bool smoke = args.value("smoke", false);
    JobPtr job = ctx.jobs().create("publish", "Deploy " + FileUtils::pathToGenericUtf8(dir.filename()) + " to " + deck.host);
    submit([job, dir, target, deck, smoke]() {
        std::string output, error;
        const publish::PublishTools tools = jobTools(job);
        if (!publish::deckDeploy(dir, target, deck, tools, output, error)) {
            job->fail(error, {{"output", output}});
            return;
        }
        Json result = {{"host", deck.host}, {"remote_dir", deck.remoteDir + "/current"}, {"output", output}};
        if (smoke) {
            publish::ReleaseInfo info;
            std::string loadError;
            publish::ReleaseInfo::load(dir, info, loadError);
            auto it = info.targets.find(target);
            int code = -1;
            std::string smokeOutput;
            if (it == info.targets.end() || !publish::deckSmoke(deck, it->second.executable, tools, code, smokeOutput, error)) {
                job->fail("Deployed, but the smoke run did not start: " + error, result);
                return;
            }
            result["smoke_exit_code"] = code;
            result["smoke_log_tail"] = smokeOutput;
            if (code != 0) {
                job->fail("Deployed; the smoke run exited with " + std::to_string(code) + " (structured smoke_log_tail).", result);
                return;
            }
        }
        job->succeed("Deployed to " + deck.host + ":" + deck.remoteDir + "/current (the build before is previous/).", result);
    });
    replyWhenJobDone(call, job, args.value("wait", true), args.value("timeout_seconds", 600.0));
}

} // namespace

void registerPublishTools(McpRegistry& registry) {
    const Json jobTimeout = withDefault(number("", 0, 3600), 300);
    const Json stringItem = Json{{"type", "string"}};
    {
        ToolDef tool;
        tool.name = "install_player_runtime";
        tool.title = "Install Player Runtime";
        tool.description =
            "Install a prebuilt player runtime (get_build_info players[]) as a job: id downloads this editor's release "
            "archive (hash-checked), archive installs a local .zip.";
        tool.inputSchema = object({
            {"id", string("<platform>-<arch>-<backend>-<flavour>.")},
            {"archive", string("Absolute .zip path.")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", jobTimeout}
        });
        tool.annotations.openWorld = true;
        tool.annotations.idempotent = true;
        tool.handler = installPlayerRuntimeTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "publish_prepare";
        tool.title = "Prepare Release";
        tool.description =
            "publishing.json targets -> <releasesDir>/<release>: validates each build, copies it, splits debug symbols "
            "into symbols/, writes release.json. Refuses an existing name.";
        tool.inputSchema = object({
            {"release", string("Name, e.g. 1.0.0.")},
            {"targets", array(stringItem, "Default: all.")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", jobTimeout}
        }, {"release"});
        tool.handler = publishPrepareTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "publish_verify";
        tool.title = "Verify Release";
        tool.description = "Re-checks a prepared release: payload rules and every file against release.json. Lists problems.";
        tool.inputSchema = object({
            {"release", string("Name or absolute dir.")},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", jobTimeout}
        }, {"release"});
        tool.annotations.readOnly = true;
        tool.annotations.idempotent = true;
        tool.handler = publishVerifyTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "publish_steam_vdf";
        tool.title = "Write SteamPipe Scripts";
        tool.description = "Writes a verified release's steampipe/app_build_<app>.vdf and depot scripts. Never uploads.";
        tool.inputSchema = object({
            {"release", string("Name or absolute dir.")},
            {"preview", withDefault(boolean("Preview build."), true)},
            {"branch", string("Beta branch to SetLive (not default/public).")}
        }, {"release"});
        tool.annotations.idempotent = true;
        tool.handler = publishSteamVdfTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "publish_deploy_deck";
        tool.title = "Deploy to Steam Deck";
        tool.description =
            "Copies a verified release's linux x64 target to publishing.json's deck over ssh/scp, checks it there, "
            "swaps it in (keeps one previous). smoke: run it 600 frames.";
        tool.inputSchema = object({
            {"release", string("Name or absolute dir.")},
            {"target", string("Default: the steamdeck target.")},
            {"smoke", withDefault(boolean(""), false)},
            {"wait", withDefault(boolean(""), true)},
            {"timeout_seconds", withDefault(number("", 0, 3600), 600)}
        }, {"release"});
        tool.annotations.openWorld = true;
        tool.handler = publishDeployDeckTool;
        registry.addTool(std::move(tool));
    }
}

}
