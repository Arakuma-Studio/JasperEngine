// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S99 R13-R16 (docs/specs/S99-agent-feedback-area-lights.md AC4): the cost model of each job kind,
// the history's median and clamp, a corrupt or missing history file, and the estimate and report
// a job carries.

#include <gtest/gtest.h>

#include "TempDir.h"
#include "mcp/JobEstimate.h"
#include "mcp/McpJobs.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace JobEstimateTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor::mcp;

namespace {

    std::string jobEstimateReadText(const std::filesystem::path& file) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    void jobEstimateWriteText(const std::filesystem::path& file, const std::string& text) {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }

}

TEST(JobEstimate, RecordModelGrowsWithFramesAndResolution) {
    // entering Play, then a frame's readback and encode per frame
    EXPECT_NEAR(jobmodel::recordSeconds(90, 640, 360, false), 1.5 + 90 * (0.01 + 640.0 * 360.0 * 4e-8), 1e-9);
    EXPECT_NEAR(jobmodel::recordSeconds(0, 1920, 1080, false), 1.5, 1e-9);
    EXPECT_GT(jobmodel::recordSeconds(180, 640, 360, false), jobmodel::recordSeconds(90, 640, 360, false));
    EXPECT_GT(jobmodel::recordSeconds(90, 1920, 1080, false), jobmodel::recordSeconds(90, 640, 360, false));
    // a movie also pipes through FFmpeg and decodes once
    EXPECT_GT(jobmodel::recordSeconds(90, 1920, 1080, true), jobmodel::recordSeconds(90, 1920, 1080, false));
}

TEST(JobEstimate, BuildModelCountsFilesBytesAndTheCompile) {
    const uint64_t mb = 1024 * 1024;
    EXPECT_NEAR(jobmodel::buildSeconds(jobmodel::BuildKind::Prebuilt, 100, 40 * mb), 3.0 + 0.4 + 1.0, 1e-9);
    EXPECT_NEAR(jobmodel::buildSeconds(jobmodel::BuildKind::Source, 100, 200 * mb), 2.0 + 0.2 + 1.0, 1e-9);
    EXPECT_GT(jobmodel::buildSeconds(jobmodel::BuildKind::Prebuilt, 1000, 0), jobmodel::buildSeconds(jobmodel::BuildKind::Prebuilt, 10, 0));
    EXPECT_GT(jobmodel::buildSeconds(jobmodel::BuildKind::Prebuilt, 10, 400 * mb), jobmodel::buildSeconds(jobmodel::BuildKind::Prebuilt, 10, 0));
    // compiling costs minutes; the web's toolchain more
    EXPECT_GT(jobmodel::buildSeconds(jobmodel::BuildKind::CompiledDesktop, 10, 0), 50.0 + jobmodel::buildSeconds(jobmodel::BuildKind::Prebuilt, 10, 0));
    EXPECT_GT(jobmodel::buildSeconds(jobmodel::BuildKind::CompiledWeb, 10, 0), jobmodel::buildSeconds(jobmodel::BuildKind::CompiledDesktop, 10, 0));
}

TEST(JobEstimate, ImpostorModelScalesWithCountViewsAndResolution) {
    EXPECT_DOUBLE_EQ(jobmodel::impostorSeconds(0, 8, 256), 0.0);
    // 8 views of 256 px: a 4 x 2 atlas
    const double one = 0.5 + 2.0 * 8 * (1.0 / 60.0 + 256.0 * 256.0 * 1e-8) + 2.0 * (8.0 * 256.0 * 256.0) * 3e-8;
    EXPECT_NEAR(jobmodel::impostorSeconds(1, 8, 256), one, 1e-9);
    EXPECT_NEAR(jobmodel::impostorSeconds(3, 8, 256), 3.0 * one, 1e-9);
    EXPECT_GT(jobmodel::impostorSeconds(1, 16, 256), one);
    EXPECT_GT(jobmodel::impostorSeconds(1, 8, 512), one);
}

TEST(JobEstimate, TitleExportModelScalesWithLayersSizeAndOutputs) {
    EXPECT_NEAR(jobmodel::titleExportSeconds(10, 1000, 500, 1), 0.2 + 0.1 + 10 * 500000.0 * 2e-8, 1e-9);
    EXPECT_GT(jobmodel::titleExportSeconds(20, 1000, 500, 1), jobmodel::titleExportSeconds(10, 1000, 500, 1));
    EXPECT_GT(jobmodel::titleExportSeconds(10, 2000, 1000, 1), jobmodel::titleExportSeconds(10, 1000, 500, 1));
    EXPECT_GT(jobmodel::titleExportSeconds(10, 1000, 500, 3), jobmodel::titleExportSeconds(10, 1000, 500, 1));
}

TEST(JobEstimate, RemainingTimeComesFromTheEstimateThenTheProgress) {
    // no progress known: what the estimate has left, never negative
    EXPECT_DOUBLE_EQ(jobRemainingSeconds(60.0, 20.0, -1.0), 40.0);
    EXPECT_DOUBLE_EQ(jobRemainingSeconds(60.0, 90.0, -1.0), 0.0);
    // half done after 20 s of a 60 s estimate: between the estimate's 40 and the progress's 20
    EXPECT_DOUBLE_EQ(jobRemainingSeconds(60.0, 20.0, 0.5), 0.5 * 40.0 + 0.5 * 20.0);
    // nearly done: the progress decides
    EXPECT_NEAR(jobRemainingSeconds(10.0, 100.0, 0.99), 0.99 * 100.0 * 0.01 / 0.99, 1e-9);
    EXPECT_DOUBLE_EQ(jobRemainingSeconds(60.0, 20.0, 1.0), 0.0);
}

TEST(JobEstimate, DurationsReadAsSecondsMinutesHours) {
    EXPECT_EQ(formatJobDuration(12.4), "12 s");
    EXPECT_EQ(formatJobDuration(-3.0), "0 s");
    EXPECT_EQ(formatJobDuration(185.0), "3 min");
    EXPECT_EQ(formatJobDuration(3900.0), "1 h 05 min");
}

TEST(JobHistory, EstimateIsTheModelUntilAKindHasCompletions) {
    JobHistory history;
    const JobEstimate e = history.estimate("record_game:png", 20.0, 1234);
    EXPECT_EQ(e.basis, "model");
    EXPECT_EQ(e.confidence, "low");
    EXPECT_DOUBLE_EQ(e.seconds, 20.0);
    EXPECT_EQ(e.bytes, 1234u);
    EXPECT_EQ(e.toJson()["bytes"], 1234);
    // no bytes key without a size estimate
    EXPECT_FALSE(history.estimate("bake_impostors", 5.0, 0).toJson().contains("bytes"));
}

TEST(JobHistory, EstimateUsesTheMedianRatio) {
    JobHistory history;
    history.add("build_project:linux:prebuilt", 20.0, 10.0);   // 2
    history.add("build_project:linux:prebuilt", 30.0, 10.0);   // 3
    history.add("build_project:linux:prebuilt", 100.0, 10.0);  // 10 (an outlier the median ignores)
    double ratio = 0.0;
    ASSERT_TRUE(history.ratio("build_project:linux:prebuilt", ratio));
    EXPECT_DOUBLE_EQ(ratio, 3.0);
    const JobEstimate e = history.estimate("build_project:linux:prebuilt", 7.0, 0);
    EXPECT_EQ(e.basis, "history");
    EXPECT_DOUBLE_EQ(e.seconds, 21.0);
    EXPECT_DOUBLE_EQ(e.modelSeconds, 7.0);
    EXPECT_EQ(e.samples, 3u);
    EXPECT_EQ(e.confidence, "medium");
    // an even count: the mean of the middle two
    history.add("build_project:linux:prebuilt", 40.0, 10.0);   // 4
    ASSERT_TRUE(history.ratio("build_project:linux:prebuilt", ratio));
    EXPECT_DOUBLE_EQ(ratio, 3.5);
    // other kinds are their own
    EXPECT_FALSE(history.ratio("build_project:web:compiled", ratio));
}

TEST(JobHistory, RatioIsClampedToAQuarterAndFour) {
    JobHistory slow, fast;
    for (int i = 0; i < 5; i++) {
        slow.add("export_title:bake", 100.0, 1.0);
        fast.add("export_title:bake", 1.0, 100.0);
    }
    double ratio = 0.0;
    ASSERT_TRUE(slow.ratio("export_title:bake", ratio));
    EXPECT_DOUBLE_EQ(ratio, JobHistory::kMaxRatio);
    EXPECT_DOUBLE_EQ(slow.estimate("export_title:bake", 2.0, 0).seconds, 8.0);
    // a model that far off is not trusted, however steady
    EXPECT_EQ(slow.estimate("export_title:bake", 2.0, 0).confidence, "low");
    ASSERT_TRUE(fast.ratio("export_title:bake", ratio));
    EXPECT_DOUBLE_EQ(ratio, JobHistory::kMinRatio);
    EXPECT_DOUBLE_EQ(fast.estimate("export_title:bake", 2.0, 0).seconds, 0.5);
}

TEST(JobHistory, SteadyHistoryGivesHighConfidence) {
    JobHistory history;
    for (double seconds : {11.0, 12.0, 12.5, 13.0, 12.0}) history.add("record_game:mp4", seconds, 10.0);
    EXPECT_EQ(history.estimate("record_game:mp4", 10.0, 0).confidence, "high");
    // a scattered one only medium
    JobHistory scattered;
    for (double seconds : {5.0, 30.0, 12.0, 2.0, 25.0}) scattered.add("record_game:mp4", seconds, 10.0);
    EXPECT_EQ(scattered.estimate("record_game:mp4", 10.0, 0).confidence, "medium");
}

TEST(JobHistory, KeepsTheLastSixteenPerKind) {
    JobHistory history;
    for (int i = 1; i <= 20; i++) history.add("bake_impostors", static_cast<double>(i), 1.0);
    const std::vector<JobHistory::Sample>& samples = history.samples("bake_impostors");
    ASSERT_EQ(samples.size(), JobHistory::kKeep);
    EXPECT_DOUBLE_EQ(samples.front().seconds, 5.0);
    EXPECT_DOUBLE_EQ(samples.back().seconds, 20.0);
    // nonsense measures are not kept
    history.add("bake_impostors", -1.0, 1.0);
    history.add("bake_impostors", 1.0, 0.0);
    history.add("bake_impostors", std::nan(""), 1.0);
    EXPECT_DOUBLE_EQ(history.samples("bake_impostors").back().seconds, 20.0);
}

TEST(JobHistory, SerializedTextParsesBack) {
    JobHistory history;
    history.add("record_game:png", 12.5, 10.0);
    history.add("build_project:source", 3.0, 2.0);
    const JobHistory back = JobHistory::parse(history.serialize());
    ASSERT_EQ(back.samples("record_game:png").size(), 1u);
    EXPECT_DOUBLE_EQ(back.samples("record_game:png")[0].seconds, 12.5);
    EXPECT_DOUBLE_EQ(back.samples("record_game:png")[0].model, 10.0);
    EXPECT_EQ(back.samples("build_project:source").size(), 1u);
}

TEST(JobHistory, CorruptTextReadsAsEmpty) {
    for (const char* text : {"", "not json", "{\"kinds\": [1, 2]}", "[]", "{\"kinds\": {\"a\": 7}}", "{\"version\": 1}"}) {
        const JobHistory history = JobHistory::parse(text);
        EXPECT_TRUE(history.samples("a").empty()) << text;
        EXPECT_EQ(history.estimate("a", 3.0, 0).basis, "model") << text;
    }
    // the readable samples of a damaged file are kept, the others dropped
    const JobHistory partial = JobHistory::parse(
        R"({"kinds": {"a": [{"seconds": 4, "model": 2}, {"seconds": "x", "model": 2}, 5, {"model": 1}, {"seconds": 6, "model": -1}]}})");
    ASSERT_EQ(partial.samples("a").size(), 1u);
    EXPECT_DOUBLE_EQ(partial.samples("a")[0].seconds, 4.0);
}

TEST(JobHistory, MissingOrCorruptFileStartsEmptyAndIsReplaced) {
    jasperengine::test::TempDir dir;
    const std::filesystem::path file = JobHistory::fileFor(dir.path());
    EXPECT_EQ(file, dir.path() / ".jasperengine" / "user" / "job-history.json");
    EXPECT_TRUE(JobHistory::load(file).samples("record_game:png").empty());
    // the first completion creates the folder and the file
    ASSERT_TRUE(JobHistory::record(file, "record_game:png", 9.0, 6.0));
    ASSERT_EQ(JobHistory::load(file).samples("record_game:png").size(), 1u);
    // a corrupt file reads as empty, and the next completion writes a good one over it
    jobEstimateWriteText(file, "{\"kinds\": {\"record_game:png\": [{\"seconds\": 9, \"mod");
    EXPECT_TRUE(JobHistory::load(file).samples("record_game:png").empty());
    ASSERT_TRUE(JobHistory::record(file, "record_game:png", 3.0, 6.0));
    const JobHistory back = JobHistory::load(file);
    ASSERT_EQ(back.samples("record_game:png").size(), 1u);
    EXPECT_DOUBLE_EQ(back.samples("record_game:png")[0].seconds, 3.0);
    EXPECT_FALSE(JobHistory::parse(jobEstimateReadText(file)).samples("record_game:png").empty());
}

TEST(McpJob, EstimateAndRemainingTimeShowWhileItRuns) {
    McpJob job("job-1", "record", "Record 90 frame(s) to png");
    EXPECT_FALSE(job.toJson().contains("estimate"));
    JobEstimate estimate;
    estimate.seconds = 3600.0;
    estimate.bytes = 4096;
    job.setEstimate(estimate);
    const Json running = job.toJson();
    ASSERT_TRUE(running.contains("estimate"));
    EXPECT_EQ(running["estimate"]["seconds"], 3600.0);
    EXPECT_EQ(running["estimate"]["bytes"], 4096);
    EXPECT_EQ(running["estimate"]["basis"], "model");
    EXPECT_EQ(running["estimate"]["confidence"], "low");
    ASSERT_TRUE(running.contains("remaining_seconds"));
    EXPECT_GT(running["remaining_seconds"].get<double>(), 3500.0);
    EXPECT_LE(running["remaining_seconds"].get<double>(), 3600.0);
    job.succeed("done", Json::object());
    EXPECT_FALSE(job.toJson().contains("remaining_seconds"));
}

TEST(McpJob, CompletionReportsTheMeasureAndTheFallbacks) {
    McpJob job("job-2", "build", "Build Linux (Release)");
    JobEstimate estimate;
    estimate.seconds = 10.0;
    job.setEstimate(estimate);
    job.addFallback({"method", "prebuilt", "compiled", "C++ scripts (Player.cpp)"});
    job.addFallback({"method", "prebuilt", "compiled", "C++ scripts (Player.cpp)"});
    job.addFallback({"texture_format", "BC7 RDO 1 + Zstd", "RGBA8 + Zstd", "1 texture(s) for Desktop are not a multiple of 4 (first: ui.png)"});
    job.succeed("Built.", Json{{"mode", "player"}});
    const Json result = job.result();
    EXPECT_EQ(result["mode"], "player");
    ASSERT_TRUE(result.contains("report"));
    const Json& report = result["report"];
    EXPECT_EQ(report["estimated_seconds"], 10.0);
    EXPECT_GE(report["seconds"].get<double>(), 0.0);
    EXPECT_TRUE(report["ratio"].is_number());
    ASSERT_EQ(report["fallbacks"].size(), 2u);
    EXPECT_EQ(report["fallbacks"][0], (Json{{"step", "method"}, {"from", "prebuilt"}, {"to", "compiled"}, {"reason", "C++ scripts (Player.cpp)"}}));
    EXPECT_EQ(report["fallbacks"][1]["step"], "texture_format");
    // a failure reports too
    McpJob failed("job-3", "build", "Build");
    failed.setEstimate(estimate);
    failed.fail("Build failed.", Json::object());
    EXPECT_TRUE(failed.result().contains("report"));
    // a job without an estimate keeps its result as it was
    McpJob plain("job-4", "shaders", "Generate shaders");
    plain.addFallback({"a", "b", "c", "d"});
    plain.succeed("ok", Json{{"x", 1}});
    EXPECT_EQ(plain.result(), (Json{{"x", 1}}));
}

TEST(McpJob, SuccessAddsItsMeasureToTheHistory) {
    jasperengine::test::TempDir dir;
    const std::filesystem::path file = JobHistory::fileFor(dir.path());
    McpJob job("job-5", "record", "Record");
    job.estimateFor(dir.path(), "record_game:png", 5.0, 0);
    JobEstimate estimate;
    ASSERT_TRUE(job.estimate(estimate));
    EXPECT_EQ(estimate.basis, "model");
    job.succeed("ok", Json::object());
    ASSERT_EQ(JobHistory::load(file).samples("record_game:png").size(), 1u);
    EXPECT_DOUBLE_EQ(JobHistory::load(file).samples("record_game:png")[0].model, 5.0);
    // the next job of the kind is estimated from it
    McpJob next("job-6", "record", "Record");
    next.estimateFor(dir.path(), "record_game:png", 5.0, 0);
    ASSERT_TRUE(next.estimate(estimate));
    EXPECT_EQ(estimate.basis, "history");
    EXPECT_EQ(estimate.samples, 1u);
    // a failure adds nothing
    next.fail("no", Json::object());
    EXPECT_EQ(JobHistory::load(file).samples("record_game:png").size(), 1u);
    // what actually ran is what the history records (a build that compiled instead)
    McpJob build("job-7", "build", "Build");
    build.estimateFor(dir.path(), "build_project:linux:prebuilt", 4.0, 0);
    build.setHistoryModel("build_project:linux:compiled", 64.0);
    build.succeed("ok", Json::object());
    const JobHistory history = JobHistory::load(file);
    EXPECT_TRUE(history.samples("build_project:linux:prebuilt").empty());
    ASSERT_EQ(history.samples("build_project:linux:compiled").size(), 1u);
    EXPECT_DOUBLE_EQ(history.samples("build_project:linux:compiled")[0].model, 64.0);
}

TEST(McpJob, CorruptHistoryFileNeverFailsAJob) {
    jasperengine::test::TempDir dir;
    const std::filesystem::path file = JobHistory::fileFor(dir.path());
    jobEstimateWriteText(file, "\x01\x02 garbage {");
    McpJob job("job-8", "edit_terrain", "Bake impostors");
    job.estimateFor(dir.path(), "bake_impostors", 2.0, 0);
    JobEstimate estimate;
    ASSERT_TRUE(job.estimate(estimate));
    EXPECT_EQ(estimate.basis, "model");
    EXPECT_DOUBLE_EQ(estimate.seconds, 2.0);
    job.succeed("Baked 1 impostor(s).", Json::object());
    EXPECT_EQ(job.status(), JobStatus::Succeeded);
    EXPECT_EQ(JobHistory::load(file).samples("bake_impostors").size(), 1u);
}

}  // namespace JobEstimateTest_cpp
