// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Organisation.h"

#include "Out.h"
#include "Workspace.h"
#include "util/UserFiles.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

const char* jasperengine::editor::bookmarkKindName(Bookmark::Kind kind) {
    switch (kind) {
        case Bookmark::Kind::Asset: return "asset";
        case Bookmark::Kind::Folder: return "folder";
        case Bookmark::Kind::Scene: return "scene";
        case Bookmark::Kind::GameObject: return "gameobject";
    }
    return "asset";
}

bool jasperengine::editor::parseBookmarkKind(const std::string& text, Bookmark::Kind& kind) {
    for (Bookmark::Kind candidate : {Bookmark::Kind::Asset, Bookmark::Kind::Folder, Bookmark::Kind::Scene, Bookmark::Kind::GameObject}) {
        if (text == bookmarkKindName(candidate)) {
            kind = candidate;
            return true;
        }
    }
    return false;
}

const std::vector<SavedSearch>& Organisation::builtinSearches() {
    static const std::vector<SavedSearch> searches = {
        {"All Materials", "t:material", "", false, true},
        {"All Models", "t:model", "", false, true},
        {"All Prefabs", "t:prefab", "", false, true},
        {"All Scenes", "t:scene", "", false, true},
        {"All Scripts", "t:script", "", false, true},
    };
    return searches;
}

void Organisation::open(const fs::path& path) {
    close();
    projectPath = path;
    std::string text;
    if (userfiles::read(file(), text)) {
        std::string error;
        if (!parse(text, &error)) {
            Out::warning("%s could not be read (%s): bookmarks, saved searches and recent searches start empty.",
                         file().generic_string().c_str(), error.c_str());
        }
    }
    revision++;
}

void Organisation::close() {
    projectPath.clear();
    userSaved.clear();
    projectSaved.clear();
    bookmarkList.clear();
    recentKeys.clear();
    revision++;
}

fs::path Organisation::file() const {
    return projectPath.empty() ? fs::path() : Workspace::getUserDirectory(projectPath) / "organisation.yaml";
}

std::vector<SavedSearch> Organisation::savedSearches() const {
    std::vector<SavedSearch> out = userSaved;
    out.insert(out.end(), projectSaved.begin(), projectSaved.end());
    return out;
}

const SavedSearch* Organisation::findSearch(const std::string& name) const {
    for (const auto* list : {&userSaved, &projectSaved, &builtinSearches()}) {
        for (const SavedSearch& search : *list) {
            if (search.name == name) return &search;
        }
    }
    return nullptr;
}

void Organisation::setUserSearches(std::vector<SavedSearch> searches) {
    for (SavedSearch& search : searches) {
        search.project = false;
        search.builtin = false;
    }
    userSaved = std::move(searches);
    revision++;
}

void Organisation::setProjectSearches(std::vector<SavedSearch> searches) {
    for (SavedSearch& search : searches) {
        search.project = true;
        search.builtin = false;
    }
    projectSaved = std::move(searches);
    revision++;
}

void Organisation::setBookmarks(std::vector<Bookmark> bookmarks) {
    bookmarkList = std::move(bookmarks);
    revision++;
}

void Organisation::noteRecent(const std::string& key) {
    if (key.empty()) return;
    recentKeys.erase(std::remove(recentKeys.begin(), recentKeys.end(), key), recentKeys.end());
    recentKeys.insert(recentKeys.begin(), key);
    if (recentKeys.size() > MAX_RECENTS) recentKeys.resize(MAX_RECENTS);
    revision++;
    std::string error;
    if (isOpen() && !save(&error)) Out::warning("Search recents: %s", error.c_str());
}

void Organisation::forgetRecent(const std::string& key) {
    const size_t before = recentKeys.size();
    recentKeys.erase(std::remove(recentKeys.begin(), recentKeys.end(), key), recentKeys.end());
    if (recentKeys.size() != before) revision++;
}

void Organisation::encodeSearches(const std::vector<SavedSearch>& searches, YAML::Node& list) {
    list = YAML::Node(YAML::NodeType::Sequence);
    for (const SavedSearch& search : searches) {
        YAML::Node node(YAML::NodeType::Map);
        node["name"] = search.name;
        node["query"] = search.query;
        if (!search.folder.empty()) node["folder"] = search.folder;
        list.push_back(node);
    }
}

std::vector<SavedSearch> Organisation::decodeSearches(const YAML::Node& list, bool project) {
    std::vector<SavedSearch> out;
    if (!list || !list.IsSequence()) return out;
    for (const auto& node : list) {
        if (!node.IsMap() || !node["name"] || !node["query"]) continue;
        SavedSearch search;
        search.name = node["name"].as<std::string>("");
        search.query = node["query"].as<std::string>("");
        search.folder = node["folder"] ? node["folder"].as<std::string>("") : std::string();
        search.project = project;
        if (!search.name.empty()) out.push_back(std::move(search));
    }
    return out;
}

std::string Organisation::serialize() const {
    YAML::Node root(YAML::NodeType::Map);
    root["organisationVersion"] = currentVersion;
    YAML::Node bookmarks(YAML::NodeType::Sequence);
    for (const Bookmark& bookmark : bookmarkList) {
        YAML::Node node(YAML::NodeType::Map);
        node.SetStyle(YAML::EmitterStyle::Flow);
        node["kind"] = bookmarkKindName(bookmark.kind);
        if (bookmark.kind == Bookmark::Kind::GameObject) {
            node["scene"] = bookmark.guid;
            node["scenePath"] = bookmark.path;
            node["entity"] = bookmark.entity;
            node["entityPath"] = bookmark.entityPath;
        } else {
            if (!bookmark.guid.empty()) node["guid"] = bookmark.guid;
            node["path"] = bookmark.path;
        }
        node["name"] = bookmark.name;
        bookmarks.push_back(node);
    }
    root["bookmarks"] = bookmarks;
    YAML::Node searches;
    encodeSearches(userSaved, searches);
    root["savedSearches"] = searches;
    YAML::Node recents(YAML::NodeType::Sequence);
    for (const std::string& key : recentKeys) recents.push_back(key);
    root["paletteRecent"] = recents;
    YAML::Emitter out;
    out << root;
    return std::string(out.c_str()) + "\n";
}

bool Organisation::parse(const std::string& text, std::string* error) {
    YAML::Node root;
    try {
        root = YAML::Load(text);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    if (root.IsNull()) return true;
    if (!root.IsMap()) {
        if (error) *error = "not a map";
        return false;
    }
    bookmarkList.clear();
    if (const YAML::Node list = root["bookmarks"]; list && list.IsSequence()) {
        for (const auto& node : list) {
            if (!node.IsMap()) continue;
            Bookmark bookmark;
            if (!parseBookmarkKind(node["kind"] ? node["kind"].as<std::string>("") : std::string(), bookmark.kind)) continue;
            if (bookmark.kind == Bookmark::Kind::GameObject) {
                bookmark.guid = node["scene"] ? node["scene"].as<std::string>("") : std::string();
                bookmark.path = node["scenePath"] ? node["scenePath"].as<std::string>("") : std::string();
                bookmark.entity = node["entity"] ? node["entity"].as<uint64_t>(0) : 0;
                bookmark.entityPath = node["entityPath"] ? node["entityPath"].as<std::string>("") : std::string();
            } else {
                bookmark.guid = node["guid"] ? node["guid"].as<std::string>("") : std::string();
                bookmark.path = node["path"] ? node["path"].as<std::string>("") : std::string();
            }
            bookmark.name = node["name"] ? node["name"].as<std::string>("") : std::string();
            bookmarkList.push_back(std::move(bookmark));
        }
    }
    userSaved = decodeSearches(root["savedSearches"], false);
    recentKeys.clear();
    if (const YAML::Node list = root["paletteRecent"]; list && list.IsSequence()) {
        for (const auto& node : list) {
            if (recentKeys.size() >= MAX_RECENTS) break;
            const std::string key = node.as<std::string>("");
            if (!key.empty()) recentKeys.push_back(key);
        }
    }
    return true;
}

bool Organisation::save(std::string* error) const {
    if (!isOpen()) {
        if (error) *error = "no project is open";
        return false;
    }
    return userfiles::write(file(), serialize(), error);
}
