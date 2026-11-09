// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace jasperengine::editor::cli {

    // The MCP tools from a shell (docs/mcp/cli.md, S31):
    //   call [globals] <tool> [--flags | '{json}' | @file]   one tool call
    //   batch [globals] [--continue] [--atomic] < calls.ndjson one session, one envelope per line
    //   list-tools [globals] [--json] [--group G|all]
    //   editors [--json]                                     the running editors serving HTTP
    // Flags, --help and validation come from each tool's JSON Schema. Exit codes: 0 ok, 1 the
    // tool reported an error, 2 usage, 3 connection (timeout, refused, project_locked),
    // 4 internal. None of them starts the backend, opens a window or writes settings.yaml or
    // editor.log in the working directory.
    class CallCommand {
    public:
        // argv[0] is the subcommand ("call", "batch", "list-tools" or "editors")
        static int run(int argc, char** argv, const char* executableName);
        // After run(): the editor it started for --project stopped, or did not open the project within the
        // spawn timeout, before any tool ran (exit 3 says only "connection"; this is the part of it where no
        // editor came up at all)
        static bool spawnFailed();
    };

}
