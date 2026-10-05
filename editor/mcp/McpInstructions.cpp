// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "McpServer.h"

// Placeholder: the full agent guide (ported from the former in-editor agent's system prompt
// and extended for the MCP tools) replaces this text. Keep the signature.
const char* jasperengine::editor::mcp::mcpServerInstructions() {
    return
        "You are driving the JasperEngine editor (a Unity-style C++ game engine editor) through MCP. "
        "Every edit goes through the editor's command system, so it is undoable (undo / redo tools) and "
        "shows live in the editor window. Start with editor_status and get_project_summary to see the open "
        "project and its scenes; scenes are addressed by id (or name), GameObjects by id or by a '/'-joined "
        "name path from the scene root. Use batch to run several tool calls in one round-trip. Tools that "
        "start long operations return a job id: poll it with get_job. Save scenes (save_scene / "
        "save_all_scenes) before you finish: the editor never saves on its own.";
}
