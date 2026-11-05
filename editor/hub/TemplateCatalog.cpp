// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The template fields (id, name, description, accentColor, thumbnail, source) follow OpenEngine's
// Apps/Editor/ProjectTemplates/manifest.json, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#include "hub/TemplateCatalog.h"

#include "hub/HubPaths.h"
#include "hub/StagingFolders.h"
#include "io/AtomicFile.h"
#include "util/FileUtils.h"

#include "json.hpp"
#include "project_templates.h"

#include <algorithm>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::hub;

namespace fs = std::filesystem;

namespace {

    using Json = nlohmann::ordered_json;

    bool readFile(const fs::path& file, std::string& out) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        std::stringstream text;
        text << in.rdbuf();
        out = text.str();
        return true;
    }

    struct Embedded {
        std::string relative;
        const unsigned char* data;
        unsigned int size;
    };

    std::vector<Embedded> embeddedFiles(const std::string& id) {
        std::vector<Embedded> files;
        const std::string prefix = id + "/";
        for (const ProjectTemplateEmbeddedFile* f = project_template_embedded_files; f->path; f++) {
            const std::string path = f->path;
            if (path.compare(0, prefix.size(), prefix) == 0) files.push_back({path.substr(prefix.size()), f->data, f->size});
        }
        return files;
    }

    bool removeAll(const fs::path& dir) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        return !ec;
    }

    // one file of a copy, whole or not at all. Its source remains, and the copy may have thousands
    // of files: not synced one by one
    bool copyOneFile(const fs::path& from, const fs::path& to, std::error_code& ec) {
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        return FileUtils::copyFileAtomic(from, to, ec, options);
    }

}

fs::path templates::userDirectory(const fs::path& settingsDirectory) {
    return settingsDirectory / "templates";
}

bool templates::parseInfo(const std::string& text, ProjectTemplate& info, std::string& error) {
    const Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "template.json is not a JSON object";
        return false;
    }
    if (!j.contains("schemaVersion") || !j["schemaVersion"].is_number_integer() || j["schemaVersion"].get<int>() != ProjectTemplate::kSchemaVersion) {
        error = "template.json has no schemaVersion 1";
        return false;
    }
    auto string = [&](const char* key, std::string& out) {
        if (j.contains(key) && j[key].is_string()) out = j[key].get<std::string>();
    };
    string("id", info.id);
    string("name", info.name);
    string("description", info.description);
    string("category", info.category);
    string("accentColor", info.accentColor);
    string("thumbnail", info.thumbnail);
    string("startScene", info.startScene);
    if (j.contains("order") && j["order"].is_number_integer()) info.order = j["order"].get<int>();
    if (!isValidCatalogId(info.id)) {
        error = "template id \"" + info.id + "\" is not a slug (letters, digits, '-', '_', at most 64)";
        return false;
    }
    if (!info.thumbnail.empty() && !isSafeRelativePath(info.thumbnail)) {
        error = "thumbnail \"" + info.thumbnail + "\" is not a relative path inside the template";
        return false;
    }
    if (info.name.empty()) info.name = info.id;
    return true;
}

std::string templates::infoJson(const ProjectTemplate& info) {
    Json j = Json::object();
    j["schemaVersion"] = ProjectTemplate::kSchemaVersion;
    j["id"] = info.id;
    j["name"] = info.name;
    j["description"] = info.description;
    j["category"] = info.category;
    j["order"] = info.order;
    j["accentColor"] = info.accentColor;
    j["thumbnail"] = info.thumbnail;
    j["startScene"] = info.startScene;
    return j.dump(2) + "\n";
}

std::vector<ProjectTemplate> templates::all(const fs::path& userDir, std::vector<std::string>* errors) {
    std::vector<ProjectTemplate> list;
    // the built-in ones: each embedded "<id>/template.json"
    for (const ProjectTemplateEmbeddedFile* f = project_template_embedded_files; f->path; f++) {
        const std::string path = f->path;
        const size_t slash = path.find('/');
        if (slash == std::string::npos || path.substr(slash + 1) != kInfoFile) continue;
        ProjectTemplate info;
        std::string error;
        if (!parseInfo(std::string(reinterpret_cast<const char*>(f->data), f->size), info, error)) {
            if (errors) errors->push_back(path + ": " + error);
            continue;
        }
        info.builtIn = true;
        for (const Embedded& file : embeddedFiles(path.substr(0, slash))) {
            if (file.relative == info.thumbnail) info.thumbnailPng.assign(file.data, file.data + file.size);
        }
        list.push_back(std::move(info));
    }
    std::stable_sort(list.begin(), list.end(), [](const ProjectTemplate& a, const ProjectTemplate& b) { return a.order < b.order; });
    // the user's
    std::vector<ProjectTemplate> user;
    std::error_code ec;
    if (!userDir.empty() && fs::is_directory(userDir, ec)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(userDir, ec)) {
            if (!entry.is_directory(ec)) continue;
            std::string text;
            if (!readFile(entry.path() / kInfoFile, text)) continue;
            ProjectTemplate info;
            std::string error;
            if (!parseInfo(text, info, error)) {
                if (errors) errors->push_back(entry.path().filename().string() + ": " + error);
                continue;
            }
            info.directory = entry.path();
            std::string png;
            if (!info.thumbnail.empty() && readFile(entry.path() / info.thumbnail, png)) info.thumbnailPng.assign(png.begin(), png.end());
            user.push_back(std::move(info));
        }
    }
    std::sort(user.begin(), user.end(), [](const ProjectTemplate& a, const ProjectTemplate& b) { return a.name < b.name; });
    for (ProjectTemplate& t : user) list.push_back(std::move(t));
    return list;
}

const ProjectTemplate* templates::find(const std::vector<ProjectTemplate>& list, const std::string& id) {
    for (const ProjectTemplate& t : list) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

bool templates::materialize(const ProjectTemplate& info, const fs::path& target, std::string& error, const std::atomic<bool>* cancel) {
    const std::string problem = targetProblem(target);
    if (!problem.empty()) {
        error = problem;
        return false;
    }
    std::error_code ec;
    // built next to the target and renamed to it whole: a crash or a failure midway leaves the
    // target as it was (absent, or the empty folder the user made)
    fs::path place = fs::absolute(target, ec).lexically_normal();
    if (!place.has_filename()) place = place.parent_path();
    hub::staging::sweep({{place.parent_path()}, {}});
    hub::staging::Claim claim(place.parent_path(), place.filename().string());
    if (!claim.ok()) {
        error = claim.error();
        return false;
    }
    const fs::path staging = claim.folder();
    auto fail = [&](const std::string& why) {
        removeAll(staging);
        error = why;
        return false;
    };
    auto publish = [&]() {
        const bool existed = fs::exists(place, ec);
        if (existed) fs::remove(place, ec);   // an empty folder
        if (!io::renameWithRetry(staging, place, ec)) {
            const std::string why = "cannot move the project to " + place.string() + ": " + ec.message();
            if (existed) fs::create_directory(place, ec);
            return fail(why);
        }
        return true;
    };
    auto skipped = [&](const std::string& relative) { return relative == kInfoFile || relative == info.thumbnail; };
    if (info.builtIn) {
        for (const Embedded& file : embeddedFiles(info.id)) {
            if (cancel && cancel->load()) return fail("cancelled");
            if (skipped(file.relative)) continue;
            if (!isSafeRelativePath(file.relative)) return fail("unsafe path in the template: " + file.relative);
            const io::WriteResult written =
                io::writeFileAtomic(staging / fs::path(file.relative), std::string_view(reinterpret_cast<const char*>(file.data), file.size));
            if (!written.ok()) return fail(written.message);
            hub::staging::holdForTest();
        }
        return publish();
    }
    if (info.directory.empty() || !fs::is_directory(info.directory, ec)) return fail("the template's folder is gone: " + info.directory.string());
    for (fs::recursive_directory_iterator it(info.directory, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
        if (ec) return fail("cannot read the template: " + ec.message());
        if (cancel && cancel->load()) return fail("cancelled");
        const fs::path relative = fs::relative(it->path(), info.directory, ec);
        const std::string rel = relative.generic_string();
        if (it->is_symlink(ec)) return fail("the template has a symbolic link: " + rel);
        if (it->is_directory(ec)) continue;
        if (skipped(rel)) continue;
        if (!isSafeRelativePath(rel)) return fail("unsafe path in the template: " + rel);
        fs::create_directories((staging / relative).parent_path(), ec);
        if (!copyOneFile(it->path(), staging / relative, ec)) {
            return fail("cannot copy " + rel + ": " + ec.message());
        }
        hub::staging::holdForTest();
    }
    return publish();
}

bool templates::excludedFromTemplate(const std::string& name) {
    return name == ".jasperengine" || name == "build" || name == "Recordings" || name == ".trash" || name == ".git" ||
           name == "settings.yaml" || name == "editor_layout_v2.ini" || name == ".DS_Store" || name == "Thumbs.db";
}

bool templates::saveProjectAsTemplate(const fs::path& project, const fs::path& userDir, const std::string& name, const std::string& description,
                                      const std::string& startScene, const std::vector<uint8_t>& thumbnailPng, std::string& id,
                                      std::string& error) {
    id = slugify(name);
    if (!isValidCatalogId(id)) {
        error = "a template needs a name with letters or digits";
        return false;
    }
    const fs::path target = userDir / id;
    std::error_code ec;
    if (fs::exists(target, ec)) {
        error = "a template \"" + id + "\" exists (" + target.string() + ")";
        return false;
    }
    // built where the catalog does not look (it reads <userDir>/*/template.json) and renamed in
    // whole: a crash midway leaves no half template, and the name stays free
    const fs::path stagingRoot = userDir / ".staging";
    hub::staging::sweep({{stagingRoot}, {}});
    hub::staging::Claim claim(stagingRoot, id);
    if (!claim.ok()) {
        error = claim.error();
        return false;
    }
    const fs::path staging = claim.folder();
    auto fail = [&](const std::string& why) {
        removeAll(staging);
        claim.release();
        fs::remove(stagingRoot, ec);   // when nothing else is in it
        error = why;
        return false;
    };
    for (fs::recursive_directory_iterator it(project, fs::directory_options::none, ec), end; it != end; it.increment(ec)) {
        if (ec) return fail("cannot read the project: " + ec.message());
        const fs::path relative = fs::relative(it->path(), project, ec);
        const std::string top = relative.begin() != relative.end() ? relative.begin()->string() : std::string();
        if (excludedFromTemplate(top)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_symlink(ec)) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec)) {
            fs::create_directories(staging / relative, ec);
            continue;
        }
        if (!copyOneFile(it->path(), staging / relative, ec)) {
            return fail("cannot copy " + relative.generic_string() + ": " + ec.message());
        }
        hub::staging::holdForTest();
    }
    ProjectTemplate info;
    info.id = id;
    info.name = name;
    info.description = description;
    info.category = "My Templates";
    info.order = 100;
    info.startScene = startScene.empty() ? "Main" : startScene;
    info.thumbnail = thumbnailPng.empty() ? std::string() : "thumbnail.png";
    io::WriteResult written = io::writeFileAtomic(staging / kInfoFile, infoJson(info));
    if (!written.ok()) return fail(written.message);
    if (!thumbnailPng.empty()) {
        written = io::writeFileAtomic(staging / "thumbnail.png", std::string_view(reinterpret_cast<const char*>(thumbnailPng.data()), thumbnailPng.size()));
        if (!written.ok()) return fail(written.message);
    }
    if (!io::renameWithRetry(staging, target, ec)) return fail("cannot move the template to " + target.string() + ": " + ec.message());
    claim.release();
    fs::remove(stagingRoot, ec);   // when nothing else is in it
    return true;
}
