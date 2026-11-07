// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "window/vcs/VcsUi.h"

#include "AppSettings.h"
#include "Out.h"
#include "Project.h"
#include "Theme.h"
#include "command/CommandHistory.h"
#include "command/UndoTimeline.h"
#include "command/type/SetAssetMetaCmd.h"
#include "external/IconsFontAwesome6.h"
#include "vcs/SceneDiffTracker.h"
#include "vcs/VcsEditorOps.h"
#include "vcs/VcsService.h"
#include "window/StatusBar.h"
#include "window/vcs/SceneDiffWindow.h"
#include "window/vcs/VcsCommitWindow.h"
#include "window/vcs/VcsConflictsWindow.h"
#include "window/vcs/VcsHistoryWindow.h"
#include "window/vcs/VcsOverlay.h"

#include "imgui.h"
#include "window/EditorModal.h"

#include <algorithm>

namespace jasperengine::editor::vcs {

namespace fs = std::filesystem;

struct VcsUi::Dialog {
    std::string title;
    std::string message;
    std::vector<std::pair<std::string, std::function<void()>>> choices;
    bool opened = false;
    uint64_t id = 0;
};

namespace {

    uint64_t nextDialogId = 1;

    bool isDocument(const fs::path& file) {
        const fs::path extension = file.extension();
        return extension == ".scene" || extension == ".prefab";
    }

    std::string fileName(const fs::path& file) {
        return file.filename().string();
    }

}

VcsUi::VcsUi(Project* project)
        : project(project),
          commitWindow(std::make_unique<VcsCommitWindow>(this)),
          historyWindow(std::make_unique<VcsHistoryWindow>(this)),
          sceneDiffWindow(std::make_unique<SceneDiffWindow>(this)),
          conflictsWindow(std::make_unique<VcsConflictsWindow>(this)) {
    // R19: a conflicted document stays closed until it is resolved (the file holds the merge
    // driver's result, which loads, but loading it would hide the conflict)
    Project::documentOpenGuard = [this](const fs::path& file, std::string& why) {
        if (!isConflicted(file)) return true;
        why = fileName(file) + " is in conflict: resolve it in Window > Version Control > Conflicts first.";
        return false;
    };
    Project::readOnlySaveHandler = [this](const fs::path& file, std::function<void()> retry) { offerReadOnlySave(file, std::move(retry)); };
}

VcsUi::~VcsUi() {
    Project::documentOpenGuard = nullptr;
    Project::readOnlySaveHandler = nullptr;
}

void VcsUi::update() {
    VcsService& service = project->getVcs();
    // R5: back in the editor, the status is read at once (a commit made in a terminal)
    const bool focused = !ImGui::GetIO().AppFocusLost;
    if (focused && !appFocused) service.wake();
    appFocused = focused;
    service.update();
    SceneDiffTracker& diffs = project->getSceneDiffs();
    // R15: diffs only while a consumer shows them (Show VCS Changes, the Scene Diff window)
    diffs.setWatchAllOpen(service.active() && AppSettings::getVersionControlSettings().showChanges);
    sceneDiffWindow->watch();
    diffs.update(*project);
    lockOnEdit();
}

void VcsUi::show() {
    commitWindow->show();
    historyWindow->show();
    sceneDiffWindow->show();
    conflictsWindow->show();
    drawTextDiff();
    drawDialogs();
}

// ---------------------------------------------------------------- dialogs

void VcsUi::ask(std::string title, std::string message, std::vector<std::pair<std::string, std::function<void()>>> choices) {
    auto dialog = std::make_unique<Dialog>();
    dialog->title = std::move(title);
    dialog->message = std::move(message);
    dialog->choices = std::move(choices);
    dialog->id = nextDialogId++;
    dialogs.push_back(std::move(dialog));
}

void VcsUi::drawDialogs() {
    if (dialogs.empty()) return;
    Dialog& dialog = *dialogs.front();
    const std::string popupId = dialog.title + "##vcs_dialog_" + std::to_string(dialog.id);
    if (!dialog.opened) {
        ImGui::OpenPopup(popupId.c_str());
        dialog.opened = true;
    }
    bool close = false;
    std::function<void()> chosen;
    ImGui::SetNextWindowSizeConstraints(ImVec2(Theme::dpi(320.0f), 0.0f), ImVec2(Theme::dpi(640.0f), FLT_MAX));
    if (EditorModal::begin(popupId.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        EditorModal::message(dialog.message);
        ImGui::PushTextWrapPos(Theme::dpi(600.0f));
        ImGui::TextUnformatted(dialog.message.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        for (size_t i = 0; i < dialog.choices.size(); i++) {
            if (i > 0) ImGui::SameLine();
            // a null action is the Cancel of ask(); the others answer by their label's words
            const std::string choice = dialog.choices[i].second ? EditorModal::choiceOf(dialog.choices[i].first) : std::string("cancel");
            if (EditorModal::button(dialog.choices[i].first.c_str(), choice.c_str())) {
                chosen = dialog.choices[i].second;
                close = true;
            }
        }
        if (close) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    } else {
        close = true;
    }
    if (close) {
        dialogs.erase(dialogs.begin());
        if (chosen) chosen();
    }
}

// ---------------------------------------------------------------- menus

bool VcsUi::canRun(VcsAction action, const std::vector<fs::path>& files, std::string* why) const {
    const VcsService& service = project->getVcs();
    auto refuse = [why](std::string reason) {
        if (why) *why = std::move(reason);
        return false;
    };
    if (!service.active()) return refuse(service.stateMessage().empty() ? "Version control: not available" : service.stateMessage());
    if (service.busy() && action != VcsAction::ShowHistory && action != VcsAction::DiffAgainstBaseline) {
        return refuse("Busy: " + service.busyLabel());
    }
    const SnapshotPtr& snapshot = service.snapshot();
    switch (action) {
        case VcsAction::Commit:
            if (files.empty()) return true;
            for (const fs::path& file : files) {
                const VcsChange* change = service.find(file);
                if (change && change->changed()) return true;
                RepoPath key;
                if (fs::is_directory(file) && service.toKey(file, key) && service.dirStateKey(key) != DirState::Clean) return true;
            }
            return refuse("No changes to commit");
        case VcsAction::Revert: {
            if (files.empty()) return refuse("Select the files to revert");
            if (!snapshot) return refuse("The status is not read yet");
            std::vector<RepoPath> keys;
            std::string reason;
            if (!keysOf(service, files, keys, &reason)) return refuse(reason);
            reason = revertRefusal(*snapshot, keys);
            return reason.empty() ? true : refuse(reason);
        }
        case VcsAction::Lock:
            if (!service.caps().locking) return refuse("Locking needs Git LFS 3.0 or newer (or Subversion)");
            if (files.empty()) return refuse("Select the files to lock");
            for (const fs::path& file : files) {
                const VcsChange* change = service.find(file);
                if (!service.isLockable(file)) return refuse(fileName(file) + " is not lockable (the lockable attribute in .gitattributes, or svn:needs-lock)");
                if (change && change->lock == LockState::LockedByOther) return refuse(fileName(file) + " is locked by " + change->lockOwner);
                if (change && change->lock == LockState::LockedByMe) return refuse(fileName(file) + " is locked by you already");
            }
            return true;
        case VcsAction::Unlock:
            if (!service.caps().locking) return refuse("Locking needs Git LFS 3.0 or newer (or Subversion)");
            for (const fs::path& file : files) {
                const VcsChange* change = service.find(file);
                if (change && (change->lock == LockState::LockedByMe || change->lock == LockState::LockedByOther)) return true;
            }
            return refuse("Nothing selected is locked");
        case VcsAction::ShowHistory:
            return true;
        case VcsAction::DiffAgainstBaseline:
            if (files.size() != 1) return refuse("Select one file");
            if (fs::is_directory(files[0])) return refuse("Select a file, not a folder");
            return true;
        case VcsAction::Refresh:
            return true;
    }
    return true;
}

void VcsUi::run(VcsAction action, const std::vector<fs::path>& files, uint32_t sceneId) {
    VcsService& service = project->getVcs();
    std::vector<RepoPath> keys;
    switch (action) {
        case VcsAction::Commit:
            openCommit(files);
            return;
        case VcsAction::Revert:
            startRevert(files);
            return;
        case VcsAction::Lock:
            if (!keysOf(service, files, keys, nullptr)) return;
            service.lock(keys, [](const VcsResult& result) {
                if (result.ok) Out::info("%s", result.message.c_str());
                else Out::error("Lock failed: %s", result.message.c_str());
            });
            return;
        case VcsAction::Unlock: {
            if (!keysOf(service, files, keys, nullptr)) return;
            std::string owners;
            for (const RepoPath& key : keys) {
                const VcsChange* change = service.findKey(key);
                if (change && change->lock == LockState::LockedByOther) owners += (owners.empty() ? "" : ", ") + change->lockOwner;
            }
            auto unlock = [&service, keys](bool force) {
                service.unlock(keys, force, [](const VcsResult& result) {
                    if (result.ok) Out::info("%s", result.message.c_str());
                    else Out::error("Unlock failed: %s", result.message.c_str());
                });
            };
            if (owners.empty()) {
                unlock(false);
            } else {
                ask("Break Lock", "Someone else holds the lock (" + owners + "). Break it? They may lose work they have not committed.",
                    {{"Break Lock", [unlock]() { unlock(true); }}, {"Cancel", nullptr}});
            }
            return;
        }
        case VcsAction::ShowHistory:
            openHistory(files.size() == 1 ? files[0] : fs::path());
            return;
        case VcsAction::DiffAgainstBaseline: {
            if (files.size() != 1) return;
            if (isDocument(files[0])) {
                uint32_t document = sceneId;
                if (document == 0) {
                    for (SceneProject* candidate : project->getScenesAndPrefabStages()) {
                        fs::path file = candidate->filepath;
                        if (const PrefabStage* stage = project->getPrefabStage(candidate->id)) file = stage->assetPath;
                        if (!file.empty() && (file.is_relative() ? project->getProjectPath() / file : file).lexically_normal() == files[0].lexically_normal() &&
                            candidate->scene) {
                            document = candidate->id;
                        }
                    }
                }
                if (document != 0) {
                    openSceneDiff(document);
                    return;
                }
                sceneDiffWindow->openFile(files[0]);
                return;
            }
            openTextDiff(files[0]);
            return;
        }
        case VcsAction::Refresh:
            service.refresh(true, [](const VcsResult& result) {
                if (!result.ok) Out::error("Version control refresh failed: %s", result.message.c_str());
            });
            return;
    }
}

void VcsUi::drawContextMenu(const std::vector<fs::path>& files, uint32_t sceneId) {
    if (!ImGui::BeginMenu(ICON_FA_CODE_BRANCH "  Version Control")) return;
    struct Item {
        VcsAction action;
        const char* label;
    };
    const Item items[] = {{VcsAction::Commit, "Commit..."}, {VcsAction::Revert, "Revert..."}, {VcsAction::Lock, "Lock"},
                          {VcsAction::Unlock, "Unlock"}, {VcsAction::ShowHistory, "Show History"},
                          {VcsAction::DiffAgainstBaseline, "Diff Against Baseline"}, {VcsAction::Refresh, "Refresh"}};
    for (const Item& item : items) {
        if (item.action == VcsAction::ShowHistory || item.action == VcsAction::Refresh) ImGui::Separator();
        std::string why;
        const bool enabled = canRun(item.action, files, &why);
        if (ImGui::MenuItem(item.label, nullptr, false, enabled)) run(item.action, files, sceneId);
        if (!enabled && !why.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", why.c_str());
    }
    ImGui::EndMenu();
}

void VcsUi::startRevert(const std::vector<fs::path>& files) {
    // R11: a dirty open document among them asks first
    const std::vector<std::string> dirty = dirtyDocumentsAmong(*project, files);
    auto revert = [this, files]() {
        revertFiles(*project, files, [](const VcsResult& result, const std::string&) {
            if (!result.ok) Out::error("Revert failed: %s", result.message.c_str());
        });
    };
    if (dirty.empty()) {
        revert();
        return;
    }
    std::string names;
    for (const std::string& name : dirty) names += (names.empty() ? "" : ", ") + name;
    ask("Revert", "Revert discards unsaved changes in " + names + ".", {{"Revert", revert}, {"Cancel", nullptr}});
}

// ---------------------------------------------------------------- windows

void VcsUi::openCommit(const std::vector<fs::path>& preselect) {
    commitWindow->open(preselect);
}

void VcsUi::openHistory(const fs::path& file) {
    historyWindow->open(file);
}

void VcsUi::openSceneDiff(uint32_t sceneId, const std::string& rev) {
    sceneDiffWindow->open(sceneId, rev);
}

void VcsUi::openConflicts() {
    conflictsWindow->open();
}

void VcsUi::toggleWindow(const char* name) {
    const std::string window = name;
    if (window == COMMIT_WINDOW) commitWindow->toggle();
    else if (window == HISTORY_WINDOW) historyWindow->toggle();
    else if (window == SCENE_DIFF_WINDOW) sceneDiffWindow->toggle();
    else if (window == CONFLICTS_WINDOW) conflictsWindow->toggle();
}

bool VcsUi::isWindowOpen(const char* name) const {
    const std::string window = name;
    if (window == COMMIT_WINDOW) return commitWindow->isOpen();
    if (window == HISTORY_WINDOW) return historyWindow->isOpen();
    if (window == SCENE_DIFF_WINDOW) return sceneDiffWindow->isOpen();
    if (window == CONFLICTS_WINDOW) return conflictsWindow->isOpen();
    return false;
}

void VcsUi::openTextDiff(const fs::path& file, const std::string& rev) {
    VcsService& service = project->getVcs();
    RepoPath key;
    if (!service.toKey(file, key)) return;
    textDiffOpen = true;
    textDiffFile = file;
    textDiffRev = rev;
    textDiff.clear();
    textDiffPending = true;
    service.textDiff(key, rev, [this, file, rev](const VcsResult& result, std::string diff) {
        if (textDiffFile != file || textDiffRev != rev) return;
        textDiffPending = false;
        textDiff = result.ok ? (diff.empty() ? std::string("No changes.") : std::move(diff)) : "The diff failed: " + result.message;
    });
}

void VcsUi::drawTextDiff() {
    if (!textDiffOpen) return;
    ImGui::SetNextWindowSize(Theme::dpi(ImVec2(720.0f, 480.0f)), ImGuiCond_FirstUseEver);
    const std::string title = "Diff: " + fileName(textDiffFile) + " against " + textDiffRev + "###VcsTextDiff";
    if (ImGui::Begin(title.c_str(), &textDiffOpen)) {
        if (textDiffPending) {
            ImGui::TextDisabled("Reading the diff...");
        } else {
            // one line per row, coloured as a unified diff
            ImGui::BeginChild("##diff", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
            size_t start = 0;
            while (start < textDiff.size()) {
                const size_t end = textDiff.find('\n', start);
                const std::string line = textDiff.substr(start, end == std::string::npos ? std::string::npos : end - start);
                ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
                if (line.rfind("+++", 0) == 0 || line.rfind("---", 0) == 0) color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
                else if (!line.empty() && line[0] == '+') color = colorOf(Badge::Added);
                else if (!line.empty() && line[0] == '-') color = colorOf(Badge::Deleted);
                else if (line.rfind("@@", 0) == 0) color = colorOf(Badge::RemoteChanged);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopStyleColor();
                if (end == std::string::npos) break;
                start = end + 1;
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- status bar

void VcsUi::drawStatusBarSegment() {
    const VcsService& service = project->getVcs();
    if (service.state() == VcsState::Off || service.state() == VcsState::NoRepository) return;
    std::string label;
    size_t conflicts = 0, staged = 0, modified = 0, untracked = 0;
    VcsOperation operation = VcsOperation::None;
    if (!service.active()) {
        label = ICON_FA_CODE_BRANCH " " + std::string(service.state() == VcsState::Detecting ? "..." : "Version control: not available");
    } else {
        const SnapshotPtr& snapshot = service.snapshot();
        if (snapshot) {
            for (const VcsChange& change : snapshot->entries) {
                if (change.conflict) conflicts++;
                else if (change.index == FileState::Untracked) untracked++;
                else {
                    if (change.index != FileState::Unmodified) staged++;
                    if (change.worktree != FileState::Unmodified) modified++;
                }
            }
        }
        // a tag, a bare commit or a branch, as git's prompt names them
        const VcsBranchInfo* branch = snapshot ? &snapshot->branch : nullptr;
        const char* icon = ICON_FA_CODE_BRANCH;
        if (branch && branch->operation != VcsOperation::Rebase && !branch->tag.empty()) icon = ICON_FA_TAG;
        else if (branch && branch->detached && branch->operation != VcsOperation::Rebase) icon = ICON_FA_CODE_COMMIT;
        label = std::string(icon) + " " + service.caps().provider;
        if (branch) {
            operation = branch->operation;
            label += " " + headLabel(*branch);
            if (branch->ahead > 0) label += " " ICON_FA_ARROW_UP + std::to_string(branch->ahead);
            if (branch->behind > 0) label += " " ICON_FA_ARROW_DOWN + std::to_string(branch->behind);
            const size_t changed = snapshot->changedCount();
            if (conflicts > 0) label += "  " + std::to_string(conflicts) + (conflicts == 1 ? " conflict" : " conflicts");
            else if (changed > 0) label += "  " + std::to_string(changed) + (changed == 1 ? " change" : " changes");
        }
        if (service.busy()) label += "  " + service.busyLabel();
    }
    const bool error = !service.lastError().empty() || !service.lockError().empty();
    if (error) label += "  " ICON_FA_TRIANGLE_EXCLAMATION;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    // conflicts and errors in red, an unfinished merge or rebase in the modified amber
    const bool colored = error || conflicts > 0 || operation != VcsOperation::None;
    if (colored) {
        const ImU32 color = (error || conflicts > 0) ? colorOf(Badge::Conflict) : colorOf(Badge::Modified);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color));
    }
    if (ImGui::SmallButton((label + "##vcs_status").c_str()) && service.active()) {
        if (conflicts > 0) openConflicts();
        else openCommit();
    }
    if (colored) ImGui::PopStyleColor();
    ImGui::PopStyleColor();
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted(service.stateMessage().c_str());
        if (const SnapshotPtr& snapshot = service.snapshot()) {
            const VcsBranchInfo& branch = snapshot->branch;
            if (branch.detached && branch.operation != VcsOperation::Rebase) {
                ImGui::Text("Detached HEAD at %s%s", branch.headOid.substr(0, 8).c_str(),
                            branch.tag.empty() ? "" : (" (tag " + branch.tag + ")").c_str());
            }
            if (!branch.upstream.empty()) ImGui::Text("Upstream %s: %d ahead, %d behind", branch.upstream.c_str(), branch.ahead, branch.behind);
            ImGui::Text("Changes: %zu staged, %zu modified, %zu untracked, %zu conflicted", staged, modified, untracked, conflicts);
            switch (branch.operation) {
                case VcsOperation::None: break;
                case VcsOperation::Merge:
                    ImGui::TextUnformatted("Merging: resolve the conflicts, then commit to finish the merge.");
                    break;
                case VcsOperation::Rebase:
                    if (branch.operationTotal > 0) {
                        ImGui::Text("Rebasing %s: step %d of %d. Resolve conflicts, then continue the rebase in a terminal.",
                                    branch.operationBranch.c_str(), branch.operationStep, branch.operationTotal);
                    } else {
                        ImGui::Text("Rebasing %s. Resolve conflicts, then continue the rebase in a terminal.", branch.operationBranch.c_str());
                    }
                    break;
                case VcsOperation::CherryPick:
                    ImGui::TextUnformatted("Cherry-picking: resolve the conflicts, then continue in a terminal.");
                    break;
                case VcsOperation::Revert:
                    ImGui::TextUnformatted("Reverting a commit: resolve the conflicts, then continue in a terminal.");
                    break;
                case VcsOperation::Bisect:
                    ImGui::TextUnformatted("Bisecting: git bisect reset in a terminal ends it.");
                    break;
            }
            if (snapshot->mergeDriverMissing) ImGui::TextDisabled("Smart merge: not set up (Preferences > Version Control)");
        }
        if (!service.lockError().empty()) ImGui::Text("Locks may be out of date: %s", service.lockError().c_str());
        if (!service.lastError().empty()) ImGui::Text("Last error: %s", service.lastError().c_str());
        if (service.active()) ImGui::TextDisabled(conflicts > 0 ? "Click to open the Conflicts window" : "Click to open the Commit window");
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
}

// ---------------------------------------------------------------- the lock banner (R28)

bool VcsUi::drawLockBanner(const std::vector<fs::path>& files) {
    VcsService& service = project->getVcs();
    if (!service.active() || !service.caps().locking || files.empty()) return false;
    std::vector<fs::path> lockable;
    std::string owner;
    bool other = false, mine = true;
    for (const fs::path& file : files) {
        const VcsChange* change = service.find(file);
        const bool isLockable = service.isLockable(file) || (change && change->lock != LockState::Unsupported);
        if (!isLockable) continue;
        lockable.push_back(file);
        const LockState lock = change ? change->lock : LockState::Unlocked;
        if (lock == LockState::LockedByOther) {
            other = true;
            owner = change->lockOwner;
        }
        if (lock != LockState::LockedByMe) mine = false;
    }
    if (lockable.empty()) return false;
    const bool anyway = other && std::all_of(lockable.begin(), lockable.end(), [this](const fs::path& file) { return editAnyway.count(file) != 0; });
    ImGui::PushID("vcs_lock_banner");
    ImGui::AlignTextToFramePadding();
    const bool busy = service.busy();
    if (other) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(lockColor(LockState::LockedByOther)), ICON_FA_LOCK);
        ImGui::SameLine();
        ImGui::Text(anyway ? "Locked by %s (editing anyway)" : "Locked by %s \xE2\x80\x94 read only", owner.c_str());
        if (!anyway) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit Anyway")) {
                for (const fs::path& file : lockable) editAnyway.insert(file);
            }
            ImGui::SetItemTooltip("Edit it although %s holds the lock: your changes may conflict with theirs", owner.c_str());
        }
    } else if (mine) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(lockColor(LockState::LockedByMe)), ICON_FA_LOCK);
        ImGui::SameLine();
        ImGui::TextUnformatted("Locked by you");
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::SmallButton("Unlock")) run(VcsAction::Unlock, lockable);
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled(ICON_FA_LOCK_OPEN);
        ImGui::SameLine();
        ImGui::TextUnformatted("Not locked");
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::SmallButton("Lock")) run(VcsAction::Lock, lockable);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Lock it on the server so no one else edits it at the same time");
    }
    ImGui::PopID();
    ImGui::Separator();
    return other && !anyway;
}

// ---------------------------------------------------------------- conflicts, read-only saves

bool VcsUi::isConflicted(const fs::path& file) const {
    const VcsChange* change = project->getVcs().find(file);
    return change && change->conflict;
}

void VcsUi::lockFile(const fs::path& file, std::function<void(bool)> done) {
    VcsService& service = project->getVcs();
    RepoPath key;
    if (!service.toKey(file, key)) {
        if (done) done(false);
        return;
    }
    service.lock({key}, [file, done](const VcsResult& result) {
        if (!result.ok) {
            const std::string message = "Could not lock " + fileName(file) + ": " + result.message;
            Out::warning("%s", message.c_str());
            StatusBar::setTransientMessage(message, 6.0);
        }
        if (done) done(result.ok);
    });
}

void VcsUi::offerReadOnlySave(const fs::path& file, std::function<void()> retrySave) {
    const VcsService& service = project->getVcs();
    std::vector<std::pair<std::string, std::function<void()>>> choices;
    if (service.active() && service.caps().locking) {
        choices.emplace_back("Lock and Save", [this, file, retrySave]() {
            lockFile(file, [retrySave](bool locked) {
                if (locked && retrySave) retrySave();
            });
        });
    }
    choices.emplace_back("Make Writable and Save", [file, retrySave]() {
        std::error_code ec;
        fs::permissions(file, fs::perms::owner_write, fs::perm_options::add, ec);
        if (ec) {
            Out::error("Could not make %s writable: %s", fileName(file).c_str(), ec.message().c_str());
            return;
        }
        if (retrySave) retrySave();
    });
    choices.emplace_back("Cancel", nullptr);
    ask("Read-Only File", fileName(file) + " is read only: version control keeps a lockable file read only until you lock it.", std::move(choices));
}

// ---------------------------------------------------------------- lock-on-edit (R12)

void VcsUi::lockOnEdit() {
    VcsService& service = project->getVcs();
    if (!service.active() || !service.caps().locking || !service.snapshot()) return;
    const LockOnEdit mode = AppSettings::getVersionControlSettings().lockOnEdit;
    // the first edit of a lockable file: a document becoming dirty, an asset's import settings
    std::vector<std::pair<fs::path, uint32_t>> edited;
    for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
        const bool modified = sceneProject->isModified;
        bool& before = wasModified[sceneProject->id];
        if (modified && !before && !sceneProject->filepath.empty()) {
            fs::path file = sceneProject->filepath;
            if (const PrefabStage* stage = project->getPrefabStage(sceneProject->id)) file = stage->assetPath;
            if (file.is_relative()) file = project->getProjectPath() / file;
            edited.emplace_back(file, sceneProject->id);
        }
        before = modified;
    }
    if (const UndoEntry* top = project->getProjectCommandHistory()->top()) {
        if (top->info.id != projectHistorySeen) {
            projectHistorySeen = top->info.id;
            if (const auto* meta = dynamic_cast<const SetAssetMetaCmd*>(top->cmd.get())) {
                edited.emplace_back(project->getProjectPath() / meta->getAsset(), 0);
            }
        }
    }
    for (const auto& [file, sceneId] : edited) {
        const VcsChange* change = service.find(file);
        const bool lockable = service.isLockable(file) || (change && change->lock != LockState::Unsupported);
        if (!lockable) continue;
        const LockState lock = change ? change->lock : LockState::Unlocked;
        if (lock == LockState::LockedByMe) continue;
        Project* owner = project;
        const uint32_t document = sceneId;
        auto undoEdit = [owner, document]() {
            if (document != 0) UndoTimeline::undo(document);
            else owner->getProjectCommandHistory()->undo();
        };
        if (lock == LockState::LockedByOther) {
            // always a warning: the edit continues only after Edit Anyway
            if (editAnyway.count(file)) continue;
            ask("Locked by " + change->lockOwner, fileName(file) + " is locked by " + change->lockOwner + ". Your edit may conflict with theirs.",
                {{"Edit Anyway", [this, file]() { editAnyway.insert(file); }}, {"Undo the Edit", undoEdit}});
            continue;
        }
        if (mode == LockOnEdit::Off || editWithoutLock.count(file)) continue;
        if (mode == LockOnEdit::Auto) {
            lockFile(file, nullptr);
            continue;
        }
        ask("Lock " + fileName(file) + "?", fileName(file) + " is lockable: lock it so no one else edits it while you do.",
            {{"Lock", [this, file]() { lockFile(file, nullptr); }},
             {"Edit Without Lock", [this, file]() { editWithoutLock.insert(file); }},
             {"Cancel", undoEdit}});
    }
}

}
