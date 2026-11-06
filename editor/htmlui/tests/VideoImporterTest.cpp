// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The video importer (docs/html-ui/phase-6-video.md §3.12.1): AV1/Opus WebM is copied, other videos
// are converted by FFmpeg (here a stand-in script, so the plumbing is tested without an encoder in
// the build image: it lists encoders and "converts" by copying the AV1 test card, or fails), no
// FFmpeg copies the original with HTMLUI-IMP-0010, a failing one gives HTMLUI-IMP-0011.
// The real FFmpeg conversion runs in the MCP test (tools/mcp/tests/test_html_ui_video.py).

#include "htmlui/VideoImporter.h"

#include "htmlui/compat/DiagnosticCodes.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace jasperengine::editor::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    fs::path videoFixtures(){
        return fs::path(JASPER_EDITOR_TEST_SOURCE_DIR) / "engine/tests/htmlui/fixtures/video";
    }

    class VideoImporterTest : public ::testing::Test {
    protected:
        void SetUp() override{
            root = fs::temp_directory_path() / ("p6video-importer-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "-" +
                                                ::testing::UnitTest::GetInstance()->current_test_info()->name());
            fs::remove_all(root);
            fs::create_directories(root / "in");
            fs::create_directories(root / "out");
            const char* previous = std::getenv("JASPER_FFMPEG");
            saved = previous ? previous : "";
            hadSaved = previous != nullptr;
        }

        void TearDown() override{
            if (hadSaved)
                setenv("JASPER_FFMPEG", saved.c_str(), 1);
            else
                unsetenv("JASPER_FFMPEG");
            VideoImporter::setFfmpegPath("");
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        // a stand-in ffmpeg: -encoders lists `encoders`; a conversion copies the AV1 test card to
        // its last argument (exit 0), or prints an error and exits 1 when `fail`
        fs::path fakeFfmpeg(const std::string& encoders, bool fail, const std::string& filters = ""){
            const fs::path script = root / "ffmpeg";
            std::ofstream out(script);
            out << "#!/bin/sh\n"
                << "case \"$*\" in *-encoders*) printf ' V....D " << encoders << " the encoder\\n'; exit 0;; esac\n"
                << "case \"$*\" in *-filters*) printf '" << filters << "'; exit 0;; esac\n";
            if (fail)
                out << "echo 'in.mp4: Invalid data found when processing input' >&2; exit 1\n";
            else
                out << "for last; do :; done\ncp '" << (videoFixtures() / "testcard.webm").string() << "' \"$last\"\n";
            out.close();
            fs::permissions(script, fs::perms::owner_all);
            return script;
        }

        // a file named like an MP4 (the VP9 test clip: the probe reads it as not playable)
        fs::path input(const std::string& name = "clip.mp4"){
            const fs::path path = root / "in" / name;
            fs::copy_file(videoFixtures() / "testcard-vp9.webm", path, fs::copy_options::overwrite_existing);
            return path;
        }

        fs::path root;
        std::string saved;
        bool hadSaved = false;
    };

}

// two arguments as the human-readable command line quotes them (the platform shell's quoting)
static std::string quotedPair(const std::string& a, const std::string& b){
#ifdef _WIN32
    return "\"" + a + "\" \"" + b + "\"";
#else
    return "'" + a + "' '" + b + "'";
#endif
}

TEST_F(VideoImporterTest, PlayableVideosAreCopied){
    EXPECT_TRUE(VideoImporter::isVideoFile("a.webm"));
    EXPECT_TRUE(VideoImporter::isVideoFile("a.MP4"));
    EXPECT_FALSE(VideoImporter::isVideoFile("a.png"));
    EXPECT_TRUE(VideoImporter::isPlayable(videoFixtures() / "testcard.webm"));
    EXPECT_FALSE(VideoImporter::isPlayable(videoFixtures() / "testcard-vp9.webm"));
    const VideoImportResult result = VideoImporter::importFile(videoFixtures() / "testcard.webm", root / "out", false);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.copied);
    EXPECT_FALSE(result.converted);
    EXPECT_TRUE(fs::exists(root / "out" / "testcard.webm"));
    EXPECT_TRUE(result.diagnostics.empty());
}

TEST_F(VideoImporterTest, ConvertsWithFfmpeg){
    setenv("JASPER_FFMPEG", fakeFfmpeg("libsvtav1", false).c_str(), 1);
    EXPECT_EQ(VideoImporter::findFfmpeg(), (root / "ffmpeg").string());
    EXPECT_EQ(VideoImporter::av1Encoders(VideoImporter::findFfmpeg()), std::vector<std::string>{"libsvtav1"});
    const VideoImportResult result = VideoImporter::importFile(input(), root / "out", false);
    EXPECT_TRUE(result.ok) << result.message;
    EXPECT_TRUE(result.converted);
    EXPECT_EQ(result.output, root / "out" / "clip.webm");
    EXPECT_TRUE(fs::is_regular_file(result.output));
    EXPECT_FALSE(fs::exists(root / "out" / "clip.mp4")) << "the original is not copied";
    EXPECT_EQ(result.encoder, "libsvtav1");
    EXPECT_NE(result.command.find(quotedPair("-c:v", "libsvtav1")), std::string::npos) << result.command;
    EXPECT_NE(result.command.find(quotedPair("-c:a", "libopus")), std::string::npos);
    EXPECT_NE(result.command.find(quotedPair("-f", "webm")), std::string::npos);
    // an existing output is not replaced unless asked
    const VideoImportResult again = VideoImporter::importFile(input(), root / "out", false);
    EXPECT_FALSE(again.ok);
    EXPECT_NE(again.message.find("already exists"), std::string::npos);
    EXPECT_TRUE(VideoImporter::importFile(input(), root / "out", true).ok);
}

TEST_F(VideoImporterTest, LookupOrder){
    // the editor setting (Preferences > External Tools > FFmpeg) when JASPER_FFMPEG is not set
    const std::string fake = fakeFfmpeg("libsvtav1", false).string();
    unsetenv("JASPER_FFMPEG");
    VideoImporter::setFfmpegPath(fake);
    EXPECT_EQ(VideoImporter::findFfmpeg(), fake);
    // a path being edited, without changing the set one
    const std::string missing = (root / "not-there").string();
    std::string why;
    const std::string withMissing = VideoImporter::findFfmpeg(&why, &missing);
    EXPECT_NE(withMissing, fake);
    if (withMissing.empty()){
        EXPECT_NE(why.find("not-there"), std::string::npos) << why;
    }
    EXPECT_EQ(VideoImporter::findFfmpeg(), fake);
    // JASPER_FFMPEG wins over the setting, also when it names nothing
    setenv("JASPER_FFMPEG", "", 1);
    EXPECT_EQ(VideoImporter::findFfmpeg(), "");
}

TEST_F(VideoImporterTest, EncoderPreference){
    setenv("JASPER_FFMPEG", fakeFfmpeg("libaom-av1", false).c_str(), 1);
    const VideoImportResult result = VideoImporter::importFile(input(), root / "out", false);
    EXPECT_TRUE(result.converted);
    EXPECT_EQ(result.encoder, "libaom-av1");
    EXPECT_NE(result.command.find(quotedPair("-c:v", "libaom-av1")), std::string::npos);
}

TEST_F(VideoImporterTest, WithoutFfmpegTheOriginalIsCopiedAndDiagnosed){
    setenv("JASPER_FFMPEG", (root / "no-such-ffmpeg").c_str(), 1);
    EXPECT_EQ(VideoImporter::findFfmpeg(), "");
    const VideoImportResult result = VideoImporter::importFile(input(), root / "out", false);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.copied);
    EXPECT_TRUE(fs::exists(root / "out" / "clip.mp4"));
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, hc::codes::VideoNotConverted);
    EXPECT_NE(result.diagnostics[0].suggestion.find("ffmpeg"), std::string::npos) << "the command to run";
    // an FFmpeg without an AV1 encoder: the same
    setenv("JASPER_FFMPEG", fakeFfmpeg("libx264", false).c_str(), 1);
    const VideoImportResult noEncoder = VideoImporter::importFile(input("other.mov"), root / "out", false);
    ASSERT_EQ(noEncoder.diagnostics.size(), 1u);
    EXPECT_EQ(noEncoder.diagnostics[0].code, hc::codes::VideoNotConverted);
    EXPECT_NE(noEncoder.message.find("no AV1 encoder"), std::string::npos) << noEncoder.message;
}

TEST_F(VideoImporterTest, HdrInputIsToneMapped){
    // an ffprobe next to the ffmpeg says the input is PQ
    const fs::path ffprobe = root / "ffprobe";
    {
        std::ofstream out(ffprobe);
        out << "#!/bin/sh\necho smpte2084\n";
    }
    fs::permissions(ffprobe, fs::perms::owner_all);
    setenv("JASPER_FFMPEG", fakeFfmpeg("libsvtav1", false, " ... zscale            V->V       Apply resizing\\n ... tonemap           V->V       Conversion\\n").c_str(), 1);
    EXPECT_TRUE(VideoImporter::isHdrInput(VideoImporter::findFfmpeg(), input()));
    EXPECT_TRUE(VideoImporter::hasToneMapping(VideoImporter::findFfmpeg()));
    const VideoImportResult result = VideoImporter::importFile(input(), root / "out", true);
    EXPECT_TRUE(result.ok && result.hdr && result.toneMapped) << result.message;
    EXPECT_NE(result.command.find("tonemap=tonemap=hable"), std::string::npos) << result.command;
    EXPECT_NE(result.message.find("HDR tone-mapped"), std::string::npos) << result.message;
    // an FFmpeg without zimg: converted as it is, and said so
    setenv("JASPER_FFMPEG", fakeFfmpeg("libsvtav1", false, " ... scale  V->V  Scale\\n").c_str(), 1);
    const VideoImportResult plain = VideoImporter::importFile(input(), root / "out", true);
    EXPECT_TRUE(plain.ok && plain.hdr && !plain.toneMapped) << plain.message;
    EXPECT_EQ(plain.command.find("tonemap"), std::string::npos) << plain.command;
    EXPECT_NE(plain.message.find("washed out"), std::string::npos) << plain.message;
    // SDR input (no ffprobe answer): no tone mapping
    fs::remove(ffprobe);
    const VideoImportResult sdr = VideoImporter::importFile(input(), root / "out", true);
    EXPECT_FALSE(sdr.hdr);
    EXPECT_EQ(sdr.command.find("zscale"), std::string::npos);
}

TEST_F(VideoImporterTest, AFailingConversion){
    setenv("JASPER_FFMPEG", fakeFfmpeg("libsvtav1", true).c_str(), 1);
    const VideoImportResult result = VideoImporter::importFile(input(), root / "out", false);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.converted);
    EXPECT_FALSE(fs::exists(root / "out" / "clip.webm"));
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code, hc::codes::VideoConversionFailed);
    EXPECT_NE(result.diagnostics[0].message.find("Invalid data"), std::string::npos) << result.diagnostics[0].message;
}
