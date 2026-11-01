// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "hub/RecentProjects.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

using namespace jasperengine::editor;
using namespace jasperengine::editor::hub;

namespace fs = std::filesystem;

namespace {

    std::string lower(std::string text) {
        for (char& c : text) c = (char)std::tolower((unsigned char)c);
        return text;
    }

    // days since 1970-01-01 of a civil date (Howard Hinnant's algorithm): timegm without the
    // platform differences
    long long daysFromCivil(int y, unsigned m, unsigned d) {
        y -= m <= 2;
        const long long era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + (long long)doe - 719468;
    }

}

std::vector<RecentProject> recent::fromYaml(const YAML::Node& node) {
    std::vector<RecentProject> projects;
    if (!node || !node.IsSequence()) return projects;
    for (const YAML::Node& entry : node) {
        RecentProject p;
        if (entry.IsScalar()) {
            // the old form: a path
            p.path = entry.as<std::string>();
        } else if (entry.IsMap()) {
            if (entry["path"] && entry["path"].IsScalar()) p.path = entry["path"].as<std::string>();
            if (entry["name"] && entry["name"].IsScalar()) p.name = entry["name"].as<std::string>();
            if (entry["last_opened"] && entry["last_opened"].IsScalar()) p.lastOpened = entry["last_opened"].as<std::string>();
            if (entry["pinned"] && entry["pinned"].IsScalar()) p.pinned = entry["pinned"].as<bool>(false);
            if (entry["thumbnail"] && entry["thumbnail"].IsScalar()) p.thumbnail = entry["thumbnail"].as<std::string>();
        }
        if (p.path.empty()) continue;
        if (p.name.empty()) p.name = p.path.filename().string();
        const bool duplicate = std::any_of(projects.begin(), projects.end(), [&](const RecentProject& other) { return other.path == p.path; });
        if (!duplicate) projects.push_back(std::move(p));
    }
    enforceCap(projects);
    return projects;
}

YAML::Node recent::toYaml(const std::vector<RecentProject>& projects) {
    YAML::Node node(YAML::NodeType::Sequence);
    for (const RecentProject& p : projects) {
        YAML::Node entry(YAML::NodeType::Map);
        entry["path"] = p.path.generic_string();
        entry["name"] = p.name;
        if (!p.lastOpened.empty()) entry["last_opened"] = p.lastOpened;
        if (p.pinned) entry["pinned"] = true;
        if (!p.thumbnail.empty()) entry["thumbnail"] = p.thumbnail.generic_string();
        node.push_back(entry);
    }
    return node;
}

void recent::touch(std::vector<RecentProject>& projects, const fs::path& path, const std::string& name, const std::string& now) {
    RecentProject p;
    auto it = std::find_if(projects.begin(), projects.end(), [&](const RecentProject& other) { return other.path == path; });
    if (it != projects.end()) {
        p = *it;
        projects.erase(it);
    }
    p.path = path;
    if (!name.empty()) p.name = name;
    if (p.name.empty()) p.name = path.filename().string();
    p.lastOpened = now;
    projects.insert(projects.begin(), std::move(p));
    enforceCap(projects);
}

void recent::remove(std::vector<RecentProject>& projects, const fs::path& path) {
    projects.erase(std::remove_if(projects.begin(), projects.end(), [&](const RecentProject& p) { return p.path == path; }), projects.end());
}

void recent::setPinned(std::vector<RecentProject>& projects, const fs::path& path, bool pinned) {
    for (RecentProject& p : projects) {
        if (p.path == path) p.pinned = pinned;
    }
}

void recent::relocate(std::vector<RecentProject>& projects, const fs::path& from, const fs::path& to) {
    remove(projects, to);
    for (RecentProject& p : projects) {
        if (p.path == from) p.path = to;
    }
}

void recent::enforceCap(std::vector<RecentProject>& projects) {
    // the list is most recent first: unpinned ones are dropped from the back
    while (projects.size() > kCap) {
        auto last = std::find_if(projects.rbegin(), projects.rend(), [](const RecentProject& p) { return !p.pinned; });
        if (last == projects.rend()) break;   // more than 30 pinned: all stay
        projects.erase(std::next(last).base());
    }
}

std::vector<RecentProject> recent::listed(const std::vector<RecentProject>& projects, Sort sort, const std::string& filter) {
    const std::string needle = lower(filter);
    std::vector<RecentProject> out;
    for (const RecentProject& p : projects) {
        if (needle.empty() || lower(p.name).find(needle) != std::string::npos || lower(p.path.generic_string()).find(needle) != std::string::npos) {
            out.push_back(p);
        }
    }
    std::stable_sort(out.begin(), out.end(), [sort](const RecentProject& a, const RecentProject& b) {
        if (a.pinned != b.pinned) return a.pinned;
        if (sort == Sort::Name) return lower(a.name) < lower(b.name);
        // ISO 8601 UTC sorts as text; never opened last
        if (a.lastOpened.empty() != b.lastOpened.empty()) return !a.lastOpened.empty();
        return a.lastOpened > b.lastOpened;
    });
    return out;
}

std::string recent::iso(std::chrono::system_clock::time_point time) {
    const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    long long days = seconds / 86400;
    long long rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        days -= 1;
    }
    // civil from days (Hinnant)
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long y = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    char text[32];
    std::snprintf(text, sizeof(text), "%04lld-%02u-%02uT%02lld:%02lld:%02lldZ", y + (m <= 2), m, d, rest / 3600, (rest / 60) % 60, rest % 60);
    return text;
}

bool recent::parseIso(const std::string& text, std::chrono::system_clock::time_point& time) {
    int y, mo, d, h, mi, s;
    if (text.size() != 20 || std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2dZ", &y, &mo, &d, &h, &mi, &s) != 6) return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return false;
    const long long seconds = daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s;
    time = std::chrono::system_clock::time_point(std::chrono::seconds(seconds));
    return true;
}

std::string recent::openedAgo(const std::string& lastOpened, std::chrono::system_clock::time_point now) {
    std::chrono::system_clock::time_point opened;
    if (!parseIso(lastOpened, opened)) return "Never opened";
    const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(now - opened).count();
    auto plural = [](long long n, const char* unit) {
        return "Opened " + std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
    };
    if (seconds < 60) return "Opened just now";
    if (seconds < 3600) return plural(seconds / 60, "minute");
    if (seconds < 86400) return plural(seconds / 3600, "hour");
    if (seconds < 86400 * 30) return plural(seconds / 86400, "day");
    if (seconds < 86400 * 365) return plural(seconds / (86400 * 30), "month");
    return plural(seconds / (86400 * 365), "year");
}
