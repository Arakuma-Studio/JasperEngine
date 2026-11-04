// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ExternalCodeEditor.h"

#include "AppSettings.h"
#include "EditorHost.h"
#include "Out.h"
#include "Platform.h"
#include "Project.h"
#include "reload/HotReloadService.h"
#include "util/CommandRunner.h"
#include "util/FileUtils.h"
#include "util/ShellEnv.h"
#include "window/CodeEditor.h"

#include <cctype>

namespace fs = std::filesystem;

namespace jasperengine::editor {

namespace {

    std::string replaceAll(std::string text, const std::string& from, const std::string& to){
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
        return text;
    }

    // The first of the names found on PATH (absolute), "" when none is
    std::string findProgram(std::initializer_list<const char*> names){
        for (const char* name : names) {
            const std::string found = ShellEnv::findExecutable(name);
            if (!found.empty()) return found;
        }
        return {};
    }

#if defined(_WIN32)
    std::string findDevenv(){
        // no shell: vswhere and its arguments as they are, with -utf8 (CommandRunner::vswhere)
        const std::string path = CommandRunner::vswhere({"-latest", "-property", "productPath"});
        std::error_code ec;
        return !path.empty() && fs::is_regular_file(FileUtils::pathFromUtf8(path), ec) ? path : std::string();
    }
#endif

}

const std::vector<ExternalCodeEditor::Choice>& ExternalCodeEditor::choices(){
    static const std::vector<Choice> list = {
        {"builtin", "Built-in Code Editor"}, {"vscode", "Visual Studio Code"}, {"visualstudio", "Visual Studio"},
        {"rider", "Rider"}, {"clion", "CLion"}, {"xcode", "Xcode"}, {"sublime", "Sublime Text"}, {"zed", "Zed"},
        {"system", "System Default"}, {"custom", "Custom"},
    };
    return list;
}

std::vector<std::string> ExternalCodeEditor::splitTemplate(const std::string& text){
    std::vector<std::string> out;
    std::string current;
    bool quoted = false, any = false;
    for (char c : text) {
        if (c == '"') {
            quoted = !quoted;
            any = true;
        } else if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (any) out.push_back(current);
            current.clear();
            any = false;
        } else {
            current += c;
            any = true;
        }
    }
    if (any) out.push_back(current);
    return out;
}

std::vector<std::string> ExternalCodeEditor::command(const std::string& editorId, const fs::path& file, int line, int column,
                                                     const fs::path& project, const std::string& customTemplate, std::string* error){
    const std::string f = FileUtils::pathToUtf8(file);
    const std::string p = FileUtils::pathToUtf8(project);
    const std::string l = std::to_string(std::max(line, 1));
    const std::string c = std::to_string(std::max(column, 1));
    auto missing = [&](const char* what) {
        if (error) *error = std::string(what) + " was not found on PATH";
        return std::vector<std::string>();
    };

    if (editorId == "vscode") {
        const std::string code = findProgram({"code", "code-insiders", "codium"});
        if (code.empty()) return missing("Visual Studio Code (code)");
        std::vector<std::string> argv = {code};
        if (!p.empty()) argv.push_back(p);
        argv.push_back("-g");
        argv.push_back(f + ":" + l + ":" + c);
        return argv;
    }
    if (editorId == "visualstudio") {
#if defined(_WIN32)
        const std::string devenv = findDevenv();
        if (devenv.empty()) return missing("Visual Studio (devenv, through vswhere)");
        return {devenv, "/edit", f, "/command", "Edit.GoTo " + l};
#else
        if (error) *error = "Visual Studio is a Windows program";
        return {};
#endif
    }
    if (editorId == "rider") {
        const std::string rider = findProgram({"rider", "rider64", "rider.sh"});
        if (rider.empty()) return missing("Rider (rider)");
        return {rider, "--line", l, f};
    }
    if (editorId == "clion") {
        const std::string clion = findProgram({"clion", "clion64", "clion.sh"});
        if (clion.empty()) return missing("CLion (clion)");
        return {clion, "--line", l, f};
    }
    if (editorId == "xcode") {
#if defined(__APPLE__)
        const std::string xed = findProgram({"xed"});
        if (xed.empty()) return missing("Xcode (xed)");
        return {xed, "-l", l, f};
#else
        if (error) *error = "Xcode is a macOS program";
        return {};
#endif
    }
    if (editorId == "sublime") {
        const std::string subl = findProgram({"subl", "sublime_text"});
        if (subl.empty()) return missing("Sublime Text (subl)");
        return {subl, f + ":" + l + ":" + c};
    }
    if (editorId == "zed") {
        const std::string zed = findProgram({"zed", "zeditor"});
        if (zed.empty()) return missing("Zed (zed)");
        return {zed, f + ":" + l + ":" + c};
    }
    if (editorId == "custom") {
        std::vector<std::string> argv = splitTemplate(customTemplate);
        if (argv.empty()) {
            if (error) *error = "the Custom external editor has no command (Preferences > External Tools)";
            return {};
        }
        for (std::string& argument : argv) {
            argument = replaceAll(argument, "{file}", f);
            argument = replaceAll(argument, "{line}", l);
            argument = replaceAll(argument, "{column}", c);
            argument = replaceAll(argument, "{project}", p);
        }
        return argv;
    }
    if (error) *error = "'" + editorId + "' starts no program";
    return {};
}

bool ExternalCodeEditor::hasExternalPreference(){
    return AppSettings::getExternalScriptEditor() != "builtin";
}

bool ExternalCodeEditor::openExternal(const fs::path& file, int line, int column, std::string* error){
    const std::string id = AppSettings::getExternalScriptEditor();
    std::string why;
    bool started = false;
    if (id == "system") {
        started = Platform::openWithSystemDefault(FileUtils::pathToUtf8(file), &why);
    } else if (id != "builtin") {
        fs::path projectPath;
        if (Project* project = HotReloadService::get().getProject()) projectPath = project->getProjectPath();
        const std::vector<std::string> argv = command(id, file, line, column, projectPath, AppSettings::getExternalScriptEditorCommand(), &why);
        started = !argv.empty() && Platform::launchDetached(argv, &why);
    }
    if (started) return true;
    if (id != "builtin") Out::warning("Could not open %s in the external editor (%s): opened in the Code Editor", FileUtils::pathToUtf8(file).c_str(), why.c_str());
    CodeEditor* editor = getEditorHost().getCodeEditor();
    if (!editor) {
        if (error) *error = why.empty() ? "no code editor" : why;
        return false;
    }
    editor->openFileAt(FileUtils::pathToUtf8(file), line, column);
    return true;
}

bool ExternalCodeEditor::open(const fs::path& file, int line, int column, bool preferred, std::string* error){
    if (preferred && hasExternalPreference()) return openExternal(file, line, column, error);
    CodeEditor* editor = getEditorHost().getCodeEditor();
    if (!editor) {
        if (error) *error = "no code editor";
        return false;
    }
    editor->openFileAt(FileUtils::pathToUtf8(file), line, column);
    return true;
}

}
