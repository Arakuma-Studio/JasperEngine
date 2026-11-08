// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The committed starter library's analysis.json (docs/specs/S36-sound-library.md R17, R20) is what
// `audio-analyze` gives on every CPU: the clips below once read differently on arm64 (fused
// multiply-adds in the decode, the meter and the tempo grid), each in a field rounded near a step.
// The MCP suite's test_d5_analysis_is_current compares every clip, on Linux only.

#include <gtest/gtest.h>

#include "audio/SoundLibrary.h"
#include "audio/SoundLibraryCli.h"
#include "util/FileUtils.h"

#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace jasperengine::editor;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

// what analysis.json leaves out of a CLI item (tools/sound-library/build_library.py ANALYSIS_EXCLUDED)
const char* const EXCLUDED[] = {"n", "path", "type", "labels", "licence", "notes", "import"};

Json committedItem(const Json& item) {
    Json out = item;
    for (const char* key : EXCLUDED) out.erase(key);
    out["format"].erase("load_type");
    return out;
}

} // namespace

TEST(AudioAnalysisLibrary, ClipsReadAsTheCommittedAnalysis) {
    const fs::path library = fs::path(JASPER_TEST_REPO_DIR) / "editor" / "resources" / "sound-library" / "library";
    std::ifstream in(library / SoundLibrary::ANALYSIS);
    ASSERT_TRUE(in) << (library / SoundLibrary::ANALYSIS);
    const Json committed = Json::parse(in);

    const std::vector<std::string> ids = {
        "ui/kenney_error_007",                // defects.aliasing.fold
        "stinger/kenney_jingle_8bit_11",      // defects.aliasing.fold
        "tick/fs_386208_ratchet_drill_3",     // bands_db
        "pop/fs_458886_cork_flask_2",         // loop.seam_step_db
        "ambience/fs_463703_telephone_drone", // music
        "music/fs_648075_8bit_music_loop_2a", // music
    };
    std::vector<std::string> args = {"audio-analyze", "--json"};
    for (const std::string& id : ids) args.push_back(SoundLibrary::PREFIX + id);
    std::vector<char*> argv;
    for (std::string& arg : args) argv.push_back(arg.data());

    SoundLibrary::get().reopen(library);
    testing::internal::CaptureStdout();
    const int code = SoundLibraryCli::runAudioAnalyze((int)argv.size(), argv.data(), "jasperengine-editor");
    const std::string out = testing::internal::GetCapturedStdout();
    SoundLibrary::get().reopen(FileUtils::getSoundLibraryDir());
    ASSERT_EQ(code, 0) << out;

    const Json result = Json::parse(out);
    ASSERT_EQ(result["errors"], Json::array()) << result["errors"].dump();
    ASSERT_EQ(result["items"].size(), ids.size());
    for (size_t i = 0; i < ids.size(); i++) {
        const Json& want = committed["items"][ids[i]];
        const Json got = committedItem(result["items"][i]);
        for (const auto& [key, value] : want.items()) {
            EXPECT_EQ(got.value(key, Json()), value) << ids[i] << ": " << key;
        }
        EXPECT_EQ(got.size(), want.size()) << ids[i] << ": the committed item's keys";
    }
}
