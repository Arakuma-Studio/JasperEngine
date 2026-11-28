// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Record Input's editor parts (docs/specs/input-recorder-and-input-scripts.md R13, R15, R37, R39):
// its settings file, where a recording goes and its name, the Inspector's summary, the toolbar
// button's states, and a failed write that keeps the recording for a retry (AC16).

#include "command/type/WriteFilesCmd.h"
#include "recorder/InputRecordingInspector.h"
#include "recorder/RecordInput.h"

#include "input/InputRecorder.h"
#include "io/AtomicFile.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

namespace RecordInputTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::recordinput;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

TEST(RecordInput, SettingsRoundTripAndClamp) {
    TestTempDir dir("record-input-settings");
    const fs::path file = settingsFile(dir.path());
    Settings defaults;
    std::string error;
    ASSERT_TRUE(loadSettings(file, defaults, error)) << error;   // no file: the defaults
    EXPECT_EQ(defaults.frameRate, 60);
    EXPECT_EQ(defaults.checkpointFrames, 60);
    EXPECT_TRUE(defaults.probes.empty());

    Settings saved;
    saved.frameRate = 30;
    saved.checkpointFrames = 0;
    saved.probes = {"Player", "Enemies/Bat (3)"};
    ASSERT_TRUE(saveSettings(file, saved, error)) << error;
    Settings loaded;
    ASSERT_TRUE(loadSettings(file, loaded, error)) << error;
    EXPECT_EQ(loaded, saved);

    TestTempDir::write(file, "frame_rate: 1000\ncheckpoint_frames: -5\n");
    ASSERT_TRUE(loadSettings(file, loaded, error)) << error;
    EXPECT_EQ(loaded.frameRate, 240);
    EXPECT_EQ(loaded.checkpointFrames, 0);
}

TEST(RecordInput, RecordingsGoToTheTestRootWhenThereIsOne) {
    TestTempDir dir("record-input-folder");
    EXPECT_EQ(recordingsFolder(dir.path()), dir.path() / "Recordings" / "Input");
    // Tests/ without its marker is not a test root
    fs::create_directories(dir.path() / "Tests");
    EXPECT_EQ(recordingsFolder(dir.path()), dir.path() / "Recordings" / "Input");
    TestTempDir::write(dir.path() / "Tests" / "tests.yaml", "version: 1\n");
    EXPECT_EQ(recordingsFolder(dir.path()), dir.path() / "Tests" / "Input");

    std::tm when{};
    when.tm_year = 2026 - 1900;
    when.tm_mon = 10;
    when.tm_mday = 21;
    when.tm_hour = 9;
    when.tm_min = 5;
    when.tm_sec = 7;
    EXPECT_EQ(recordingFileName("Level 1", when), "Level 1-20261121-090507.input.json");
    EXPECT_EQ(recordingFileName("a/b:c", when), "a_b_c-20261121-090507.input.json");
    EXPECT_EQ(fileStem("Tests/Input/menu.input.json"), "menu");
    EXPECT_TRUE(isInputFile("Recordings/Input/Level1-20261121-090507.input.json"));
    EXPECT_FALSE(isInputFile("assets/data.json"));
}

TEST(RecordInput, SummaryCountsEventsByType) {
    const std::string text = R"({"format": "jasperengine-input", "version": 1, "kind": "recording", "start": "play", "fixed_fps": 30,
        "seed": 7, "scene": "scenes/Main.scene", "platform": "linux-x64", "frames": 90, "truncated": true,
        "probes": ["Player"], "checkpoints": [{"frame": 30, "hash": "0000000000000001", "probes": [[0, 0, 0]], "values": {}}],
        "events": [{"frame": 0, "type": "key", "key": "right", "action": "down"},
                   {"frame": 10, "type": "key", "key": "right", "action": "up"},
                   {"frame": 12, "type": "touch", "action": "start", "pointer": 0, "x": 0.5, "y": 0.5},
                   {"frame": 20, "type": "gamepad", "action": "connect", "gamepad": 0, "name": "Pad"}]})";
    const FileSummary s = summarize(text);
    ASSERT_TRUE(s.ok) << s.error;
    EXPECT_EQ(s.kind, "recording");
    EXPECT_EQ(s.start, "play");
    EXPECT_EQ(s.scene, "scenes/Main.scene");
    EXPECT_EQ(s.fixedFps, 30);
    EXPECT_EQ(s.frames, 90u);
    EXPECT_EQ(s.events, 4u);
    EXPECT_EQ(s.eventsByType.at("key"), 2u);
    EXPECT_EQ(s.eventsByType.at("touch"), 1u);
    EXPECT_EQ(s.eventsByType.at("gamepad"), 1u);
    EXPECT_EQ(s.checkpoints, 1u);
    EXPECT_EQ(s.platform, "linux-x64");
    EXPECT_TRUE(s.truncated);
    EXPECT_TRUE(s.hasSeed);
    EXPECT_EQ(s.seed, 7u);

    const FileSummary broken = summarize(R"({"format": "jasperengine-input", "version": 9, "kind": "script", "events": []})");
    EXPECT_FALSE(broken.ok);
    EXPECT_NE(broken.error.find("newer"), std::string::npos) << broken.error;

    int width = 0, height = 0;
    std::string error;
    ASSERT_TRUE(parseResolution("1290x2796", width, height, error)) << error;
    EXPECT_EQ(width, 1290);
    EXPECT_EQ(height, 2796);
    EXPECT_FALSE(parseResolution("huge", width, height, error));
    EXPECT_NE(error.find("WxH"), std::string::npos) << error;
}

TEST(RecordInput, ButtonArmsBeforePlay) {
    RecordInput input;   // no project: never playing
    EXPECT_EQ(input.buttonState(), RecordInput::Button::Off);
    ASSERT_TRUE(input.canToggle());
    input.toggle();
    EXPECT_EQ(input.buttonState(), RecordInput::Button::Armed);
    EXPECT_TRUE(input.isArmed());
    input.toggle();
    EXPECT_EQ(input.buttonState(), RecordInput::Button::Off);
}

TEST(InputRecordingWrite, FailedWriteKeepsTheBuffer) {
    TestTempDir dir("record-input-write");
    const fs::path file = dir.path() / "Recordings" / "Input" / "Main-20261121-090507.input.json";

    // a recording of two key events
    input::RecordOptions options;
    options.header.kind = input::ScriptKind::Recording;
    options.header.fromPlayStart = true;
    options.header.space = input::Space::Normalized;
    options.checkpointFrames = 0;
    std::string error;
    ASSERT_TRUE(input::InputRecorder::begin(options, error)) << error;
    input::InputRecorder::record(input::keyRecord(input::RecordKind::KeyDown, 65, false, 0));
    input::InputRecorder::record(input::keyRecord(input::RecordKind::KeyUp, 65, false, 0));
    std::shared_ptr<input::RecordedInput> recorded = input::InputRecorder::end();
    ASSERT_NE(recorded, nullptr);

    RecordInput recordInput;
    RecordInput::Host host;
    // the editor's write: one undo step (a command no project history holds here)
    host.writer = [](const fs::path& path, const std::string& text, std::string& why) {
        WriteFilesCmd command({{path, text}}, nullptr);
        if (command.execute()) return true;
        why = command.failure();
        return false;
    };
    recordInput.setHost(host);

    // S03's fault injector: the publish fails
    io::setAtomicFileFaultInjector([](std::string_view step, const fs::path&) {
        return step == "rename" ? std::make_error_code(std::errc::io_error) : std::error_code();
    });
    recordInput.finishRecording(recorded, file);
    while (!recordInput.pollWrite()) {
    }
    io::setAtomicFileFaultInjector(nullptr);
    EXPECT_TRUE(recordInput.writePendingFailed());
    EXPECT_NE(recordInput.writeError().find(file.filename().string()), std::string::npos) << recordInput.writeError();
    EXPECT_FALSE(fs::exists(file));
    EXPECT_TRUE(recordInput.lastWritten().empty());

    // the retry writes the same recording
    recordInput.retryWrite();
    EXPECT_FALSE(recordInput.writePendingFailed()) << recordInput.writeError();
    EXPECT_EQ(recordInput.lastWritten(), file);
    input::ParsedScript script;
    ASSERT_TRUE(input::parseScript(TestTempDir::read(file), script, error)) << error;
    EXPECT_EQ(script.header.kind, input::ScriptKind::Recording);
    EXPECT_EQ(script.events, 2u);
}

}  // namespace RecordInputTest_cpp
