// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Build Profiles (docs/specs/build-profiles.md): named build variants, one committed file each in
// EditorSettings/BuildProfiles/<Name>.buildprofile. A profile is a platform plus overrides (D1):
// identity (Product Name, Identifier, Version Suffix, Icon), store, Graphics API, Development
// Build, output folder, a Scene List, Scripting Define Symbols, content exclusions and Player
// Settings keys. A build resolves it once into a settings snapshot (resolveBuildSettings); the
// open project never changes. Every surface (Build Settings, edit_build_profile, the CLI, the
// build) validates through validateProfile.

#include "ShippingTypes.h"
#include "io/AtomicFile.h"
#include "mcp/McpJson.h"

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor {

    class Project;

    struct BuildProfileScene {
        std::string guid;
        std::string path;   // project-relative, a hint rewritten on save

        bool operator==(const BuildProfileScene& other) const { return guid == other.guid && path == other.path; }
    };

    struct BuildProfile {
        static constexpr int FORMAT_VERSION = 1;

        std::string name;   // the file stem
        BuildPlatform platform = BuildPlatform::Linux;
        // "" inherits each of these
        std::string productName;
        std::string identifier;
        std::string versionSuffix;
        std::string icon;            // project-relative image
        std::string store;           // "" none google-play app-store steam itch web
        std::string storeTitle;
        std::string backend;         // "" the platform's default (platformBackends)
        std::string outputDir;       // "" Build Settings' folder for the platform
        std::optional<bool> development;
        std::optional<std::vector<BuildProfileScene>> scenes;   // absent: the project's Scenes In Build
        std::string startScene;      // a GUID of scenes; "" the first of them
        std::vector<std::string> defines;
        std::vector<std::string> excludeLabels;
        std::vector<std::string> excludePaths;
        std::map<std::string, mcp::Json> overrides;   // settings key -> value
        YAML::Node unknown;          // the keys this version does not know, kept verbatim on save
    };

    struct BuildProfileFile {
        static constexpr const char* EXTENSION = ".buildprofile";

        // <project>/EditorSettings/BuildProfiles
        static std::filesystem::path directory(const std::filesystem::path& projectPath);
        static std::filesystem::path pathOf(const std::filesystem::path& projectPath, const std::string& name);

        // false with `error` for a file this editor cannot use; `newer` (when given) is set for a
        // file of a newer formatVersion, which is refused with "made by a newer editor"
        static bool read(const std::filesystem::path& file, BuildProfile& out, std::string& error, bool* newer = nullptr);
        static bool parse(const std::string& text, BuildProfile& out, std::string& error, bool* newer = nullptr);
        static std::string encode(const BuildProfile& profile);
        static io::WriteResult write(const std::filesystem::path& file, const BuildProfile& profile);

        // 1-64 characters of [A-Za-z0-9 _.-], not starting with '.' or ' ' nor ending with ' ' or '.'
        static bool isValidName(const std::string& name, std::string* why = nullptr);
    };

    // A profile as the project lists it: readable, read-only (a newer editor's), or unreadable
    struct BuildProfileEntry {
        std::string name;
        std::filesystem::path file;
        bool readable = false;
        bool newer = false;
        std::string error;
        BuildProfile profile;
    };

    namespace buildprofiles {
        // Every *.buildprofile of the project, sorted by name (one directory listing)
        std::vector<BuildProfileEntry> list(const std::filesystem::path& projectPath);
        // By name, compared without case; null when none
        const BuildProfileEntry* find(const std::vector<BuildProfileEntry>& entries, const std::string& name);
        // Bumped by every change the editor makes to a profile file or to the active profile, and by
        // an external change it sees (the Build Settings window and the Game view re-read then)
        uint64_t revision();
        void touch();

        // The editor windows' view (main thread): the list and the active profile, read again when
        // revision() moves, and the folder's file stamps looked at once a second at most, so a
        // checkout's change shows (R21) and a project without profiles costs one stat a second
        // while a window asks
        struct View {
            std::filesystem::path project;
            uint64_t revision = 0;
            std::vector<BuildProfileEntry> entries;
            std::string activeName;
            std::shared_ptr<const BuildProfile> active;
        };
        const View& view(const std::filesystem::path& projectPath);
        // the settings keys a profile changes in builds: its typed fields' and its overrides
        std::set<std::string> overriddenKeys(const BuildProfile& profile);

        // One profile a build or an edit can use: false with the reason (unknown, unreadable, newer)
        bool load(const std::filesystem::path& projectPath, const std::string& name, BuildProfile& out, std::string& error);

        // The user's active profile, .jasperengine/user/build.yaml activeBuildProfile ("" none)
        std::filesystem::path activeFile(const std::filesystem::path& projectPath);
        std::string activeName(const std::filesystem::path& projectPath);
        bool setActiveName(const std::filesystem::path& projectPath, const std::string& name, std::string& error);
        // The active profile, readable; null (with one warning per name) when it is missing or unreadable
        std::shared_ptr<const BuildProfile> active(const std::filesystem::path& projectPath);

        // The settings keys a typed field writes for a platform (R2)
        std::vector<std::string> productNameKeys(BuildPlatform platform);
        std::vector<std::string> identifierKeys(BuildPlatform platform);
        std::vector<std::string> iconKeys(BuildPlatform platform);
        // "Android label", "iOS bundle identifier"...: what a conflict message calls the key
        std::string keyMeaning(const std::string& key);

        // The settings categories an override may use for a platform (R13)
        std::vector<std::string> allowedCategories(BuildPlatform platform);

        const std::vector<std::string>& storeNames();

        // What a typed field resolves to in a build, and where the value comes from ("profile",
        // "android.application_name", "player.application_name", "Project Name", "default")
        struct ResolvedValue {
            std::string value;
            std::string source;
        };
        std::map<std::string, ResolvedValue> resolveIdentity(const Project& project, const BuildProfile& profile);

        // The store findings of a profile (R16) against what it resolves to
        std::vector<BuildWarning> storeFindings(const Project& project, const BuildProfile& profile);

        // The editor's Play with an active profile (R9): the identity, defines and scene list it applies
        struct PlayProfile {
            std::string name;
            std::string appName;
            std::string identifier;
            std::string version;
            std::vector<std::string> defines;
            bool hasScenes = false;
            std::set<std::string> sceneGuids;
        };
        // false without an active readable profile
        bool playProfile(const Project& project, PlayProfile& out);
    }

    // Every finding against the open project, each naming its fix (D6); empty when it can build
    std::vector<std::string> validateProfile(Project& project, const BuildProfile& profile);

    // The settings snapshot of a build (R14): the project itself (the same pointer) when `profile`
    // is null (R15), else a settings-only Project with the profile's typed fields and overrides
    // applied. Null with `error` when the profile does not validate.
    std::shared_ptr<Project> resolveBuildSettings(Project& project, const BuildProfile* profile, std::vector<BuildWarning>& warnings,
                                                  std::string& error);

    // The project scenes a profile ships, as scene GUIDs in its order (absent list: none, the
    // project's Scenes In Build apply)
    std::vector<std::string> profileSceneGuids(const BuildProfile& profile);

    // What copyAssets, copyLua, the cooks and the pack leave out (R10-R11): one component walk
    // per path over the excluded prefixes, and one lookup in the labelled paths
    class BuildContentFilter {
    public:
        BuildContentFilter() = default;
        // `labelsByPath`: project-relative path -> its asset labels (labelledAssetPaths)
        BuildContentFilter(const BuildProfile* profile, const std::map<std::string, std::vector<std::string>>& labelsByPath);

        bool empty() const { return prefixes.empty() && files.empty() && byLabel.empty(); }
        // a project-relative file or folder; `why` gets "label full-only" or "path music/full/"
        bool excludes(std::string_view projectRelativePath, std::string* why = nullptr) const;
        const std::string& profileName() const { return profile; }

        // R11: an Exclude Path of a profile, "" when it is valid, else why
        static std::string pathRefusal(const std::string& path);

    private:
        std::string profile;
        std::set<std::string, std::less<>> prefixes;   // "music/full/"
        std::set<std::string, std::less<>> files;      // "levels/bonus.scene"
        std::map<std::string, std::string, std::less<>> byLabel;   // path -> the label that excludes it
    };

    // Every labelled asset of the project by project-relative path (the in-memory label store)
    std::map<std::string, std::vector<std::string>> labelledAssetPaths(const Project& project);

}
