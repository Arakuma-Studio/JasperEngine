// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Play Data profiles (docs/specs/project-test-runner.md R20-R22): which folder a Play session's
// data:// (PlayerPrefs included) is. Project: the project's .jasperengine/user/rundata (today's,
// the default). Clean: an empty session folder. A snapshot name: a copy of Tests/Data/<name>/.
// A session folder is deleted when Play stops, unless a test run keeps it. While one is active,
// Project::getUserDataPath, the UserSettings store, user_settings and Edit > Clear User Settings
// act on it.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::gametests {

    class PlayData {
    public:
        static PlayData& get();

        // "project", "clean" or a snapshot name; error names the choices
        static bool validProfile(Project* project, const std::string& profile, std::string& error);

        // the next Play session's profile (play {data}, a test case), over the Game view's choice
        void setNextSession(const std::string& profile, bool keepFolder = false);

        // Project::start, through App::playStarting: the session folder made and in use
        bool sessionStarting(Project* project, const std::string& gameViewChoice, std::string& error);
        // every editor frame: Play fully stopped, the session folder goes (or stays: keepFolder)
        void update(Project* project);

        bool active() const { return !sessionFolder.empty(); }
        // "project" while no session folder is in use
        const std::string& profile() const { return activeProfile; }
        const std::filesystem::path& folder() const { return sessionFolder; }
        // the folder a test case's session used, after it ended (kept or deleted)
        const std::filesystem::path& lastFolder() const { return previousFolder; }

        // Save Snapshot... (R21, R22): the project's data, or the session's while playing, into
        // Tests/Data/<name>/ as one undo step; warning gets the size note (over 64 MB)
        bool saveSnapshot(Project* project, const std::string& name, std::string& error, std::string& warning);

        static constexpr uint64_t LARGE_SNAPSHOT_BYTES = 64ull * 1024 * 1024;

        // the Game view's choice, kept per user and project (.jasperengine/user/game_view.yaml, play_data)
        static std::string selected(Project* project);
        static void select(Project* project, const std::string& profile);

        // Called on the main thread before Play runs the scripts' init, after the project's Lua
        // modules were cleared: a test case seeds math.random there (project-test-runner R14)
        std::function<void()> beforeScripts;

    private:
        void endSession(Project* project);

        std::string nextProfile;
        bool nextKeep = false;
        std::string activeProfile = "project";
        bool keep = false;
        bool sawPlay = false;
        std::filesystem::path sessionFolder;
        std::filesystem::path previousFolder;
        uint64_t counter = 0;
    };

}
