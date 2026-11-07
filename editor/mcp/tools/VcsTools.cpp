// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S29: version control (group "vcs", docs/specs/S29-version-control.md R33, docs/mcp/guide/vcs.md)
//
//   get_vcs_status       provider, branch, ahead/behind, the changed files (refresh: a poll now, with the locks)
//   diff_file            a unified text diff of a file against a revision
//   get_vcs_log          the history of the project or of a file
//   diff_scene           the semantic diff of a scene or prefab (its unsaved edits, or the file)
//   revert_to_baseline   one GameObject, component or property of it back to the baseline (undoable)
//   lock_files           LFS / svn locks; unlock_files releases them (force on another user's: confirm)
//   commit_files         exactly the given changed paths (their .meta along), optional push
//   revert_files         files back to the baseline, undoable ("Revert N files"); confirm: true
//   sync_remote          fetch, pull (fast-forward only) or push: a job; pull and push need confirm
//
// Every path is project-relative, inside the project, without a NUL; a revision is a hash, HEAD
// or HEAD~n (git), a number, BASE or HEAD (svn, where HEAD reads BASE); omitted, the baseline
// (git HEAD, svn BASE). No parameter ever becomes a flag of the tool.
#include "McpTools.h"

#include "mcp/McpCall.h"
#include "mcp/McpContext.h"
#include "mcp/McpJobs.h"
#include "mcp/McpRefs.h"
#include "mcp/McpRegistry.h"
#include "mcp/McpSchema.h"
#include "mcp/McpServer.h"
#include "mcp/McpValues.h"

#include "App.h"
#include "Backend.h"
#include "Project.h"
#include "command/UndoOrigin.h"
#include "command/type/RevertToBaselineCmd.h"
#include "util/FileUtils.h"
#include "util/LineDiff.h"
#include "vcs/SceneDiffTracker.h"
#include "window/CodeEditor.h"
#include "vcs/VcsEditorOps.h"
#include "vcs/scene/SceneDiffJson.h"

#include <algorithm>
#include <optional>

namespace jasperengine::editor::mcp {

using namespace schema;

namespace {

namespace fs = std::filesystem;
using vcs::RepoPath;
using vcs::VcsResult;
using vcs::VcsService;

const char* GROUP = "vcs";
constexpr size_t kMaxEntries = 500;

// the service, or the call fails saying why there is none
VcsService* activeVcs(const CallPtr& call) {
    Project* project = call->ctx().project();
    if (!project) {
        call->fail("No project is open.");
        return nullptr;
    }
    VcsService& service = project->getVcs();
    if (!service.active()) {
        call->fail("Version control is not active (" + std::string(vcs::vcsStateName(service.state())) + "): " + service.stateMessage());
        return nullptr;
    }
    return &service;
}

// the project-relative path of a repository key ("" when it is not in the project)
std::string projectPathOf(const VcsService& service, const RepoPath& key) {
    const RepoPath& prefix = service.projectKey();
    if (prefix.empty()) return key;
    if (!vcs::pathKeyIsWithin(key, prefix) || key.size() == prefix.size()) return std::string();
    return key.substr(prefix.size() + 1);
}

// paths (strings, project-relative, inside the project, no NUL) to absolute paths and keys
bool resolvePaths(const CallPtr& call, const VcsService& service, const Json& value, std::vector<fs::path>& absolute,
                  std::vector<RepoPath>& keys, bool required) {
    if (value.is_null()) {
        if (required) call->fail("paths is required: a non-empty list of project-relative paths.");
        return !required;
    }
    if (!value.is_array() || (required && value.empty())) {
        call->fail("paths must be a non-empty list of project-relative paths.");
        return false;
    }
    for (const Json& item : value) {
        if (!item.is_string()) {
            call->fail("paths holds a value that is not a string.");
            return false;
        }
        const std::string text = item.get<std::string>();
        if (text.find('\0') != std::string::npos) {
            call->fail("A path holds a NUL character.");
            return false;
        }
        fs::path path;
        std::string error;
        if (!McpRefs::resolveProjectPath(call->ctx().project(), text, path, error)) {
            call->fail(error);
            return false;
        }
        RepoPath key;
        if (!service.toKey(path, key) || key.empty()) {
            call->fail("\"" + text + "\" is not in the repository.");
            return false;
        }
        absolute.push_back(path);
        keys.push_back(key);
    }
    return true;
}

bool validRevision(const CallPtr& call, const VcsService& service, const std::string& rev) {
    std::shared_ptr<vcs::IVcsProvider> provider = service.providerForReads();
    if (provider && provider->validRevision(rev)) return true;
    call->fail("\"" + rev + "\" is not a revision: " +
               (service.caps().provider == "svn" ? std::string("a number, BASE or HEAD") : std::string("a 7-64 digit hash, HEAD or HEAD~n")) + ".");
    return false;
}

// rev as given, the baseline when omitted (git HEAD, svn BASE); false (and the call failed) when it is not one
bool readRevision(const CallPtr& call, const VcsService& service, std::string& rev) {
    rev = call->args().value("rev", std::string());
    if (rev.empty()) rev = service.baselineRevision();
    return validRevision(call, service, rev);
}

Json changeJson(const VcsService& service, const vcs::VcsChange& change) {
    Json entry = {{"path", projectPathOf(service, change.path)},
                  {"index", vcs::fileStateName(change.index)},
                  {"worktree", vcs::fileStateName(change.worktree)}};
    if (!change.oldPath.empty()) entry["old_path"] = projectPathOf(service, change.oldPath);
    if (change.conflict) entry["conflict"] = true;
    if (change.lock != vcs::LockState::Unsupported) entry["lock"] = vcs::lockStateName(change.lock);
    if (!change.lockOwner.empty()) entry["lock_owner"] = change.lockOwner;
    if (change.remoteChanged) entry["remote_changed"] = true;
    return entry;
}

// "Version control: ..." once, whatever the state's message starts with
std::string stateLine(const VcsService& service) {
    const std::string& message = service.stateMessage();
    return message.rfind("Version control", 0) == 0 ? message : "Version control: " + message;
}

// the snapshot's summary (get_vcs_status, jasper://vcs/status)
Json summaryJson(const VcsService& service) {
    Json out = {{"state", vcs::vcsStateName(service.state())}, {"message", service.stateMessage()}};
    if (!service.active()) return out;
    out["provider"] = service.caps().provider;
    out["locking"] = service.caps().locking;
    if (!service.lastError().empty()) out["last_error"] = service.lastError();
    const vcs::SnapshotPtr& snapshot = service.snapshot();
    if (!snapshot) {
        out["pending"] = true;
        return out;
    }
    const vcs::VcsBranchInfo& branch = snapshot->branch;
    out["branch"] = {{"name", branch.name}, {"head", branch.headOid}, {"upstream", branch.upstream},
                     {"ahead", branch.ahead}, {"behind", branch.behind}, {"detached", branch.detached},
                     {"label", vcs::headLabel(branch)}};
    if (!branch.tag.empty()) out["branch"]["tag"] = branch.tag;
    // a merge, rebase, cherry-pick, revert or bisect left unfinished
    if (branch.operation != vcs::VcsOperation::None) {
        out["branch"]["operation"] = vcs::vcsOperationName(branch.operation);
        if (branch.operationTotal > 0) {
            out["branch"]["operation_step"] = branch.operationStep;
            out["branch"]["operation_total"] = branch.operationTotal;
        }
    }
    if (!snapshot->lockError.empty()) out["lock_error"] = snapshot->lockError;
    if (snapshot->mergeDriverMissing) out["smart_merge"] = "not set up";
    out["changed"] = snapshot->changedCount();
    size_t staged = 0, modified = 0, untracked = 0, conflicted = 0;
    for (const vcs::VcsChange& change : snapshot->entries) {
        if (change.conflict) conflicted++;
        else if (change.index == vcs::FileState::Untracked) untracked++;
        else {
            if (change.index != vcs::FileState::Unmodified) staged++;
            if (change.worktree != vcs::FileState::Unmodified) modified++;
        }
    }
    out["counts"] = {{"staged", staged}, {"modified", modified}, {"untracked", untracked}, {"conflicted", conflicted}};
    return out;
}

void replyStatus(const CallPtr& call, const VcsService& service, const Json& paths, bool includeClean) {
    Json out = summaryJson(service);
    Json entries = Json::array();
    size_t total = 0;
    std::vector<RepoPath> filters;
    if (paths.is_array()) {
        for (const Json& item : paths) {
            fs::path path;
            std::string error;
            RepoPath key;
            if (item.is_string() && McpRefs::resolveProjectPath(call->ctx().project(), item.get<std::string>(), path, error) &&
                service.toKey(path, key)) {
                filters.push_back(key);
            }
        }
    }
    std::string text;
    if (const vcs::SnapshotPtr& snapshot = service.snapshot()) {
        for (const vcs::VcsChange& change : snapshot->entries) {
            if (!includeClean && change.clean()) continue;
            if (projectPathOf(service, change.path).empty()) continue;
            if (!filters.empty() && std::none_of(filters.begin(), filters.end(),
                                                 [&](const RepoPath& filter) { return vcs::pathKeyIsWithin(change.path, filter); })) continue;
            total++;
            if (entries.size() >= kMaxEntries) continue;
            entries.push_back(changeJson(service, change));
            if (entries.size() <= 20) {
                text += "\n  " + std::string(vcs::fileStateName(change.worktree != vcs::FileState::Unmodified ? change.worktree : change.index)) +
                        (change.conflict ? " conflict" : "") + "  " + projectPathOf(service, change.path) +
                        (change.lock == vcs::LockState::LockedByOther ? "  (locked by " + change.lockOwner + ")" :
                         change.lock == vcs::LockState::LockedByMe ? "  (locked by you)" : "");
            }
        }
    }
    out["entries"] = entries;
    out["total"] = total;
    out["truncated"] = total > entries.size();
    const std::string branch = out.contains("branch") ? out["branch"]["label"].get<std::string>() : std::string();
    call->ok(service.caps().provider + (branch.empty() ? std::string() : " " + branch) + ": " + std::to_string(total) +
             (total == 1 ? " entry" : " entries") + (total > 20 ? " (first 20 shown)" : "") + text, out);
}

// a continuation of an attributed call is the agent's too (its undo steps)
void attribute(const CallPtr& call, std::optional<UndoOriginScope>& scope) {
    if (call->attributed()) scope.emplace(call->getUndoOrigin());
}

// ------------------------------------------------------------------ get_vcs_status

void getVcsStatusTool(const CallPtr& call) {
    Project* project = call->ctx().project();
    if (!project) {
        call->fail("No project is open.");
        return;
    }
    VcsService& service = project->getVcs();
    const Json& args = call->args();
    const Json paths = args.value("paths", Json());
    const bool includeClean = args.value("include_clean", false);
    const bool refresh = args.value("refresh", false);
    const bool retry = service.state() == vcs::VcsState::NoRepository || service.state() == vcs::VcsState::NotAvailable;
    if (!service.active() && !(refresh && retry) && service.state() != vcs::VcsState::Detecting) {
        call->ok(stateLine(service), summaryJson(service));
        return;
    }
    if (!refresh && service.snapshot()) {
        replyStatus(call, service, paths, includeClean);
        return;
    }
    call->detach();
    service.refresh(true, [call, &service, paths, includeClean](const VcsResult& result) {
        if (!service.active()) {
            call->ok(stateLine(service), summaryJson(service));
            return;
        }
        if (!result.ok) {
            call->fail("The status poll failed: " + result.message);
            return;
        }
        replyStatus(call, service, paths, includeClean);
    });
}

// ------------------------------------------------------------------ diff_file

void diffFileTool(const CallPtr& call) {
    VcsService* service = activeVcs(call);
    if (!service) return;
    const Json& args = call->args();
    std::vector<fs::path> absolute;
    std::vector<RepoPath> keys;
    if (!resolvePaths(call, *service, Json::array({args.value("path", std::string())}), absolute, keys, true)) return;
    std::string rev;
    if (!readRevision(call, *service, rev)) return;
    // what the read compares with (svn's HEAD is its BASE)
    rev = service->effectiveRevision(rev);
    const size_t maxBytes = static_cast<size_t>(std::clamp(args.value("max_bytes", 65536), 1024, 1 << 20));
    const std::string path = args.value("path", std::string());
    const std::string source = args.value("source", std::string("editor"));
    if (source != "editor" && source != "disk") {
        call->fail("source is editor or disk.");
        return;
    }
    auto reply = [call, path, rev, maxBytes](std::string diff, const std::string& answered, bool unsaved) {
        const bool truncated = diff.size() > maxBytes;
        if (truncated) diff.resize(maxBytes);
        const std::string what = path + (unsaved ? " (the code editor's unsaved buffer)" : std::string());
        const std::string summary = diff.empty() ? what + " has no changes against " + rev + "." : what + " against " + rev + ":\n" + diff;
        call->ok(summary, Json{{"path", path}, {"rev", rev}, {"source", answered}, {"unsaved", unsaved}, {"diff", diff}, {"truncated", truncated}});
    };
    // the code editor's buffer with unsaved edits: diffed here, against the revision's text
    CodeEditor* editor = call->ctx().codeEditor();
    std::optional<std::string> buffer;
    if (source == "editor" && editor && editor->isFileModified(absolute[0].string())) buffer = editor->bufferText(absolute[0].string());
    call->detach();
    if (buffer) {
        const RepoPath key = keys[0];
        // what a save would write (the file's line endings), against the file the revision has:
        // a staged rename's source, none for a file HEAD has not got
        std::string text = FileUtils::withLineEndings(absolute[0], *buffer);
        const vcs::SnapshotPtr& snapshot = service->snapshot();
        const vcs::VcsChange* change = snapshot ? snapshot->find(key) : nullptr;
        if (rev == service->baselineRevision() && change && change->newFile()) {
            reply(LineDiff::unified({}, text, key, key), "editor", true);
            return;
        }
        const RepoPath source = change && !change->oldPath.empty() ? change->oldPath : key;
        service->baseContent(source, rev, [call, path, rev, reply, source, key, text = std::move(text)](const VcsResult& result, std::string base) {
            if (!result.ok) {
                call->fail("diff_file " + path + ": " + source + " at " + rev + " did not read: " + result.message);
                return;
            }
            reply(LineDiff::unified(base, text, source, key), "editor", true);
        });
        return;
    }
    service->textDiff(keys[0], rev, [call, path, reply](const VcsResult& result, std::string diff) {
        if (!result.ok) {
            call->fail("diff_file " + path + ": " + result.message);
            return;
        }
        reply(std::move(diff), "disk", false);
    });
}

// ------------------------------------------------------------------ get_vcs_log

void getVcsLogTool(const CallPtr& call) {
    VcsService* service = activeVcs(call);
    if (!service) return;
    const Json& args = call->args();
    std::optional<RepoPath> key;
    if (args.contains("path") && !args["path"].is_null()) {
        std::vector<fs::path> absolute;
        std::vector<RepoPath> keys;
        if (!resolvePaths(call, *service, Json::array({args["path"]}), absolute, keys, true)) return;
        key = keys[0];
    }
    const int limit = std::clamp(args.value("limit", 50), 1, 200);
    const int skip = std::max(0, args.value("skip", 0));
    call->detach();
    service->log(key, limit, skip, [call](const VcsResult& result, std::vector<vcs::VcsLogEntry> entries) {
        if (!result.ok) {
            call->fail("get_vcs_log: " + result.message);
            return;
        }
        Json list = Json::array();
        std::string text;
        for (const vcs::VcsLogEntry& entry : entries) {
            list.push_back({{"revision", entry.revision}, {"author", entry.author}, {"date", entry.date}, {"message", entry.message}});
            if (list.size() <= 20) text += "\n  " + entry.revision.substr(0, 10) + "  " + entry.date + "  " + entry.author + "  " + entry.message;
        }
        call->ok(std::to_string(entries.size()) + " revision(s)" + text, Json{{"entries", list}});
    });
}

// ------------------------------------------------------------------ diff_scene / revert_to_baseline

struct SceneTarget {
    uint32_t sceneId = 0;            // an open document
    fs::path file;                   // or a file
    std::string label;
};

bool resolveSceneTarget(const CallPtr& call, Project* project, bool mustBeOpen, SceneTarget& out) {
    const Json ref = call->args().value("scene", Json());
    // a .scene or .prefab file not in the project's scene list
    if (ref.is_string()) {
        const std::string text = ref.get<std::string>();
        const fs::path extension = FileUtils::pathFromUtf8(text).extension();
        if (extension == ".prefab" || extension == ".scene") {
            fs::path absolute;
            std::string error;
            if (!McpRefs::resolveProjectPath(project, text, absolute, error, true)) {
                call->fail(error);
                return false;
            }
            for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
                fs::path file = sceneProject->filepath;
                if (const PrefabStage* stage = project->getPrefabStage(sceneProject->id)) file = stage->assetPath;
                if (!file.empty() && (file.is_relative() ? project->getProjectPath() / file : file).lexically_normal() == absolute.lexically_normal() &&
                    sceneProject->scene) {
                    out.sceneId = sceneProject->id;
                }
            }
            out.file = absolute;
            out.label = text;
            if (mustBeOpen && out.sceneId == 0) {
                call->fail(text + " is not open: open it (open_scene, or Prefab Mode for a prefab) to revert in it.");
                return false;
            }
            return true;
        }
    }
    std::string error;
    SceneProject* sceneProject = McpRefs::resolveScene(project, ref, error);
    if (!sceneProject) {
        call->fail(error);
        return false;
    }
    out.label = sceneProject->name;
    if (sceneProject->scene) {
        out.sceneId = sceneProject->id;
    } else if (mustBeOpen) {
        call->fail(McpRefs::notOpenError(project, *sceneProject));
        return false;
    }
    if (!sceneProject->filepath.empty()) {
        out.file = sceneProject->filepath.is_relative() ? project->getProjectPath() / sceneProject->filepath : sceneProject->filepath;
    }
    if (out.file.empty()) {
        call->fail(sceneProject->name + " has never been saved: there is no baseline to diff it against.");
        return false;
    }
    return true;
}

void diffSceneTool(const CallPtr& call) {
    VcsService* service = activeVcs(call);
    if (!service) return;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    SceneTarget target;
    if (!resolveSceneTarget(call, project, false, target)) return;
    std::string rev;
    if (!readRevision(call, *service, rev)) return;
    rev = service->effectiveRevision(rev);
    const std::string source = args.value("source", std::string("editor"));
    vcs::SceneDiffTracker::Request request;
    request.sceneId = source == "editor" ? target.sceneId : 0;
    request.file = target.file;
    request.rev = rev;
    request.fromEditor = source == "editor";
    request.changedOnly = args.value("changed_only", true);
    const std::string label = target.label;
    call->detach();
    project->getSceneDiffs().diffNow(*project, request,
        [call, label, rev, source](const std::string& error, std::shared_ptr<const vcs::scene::SceneYamlModel>,
                                   std::shared_ptr<const vcs::scene::SceneDiffResult> diff) {
            if (!diff) {
                call->fail("diff_scene " + label + ": " + error);
                return;
            }
            Json out = vcs::scene::sceneDiffJson(*diff);
            out["scene"] = label;
            out["rev"] = rev;
            out["source"] = source;
            call->ok(label + " against " + rev + " (" + source + "):\n" + vcs::scene::sceneDiffText(*diff), out);
        });
}

void revertToBaselineTool(const CallPtr& call) {
    VcsService* service = activeVcs(call);
    if (!service) return;
    Project* project = call->ctx().project();
    const Json& args = call->args();
    SceneTarget target;
    if (!resolveSceneTarget(call, project, true, target)) return;
    SceneProject* sceneProject = nullptr;
    for (SceneProject* candidate : project->getScenesAndPrefabStages()) {
        if (candidate->id == target.sceneId) sceneProject = candidate;
    }
    if (!sceneProject) {
        call->fail(target.label + " is not open.");
        return;
    }
    // the GameObject (or "settings": Scene Settings)
    uint64_t objectId = 0;
    const Json entityRef = args.value("entity", Json());
    if (entityRef.is_string() && entityRef.get<std::string>() == "settings") {
        objectId = vcs::scene::kSceneSettingsId;
    } else {
        std::string error;
        const Entity entity = McpRefs::resolveEntity(sceneProject, entityRef, error);
        if (entity == NULL_ENTITY) {
            // a GameObject the baseline has and the scene does not: by its id
            if (!entityRef.is_number_integer()) {
                call->fail(error + " (a GameObject the scene lost goes by its id from diff_scene)");
                return;
            }
            objectId = entityRef.get<uint64_t>();
        } else {
            objectId = entity;
        }
    }
    std::string component = args.value("component", std::string());
    std::string property = args.value("property", std::string());
    // property as diff_scene writes it ("transform.position"): its component first
    if (!property.empty() && component.empty()) {
        const size_t split = property.find_first_of(".[");
        component = property.substr(0, split);
        property = split == std::string::npos ? std::string() : property.substr(property[split] == '.' ? split + 1 : split);
    } else if (!property.empty() && property.rfind(component + ".", 0) == 0) {
        property = property.substr(component.size() + 1);
    }
    vcs::SceneDiffTracker::Request request;
    request.sceneId = target.sceneId;
    request.file = target.file;
    request.fromEditor = true;
    const uint32_t sceneId = target.sceneId;
    call->detach();
    project->getSceneDiffs().diffNow(*project, request,
        [call, project, sceneId, objectId, component, property](const std::string& error, std::shared_ptr<const vcs::scene::SceneYamlModel> baseline,
                                                                 std::shared_ptr<const vcs::scene::SceneDiffResult> diff) {
            if (!diff || !baseline) {
                call->fail("revert_to_baseline: " + error);
                return;
            }
            const vcs::scene::ObjectDiff* object = diff->find(objectId);
            if (!object) {
                call->fail("GameObject " + std::to_string(objectId) + " has no changes against the baseline (diff_scene lists the ones that do).");
                return;
            }
            const vcs::scene::ComponentDiff* componentDiff = component.empty() ? nullptr : object->component(component);
            if (!component.empty() && !componentDiff) {
                call->fail(object->name + " has no change in " + component + ".");
                return;
            }
            const vcs::scene::PropertyDiff* propertyDiff = nullptr;
            if (!property.empty()) {
                for (const vcs::scene::PropertyDiff& candidate : componentDiff->properties) {
                    if (candidate.path == property) propertyDiff = &candidate;
                }
                if (!propertyDiff) {
                    call->fail(object->name + " has no change at " + component + "." + property + ".");
                    return;
                }
            }
            std::string why;
            RevertToBaselineCmd* command = RevertToBaselineCmd::build(project, sceneId, *baseline, *object, componentDiff, propertyDiff, &why);
            if (!command) {
                call->fail("Cannot revert: " + why);
                return;
            }
            const std::string name = command->describe();
            std::string runError;
            std::optional<UndoOriginScope> scope;
            attribute(call, scope);
            if (!runSceneCommand(sceneId, command, runError, "the revert did not apply")) {
                call->fail(runError);
                return;
            }
            call->ok(name + " (undo restores it).", Json{{"undo", name}});
        });
}

// ------------------------------------------------------------------ locks

void lockFilesTool(const CallPtr& call) {
    VcsService* service = activeVcs(call);
    if (!service) return;
    if (!service->caps().locking) {
        call->fail("Locking needs Git LFS 3.0+ (or Subversion): " + service->stateMessage());
        return;
    }
    std::vector<fs::path> absolute;
    std::vector<RepoPath> keys;
    if (!resolvePaths(call, *service, call->args().value("paths", Json()), absolute, keys, true)) return;
    call->detach();
    service->lock(keys, [call, count = keys.size()](const VcsResult& result) {
        if (!result.ok) {
            call->fail("lock_files: " + result.message);
            return;
        }
        call->ok("Locked " + std::to_string(count) + (count == 1 ? " file." : " files."), Json{{"locked", count}});
    });
}

void unlockFilesTool(const CallPtr& call) {
    VcsService* service = activeVcs(call);
    if (!service) return;
    std::vector<fs::path> absolute;
    std::vector<RepoPath> keys;
    const Json& args = call->args();
    if (!resolvePaths(call, *service, args.value("paths", Json()), absolute, keys, true)) return;
    const bool force = args.value("force", false);
    if (force && !args.value("confirm", false) && service->snapshot()) {
        for (const RepoPath& key : keys) {
            const vcs::VcsChange* change = service->snapshot()->find(key);
            if (change && change->lock == vcs::LockState::LockedByOther) {
                call->fail(projectPathOf(*service, key) + " is locked by " + change->lockOwner +
                           ": breaking another user's lock needs confirm: true (tell them first).");
                return;
            }
        }
    }
    call->detach();
    service->unlock(keys, force, [call, count = keys.size()](const VcsResult& result) {
        if (!result.ok) {
            call->fail("unlock_files: " + result.message);
            return;
        }
        call->ok("Unlocked " + std::to_string(count) + (count == 1 ? " file." : " files."), Json{{"unlocked", count}});
    });
}

// ------------------------------------------------------------------ commit_files

void commitFilesTool(const CallPtr& call) {
    VcsService* service = activeVcs(call);
    if (!service) return;
    const Json& args = call->args();
    std::vector<fs::path> absolute;
    std::vector<RepoPath> keys;
    if (!resolvePaths(call, *service, args.value("paths", Json()), absolute, keys, true)) return;
    const std::string message = args.value("message", std::string());
    if (message.find_first_not_of(" \t\r\n") == std::string::npos) {
        call->fail("message is required and must not be blank.");
        return;
    }
    const bool push = args.value("push", false);
    call->detach();
    // the paths are checked against a fresh status: only files that changed are committed
    service->refresh(false, [call, service, keys, message, push](const VcsResult& polled) {
        if (!polled.ok || !service->snapshot()) {
            call->fail("The status poll failed: " + polled.message);
            return;
        }
        const vcs::VcsSnapshot& snapshot = *service->snapshot();
        for (const RepoPath& key : keys) {
            const vcs::VcsChange* change = snapshot.find(key);
            if (!change || !change->changed()) {
                call->fail(projectPathOf(*service, key) + " has no changes: commit_files commits only paths you changed (get_vcs_status lists them).");
                return;
            }
        }
        const std::vector<RepoPath> paths = vcs::withChangedMetas(snapshot, keys);
        service->commit(paths, message, push, [call, service, paths](const VcsResult& result, const std::string& revision) {
            if (!result.ok) {
                call->fail("commit_files: " + result.message, revision.empty() ? Json(nullptr) : Json{{"revision", revision}});
                return;
            }
            Json files = Json::array();
            for (const RepoPath& path : paths) files.push_back(projectPathOf(*service, path));
            call->ok("Committed " + std::to_string(paths.size()) + (paths.size() == 1 ? " file" : " files") + " as " + revision + ".",
                     Json{{"revision", revision}, {"files", files}});
        });
    });
}

// ------------------------------------------------------------------ revert_files

void revertFilesTool(const CallPtr& call) {
    VcsService* service = activeVcs(call);
    if (!service) return;
    const Json& args = call->args();
    std::vector<fs::path> absolute;
    std::vector<RepoPath> keys;
    if (!resolvePaths(call, *service, args.value("paths", Json()), absolute, keys, true)) return;
    if (!args.value("confirm", false)) {
        call->fail("revert_files discards the changes of those files (undoable): pass confirm: true.");
        return;
    }
    Project* project = call->ctx().project();
    call->detach();
    vcs::revertFiles(*project, absolute, [call](const VcsResult& result, const std::string& undoName) {
        if (!result.ok) {
            call->fail("revert_files: " + result.message);
            return;
        }
        call->ok(undoName + " (undo restores the files).", Json{{"undo", undoName}});
    });
}

// ------------------------------------------------------------------ fetch / pull / push

void remoteTool(const CallPtr& call, const char* kind, const char* title, bool needsConfirm,
                void (VcsService::*operation)(VcsService::Done)) {
    VcsService* service = activeVcs(call);
    if (!service) return;
    const Json& args = call->args();
    const bool supported = std::string(kind) == "fetch" ? service->caps().fetch : std::string(kind) == "pull" ? service->caps().pull : service->caps().push;
    if (!supported) {
        call->fail(std::string(title) + " is not something " + service->caps().provider + " does.");
        return;
    }
    if (needsConfirm && !args.value("confirm", false)) {
        call->fail(std::string(title) + (std::string(kind) == "pull" ? " changes the working files (fast-forward only)" : " publishes commits") +
                   ": pass confirm: true.");
        return;
    }
    JobPtr job = call->ctx().jobs().create(std::string("vcs_") + kind, title);
    job->setCancelHandler([service]() { service->cancelRunning(); });
    (service->*operation)([job, title](const VcsResult& result) {
        if (result.ok) job->succeed(std::string(title) + " done." + (result.message.empty() ? std::string() : "\n" + result.message));
        else if (job->cancelRequested()) job->markCancelled();
        else job->fail(std::string(title) + " failed: " + result.message);
    });
    replyWhenJobDone(call, job, args.value("wait", true), args.value("timeout_seconds", 120.0));
}

// sync_remote: fetch, pull or push by its action
void syncRemoteTool(const CallPtr& call) {
    const std::string action = call->args().value("action", std::string());
    if (action == "fetch") remoteTool(call, "fetch", "Fetch", false, &VcsService::fetch);
    else if (action == "pull") remoteTool(call, "pull", "Pull", true, &VcsService::pull);
    else if (action == "push") remoteTool(call, "push", "Push", true, &VcsService::push);
    else call->fail("action must be fetch, pull or push.");
}

// ------------------------------------------------------------------ registration

Json pathsSchema(const std::string& description) {
    return array(string(""), description);
}

}

void registerVcsTools(McpRegistry& registry) {
    {
        ToolDef tool;
        tool.name = "get_vcs_status";
        tool.title = "Get VCS Status";
        tool.group = GROUP;
        tool.description =
            "Version control state: provider (git/svn), branch (tag, unfinished merge/rebase), ahead/behind, counts, last error, and the project's changed files "
            "{path, index, worktree, conflict?, lock?, lock_owner?, remote_changed?} (500 max). refresh polls now, with the "
            "locks (network). jasper://vcs/status holds the summary; jasper://docs/vcs the workflow.";
        tool.inputSchema = object({
            {"paths", pathsSchema("Only these files or folders (project-relative).")},
            {"include_clean", withDefault(boolean("Lockable and locked files without changes too."), false)},
            {"refresh", withDefault(boolean(""), false)}
        });
        tool.annotations.readOnly = true;
        tool.handler = getVcsStatusTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "diff_file";
        tool.title = "Diff File";
        tool.group = GROUP;
        tool.description = "A unified text diff of a project file against a revision: the code editor's unsaved buffer when it has one "
                           "(source editor), else the file on disk. Scenes and prefabs read better with diff_scene.";
        tool.inputSchema = object({
            {"path", string("Project-relative.")},
            {"rev", string("Omitted: git HEAD, svn BASE (svn's HEAD reads BASE too). Else a git hash or HEAD~n, an svn number.")},
            {"source", withDefault(stringEnum("", {"editor", "disk"}), "editor")},
            {"max_bytes", withDefault(integer("", 1024, 1048576), 65536)}
        }, {"path"});
        tool.annotations.readOnly = true;
        tool.handler = diffFileTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "get_vcs_log";
        tool.title = "Get VCS Log";
        tool.group = GROUP;
        tool.description = "The history, newest first: {revision, author, date, message}, of the project or one file.";
        tool.inputSchema = object({
            {"path", string("A project-relative file; omitted: the project.")},
            {"limit", withDefault(integer("", 1, 200), 50)},
            {"skip", withDefault(integer("Entries passed over (paging).", 0, 100000), 0)}
        });
        tool.annotations.readOnly = true;
        tool.handler = getVcsLogTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "diff_scene";
        tool.title = "Diff Scene";
        tool.group = GROUP;
        tool.description =
            "Semantic diff of a scene or prefab against a revision: GameObjects added/removed/modified/reparented/reordered, "
            "components, properties {path, base, current}. source editor includes unsaved edits, disk the saved file. Run it "
            "before commit_files.";
        tool.inputSchema = object({
            {"scene", Json{{"type", Json::array({"integer", "string"})}, {"description", "An open scene's id or name, or a .scene/.prefab path."}}},
            {"rev", string("As diff_file's: omitted, git HEAD or svn BASE.")},
            {"source", withDefault(stringEnum("", {"editor", "disk"}), "editor")},
            {"changed_only", withDefault(boolean(""), true)}
        });
        tool.annotations.readOnly = true;
        tool.handler = diffSceneTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "revert_to_baseline";
        tool.title = "Revert to Baseline";
        tool.group = GROUP;
        tool.activity = ToolActivity::Edit;
        tool.allowedWhilePlaying = false;
        tool.description =
            "Reverts one diff_scene row of an open scene or prefab to the baseline: a property (as diff_scene writes it, transform.position), "
            "a component, or the whole GameObject (entity \"settings\": Scene Settings). One undo step; returns its name.";
        tool.inputSchema = object({
            {"scene", Json{{"type", Json::array({"integer", "string"})}, {"description", "An open scene's id or name, or an open .prefab path."}}},
            {"entity", Json{{"type", Json::array({"integer", "string"})}, {"description", "Its id (a removed one too), path or \"settings\"."}}},
            {"component", string("")},
            {"property", string("")}
        }, {"entity"});
        tool.handler = revertToBaselineTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "lock_files";
        tool.title = "Lock Files";
        tool.group = GROUP;
        tool.activity = ToolActivity::Edit;
        tool.description = "Locks files on the server (Git LFS lockable files, svn:needs-lock) so no one else edits them.";
        tool.inputSchema = object({{"paths", pathsSchema("Project-relative.")}}, {"paths"});
        tool.handler = lockFilesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "unlock_files";
        tool.title = "Unlock Files";
        tool.group = GROUP;
        tool.activity = ToolActivity::Edit;
        tool.description = "Releases locks. force breaks another user's lock and then also needs confirm: true.";
        tool.inputSchema = object({
            {"paths", pathsSchema("Project-relative.")},
            {"force", withDefault(boolean(""), false)},
            {"confirm", withDefault(boolean(""), false)}
        }, {"paths"});
        tool.handler = unlockFilesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "commit_files";
        tool.title = "Commit Files";
        tool.group = GROUP;
        tool.activity = ToolActivity::Edit;
        tool.description =
            "Commits exactly these changed paths (each must have changes; an asset's changed .meta goes along) with the message; "
            "hooks run. push pushes after. Returns the revision. Never pass paths you did not change.";
        tool.inputSchema = object({
            {"paths", pathsSchema("Project-relative, each a changed path.")},
            {"message", string("")},
            {"push", withDefault(boolean(""), false)}
        }, {"paths", "message"});
        tool.handler = commitFilesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "revert_files";
        tool.title = "Revert Files";
        tool.group = GROUP;
        tool.activity = ToolActivity::Edit;
        tool.annotations.destructive = true;
        tool.allowedWhilePlaying = false;
        tool.description =
            "Discards the changes of files (their .meta along) back to the baseline; open scenes reload. Undoable (\"Revert N "
            "files\"). New files cannot be reverted: delete them. Needs confirm: true.";
        tool.inputSchema = object({
            {"paths", pathsSchema("Project-relative.")},
            {"confirm", boolean("Must be true.")}
        }, {"paths", "confirm"});
        tool.handler = revertFilesTool;
        registry.addTool(std::move(tool));
    }
    {
        ToolDef tool;
        tool.name = "sync_remote";
        tool.title = "Sync Remote";
        tool.group = GROUP;
        tool.activity = ToolActivity::Edit;
        tool.annotations.destructive = true;
        tool.allowedWhilePlaying = false;
        tool.description =
            "A job with the server: fetch (its commits, files untouched; get_vcs_status then shows behind), pull (git pull "
            "--ff-only, svn update) or push (never forced). pull and push need confirm: true.";
        tool.inputSchema = object({
            {"action", stringEnum("", {"fetch", "pull", "push"})},
            {"confirm", withDefault(boolean(""), false)},
            {"wait", withDefault(boolean("Wait for the job (else poll get_job)."), true)},
            {"timeout_seconds", withDefault(number("", 1, 600), 120)}
        }, {"action"});
        tool.handler = syncRemoteTool;
        registry.addTool(std::move(tool));
    }
    {
        ResourceDef def;
        def.uri = "jasper://vcs/status";
        def.name = "vcs-status";
        def.title = "Version Control Status";
        def.description = "get_vcs_status's summary: provider, branch, ahead/behind, errors, changed count.";
        def.mimeType = "application/json";
        def.read = [](const std::string& uri) {
            ResourceResult result;
            Project* project = Backend::getApp().getProject();
            Json body = project ? summaryJson(project->getVcs()) : Json{{"state", "off"}, {"message", "No project is open."}};
            result.contents.push_back({uri, "application/json", body.dump(), ""});
            return result;
        };
        registry.addResource(std::move(def));
    }
}

}
