// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Scripts (S05 R11): in edit mode the script properties of every GameObject using a changed
// script are parsed again, Lua is syntax-checked and lua-language-server's stubs are written.
// While a scene plays, a script change goes to S23's hook (ScriptReloadService), which reloads
// it during Play or leaves it for Play to stop (Play restores the scene from a snapshot). A C++ change marks the
// scripts as needing a build, as before. Open code editor buffers follow the disk.

#include "ReloadHandlers.h"

#include "EditorHost.h"
#include "Generator.h"
#include "Project.h"
#include "command/type/PropertyCmd.h"
#include "component/ScriptComponent.h"
#include "mcp/McpRefs.h"
#include "util/ProjectChecks.h"
#include "util/ScriptPropertyCache.h"
#include "util/Util.h"
#include "window/CodeEditor.h"

#include <algorithm>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace fs = std::filesystem;

namespace {

    std::vector<std::string> propertyNames(const ScriptEntry& entry) {
        std::vector<std::string> names;
        for (const ScriptProperty& prop : entry.properties) names.push_back(prop.name);
        return names;
    }

}

bool reload::isOpenInCodeEditor(const fs::path& path) {
    CodeEditor* editor = getEditorHost().getCodeEditor();
    return editor && editor->isFileOpen(path.string());
}

void reload::reloadCodeEditorBuffers(Context& ctx, const std::vector<fs::path>& files, bool all) {
    CodeEditor* editor = getEditorHost().getCodeEditor();
    if (!editor || (files.empty() && !all)) return;
    std::vector<std::string> reloaded, conflicts;
    if (all) editor->syncWithDisk(reloaded, conflicts);
    else editor->onFilesChanged(files, reloaded, conflicts);
    ctx.result.codeEditor["reloaded"] = reloaded;
    ctx.result.codeEditor["conflicts"] = conflicts;
    for (const std::string& path : conflicts) {
        ctx.result.problems.push_back({{"path", path}, {"message",
            "Changed on disk while the code editor has unsaved edits of it: the editor shows a Files Changed prompt. "
            "Resolve with code_editor (action reload = take the disk version, save = keep the buffer)."}});
    }
}

void reload::removeScriptClasses(Context& ctx, const std::vector<fs::path>& gone) {
    std::vector<fs::path> lua;
    for (const fs::path& path : gone) {
        if (Util::isLuaFile(path.string())) lua.push_back(path.lexically_normal());
    }
    if (lua.empty()) return;
    // the class of a script of the same name elsewhere stays (it is written again for that one)
    Generator::writeLuaScriptStubs(ctx.project.getProjectPath(), ctx.project.getProjectInternalPath(), lua);
}

void reload::reloadScripts(Context& ctx, const std::vector<fs::path>& filesIn, bool all) {
    Project* project = &ctx.project;
    std::set<fs::path> changed;
    for (const fs::path& path : filesIn) changed.insert(path.lexically_normal());
    if (changed.empty() && !all) return;

    std::set<fs::path> checkedLua;
    ScriptPropertyCache cache;
    if (ctx.playing) {
        // S23's hook takes a Lua change now (a C++ one too when C++ reload is on), else it
        // waits for Play to stop; C++ otherwise waits for the next Play's build
        std::vector<fs::path> waiting;
        for (const fs::path& path : changed) {
            if (ctx.service.handleScriptInPlay(path)) {
                ctx.service.recordDone("script", path, 0.0);
                continue;
            }
            waiting.push_back(path);
        }
        if (!waiting.empty()) {
            ctx.service.deferScripts(waiting);
            ctx.result.notes.push_back("A scene is playing: script properties are re-parsed after Play stops.");
        }
    } else {
        for (SceneProject* sceneProject : project->getScenesAndPrefabStages()) {
            if (!sceneProject->scene) continue;
            for (Entity entity : sceneProject->entities) {
                ScriptComponent* component = sceneProject->scene->findComponent<ScriptComponent>(entity);
                if (!component || component->scripts.empty()) continue;

                bool relevant = all;
                for (const ScriptEntry& entry : component->scripts) {
                    if (changed.count(project->scriptPropertyFile(entry)) || changed.count(project->scriptSourceFile(entry))) relevant = true;
                }
                if (!relevant) continue;

                std::vector<ScriptEntry> scripts = component->scripts;
                for (const ScriptEntry& entry : scripts) {
                    const fs::path file = project->scriptPropertyFile(entry);
                    std::error_code ec;
                    if (file.empty()) continue;
                    if (!fs::exists(file, ec)) {
                        ctx.result.problems.push_back({{"path", rel(*project, file)}, {"entity", mcp::McpRefs::entityJson(sceneProject, entity)},
                                                       {"message", "Script file of " + entry.className + " is missing."}});
                        continue;
                    }
                    if (entry.type == ScriptType::LUA && checkedLua.insert(file).second) {
                        std::string code;
                        if (ProjectChecks::readFile(file, code)) {
                            ProjectChecks::LuaSyntax syntax = ProjectChecks::checkLuaSyntax(code, rel(*project, file));
                            if (!syntax.ok) {
                                ctx.result.problems.push_back({{"path", rel(*project, file)}, {"line", syntax.line},
                                                               {"message", "Lua syntax error: " + syntax.message +
                                                                           " (the Inspector keeps the last properties it could read)."}});
                            }
                        }
                    }
                }

                std::vector<std::vector<std::string>> before;
                for (const ScriptEntry& entry : scripts) before.push_back(propertyNames(entry));
                if (project->updateScriptProperties(sceneProject, entity, scripts, "", "", &cache)) {
                    PropertyCmd<std::vector<ScriptEntry>> refresh(project, sceneProject->id, entity, ComponentType::ScriptComponent, "scripts", scripts);
                    refresh.execute();
                    for (size_t i = 0; i < scripts.size(); i++) {
                        std::vector<std::string> after = propertyNames(scripts[i]);
                        Json plus = Json::array(), minus = Json::array();
                        for (const std::string& name : after) {
                            if (std::find(before[i].begin(), before[i].end(), name) == before[i].end()) plus.push_back(name);
                        }
                        for (const std::string& name : before[i]) {
                            if (std::find(after.begin(), after.end(), name) == after.end()) minus.push_back(name);
                        }
                        Json item = {{"scene", sceneProject->id}, {"entity", mcp::McpRefs::entityJson(sceneProject, entity)},
                                     {"script_index", i}, {"script", scripts[i].className}, {"properties", after}};
                        if (!plus.empty()) item["added"] = plus;
                        if (!minus.empty()) item["removed"] = minus;
                        ctx.result.scriptProperties.push_back(item);
                    }
                }
            }
        }
        for (const fs::path& path : changed) ctx.service.recordDone("script", path, 0.0);
    }

    // Lua files changed but attached nowhere still get a syntax check (safe during Play)
    for (const fs::path& path : changed) {
        if (!Util::isLuaFile(path.string()) || checkedLua.count(path)) continue;
        std::string code;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || !ProjectChecks::readFile(path, code)) continue;
        ProjectChecks::LuaSyntax syntax = ProjectChecks::checkLuaSyntax(code, rel(*project, path));
        if (!syntax.ok) {
            ctx.result.problems.push_back({{"path", rel(*project, path)}, {"line", syntax.line}, {"message", "Lua syntax error: " + syntax.message}});
        }
    }

    // lua-language-server's classes of the changed scripts (of every script on a full sync)
    std::vector<fs::path> changedLua;
    for (const fs::path& path : changed) {
        if (Util::isLuaFile(path.string())) changedLua.push_back(path);
    }
    if (all || !changedLua.empty()) {
        Generator::writeLuaScriptStubs(project->getProjectPath(), project->getProjectInternalPath(), all ? std::vector<fs::path>() : changedLua);
    }
}
