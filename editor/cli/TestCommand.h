// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace jasperengine::editor::cli {

    // test --project <dir> [options] (docs/specs/project-test-runner.md R44, §6.7): the project's tests
    // through MCP run_tests, in the editor that has the project open over HTTP, else a one-shot headless
    // editor (the `call` machinery). One line per case and the counts; --junit and --json files.
    // Exit codes: 0 every selected case passed or was skipped, 1 a case failed, errored or hung, 2 bad
    // arguments or it could not run (the project locked by a stdio editor, no editor), 3 no case matched.
    class TestCommand {
    public:
        // argv[0] is "test"
        static int run(int argc, char** argv, const char* executableName);
    };

}
