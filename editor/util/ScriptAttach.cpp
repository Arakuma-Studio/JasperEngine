// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ScriptAttach.h"

#include "component/ScriptComponent.h"

#include "Out.h"
#include "Project.h"
#include "command/CommandHandle.h"
#include "command/type/AddComponentCmd.h"
#include "command/type/MultiPropertyCmd.h"
#include "command/type/PropertyCmd.h"
#include "util/FileUtils.h"
#include "util/ProjectChecks.h"
#include "util/ProjectUtils.h"
#include "util/ScriptParser.h"
#include "util/Util.h"

#include <algorithm>
#include <cctype>
#include <regex>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    std::string scriptFileExtension(const fs::path& path) {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    std::string attachRelative(Project* project, const fs::path& absolute) {
        return ProjectChecks::projectRelative(project->getProjectPath(), absolute);
    }

    std::string attachLuaRoot(Project* project) {
        const std::string rel = attachRelative(project, project->getLuaPath());
        return rel.empty() ? "." : rel;
    }

    // the file an entry reads its code from (the Lua file, the C++ source), to tell two entries apart
    fs::path attachedScriptFile(Project* project, const ScriptEntry& entry) {
        return project->scriptSourceFile(entry);
    }

    // the class a C++ script class derives, "" when not found
    std::string baseClassOf(const fs::path& header, const std::string& className) {
        std::string code;
        if (!ProjectChecks::readFile(header, code)) return "";
        const std::regex pattern("\\b(?:class|struct)\\s+(?:[A-Za-z_]\\w*\\s+)?" + className +
                                 "\\s*(?:final\\s*)?:\\s*(?:public\\s+|protected\\s+|private\\s+)?(?:virtual\\s+)?(?:jasperengine::)?([A-Za-z_]\\w*)");
        std::smatch match;
        return std::regex_search(code, match, pattern) ? match[1].str() : "";
    }

}

bool ScriptAttach::isScriptFile(const fs::path& file) {
    const std::string path = file.string();
    return Util::isLuaFile(path) || Util::isHeaderFile(path) || Util::isSourceFile(path);
}

bool ScriptAttach::makeEntry(Project* project, const fs::path& file, const std::string& classNameArg, ScriptEntry& entry,
                             std::vector<std::string>& warnings, std::string& error) {
    const std::string ext = scriptFileExtension(file);
    std::error_code ec;
    entry = ScriptEntry();
    entry.enabled = true;
    if (ext == ".lua") {
        if (!Util::isInsidePath(file, project->getLuaPath())) {
            error = "Lua scripts must be inside the project's Lua directory \"" + attachLuaRoot(project) +
                    "\" (Project Settings > Directories); move the file there first (move_asset).";
            return false;
        }
        entry.type = ScriptType::LUA;
        entry.path = project->normalizeToLuaRelative(file).generic_string();
        entry.className = file.stem().string();
        if (!classNameArg.empty() && classNameArg != entry.className) {
            warnings.push_back("A Lua script's name is its file name: using \"" + entry.className + "\".");
        }
        return true;
    }
    fs::path header, source;
    if (Util::isHeaderFile(file.string())) {
        header = file;
        for (const char* candidate : {".cpp", ".cc", ".cxx"}) {
            fs::path s = file;
            s.replace_extension(candidate);
            if (fs::is_regular_file(s, ec)) {
                source = s;
                break;
            }
        }
        if (source.empty()) {
            error = "No source file next to " + attachRelative(project, file) + " (expected " + FileUtils::pathToUtf8(file.stem()) + ".cpp).";
            return false;
        }
    } else if (Util::isSourceFile(file.string())) {
        source = file;
        for (const char* candidate : {".h", ".hpp", ".hh", ".hxx"}) {
            fs::path h = file;
            h.replace_extension(candidate);
            if (fs::is_regular_file(h, ec)) {
                header = h;
                break;
            }
        }
        if (header.empty()) {
            error = "No header next to " + attachRelative(project, file) + " (expected " + FileUtils::pathToUtf8(file.stem()) + ".h).";
            return false;
        }
    } else {
        error = "path must be a .lua script or a C++ script (.h/.hpp header or .cpp source).";
        return false;
    }
    entry.type = ScriptType::CPP;
    // Stored as the editor stores them (projectRelative is UTF-8)
    entry.headerPath = attachRelative(project, header);
    entry.path = attachRelative(project, source);
    std::string className = classNameArg;
    if (className.empty()) {
        if (auto found = ScriptParser::findScriptClassName(header)) className = *found;
    }
    if (className.empty()) {
        error = "Couldn't tell the script class in " + entry.headerPath.str() + " (several or none); pass class_name.";
        return false;
    }
    if (!ScriptParser::inheritsScriptBase(header, className).has_value()) {
        warnings.push_back("Class " + className + " was not found in " + entry.headerPath.str() + "; the build will fail until it exists.");
    }
    entry.className = className;
    return true;
}

bool ScriptAttach::attach(Project* project, SceneProject* sceneProject, const std::vector<Entity>& entities, const ScriptEntry& entry,
                          long long index, std::vector<Entity>& skipped, std::string& error, size_t* indexOut,
                          uint32_t commandSceneId) {
    const fs::path file = attachedScriptFile(project, entry);
    auto* multi = new MultiPropertyCmd();
    bool first = true;
    bool any = false;
    long long existingIndex = -1;
    for (Entity entity : entities) {
        ScriptComponent* component = sceneProject->scene->findComponent<ScriptComponent>(entity);
        std::vector<ScriptEntry> scripts = component ? component->scripts : std::vector<ScriptEntry>();
        auto existing = std::find_if(scripts.begin(), scripts.end(), [&](const ScriptEntry& script) {
            return script.type == entry.type && attachedScriptFile(project, script) == file;
        });
        if (existing != scripts.end()) {
            if (existingIndex < 0) existingIndex = existing - scripts.begin();
            skipped.push_back(entity);
            continue;
        }
        size_t position = scripts.size();
        if (index >= 0 && static_cast<size_t>(index) < scripts.size()) position = static_cast<size_t>(index);
        scripts.insert(scripts.begin() + static_cast<std::ptrdiff_t>(position), entry);
        project->updateScriptProperties(sceneProject, entity, scripts);

        if (!component) {
            multi->addCommand(std::make_unique<AddComponentCmd>(project, sceneProject->id, entity, ComponentType::ScriptComponent));
        }
        multi->addCommand(std::make_unique<PropertyCmd<std::vector<ScriptEntry>>>(
            project, sceneProject->id, entity, ComponentType::ScriptComponent, "scripts", scripts));
        if (first && indexOut) *indexOut = position;
        first = false;
        any = true;
    }
    if (!any) {
        delete multi;
        if (entities.size() == 1) {
            error = "The GameObject already has " + entry.className + " (script " + std::to_string(existingIndex) +
                    "); change it with update_script_entry or set_script_properties.";
        } else {
            error = "Every selected GameObject already has " + entry.className + ".";
        }
        return false;
    }
    multi->setNoMerge();
    if (!CommandHandle::get(commandSceneId != 0 ? commandSceneId : sceneProject->id)->addCommand(multi)) {
        error = "The editor refused the change (is the GameObject part of a prefab instance that can't take it?).";
        return false;
    }
    return true;
}

bool ScriptAttach::attachFile(Project* project, SceneProject* sceneProject, const std::vector<Entity>& entities, const fs::path& file,
                              uint32_t commandSceneId) {
    if (project->isAnyScenePlaying()) {
        Out::warning("Stop Play to add scripts");
        return false;
    }
    ScriptEntry entry;
    std::vector<std::string> warnings;
    std::string error;
    if (!makeEntry(project, file.lexically_normal(), "", entry, warnings, error)) {
        Out::warning("%s", error.c_str());
        return false;
    }
    for (const std::string& warning : warnings) Out::warning("%s", warning.c_str());
    std::vector<Entity> skipped;
    if (!attach(project, sceneProject, entities, entry, -1, skipped, error, nullptr, commandSceneId)) {
        Out::warning("%s", error.c_str());
        return false;
    }
    if (!skipped.empty()) {
        Out::info("%s was already on %zu of the GameObjects", entry.className.c_str(), skipped.size());
    }
    return true;
}

std::vector<ScriptCandidate> ScriptAttach::findProjectScripts(Project* project) {
    std::vector<ScriptCandidate> scripts;
    const fs::path projectPath = project->getProjectPath();
    const fs::path luaPath = project->getLuaPath();
    for (const fs::path& rel : project->findProjectFiles([](const std::string& p) { return Util::isLuaFile(p); })) {
        const fs::path file = (projectPath / rel).lexically_normal();
        if (!Util::isInsidePath(file, luaPath)) continue;
        ScriptCandidate candidate;
        candidate.type = ScriptType::LUA;
        candidate.name = file.stem().string();
        candidate.file = file;
        scripts.push_back(std::move(candidate));
    }
    for (const fs::path& rel : project->findProjectFiles([](const std::string& p) { return Util::isHeaderFile(p); })) {
        const fs::path header = (projectPath / rel).lexically_normal();
        const bool inScriptDir = std::any_of(project->getScriptDirs().begin(), project->getScriptDirs().end(), [&](const fs::path& dir) {
            return Util::isInsidePath(header, dir.is_absolute() ? dir : projectPath / dir);
        });
        if (!inScriptDir) continue;
        const std::optional<std::string> className = ScriptParser::findScriptClassName(header);
        if (!className) continue;
        const std::optional<bool> scriptBase = ScriptParser::inheritsScriptBase(header, *className);
        if (!scriptBase) continue;
        fs::path source;
        std::error_code ec;
        for (const char* ext : {".cpp", ".cc", ".cxx"}) {
            fs::path candidate = header;
            candidate.replace_extension(ext);
            if (fs::is_regular_file(candidate, ec)) {
                source = candidate;
                break;
            }
        }
        if (source.empty()) continue;
        ScriptCandidate candidate;
        candidate.type = ScriptType::CPP;
        candidate.name = *className;
        candidate.file = header;
        candidate.source = source;
        candidate.subclass = !*scriptBase;
        if (candidate.subclass) candidate.baseClass = baseClassOf(header, *className);
        scripts.push_back(std::move(candidate));
    }
    std::sort(scripts.begin(), scripts.end(), [](const ScriptCandidate& a, const ScriptCandidate& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.file < b.file;
    });
    return scripts;
}

bool ScriptAttach::fitsEntity(const ScriptCandidate& candidate, SceneProject* sceneProject, Entity entity) {
    if (!candidate.subclass || candidate.baseClass.empty()) return true;
    const ProjectUtils::EntityClassInfo info = ProjectUtils::getEntityClassInfo(sceneProject->scene, entity);
    return candidate.baseClass == info.name || candidate.baseClass == "EntityHandle" ||
           (candidate.baseClass == "GameObject" && info.derivesObject) || (candidate.baseClass == "Mesh" && info.derivesMesh);
}
