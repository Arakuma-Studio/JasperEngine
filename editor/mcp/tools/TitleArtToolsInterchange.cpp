// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// export_title's delivery formats (docs/specs/S87-title-localisation-interchange.md R35, R40,
// §6.3): the call's `languages`, `options` and `out` checked, then TitleExport's job run once per
// editor frame (an McpJobs job with done/total renders), the result listing files, frames per
// language and warnings.

#include "TitleArtToolsShared.h"

#include "Project.h"
#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "titleart/TitleArtBaker.h"
#include "titleart/interchange/TitleExport.h"
#include "util/FileUtils.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::mcp;
using namespace jasperengine::editor::mcp::titletools;
namespace fs = std::filesystem;

void titletools::exportTitleInterchange(const CallPtr& call, Project* project, const fs::path& file, const std::string& relative,
                                        const std::string& format) {
    const Json& args = call->args();
    TitleExportRequest request;
    request.project = project;
    request.document = file;
    request.relative = relative;
    request.format = format;
    std::string code, error;
    if (!loadTitleArtForBake(project, file, request.doc, code, error)) {
        call->fail(code + ": " + error + ". Nothing was written.", Json{{"code", code}});
        return;
    }
    request.options = args.contains("options") ? toOrdered(args["options"]) : titleart::Json::object();
    if (args.contains("languages") && !titleExportLanguages(request.doc, toOrdered(args["languages"]), request.languages, error)) {
        call->fail("bad_value: " + error + ". Nothing was written.", Json{{"code", "bad_value"}});
        return;
    }
    if (args.contains("out") && args["out"].is_string()) {
        const fs::path out = FileUtils::pathFromUtf8(args["out"].get<std::string>());
        request.out = (out.is_absolute() ? out : project->getProjectPath() / out).lexically_normal();
    }
    // several documents (strings formats): `path` as a list
    if (args.contains("path") && args["path"].is_array()) {
        for (size_t i = 1; i < args["path"].size(); i++) {
            const Json& p = args["path"][i];
            fs::path other;
            std::string otherRelative;
            titleart::TitleArtDoc doc;
            if (!p.is_string() || !resolveTitlePath(project, p.get<std::string>(), true, other, otherRelative, error) ||
                !loadTitleArtForBake(project, other, doc, code, error)) {
                call->fail("bad_value: path[" + std::to_string(i) + "]: " + error + ". Nothing was written.", Json{{"code", "bad_value"}});
                return;
            }
            request.more.emplace_back(other, std::move(doc));
        }
    }
    std::shared_ptr<TitleExportJob> job = startTitleExport(std::move(request), code, error);
    if (!job) {
        call->fail(code + ": " + error + ". Nothing was written.", Json{{"code", code}});
        return;
    }
    auto task = std::make_shared<FrameTask>();
    std::weak_ptr<FrameTask> weakTask = task;
    task->tick = [job, relative, format, weakTask](McpJob& mcpJob) {
        auto t = weakTask.lock();
        if (!t) return true;
        const bool done = job->step(8.0);
        mcpJob.setProgress(job->progress(), job->progressText());
        if (!done) return false;
        const TitleExportResult& result = job->result();
        Json out = fromOrdered(result.toJson());
        out["path"] = relative;
        out["format"] = format;
        if (!result.ok) {
            mcpJob.fail(result.code + ": " + result.error, out);
            return true;
        }
        t->text = "Exported " + relative + " as " + format + ": " + std::to_string(result.files.size()) + " file(s)";
        if (!result.files.empty()) t->text += " (" + result.files.front() + (result.files.size() > 1 ? ", ..." : "") + ")";
        if (!result.warnings.empty()) t->text += "; " + std::to_string(result.warnings.size()) + " warning(s)";
        t->text += ".";
        mcpJob.succeed(t->text, out);
        return true;
    };
    runFrameTask(call, "export_title", "Export " + relative + " (" + format + ")", task);
}
