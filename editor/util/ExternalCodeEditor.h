// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Where a file location opens (Console click-to-source, "Open in External Editor", MCP
// code_editor open): the built-in Code Editor, or the External Script Editor chosen in
// Preferences > External Tools. Launches run the editor program directly (no shell).

#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class ExternalCodeEditor {
    public:
        struct Choice {
            const char* id;          // the preference value
            const char* name;        // what Preferences shows
        };
        // Built-in Code Editor first, then Visual Studio Code, Visual Studio, Rider, CLion,
        // Xcode, Sublime Text, Zed, System Default, Custom
        static const std::vector<Choice>& choices();

        // The program and arguments that open file at line and column with that editor (the
        // custom one fills {file} {line} {column} {project} in its template); empty, with
        // the reason, when it cannot (not found, Xcode off macOS, an empty template). No
        // program for "builtin" and "system".
        static std::vector<std::string> command(const std::string& editorId, const std::filesystem::path& file, int line, int column,
                                                const std::filesystem::path& project, const std::string& customTemplate,
                                                std::string* error = nullptr);
        // A template split into arguments: whitespace separates, "double quotes" group
        static std::vector<std::string> splitTemplate(const std::string& text);

        // Opens the location: in the external editor of the preferences when `preferred` and
        // one is set, else (or when it fails to start, with a warning) in the built-in Code
        // Editor. Main thread.
        static bool open(const std::filesystem::path& file, int line, int column, bool preferred, std::string* error = nullptr);
        // Opens in the preferred external editor only ("Open in External Editor"); the
        // built-in editor when none is set
        static bool openExternal(const std::filesystem::path& file, int line, int column, std::string* error = nullptr);
        // The preference's id is an external program (not builtin)
        static bool hasExternalPreference();
    };

}
