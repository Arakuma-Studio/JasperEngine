// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace jasperengine::editor::mcp {

    // Window > MCP > MCP Server: the embedded MCP server's status (stdio / HTTP, connected
    // clients), the HTTP switch and port, the HTTP bearer token (copy, connect command, new
    // token) and the "Allow changes" switch for HTTP clients (all persisted in settings.yaml),
    // and the recent tool calls with their result and duration.
    class McpActivityWindow {
    public:
        static constexpr const char* WINDOW_NAME = "MCP Server";

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

    private:
        bool windowOpen = false;
        bool focusRequested = false;
        bool settingsLoaded = false;
        bool httpEnabled = false;
        int httpPort = 0;
        bool autoScroll = true;
        bool showToken = false;

        void drawStatus();
        void drawActivity();
    };

}
