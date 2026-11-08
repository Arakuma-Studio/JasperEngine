// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LintTypes.h"

namespace jasperengine::editor::pixelart::lint {

    const char* lintStatusName(LintStatus status) {
        switch (status) {
            case LintStatus::Pass: return "PASS";
            case LintStatus::Warn: return "WARN";
            case LintStatus::Fail: return "FAIL";
            case LintStatus::Skip: return "SKIP";
        }
        return "SKIP";
    }

    bool parseLintStatus(std::string_view text, LintStatus& out) {
        for (LintStatus s : {LintStatus::Pass, LintStatus::Warn, LintStatus::Fail, LintStatus::Skip}) {
            if (text == lintStatusName(s)) {
                out = s;
                return true;
            }
        }
        return false;
    }

    void LintResult::addSpot(const LintSpot& spot) {
        count++;
        if (at.size() < kMaxSpots) {
            at.push_back(spot);
        }
    }

    void LintReport::add(LintResult result) {
        results.push_back(std::move(result));
        switch (results.back().status) {
            case LintStatus::Pass: pass++; break;
            case LintStatus::Warn: warn++; break;
            case LintStatus::Fail: fail++; break;
            case LintStatus::Skip: skip++; break;
        }
    }

    void LintReport::recount() {
        pass = warn = fail = skip = 0;
        std::vector<LintResult> all = std::move(results);
        results.clear();
        for (LintResult& r : all) {
            add(std::move(r));
        }
    }

    LintStatus LintReport::overall() const {
        if (fail > 0) return LintStatus::Fail;
        if (warn > 0) return LintStatus::Warn;
        return LintStatus::Pass;
    }

    nlohmann::json toJson(const LintResult& result) {
        nlohmann::json at = nlohmann::json::array();
        for (const LintSpot& spot : result.at) {
            at.push_back({spot.frame, spot.x, spot.y});
        }
        nlohmann::json j = {
            {"id", result.id},
            {"status", lintStatusName(result.status)},
            {"measured", result.measured},
            {"threshold", result.threshold},
            {"threshold_source", result.thresholdSource},
            {"count", result.count},
            {"at", std::move(at)},
        };
        if (!result.message.empty()) j["message"] = result.message;
        if (!result.fix.empty()) j["fix"] = result.fix;
        return j;
    }

    nlohmann::json toJson(const LintReport& report) {
        nlohmann::json results = nlohmann::json::array();
        for (const LintResult& r : report.results) {
            results.push_back(toJson(r));
        }
        return {
            {"status", lintStatusName(report.overall())},
            {"pass", report.pass},
            {"warn", report.warn},
            {"fail", report.fail},
            {"skip", report.skip},
            {"results", std::move(results)},
        };
    }

}
