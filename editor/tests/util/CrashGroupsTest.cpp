// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The C++ grouping (editor/util/CrashGroups) against the fixtures tools/crash/symbolicate.py --group
// passes too (tools/crash/fixtures/group/expected.json, shipped-game-diagnostics R25, AC17)

#include "TempDir.h"
#include "util/CrashGroups.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace CrashGroupsTest_cpp {

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::test;
namespace fs = std::filesystem;

namespace {

    nlohmann::json readExpected(const fs::path& fixtures) {
        std::ifstream in(fixtures / "expected.json", std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return nlohmann::json::parse(text.str(), nullptr, false);
    }

    int64_t sessionSeconds(const std::string& id) {
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
        if (std::sscanf(id.c_str(), "%4d%2d%2dT%2d%2d%2dZ", &year, &month, &day, &hour, &minute, &second) != 6) return 0;
        // days from the civil date (no timegm on every platform)
        const int y = year - (month <= 2 ? 1 : 0);
        const int era = (y >= 0 ? y : y - 399) / 400;
        const int yoe = y - era * 400;
        const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
        const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        const int64_t days = static_cast<int64_t>(era) * 146097 + doe - 719468;
        return days * 86400 + hour * 3600 + minute * 60 + second;
    }

    // as the Python test: every file of a folder dated its session time + the offset
    void setTime(const fs::path& file, double unixSeconds) {
        using namespace std::chrono;
        const auto wanted = system_clock::time_point(duration_cast<system_clock::duration>(duration<double>(unixSeconds)));
        const auto fileTime = fs::file_time_type::clock::now() + duration_cast<fs::file_time_type::duration>(wanted - system_clock::now());
        std::error_code ec;
        fs::last_write_time(file, fileTime, ec);
        ASSERT_FALSE(ec) << file << ": " << ec.message();
    }

    std::string isoUtc(int64_t unixSeconds) {
        const std::time_t when = static_cast<std::time_t>(unixSeconds);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &when);
#else
        gmtime_r(&when, &tm);
#endif
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return text;
    }

}

TEST(CrashGroups, MatchesTheSharedFixtures) {
    const fs::path fixtures = fs::path(JASPER_TEST_REPO_DIR) / "tools" / "crash" / "fixtures" / "group";
    const nlohmann::json expected = readExpected(fixtures);
    ASSERT_TRUE(expected.is_object()) << fixtures / "expected.json";

    TempDir temp;
    const fs::path reports = temp.path() / "reports";
    fs::copy(fixtures, reports, fs::copy_options::recursive);
    fs::remove(reports / "expected.json");
    const double offset = expected["mtime_offset_seconds"].get<double>();
    for (const fs::directory_entry& folder : fs::directory_iterator(reports)) {
        const int64_t session = sessionSeconds(folder.path().filename().string());
        ASSERT_NE(session, 0) << folder.path();
        for (const fs::directory_entry& file : fs::directory_iterator(folder.path())) setTime(file.path(), static_cast<double>(session) + offset);
    }

    const std::vector<diag::crash::ReportInfo> listed = diag::crash::listReportsIn(reports);
    ASSERT_EQ(listed.size(), 8u);
    const std::vector<crashgroups::Group> groups = crashgroups::group(listed);
    nlohmann::json got = nlohmann::json::array();
    for (const crashgroups::Group& g : groups) {
        nlohmann::json versions = nlohmann::json::object(), platforms = nlohmann::json::object();
        for (const auto& [name, count] : g.versions) versions[name] = count;
        for (const auto& [name, count] : g.platforms) platforms[name] = count;
        got.push_back({{"signature", g.signature}, {"kind", g.kind}, {"count", g.count}, {"versions", versions},
                       {"platforms", platforms}, {"first_seen", isoUtc(g.firstSeen)}, {"last_seen", isoUtc(g.lastSeen)},
                       {"report_ids", g.reportIds}, {"unresolved", g.unresolved}});
    }
    EXPECT_EQ(got, expected["groups"]) << got.dump(1);

    for (const nlohmann::json& pair : expected["short_names"]) {
        EXPECT_EQ(crashgroups::shortName(pair[0].get<std::string>()), pair[1].get<std::string>()) << pair[0];
    }
}

}  // namespace CrashGroupsTest_cpp
