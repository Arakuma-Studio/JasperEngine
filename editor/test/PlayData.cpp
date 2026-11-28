// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "test/PlayData.h"

#include "Project.h"
#include "Out.h"
#include "io/AtomicFile.h"
#include "io/UserSettingsStore.h"
#include "test/GameTests.h"
#include "test/TestOptions.h"
#include "util/FileUtils.h"
#include "util/ProjectFileWrites.h"
#include "util/SafeWrite.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace jasperengine;
using namespace jasperengine::editor;

namespace jasperengine::editor::gametests {

namespace {

    fs::path sessionsRoot(Project* project) {
        return project->getProjectInternalPath() / "user" / "sessions";
    }

    // the store is read from the folder data:// is: what it holds goes to its file before the switch
    void flushUserSettings() {
        UserSettingsStore::instance().save();
        UserSettingsStore::instance().unload();
    }

}

PlayData& PlayData::get() {
    static PlayData data;
    return data;
}

bool PlayData::validProfile(Project* project, const std::string& profile, std::string& error) {
    if (profile == "project" || profile == "clean") return true;
    const std::vector<std::string> snapshots = project ? listSnapshots(project->getProjectPath()) : std::vector<std::string>();
    if (std::find(snapshots.begin(), snapshots.end(), profile) != snapshots.end()) return true;
    std::string list;
    for (const std::string& name : snapshots) list += (list.empty() ? "" : ", ") + name;
    error = "data must be \"project\", \"clean\" or a snapshot of Tests/Data (" + (list.empty() ? std::string("none saved yet: Game view > Play Data > Save Snapshot...") : list) +
            "); got \"" + profile + "\".";
    return false;
}

void PlayData::setNextSession(const std::string& profile, bool keepFolder) {
    nextProfile = profile;
    nextKeep = keepFolder;
}

bool PlayData::sessionStarting(Project* project, const std::string& gameViewChoice, std::string& error) {
    const std::string profile = !nextProfile.empty() ? nextProfile : (gameViewChoice.empty() ? std::string("project") : gameViewChoice);
    const bool keepFolder = !nextProfile.empty() && nextKeep;
    nextProfile.clear();
    nextKeep = false;
    if (active()) endSession(project);
    if (profile == "project") return true;
    if (!validProfile(project, profile, error)) return false;

    // an empty folder of its own, below the project's internal folder
#ifdef _WIN32
    const long pid = static_cast<long>(_getpid());
#else
    const long pid = static_cast<long>(getpid());
#endif
    const fs::path folder = sessionsRoot(project) / (std::to_string(pid) + "-" + std::to_string(++counter));
    std::error_code ec;
    safewrite::EditorInternal own(sessionsRoot(project));
    fs::remove_all(folder, ec);
    fs::create_directories(folder, ec);
    if (ec) {
        error = "cannot make the Play Data folder " + FileUtils::pathToUtf8(folder) + ": " + ec.message();
        return false;
    }
    if (profile != "clean") {
        const fs::path snapshot = findRoot(project->getProjectPath()) / "Data" / profile;
        if (!copyTree(snapshot, folder, error)) return false;
    }
    flushUserSettings();
    sessionFolder = folder;
    activeProfile = profile;
    keep = keepFolder;
    sawPlay = false;
    return true;
}

void PlayData::update(Project* project) {
    if (!active() || !project) return;
    if (project->getPlaySessionSceneId() != NULL_PROJECT_SCENE || project->isPlayStarting()) {
        sawPlay = true;
        return;
    }
    endSession(project);
}

void PlayData::endSession(Project* project) {
    flushUserSettings();
    previousFolder = sessionFolder;
    // data:// is the project's again from here (Project::getUserDataPath reads folder())
    const fs::path ended = sessionFolder;
    sessionFolder.clear();
    if (!keep && project) {
        std::error_code ec;
        safewrite::EditorInternal own(sessionsRoot(project));
        fs::remove_all(ended, ec);
        if (ec) Out::warning("Play Data: cannot remove the session folder %s: %s", FileUtils::pathToUtf8(ended).c_str(), ec.message().c_str());
    }
    activeProfile = "project";
    keep = false;
    sawPlay = false;
}

bool PlayData::saveSnapshot(Project* project, const std::string& name, std::string& error, std::string& warning) {
    if (!project) {
        error = "No project is open.";
        return false;
    }
    if (!gametest::isSnapshotName(name)) {
        error = "A snapshot name is 1 to 64 letters, digits, _ or - (got \"" + name + "\").";
        return false;
    }
    const fs::path root = findRoot(project->getProjectPath());
    if (root.empty()) {
        error = NO_ROOT_TEXT;
        return false;
    }
    // what the game wrote is on disk first
    UserSettingsStore::instance().save();
    const fs::path source = active() ? sessionFolder : project->getUserDataPath();
    const fs::path target = root / "Data" / name;
    std::vector<WriteFilesCmd::Item> items;
    uint64_t bytes = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::ostringstream content;
        content << in.rdbuf();
        bytes += content.str().size();
        items.push_back({target / it->path().lexically_relative(source), content.str()});
    }
    if (items.empty()) {
        error = "The " + std::string(active() ? "session's" : "project's") + " Play data is empty (" + FileUtils::pathToUtf8(source) +
                "): play until the game saved something, then save the snapshot.";
        return false;
    }
    // files of an earlier snapshot of that name that the new data lacks stay (named in the warning)
    std::vector<std::string> stale;
    for (fs::recursive_directory_iterator it(target, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const bool written = std::any_of(items.begin(), items.end(), [&](const WriteFilesCmd::Item& item) { return fs::equivalent(item.path, it->path(), ec); });
        if (!written) stale.push_back(FileUtils::pathToGenericUtf8(it->path().lexically_relative(target)));
    }
    if (!ProjectFileWrites::writeUndoable(project, std::move(items), error, nullptr, nullptr, {}, false, "Save Snapshot " + name)) return false;
    if (bytes > LARGE_SNAPSHOT_BYTES)
        warning = "Snapshot " + name + " is " + std::to_string(bytes / (1024 * 1024)) + " MB: it is committed with the project; keep test data small.";
    if (!stale.empty()) {
        std::string list;
        for (const std::string& file : stale) list += (list.empty() ? "" : ", ") + file;
        warning += (warning.empty() ? "" : " ") + std::string("Kept files of the earlier snapshot the new data lacks: ") + list + ".";
    }
    return true;
}

std::string PlayData::selected(Project* project) {
    if (!project) return "project";
    std::ifstream in(project->getProjectInternalPath() / "user" / "game_view.yaml", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("play_data:", 0) != 0) continue;
        std::string value = line.substr(10);
        value.erase(0, value.find_first_not_of(" \t\""));
        while (!value.empty() && (value.back() == ' ' || value.back() == '"' || value.back() == '\r')) value.pop_back();
        return value.empty() ? std::string("project") : value;
    }
    return "project";
}

void PlayData::select(Project* project, const std::string& profile) {
    if (!project) return;
    const fs::path file = project->getProjectInternalPath() / "user" / "game_view.yaml";
    io::AtomicWriteOptions options;
    options.durability = io::Durability::Cache;
    safewrite::EditorInternal own(file);
    const io::WriteResult written = io::writeFileAtomic(file, "play_data: \"" + profile + "\"\n", options);
    if (!written.ok()) Out::warning("Play Data: the choice was not saved: %s", written.message.c_str());
}

}  // namespace jasperengine::editor::gametests
