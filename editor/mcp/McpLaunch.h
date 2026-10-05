// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace jasperengine::editor::mcp {

    // `jasperengine-editor mcp [--project <dir|project.yaml>] [--create <dir> [--name N]]
    //  [--headless] [--http-port N]`: the GUI editor serving MCP on the original stdio.
    // argv[0] is "mcp". Sets the stdio aside (fd 1 becomes stderr, fd 0 /dev/null) and,
    // with --headless and no DISPLAY, starts Xvfb. Returns -1 to go on and start the
    // editor, otherwise the exit code (help, bad arguments, no Xvfb).
    int prepareMcpCommand(int argc, char** argv, const char* executableName);

    // After the editor loop: stops the Xvfb prepareMcpCommand started.
    void finishMcpCommand();

    // `jasperengine-editor --mcp-port N` (or --mcp-port=N): the normal editor with the MCP
    // HTTP server on port N. True when argv is exactly that flag (the port is stored).
    bool parseMcpPortFlag(int argc, char** argv);

}
