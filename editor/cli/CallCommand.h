// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "json.hpp"

#include <string>
#include <utility>
#include <vector>

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
    // one call of runCalls and what it gave
    struct CallResult {
        std::string tool;
        bool ok = false;
        int exit = 0;
        std::string text;
        nlohmann::json structured;
    };

    class CallCommand {
    public:
        // argv[0] is the subcommand ("call", "batch", "list-tools" or "editors")
        static int run(int argc, char** argv, const char* executableName);
        // the calls over one session of `project` (a running editor of it, else one one-shot
        // headless editor for them all: S87's export-title-art), in order, stopping at the first
        // failure unless `continueOnError`; the highest exit code
        static int runCalls(const std::string& project, const std::vector<std::pair<std::string, nlohmann::json>>& calls,
                            bool continueOnError, std::vector<CallResult>& results, const char* executableName);
    };

}
