// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "vcs/VcsEditorOps.h"

#include "Out.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "command/type/RevertFilesCmd.h"
#include "io/watch/FileWatchService.h"

#include <algorithm>
#include <set>

namespace jasperengine::editor::vcs {

namespace fs = std::filesystem;

namespace {

    bool copyFileBytes(const fs::path& from, const fs::path& to, std::string& error) {
        std::error_code ec;
        fs::create_directories(to.parent_path(), ec);
        if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
            error = "could not back up " + from.string() + ": " + ec.message();
            return false;
        }
        return true;
    }

}

std::vector<RepoPath> withChangedMetas(const VcsSnapshot& snapshot, const std::vector<RepoPath>& keys) {
    std::vector<RepoPath> out;
    std::set<RepoPath, PathKeyLess> seen;
    for (const RepoPath& key : keys) {
        if (seen.insert(key).second) out.push_back(key);
        if (!metaAssetOf(key).empty()) continue;
        const std::string meta = metaOf(key);
        const VcsChange* change = snapshot.find(meta);
        if (change && change->changed() && seen.insert(meta).second) out.push_back(meta);
    }
    return out;
}

bool keysOf(const VcsService& service, const std::vector<fs::path>& paths, std::vector<RepoPath>& keys, std::string* why) {
    keys.clear();
    for (const fs::path& path : paths) {
        RepoPath key;
        if (!service.toKey(path, key) || key.empty()) {
            if (why) *why = path.string() + " is not in the repository";
            return false;
        }
        keys.push_back(key);
    }
    return true;
}

std::string revertRefusal(const VcsSnapshot& snapshot, const std::vector<RepoPath>& keys, bool unschedulesAdds) {
    for (const RepoPath& key : keys) {
        const VcsChange* change = snapshot.find(key);
        if (!change || !change->changed()) return key + " has no changes to revert";
        const bool untracked = change->index == FileState::Untracked || change->worktree == FileState::Untracked;
        if (change->newFile() && (untracked || !unschedulesAdds))
            return key + " is new: there is no committed version to revert to (Delete it instead)";
    }
    return std::string();
}

std::vector<std::string> dirtyDocumentsAmong(Project& project, const std::vector<fs::path>& files) {
    std::vector<std::string> names;
    for (SceneProject* sceneProject : project.getScenesAndPrefabStages()) {
        if (!sceneProject->isModified || sceneProject->filepath.empty()) continue;
        fs::path file = sceneProject->filepath;
        if (file.is_relative()) file = project.getProjectPath() / file;
        for (const fs::path& path : files) {
            if (file.lexically_normal() == path.lexically_normal()) names.push_back(sceneProject->name);
        }
    }
    return names;
}

void revertFiles(Project& project, const std::vector<fs::path>& files, std::function<void(const VcsResult&, const std::string&)> done) {
    VcsService& service = project.getVcs();
    auto fail = [&done](const std::string& message) {
        if (done) done(VcsResult::failure(message), std::string());
    };
    if (!service.active() || !service.snapshot()) return fail("Version control is not active: " + service.stateMessage());
    std::vector<RepoPath> keys;
    std::string why;
    if (!keysOf(service, files, keys, &why)) return fail(why);
    const VcsSnapshot& snapshot = *service.snapshot();
    const std::string refusal = revertRefusal(snapshot, keys, service.caps().provider == "svn");
    if (!refusal.empty()) return fail(refusal);
    // a rename goes back with the path it came from (a copy leaves its source alone)
    std::vector<RepoPath> targets = withChangedMetas(snapshot, keys);
    for (const RepoPath& key : keys) {
        const VcsChange* change = snapshot.find(key);
        if (change && change->renamed() && !change->oldPath.empty()) targets.push_back(change->oldPath);
    }
    struct Plan {
        fs::path backup;
        std::vector<RevertFilesCmd::File> files;
        std::vector<RepoPath> keys;
    };
    auto plan = std::make_shared<Plan>();
    plan->backup = RevertFilesCmd::newBackupDir(project);
    plan->keys = targets;
    for (const RepoPath& key : targets) {
        RevertFilesCmd::File file;
        file.path = service.toAbsolute(key);
        plan->files.push_back(file);
    }
    const size_t reverted = keys.size();
    Project* owner = &project;
    service.run("Reverting " + std::to_string(reverted) + (reverted == 1 ? " file..." : " files..."),
        [plan](IVcsProvider& provider, const jobs::CancelToken&) {
            // the copies first: a revert that cannot be undone never runs
            std::string error;
            for (size_t i = 0; i < plan->files.size(); i++) {
                RevertFilesCmd::File& file = plan->files[i];
                std::error_code ec;
                file.before = fs::exists(file.path, ec);
                if (file.before && !copyFileBytes(file.path, RevertFilesCmd::copyPath(plan->backup, false, i), error)) {
                    fs::remove_all(plan->backup, ec);
                    return VcsResult::failure(error);
                }
            }
            VcsResult result = provider.revert(plan->keys);
            std::error_code ec;
            if (!result.ok) {
                fs::remove_all(plan->backup, ec);
                return result;
            }
            for (size_t i = 0; i < plan->files.size(); i++) {
                RevertFilesCmd::File& file = plan->files[i];
                file.after = fs::exists(file.path, ec);
                if (file.after && !copyFileBytes(file.path, RevertFilesCmd::copyPath(plan->backup, true, i), error)) {
                    return VcsResult::failure("reverted, but " + error);
                }
            }
            return result;
        },
        [owner, plan, reverted, done](const VcsResult& result) {
            if (!result.ok) {
                Out::error("Revert failed: %s", result.message.c_str());
                if (done) done(result, std::string());
                return;
            }
            // git's writes reach the watcher before the reload, so no "changed on disk" bar
            // comes after it
            FileWatchService::get().flush(std::chrono::milliseconds(1000));
            auto* command = new RevertFilesCmd(owner, plan->backup, plan->files, reverted);
            const std::string name = command->describe();
            if (!owner->getProjectCommandHistory()->addCommand(command)) {
                const std::string message = "Reverted, but the step could not be recorded: " + CommandHistory::lastBusyRefusal();
                Out::warning("%s", message.c_str());
                if (done) done(VcsResult::failure(message), std::string());
                return;
            }
            Out::info("%s", name.c_str());
            if (done) done(VcsResult::success(name), name);
        });
}

void smartMerge(VcsService& service, const RepoPath& key, std::map<std::string, scene::MergeSide> resolutions,
                std::function<void(const SmartMergeOutcome&)> done) {
    auto outcome = std::make_shared<SmartMergeOutcome>();
    service.run("Merging " + key + "...", [key, outcome, resolutions = std::move(resolutions)](IVcsProvider& provider, const jobs::CancelToken&) {
        const VcsResult sides = provider.conflictSides(key, outcome->sides);
        if (!sides.ok) {
            outcome->error = sides.message;
            return sides;
        }
        outcome->merge = scene::mergeSceneTexts(outcome->sides.hasBase ? outcome->sides.base : std::string(), outcome->sides.ours,
                                                outcome->sides.theirs, resolutions);
        outcome->ok = outcome->merge.ok;
        if (!outcome->ok) outcome->error = outcome->merge.error;
        return outcome->ok ? VcsResult::success() : VcsResult::failure(outcome->error);
    }, [outcome, done](const VcsResult&) {
        if (done) done(*outcome);
    });
}

}
