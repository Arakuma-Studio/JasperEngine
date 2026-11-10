// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleArtMigrations.h"

using namespace jasperengine;
using namespace jasperengine::titleart;

TitleJsonMigrations::TitleJsonMigrations(std::string format, int firstVersion)
    : formatName(std::move(format)), first(firstVersion) {
}

bool TitleJsonMigrations::add(MigrationStep step) {
    if (!step.apply || step.to != step.from + 1) return false;
    if (step.from != latestVersion()) return false;
    chain.push_back(step);
    return true;
}

void TitleJsonMigrations::removeLast() {
    if (!chain.empty()) chain.pop_back();
}

int TitleJsonMigrations::latestVersion() const {
    return chain.empty() ? first : chain.back().to;
}

bool TitleJsonMigrations::run(Json& root, int fromVersion, int toVersion, MigrationLog& log, std::string* error) const {
    if (fromVersion < first) {
        if (error) *error = formatName + " version " + std::to_string(fromVersion) + " is older than any this build reads (" + std::to_string(first) + ")";
        return false;
    }
    int version = fromVersion;
    for (const MigrationStep& step : chain) {
        if (version >= toVersion) break;
        if (step.from != version) continue;
        step.apply(root, log);
        log.applied.push_back("v" + std::to_string(step.from) + "->v" + std::to_string(step.to) + " " + step.name);
        version = step.to;
    }
    if (version != toVersion) {
        if (error) *error = "no " + formatName + " migration from version " + std::to_string(version) + " to " + std::to_string(toVersion);
        return false;
    }
    if (root.is_object()) root["version"] = toVersion;
    return true;
}

// the .titleart table: v1 is the first version, so it has no steps yet. A step that a later
// format change needs is appended here (with its fixture under engine/tests/core/titleart/fixtures/).
TitleJsonMigrations& titleart::titleArtMigrations() {
    static TitleJsonMigrations* table = new TitleJsonMigrations(TITLE_ART_FORMAT, 1);
    return *table;
}
