// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Every export's Build Report kept in the project (docs/specs/build-size-tools.md R6):
// <project>/.jasperengine/build-reports/<platform>/<id>.json, the newest kKeep per platform, id
// <yyyyMMdd-HHmmss>-<method> in UTC ("-2", "-3"... on a collision). Compare With... and
// get_build_info's report / compare_to read it.

#pragma once

#include "cook/BuildReport.h"

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    namespace BuildReportHistory {
        constexpr size_t kKeep = 10;

        struct Entry {
            std::string platform;
            std::string id;
            std::filesystem::path file;
        };

        std::filesystem::path directory(const std::filesystem::path& projectPath);
        // a free id for a report made at `when` (UTC) by `method`, and its ISO 8601 time
        std::string newId(const std::filesystem::path& projectPath, const std::string& platform, const std::string& method, std::time_t when);
        std::string isoTime(std::time_t when);
        // Writes `bytes` (the report's file, byte for byte) as the platform's entry `report.id` and
        // deletes all but the newest kKeep. True: the export goes on; `warning` says why no
        // entry was written (W-BUILD-REPORT-HISTORY), never a failed export.
        bool record(const std::filesystem::path& projectPath, const BuildReport& report, const std::string& bytes, std::string& warning);
        // a platform's entries newest first ("" every platform, newest first)
        std::vector<Entry> list(const std::filesystem::path& projectPath, const std::string& platform = {});
        // "last", a history id, a build-report.json or the folder holding one; empty with `error`
        // naming the history ids when nothing matches
        std::filesystem::path resolve(const std::filesystem::path& projectPath, const std::string& idOrPath, std::string& error);
        // the entry before `report`'s id of its platform ("" none)
        std::string previous(const std::filesystem::path& projectPath, const BuildReport& report);
        // this session's last export, else the newest entry of the history (R6: after a restart)
        std::filesystem::path last(const std::filesystem::path& projectPath);
    }

}
