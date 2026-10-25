// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "cli/SchemaArgs.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// The MCP client of `jasperengine-editor call`: JSON-RPC over the editor's Streamable HTTP
// (attach to a running editor) or over the stdio of an editor it starts itself (spawn).
namespace jasperengine::editor::cli {

    // A failure talking to the editor, with the exit code it maps to: Connection (refused,
    // 401/403, unknown session, timeout, the spawned editor gone) or Internal (malformed JSON-RPC)
    class ClientError : public std::runtime_error {
    public:
        ClientError(Exit code, const std::string& message): std::runtime_error(message), exitCode(code) {}
        Exit exitCode;
    };

    using NotificationSink = std::function<void(const Json& notification)>;

    class McpClient {
    public:
        virtual ~McpClient() = default;

        // initialize + notifications/initialized; the initialize result. Throws ClientError
        // (Internal for an unsupported protocol version).
        Json initialize(double timeoutSeconds);
        // One request; notifications that come before its response go to sink. Returns the
        // response message ({result} or {error}). On timeout or interrupt it sends
        // notifications/cancelled and throws ClientError(Connection).
        Json request(const std::string& method, const Json& params, double timeoutSeconds, const NotificationSink& sink = {});
        void notify(const std::string& method, const Json& params = Json::object());
        // Ends the session (HTTP DELETE / closing the editor's stdin)
        virtual void close() {}

        // Polled while waiting (Ctrl-C): true stops the wait like a timeout
        std::function<bool()> interrupted;
        const Json& serverInfo() const { return info; }
        // http://127.0.0.1:<port>/mcp, empty for a spawned editor
        virtual std::string url() const { return {}; }

    protected:
        // Sends one message; for a request, reads until its response (notifications to sink)
        virtual Json exchange(const Json& message, bool isRequest, double timeoutSeconds, const NotificationSink& sink) = 0;
        bool stopRequested() const { return interrupted && interrupted(); }

    private:
        int64_t nextId = 0;
        Json info;
    };

    // Streamable HTTP on 127.0.0.1: one POST per message (Connection: close), Mcp-Session-Id
    // kept after initialize, a bearer token when given. Sends no Origin and Host 127.0.0.1:<port>.
    class HttpClient : public McpClient {
    public:
        HttpClient(int port, std::string path, std::string token);
        void close() override;
        std::string url() const override;

    protected:
        Json exchange(const Json& message, bool isRequest, double timeoutSeconds, const NotificationSink& sink) override;

    private:
        int port;
        std::string path;
        std::string token;
        std::string sessionId;
        std::string protocolVersion;
    };

    // `<executable> mcp --project DIR --headless` with pipes on stdio, started in a scratch
    // working directory (an empty settings.yaml: the user's settings are never read or
    // rewritten); its stderr goes to <workdir>/editor.stderr.log. close() closes its stdin,
    // which makes it quit, and kills only that process if it is still there after 30 s.
    class StdioClient : public McpClient {
    public:
        StdioClient(const std::filesystem::path& executable, const std::vector<std::string>& args,
                    const std::filesystem::path& workdir);
        ~StdioClient() override;
        void close() override;
        long pid() const { return childPid; }
        // The last lines the editor wrote to stderr
        std::string stderrTail(size_t lines = 20) const;
        // The spawned editor exited (with its code in exitCode)
        bool exited(int& exitCode);

    protected:
        Json exchange(const Json& message, bool isRequest, double timeoutSeconds, const NotificationSink& sink) override;

    private:
        std::filesystem::path stderrLog;
        long childPid = -1;
        intptr_t toChild = -1;     // the editor's stdin (write end)
        intptr_t fromChild = -1;   // the editor's stdout (read end)
#ifdef _WIN32
        void* process = nullptr;
#endif
        std::string buffer;        // stdout bytes not yet split into lines
        bool finished = false;
        int finishedCode = 0;
        bool readLine(std::string& line, int timeoutMs);
        void writeLine(const std::string& line);
    };

}
