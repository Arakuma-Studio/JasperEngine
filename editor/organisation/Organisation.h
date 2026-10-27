// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// A user's organisation of a project (S25 R13, R26, R28, §6.6): saved searches (OpenEngine's
// smart folders), bookmarks and the Search palette's recent entries, per user and project in
//
//   # <project>/.jasperengine/user/organisation.yaml
//   organisationVersion: 1
//   bookmarks: [{kind: asset, guid: 4b1e..., path: textures/hero.png, name: hero.png},
//               {kind: gameobject, scene: 9c1d..., entity: 42, entityPath: Player/Gun, name: Gun}]
//   savedSearches: [{name: Big Textures, query: "t:texture l:Hero", folder: textures}]
//   paletteRecent: ["command:edit.duplicate", "asset:4b1e..."]
//
// and the saved searches shared with the project (scope project) in project.yaml
// (`savedSearches:`, encoded by Stream with the label colours). Edits go through the undoable
// commands (OrganisationCommands.h), which call the setters here and save.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace YAML { class Node; }

namespace jasperengine::editor {

    struct SavedSearch {
        std::string name;
        std::string query;      // the R20 grammar
        std::string folder;     // project-relative scope ("" the whole project)
        bool project = false;   // shared in project.yaml (else this user's)
        bool builtin = false;   // All Materials... (read-only)

        bool operator==(const SavedSearch& other) const {
            return name == other.name && query == other.query && folder == other.folder && project == other.project && builtin == other.builtin;
        }
    };

    struct Bookmark {
        enum class Kind { Asset, Folder, Scene, GameObject };
        Kind kind = Kind::Asset;
        std::string guid;        // asset, scene (a scene's own GUID), gameobject: its scene's
        std::string path;        // the path hint: project-relative (the scene's, for gameobject)
        std::string name;        // shown
        uint64_t entity = 0;     // gameobject: the saved entity id
        std::string entityPath;  // gameobject: "Player/Gun", when the id is not found

        bool operator==(const Bookmark& other) const {
            return kind == other.kind && guid == other.guid && path == other.path && name == other.name && entity == other.entity &&
                   entityPath == other.entityPath;
        }
    };

    const char* bookmarkKindName(Bookmark::Kind kind);   // asset, folder, scene, gameobject
    bool parseBookmarkKind(const std::string& text, Bookmark::Kind& kind);

    class Organisation {
    public:
        static constexpr int currentVersion = 1;
        static constexpr size_t MAX_RECENTS = 20;

        // Unity's built-in Favorites: All Materials, All Models, All Prefabs, All Scenes, All Scripts
        static const std::vector<SavedSearch>& builtinSearches();

        void open(const std::filesystem::path& projectPath);
        void close();
        bool isOpen() const { return !projectPath.empty(); }
        const std::filesystem::path& projectRoot() const { return projectPath; }
        std::filesystem::path file() const;

        // The user's and the project's, in that order (not the built-ins)
        std::vector<SavedSearch> savedSearches() const;
        const std::vector<SavedSearch>& userSearches() const { return userSaved; }
        const std::vector<SavedSearch>& projectSearches() const { return projectSaved; }
        // By name: the user's first, then the project's, then the built-ins
        const SavedSearch* findSearch(const std::string& name) const;
        void setUserSearches(std::vector<SavedSearch> searches);
        void setProjectSearches(std::vector<SavedSearch> searches);

        const std::vector<Bookmark>& bookmarks() const { return bookmarkList; }
        void setBookmarks(std::vector<Bookmark> bookmarks);

        const std::vector<std::string>& recents() const { return recentKeys; }
        // A palette entry ran: its key first, at most MAX_RECENTS kept (saved)
        void noteRecent(const std::string& key);
        void forgetRecent(const std::string& key);

        // organisation.yaml; false with *error
        bool save(std::string* error = nullptr) const;
        std::string serialize() const;
        bool parse(const std::string& text, std::string* error = nullptr);

        // project.yaml's savedSearches (Stream)
        static void encodeSearches(const std::vector<SavedSearch>& searches, YAML::Node& list);
        static std::vector<SavedSearch> decodeSearches(const YAML::Node& list, bool project);

        uint64_t version() const { return revision; }

    private:
        std::filesystem::path projectPath;
        std::vector<SavedSearch> userSaved;
        std::vector<SavedSearch> projectSaved;
        std::vector<Bookmark> bookmarkList;
        std::vector<std::string> recentKeys;
        uint64_t revision = 1;
    };

}
