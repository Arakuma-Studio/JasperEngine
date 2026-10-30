// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The recent projects of settings.yaml (docs/specs/S27-editor-tools.md R21): records of path,
// name, last opened (ISO 8601 UTC), pinned and thumbnail, at most 30, pinned ones never evicted.
// The old list of paths reads as path-only records. File > Recent Projects shows the first 10;
// the Project Hub all of them.

#include "yaml-cpp/yaml.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::hub {

    struct RecentProject {
        std::filesystem::path path;
        std::string name;
        std::string lastOpened;           // "2026-10-30T14:05:00Z"; empty: never (a migrated path)
        bool pinned = false;
        std::filesystem::path thumbnail;  // the project's .jasperengine/user/hub-thumbnail.png

        bool operator==(const RecentProject& other) const {
            return path == other.path && name == other.name && lastOpened == other.lastOpened && pinned == other.pinned &&
                   thumbnail == other.thumbnail;
        }
    };

    namespace recent {

        constexpr size_t kCap = 30;
        constexpr size_t kMenuCount = 10;

        std::vector<RecentProject> fromYaml(const YAML::Node& node);
        YAML::Node toYaml(const std::vector<RecentProject>& projects);

        // `path` opened now: first, its name and time set; past the cap the oldest unpinned go
        void touch(std::vector<RecentProject>& projects, const std::filesystem::path& path, const std::string& name,
                   const std::string& now);
        void remove(std::vector<RecentProject>& projects, const std::filesystem::path& path);
        void setPinned(std::vector<RecentProject>& projects, const std::filesystem::path& path, bool pinned);
        // a moved project found again (Locate...)
        void relocate(std::vector<RecentProject>& projects, const std::filesystem::path& from, const std::filesystem::path& to);
        // the cap: unpinned records past it go, the least recently opened first
        void enforceCap(std::vector<RecentProject>& projects);

        enum class Sort : uint8_t { LastOpened, Name };
        // pinned first, then by the sort; `filter` keeps names or paths containing it (any case)
        std::vector<RecentProject> listed(const std::vector<RecentProject>& projects, Sort sort, const std::string& filter);

        // ISO 8601 UTC of a time point, and back (false when the text is not one)
        std::string iso(std::chrono::system_clock::time_point time);
        bool parseIso(const std::string& text, std::chrono::system_clock::time_point& time);
        // "Opened 3 days ago", "Opened just now", "Never opened"
        std::string openedAgo(const std::string& lastOpened, std::chrono::system_clock::time_point now);

    }

}
