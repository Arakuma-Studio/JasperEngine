// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace jasperengine::editor {

    class CommandLine {
    public:
        // Runs the project export subcommand.
        //
        // `argc` and `argv` should already be shifted past the `export`
        // subcommand token (i.e. argv[0] is the subcommand itself).
        // `executableName` is used only for help and error output.
        static int runExportCommand(int argc, char** argv, const char* executableName);

        // Runs the standalone shader generation subcommand.
        static int runShadersCommand(int argc, char** argv, const char* executableName);

        // Developer check: dumps every scene's GameObjects of a project (see CommandLine.cpp)
        static int runSceneDumpCommand(int argc, char** argv, const char* executableName);

        static int runBenchmarkHelp(const char* executableName);

        // Checks a project's Lua scripts, shader forks, C++ scripts and HTML/CSS UI files (see docs/mcp/checks.md)
        static int runCheckCommand(int argc, char** argv, const char* executableName);

        // Prints the HTML/CSS compatibility support tables as JSON (developer: docs generation)
        static int runUiSupportCommand(int argc, char** argv, const char* executableName);

        // Lists the HTML UI effects (built-in and a project's), as text or JSON; --validate compiles them
        static int runUiEffectsCommand(int argc, char** argv, const char* executableName);

        // Imports the remote resources of HTML UI documents (editor/htmlui/HtmlUiImportCli.h)
        static int runImportUiCommand(int argc, char** argv, const char* executableName);

        // Prints the top level usage, or reports an unrecognized argument.
        static int runHelpCommand(int argc, char** argv, const char* executableName);
    };

}
