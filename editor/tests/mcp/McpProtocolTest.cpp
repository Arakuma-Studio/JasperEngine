// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The MCP server's protocol layer without an editor: MCP 2026-07-28 next to the
// initialize-era versions (server/discover, per-request _meta, resultType and cache hints,
// HTTP statuses of its errors), the HTTP transport's header checks (MCP-Protocol-Version /
// Mcp-Method / Mcp-Name, base64 header values) and its bearer token.

#include <gtest/gtest.h>

#include "mcp/McpServer.h"
#include "mcp/McpTransportHttp.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine::editor::mcp;

namespace {

    constexpr const char* MODERN = "2026-07-28";

    // Records what the server answered and with which HTTP status (200: a plain respond)
    class RecordingReply : public McpReply {
    public:
        std::vector<Json> messages;
        std::vector<int> statuses;

        void respond(const Json& message) override {
            messages.push_back(message);
            statuses.push_back(200);
        }
        void respondError(const Json& message, int httpStatus) override {
            messages.push_back(message);
            statuses.push_back(httpStatus);
        }
        void notify(const Json&) override {}
    };

    Json modernMeta(const std::string& version = MODERN) {
        return Json{{"io.modelcontextprotocol/protocolVersion", version},
                    {"io.modelcontextprotocol/clientInfo", {{"name", "unit-test"}, {"version", "1"}}},
                    {"io.modelcontextprotocol/clientCapabilities", Json::object()}};
    }

    Json request(const std::string& method, Json params = Json::object(), int id = 1) {
        return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
    }

    // One request through the server; the single answer and its HTTP status
    std::pair<Json, int> ask(const SessionPtr& session, const Json& message) {
        auto reply = std::make_shared<RecordingReply>();
        McpServer::get().handleMessage(session, message, reply);
        EXPECT_EQ(reply->messages.size(), 1u) << message.dump();
        if (reply->messages.empty()) return {Json(), 0};
        return {reply->messages[0], reply->statuses[0]};
    }

    SessionPtr statelessSession() {
        return McpServer::get().createStatelessSession("http");
    }

    std::map<std::string, std::string> modernHeaders(const std::string& method, const std::string& name = "") {
        std::map<std::string, std::string> headers = {{"mcp-protocol-version", MODERN}, {"mcp-method", method}};
        if (!name.empty()) headers["mcp-name"] = name;
        return headers;
    }

}

// -- 2026-07-28 next to the initialize-era versions ------------------------------------------

TEST(McpProtocol, DiscoverAnswersWithVersionsAndCacheHints) {
    const auto [answer, status] = ask(statelessSession(), request("server/discover", {{"_meta", modernMeta()}}));
    EXPECT_EQ(status, 200);
    ASSERT_TRUE(answer.contains("result")) << answer.dump();
    const Json& result = answer["result"];
    EXPECT_EQ(result["supportedVersions"], (Json::array({MODERN, "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"})));
    EXPECT_TRUE(result["capabilities"].contains("tools"));
    EXPECT_FALSE(result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"].get<std::string>().empty());
    EXPECT_FALSE(result["instructions"].get<std::string>().empty());
    EXPECT_EQ(result["resultType"], "complete");
    EXPECT_EQ(result["ttlMs"], 0);
    EXPECT_EQ(result["cacheScope"], "private");
}

TEST(McpProtocol, ModernResultsSayComplete) {
    const auto [list, listStatus] = ask(statelessSession(), request("tools/list", {{"_meta", modernMeta()}}));
    EXPECT_EQ(listStatus, 200);
    ASSERT_TRUE(list.contains("result")) << list.dump();
    EXPECT_TRUE(list["result"]["tools"].is_array());
    EXPECT_EQ(list["result"]["resultType"], "complete");
    EXPECT_EQ(list["result"]["ttlMs"], 0);
    EXPECT_EQ(list["result"]["cacheScope"], "private");
    EXPECT_FALSE(list["result"]["_meta"]["io.modelcontextprotocol/serverInfo"]["name"].get<std::string>().empty());

    // Not a cacheable method: resultType, no cache hints
    const auto [completion, completionStatus] = ask(statelessSession(), request("completion/complete", {{"_meta", modernMeta()}}));
    EXPECT_EQ(completion["result"]["resultType"], "complete");
    EXPECT_FALSE(completion["result"].contains("ttlMs"));
}

TEST(McpProtocol, ModernErrorsCarryTheirHttpStatus) {
    // A version the server does not speak: 400, -32022 naming the supported ones
    {
        const auto [answer, status] = ask(statelessSession(), request("tools/list", {{"_meta", modernMeta("2099-01-01")}}));
        EXPECT_EQ(status, 400);
        EXPECT_EQ(answer["error"]["code"], -32022);
        EXPECT_EQ(answer["error"]["data"]["requested"], "2099-01-01");
        EXPECT_EQ(answer["error"]["data"]["supported"], (Json::array({MODERN, "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"})));
        EXPECT_EQ(answer["id"], 1);
    }
    // The required clientCapabilities missing: 400, Invalid params
    {
        Json meta = modernMeta();
        meta.erase("io.modelcontextprotocol/clientCapabilities");
        const auto [answer, status] = ask(statelessSession(), request("tools/list", {{"_meta", meta}}));
        EXPECT_EQ(status, 400);
        EXPECT_EQ(answer["error"]["code"], -32602);
    }
    // An unknown method, initialize and logging/setLevel (gone in 2026-07-28): 404, -32601
    for (const char* method : {"no/such/method", "initialize", "logging/setLevel"}) {
        const auto [answer, status] = ask(statelessSession(), request(method, {{"_meta", modernMeta()}}));
        EXPECT_EQ(status, 404) << method;
        EXPECT_EQ(answer["error"]["code"], -32601) << method;
    }
}

TEST(McpProtocol, StdioRequestWithMetaIsModern) {
    // A stdio client probes with server/discover before (or instead of) initialize
    const SessionPtr session = McpServer::get().createSession("stdio");
    const auto [discover, status] = ask(session, request("server/discover", {{"_meta", modernMeta()}}));
    EXPECT_EQ(discover["result"]["resultType"], "complete");
    const auto [wrong, wrongStatus] = ask(session, request("server/discover", {{"_meta", modernMeta("2025-11-25")}}));
    EXPECT_EQ(wrong["error"]["code"], -32022);
    McpServer::get().closeSession(session->id);
}

TEST(McpProtocol, LegacyRequestsAnswerAsBefore) {
    const SessionPtr session = McpServer::get().createSession("http");
    const auto [init, initStatus] = ask(session, request("initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()},
                                                                       {"clientInfo", {{"name", "legacy"}, {"version", "1"}}}}));
    EXPECT_EQ(initStatus, 200);
    EXPECT_EQ(init["result"]["protocolVersion"], "2025-06-18");
    EXPECT_FALSE(init["result"].contains("resultType"));
    const auto [unknown, unknownStatus] = ask(session, request("initialize", {{"protocolVersion", "1999-01-01"}}));
    EXPECT_EQ(unknown["result"]["protocolVersion"], "2025-11-25");

    const auto [list, listStatus] = ask(session, request("tools/list"));
    EXPECT_FALSE(list["result"].contains("resultType"));
    EXPECT_FALSE(list["result"].contains("ttlMs"));
    const auto [missing, missingStatus] = ask(session, request("no/such/method"));
    EXPECT_EQ(missingStatus, 200);
    EXPECT_EQ(missing["error"]["code"], -32601);
    const auto [level, levelStatus] = ask(session, request("logging/setLevel", {{"level", "info"}}));
    EXPECT_TRUE(level.contains("result"));
    McpServer::get().closeSession(session->id);
}

// -- HTTP transport ------------------------------------------------------------------------

TEST(McpHttp, DecodeHeaderValue) {
    EXPECT_EQ(McpTransportHttp::decodeHeaderValue("get_weather"), "get_weather");
    EXPECT_EQ(McpTransportHttp::decodeHeaderValue("=?base64?SGVsbG8sIOS4lueVjA==?="), "Hello, \xE4\xB8\x96\xE7\x95\x8C");
    EXPECT_EQ(McpTransportHttp::decodeHeaderValue("=?base64?IHBhZGRlZCA=?="), " padded ");
    EXPECT_EQ(McpTransportHttp::decodeHeaderValue("=?base64?PT9iYXNlNjQ/bGl0ZXJhbD89?="), "=?base64?literal?=");
    // Not the sentinel: as sent
    EXPECT_EQ(McpTransportHttp::decodeHeaderValue("=?base64?abc"), "=?base64?abc");
    EXPECT_EQ(McpTransportHttp::decodeHeaderValue("=?BASE64?SGk=?="), "=?BASE64?SGk=?=");
    EXPECT_EQ(McpTransportHttp::decodeHeaderValue(""), "");
}

TEST(McpHttp, ModernHeadersMustMirrorTheBody) {
    const Json list = request("tools/list", {{"_meta", modernMeta()}});
    EXPECT_TRUE(McpTransportHttp::checkModernHeaders(modernHeaders("tools/list"), list).is_null());

    auto code = [](const std::map<std::string, std::string>& headers, const Json& message) {
        const Json error = McpTransportHttp::checkModernHeaders(headers, message);
        return error.is_null() ? 0 : error["error"]["code"].get<int>();
    };
    // MCP-Protocol-Version missing, or not the _meta version
    std::map<std::string, std::string> headers = modernHeaders("tools/list");
    headers.erase("mcp-protocol-version");
    EXPECT_EQ(code(headers, list), -32020);
    headers["mcp-protocol-version"] = "2025-11-25";
    EXPECT_EQ(code(headers, list), -32020);
    // The header names 2026-07-28 but the body has no _meta version
    EXPECT_EQ(code(modernHeaders("tools/list"), request("tools/list")), -32020);
    // Both name a version the server does not speak
    headers = modernHeaders("tools/list");
    headers["mcp-protocol-version"] = "2099-01-01";
    const Json future = request("tools/list", {{"_meta", modernMeta("2099-01-01")}});
    EXPECT_EQ(code(headers, future), -32022);
    EXPECT_EQ(McpTransportHttp::checkModernHeaders(headers, future)["error"]["data"]["requested"], "2099-01-01");
    // Mcp-Method
    headers = modernHeaders("tools/list");
    headers.erase("mcp-method");
    EXPECT_EQ(code(headers, list), -32020);
    EXPECT_EQ(code(modernHeaders("tools/call", "x"), list), -32020);

    // Mcp-Name: the tool name, prompt name or resource uri; base64 when not plain ASCII
    const Json call = request("tools/call", {{"name", "editor_status"}, {"arguments", Json::object()}, {"_meta", modernMeta()}});
    EXPECT_EQ(code(modernHeaders("tools/call", "editor_status"), call), 0);
    EXPECT_EQ(code(modernHeaders("tools/call", "=?base64?ZWRpdG9yX3N0YXR1cw==?="), call), 0);
    EXPECT_EQ(code(modernHeaders("tools/call", "create_entity"), call), -32020);
    EXPECT_EQ(code(modernHeaders("tools/call"), call), -32020);
    const Json read = request("resources/read", {{"uri", "jasper://scene/caf\xC3\xA9"}, {"_meta", modernMeta()}});
    EXPECT_EQ(code(modernHeaders("resources/read", "=?base64?amFzcGVyOi8vc2NlbmUvY2Fmw6k=?="), read), 0);
    EXPECT_EQ(code(modernHeaders("resources/read", "jasper://scene/cafe"), read), -32020);
    const Json prompt = request("prompts/get", {{"name", "fix_errors"}, {"_meta", modernMeta()}});
    EXPECT_EQ(code(modernHeaders("prompts/get", "fix_errors"), prompt), 0);
    // The error answers the request's id
    EXPECT_EQ(McpTransportHttp::checkModernHeaders(modernHeaders("tools/call"), call)["id"], 1);
}

TEST(McpHttp, BearerToken) {
    const std::string token = McpTransportHttp::newToken();
    ASSERT_EQ(token.size(), 64u);
    EXPECT_EQ(token.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_NE(token, McpTransportHttp::newToken());

    EXPECT_TRUE(McpTransportHttp::bearerMatches("Bearer " + token, token));
    EXPECT_TRUE(McpTransportHttp::bearerMatches("bearer " + token, token));
    EXPECT_TRUE(McpTransportHttp::bearerMatches("  BEARER   " + token + " ", token));
    EXPECT_FALSE(McpTransportHttp::bearerMatches("Basic " + token, token));
    EXPECT_FALSE(McpTransportHttp::bearerMatches(token, token));
    EXPECT_FALSE(McpTransportHttp::bearerMatches("Bearer " + token.substr(1), token));
    EXPECT_FALSE(McpTransportHttp::bearerMatches("Bearer " + token + "0", token));
    EXPECT_FALSE(McpTransportHttp::bearerMatches("", token));
    // No token: nothing gets in
    EXPECT_FALSE(McpTransportHttp::bearerMatches("Bearer ", ""));
    EXPECT_FALSE(McpTransportHttp::bearerMatches("", ""));

    EXPECT_TRUE(McpTransportHttp::constantTimeEquals("abc", "abc"));
    EXPECT_FALSE(McpTransportHttp::constantTimeEquals("abc", "abd"));
    EXPECT_FALSE(McpTransportHttp::constantTimeEquals("abc", "ab"));
    EXPECT_TRUE(McpTransportHttp::constantTimeEquals("", ""));
}
