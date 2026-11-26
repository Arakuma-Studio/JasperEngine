// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Record Input and Replay Input in the editor (docs/specs/input-recorder-and-input-scripts.md R13,
// R16, R21, R36-R40). Record Input is armed before Play: the session then runs at its frame rate
// with a new seed, holds the game until its scene runs and records from the frame after; Stop
// Recording (or Play's stop) encodes the recording on a job and writes it into Tests/Input/ (a
// project with a test root) or Recordings/Input/ as one undo step. A replay starts its session the
// same way (the recording's seed, scene and frame rate) and runs the file from the frame after the
// scene runs, real input to the game blocked; its `shot` steps are PNGs of the Game view taken with
// the game held until nothing loads. The toolbar button, the Game view banner, the .input.json
// Inspector and MCP's play {input} all drive this one state.

#include "input/InputScript.h"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::input {
    class RecordedInput;
    class Replay;
}

namespace jasperengine::editor {
    class Project;
}

namespace jasperengine::editor::recordinput {

    namespace fs = std::filesystem;
    using Json = input::ScriptJson;

    // Preferences > Play Mode > Record Input (R37), per project in .jasperengine/user/record-input.yaml
    struct Settings {
        int frameRate = 60;
        int checkpointFrames = 60;          // 0: no checkpoints
        std::vector<std::string> probes;    // GameObject paths (at most 16); empty: chosen at the start

        bool operator==(const Settings& other) const = default;
    };
    inline constexpr int MAX_PROBES = 16;

    fs::path settingsFile(const fs::path& projectPath);
    // a missing file leaves the defaults; values are clamped (frame rate 1-240, checkpoints 0-100000)
    bool loadSettings(const fs::path& file, Settings& settings, std::string& error);
    bool saveSettings(const fs::path& file, const Settings& settings, std::string& error);

    // where a recording goes: <test root>/Input when the project has one, else Recordings/Input
    fs::path recordingsFolder(const fs::path& projectPath);
    // "<scene>-<yyyymmdd-hhmmss>.input.json" (characters a file name cannot hold become '_')
    std::string recordingFileName(const std::string& scene, const std::tm& when);
    // "menu.input.json" -> "menu"
    std::string fileStem(const fs::path& file);

    // What the Inspector shows of a .input.json (R39)
    struct FileSummary {
        bool ok = false;
        std::string error;
        std::string kind;                   // recording | script
        std::string start;                  // play | now (recordings)
        std::string scene;
        std::string platform;
        std::string space;
        int fixedFps = 60;
        uint32_t frames = 0;                // the update frames it runs
        size_t events = 0;
        std::map<std::string, size_t> eventsByType;   // "key", "touch", ...
        size_t checkpoints = 0;
        size_t probes = 0;
        bool truncated = false;
        bool hasSeed = false;
        uint32_t seed = 0;
    };
    FileSummary summarize(std::string_view text);

    // "1290x2796" or a Game view resolution's name -> its size; false with error naming the fix
    bool parseResolution(const std::string& text, int& width, int& height, std::string& error);

    // Writes a recording's text to its file: true, or false with the reason (the editor's writer is
    // one undo step, WriteFilesCmd)
    using Writer = std::function<bool(const fs::path& path, const std::string& text, std::string& error)>;

    // A recording encoded and waiting for its write: kept until a write succeeds or the next
    // recording starts (R15), so a failed write can be retried
    struct PendingWrite {
        fs::path path;                      // absolute
        std::string text;
        std::string error;                  // the last write's failure ("" before any)
    };
    // true when written; false keeps `pending` whole with error naming the file and why
    bool writePending(PendingWrite& pending, const Writer& writer);

    // A replay's shot taken (or failed)
    struct Shot {
        std::string name;
        fs::path path;                      // absolute
        std::string error;
        int width = 0;
        int height = 0;
    };

    class RecordInput {
    public:
        static RecordInput& get();

        // the editor's side: the project, the Game view's image and the undoable writer
        struct Host {
            std::function<Project*()> project;
            std::function<bool(std::vector<uint8_t>& rgba, int& width, int& height)> readGameView;
            std::function<bool()> assetsIdle;       // nothing loading: a shot may be read
            Writer writer;
        };
        void setHost(Host host);

        // ---- settings: the project's, read when it opens (again only for another project)
        void loadSettings(const fs::path& projectPath);
        const Settings& settings() const { return currentSettings; }
        bool setSettings(const Settings& settings, std::string& error);

        // ---- Record Input (R13, R36)
        enum class Button : uint8_t { Off, Armed, Recording, Disabled };
        Button buttonState() const;
        bool isArmed() const { return armed; }
        void setArmed(bool on);
        // playmode.recordInput: arms or disarms before Play, stops a recording during it
        void toggle();
        bool canToggle() const;
        bool isRecording() const;
        // the recording that runs: where it will be written ("" when none)
        const fs::path& recordingFile() const { return recordFile; }
        // "REC 00:12 · 734 events"
        std::string recordingLabel() const;
        // Stop Recording: Play goes on; the file is written on a job
        void stopRecording();
        // the last recording written, and the failure of a write that waits for a retry
        const fs::path& lastWritten() const { return lastWrittenFile; }
        bool writePendingFailed() const { return pending && !pending->error.empty() && !writing; }
        std::string writeError() const { return pending ? pending->error : std::string(); }
        void retryWrite();

        // ---- Replay Input (R16, R21, R40)
        // Parses `file` (project-relative or absolute) for a replay. False with error naming the fix.
        bool loadReplay(const std::string& file, std::shared_ptr<input::ParsedScript>& script, std::string& error) const;
        // The session about to start replays `script`: called just before Project::start. Its
        // seed, frame rate and hold apply when Play starts (playStarting); shots go to `shotFolder`
        // (default Recordings/Input/<file stem>/), or are skipped when `shots` is false.
        void prepareReplay(std::shared_ptr<input::ParsedScript> script, const std::string& file, const fs::path& shotFolder = {},
                           bool shots = true);
        // Play did not start after prepareReplay
        void cancelPrepared();
        // Replay Input... and the Inspector's Replay: starts Play on the recording's scene (else
        // the selected one); false with error
        bool replayFile(const std::string& file, std::string& error);
        // the same with a parsed file, its shots into `shotFolder` (skipped when `shots` is false)
        bool startSession(std::shared_ptr<input::ParsedScript> script, const std::string& file, const fs::path& shotFolder, bool shots,
                          std::string& error);
        bool isReplaying() const { return pendingScript != nullptr || replayRunning() || shotPending(); }
        bool replayRunning() const;
        bool replayStarted() const { return replay != nullptr; }
        const input::ParsedScript* replayScript() const;
        const input::Replay* currentReplay() const { return replay.get(); }
        const std::string& replayFileName() const { return replayPath; }
        // the shots of the last replay ({name, path}, project-relative where possible)
        Json shotsJson() const;
        const std::vector<Shot>& shots() const { return takenShots; }
        // the replay's state (get_play_state input.replaying), null when none
        Json replayingJson() const;
        // the Game view's banner: "Replaying menu.input.json · frame 120/600" ("" when none)
        std::string bannerText() const;
        std::string divergenceText() const;

        // ---- the session (App, PlayTools)
        // App::playStarting, before Project::start runs the scene
        void playStarting();
        // App, every editor frame: the session's start, the recording's stop, shots, writes
        void tick();
        // the game must stay held (the scene not running yet, a shot being read)
        bool wantsHold() const { return holding; }
        // the frame rate this session runs at (an armed recording's or a replay's), 0: none
        int sessionFixedFps() const { return sessionFps; }

        // tests: the writer and the stop of a recording without a running editor
        void finishRecording(std::shared_ptr<input::RecordedInput> recorded, const fs::path& file);
        // tests: true once the write of the last recording ended (written or failed)
        bool pollWrite();

        RecordInput() = default;
        ~RecordInput();

    private:
        Host host;
        Settings currentSettings;
        fs::path settingsPath;
        bool armed = false;

        // the session
        enum class Phase : uint8_t { Idle, Starting, Running };
        Phase phase = Phase::Idle;
        int sessionFps = 0;
        bool holding = false;
        bool heldByUs = false;
        bool recordThisSession = false;
        bool seeded = false;            // PlayData's beforeScripts is this session's seed
        uint32_t recordSeed = 0;
        fs::path recordFile;

        // writes
        std::optional<PendingWrite> pending;
        std::future<std::optional<PendingWrite>> encoding;
        bool writing = false;
        fs::path lastWrittenFile;

        // replay
        std::shared_ptr<input::ParsedScript> pendingScript;
        std::shared_ptr<input::Replay> replay;
        std::string replayPath;
        fs::path shotDir;
        bool takeShots = true;
        std::vector<Shot> takenShots;
        struct ShotRequest {
            std::string name;
            int settledFrames = 0;
            double waitedSeconds = 0.0;
            std::chrono::steady_clock::time_point started;
        };
        std::optional<ShotRequest> shotRequest;
        std::vector<std::future<Shot>> shotWrites;

        bool shotPending() const { return shotRequest.has_value() || !shotWrites.empty(); }
        Project* project() const;
        bool playing() const;
        bool sceneRunning() const;
        void applyHold(bool hold);
        void beginRecording();
        void startReplay();
        void endSession();
        void pollShot();
        void startWrite();
    };

}
