// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef QACHECKS_H
#define QACHECKS_H

#include "json.hpp"

#include <map>
#include <string>

namespace jasperengine::editor::qa {

    // The check row every verdict tool shares (docs/specs/S35-agent-audio.md R21, R23; S90; S91 D5):
    // {id, kind, status PASS|FAIL|WARN|SKIP, round, measured, threshold, threshold_source,
    // limited_by, at[], fix_hint, method}, with the anti-gaming parts: the rounds of consecutive
    // FAILs, the thresholds that differ from their defaults, and the guidance that ends every result.

    using Json = nlohmann::json;

    inline constexpr int MAX_AT = 10;

    // a SKIP row: what a check that has not measured anything says
    Json row(const std::string& id, const std::string& kind);
    // one more time (seconds, to the ms) in row.at, up to MAX_AT
    void addAt(Json& row, double seconds);

    // FAIL if a row FAILs, else WARN, else PASS if a row PASSes, else SKIP
    std::string verdict(const Json& rows);
    // fail_on ("fail" or "warn"): the verdict reaches it
    bool reaches(const std::string& verdict, const std::string& failOn);

    // Each row's round: the consecutive FAILs before it (`previous`, by id) plus this one, 0 when it
    // does not fail. True when a row went past fixRounds: the agent stops fixing and reports.
    bool countRounds(Json& rows, const std::map<std::string, int>& previous, int fixRounds);

    // The consecutive FAILs of each check in one scope (a scene, an effect) during this editor
    // session. Main thread.
    class FailRounds {
    public:
        std::map<std::string, int> previous(const std::string& scope) const;
        // what the rows of a run say: a FAIL keeps its round, anything else starts over
        void record(const std::string& scope, const Json& rows);

    private:
        std::map<std::string, int> rounds;   // "<scope>|<id>"
    };

    // thresholds{values, changed[]}: every threshold a run used, and the ones a project changed
    class ThresholdAudit {
    public:
        void add(const std::string& key, const Json& value, const Json& standard);
        Json json() const;
        bool changedAny() const { return !changed.empty(); }

    private:
        Json values = Json::object();
        Json changed = Json::array();
    };

    // The guidance of a result: stop after fixRounds, else how to go on after a FAIL (`retry`: "capture
    // again and check again"), then `closing`, which every result ends with
    std::string guidance(bool stop, bool anyFail, int fixRounds, const std::string& retry, const std::string& closing);

    // "FAIL id: <measured>. <limited_by> <fix_hint>", the text line of a row
    std::string rowLine(const Json& row);

}

#endif //QACHECKS_H
