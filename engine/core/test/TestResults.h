// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The one result format of every test run (docs/specs/project-test-runner.md R19, §6.4): the
// results document, its JUnit form, the selection rules (R18) and the [test] log lines (R26).

#include "Export.h"
#include "test/TestTypes.h"

#include <string>
#include <vector>

namespace jasperengine::gametest {

    // "format" of a results document
    inline constexpr const char* RESULTS_FORMAT = "jasperengine-test-results";

    JASPERENGINE_API Json caseJson(const CaseResult& result, size_t maxConsole = 0);
    JASPERENGINE_API Json countsJson(const Counts& counts);
    JASPERENGINE_API Json resultsJson(const RunResult& result);
    // false + error: not a results document, a newer version ("made by a newer editor"), bad fields
    JASPERENGINE_API bool resultsFromJson(const Json& json, RunResult& out, std::string& error);
    JASPERENGINE_API bool caseFromJson(const Json& json, CaseResult& out, std::string& error);
    // one <testsuite> per file (§6.4)
    JASPERENGINE_API std::string junitXml(const RunResult& result);

    JASPERENGINE_API Counts countResults(const std::vector<CaseResult>& cases, int excludedEdit);

    // R18: a filter is case-insensitive substrings of the case id, or tag:<t>, joined by ","
    // (or); an empty filter selects everything
    JASPERENGINE_API bool matchesFilter(const CaseSpec& spec, const std::string& filter);

    // §6.1: a case id as a path part ("ui/menu.test.lua#Start" -> "ui__menu.test.lua__Start")
    JASPERENGINE_API std::string caseIdPath(const std::string& caseId);

    // "20261119-101500-3f2a": the UTC time and four random hex digits
    JASPERENGINE_API std::string newRunId();
    JASPERENGINE_API std::string isoTimeUtc();

    // R26: the whole line (without the "[test] " the log adds) at most 16 KB
    JASPERENGINE_API std::string beginLine(int cases, const std::string& runId);
    JASPERENGINE_API std::string caseLine(const CaseResult& result);
    JASPERENGINE_API std::string summaryLine(const Counts& counts);

    // the run's target platform name for this build ("linux", "android", "web"...)
    JASPERENGINE_API std::string hostPlatformName();
    JASPERENGINE_API std::string hostArchName();
    // the running graphics backend: glcore, gles3, metal, d3d11, vulkan
    JASPERENGINE_API std::string backendName();

}
