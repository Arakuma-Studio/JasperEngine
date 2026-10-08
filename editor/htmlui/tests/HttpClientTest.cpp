// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HttpClient (phase-2 §3.13.1): curl through a config file, the editor's User-Agent, errors,
// timeouts, redirects, host rewrites and the JASPERENGINE_IMPORT_MIRROR mode.

#include "ImporterTestCommon.h"

#include "htmlui/HtmlUiImporters.h"

using namespace jasperengine::editor::htmlui;
using namespace jasperengine::editor::htmlui::test;

TEST(HttpClient, CurlIsAvailable){
    EXPECT_TRUE(HttpClient::available());
}

TEST(HttpClient, GetsABodyWithTypeAndRewrittenUrl){
    HttpClient client;
    const HttpResponse response = client.get("https://images.example.com/placeholder");
    ASSERT_TRUE(response.ok()) << response.error;
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.contentType, "image/svg+xml");
    // the caller's URL, not the fixture server's
    EXPECT_EQ(response.finalUrl, "https://images.example.com/placeholder");
    std::string body;
    ASSERT_TRUE(HttpClient::readBody(response, body));
    EXPECT_NE(body.find("<svg"), std::string::npos);
}

TEST(HttpClient, SendsTheEditorUserAgentNeverABrowserOne){
    sharedServer().reset();
    HttpClient client;
    ASSERT_TRUE(client.get("https://fonts.googleapis.com/css2?family=Inter:wght@400").ok());
    const auto requests = sharedServer().requests();
    ASSERT_FALSE(requests.empty());
    for (const auto& request : requests){
        EXPECT_EQ(request.second, HttpClient::kUserAgent) << request.first;
        EXPECT_EQ(request.second.find("Mozilla"), std::string::npos);
    }
}

TEST(HttpClient, HttpErrorsKeepTheirStatus){
    HttpClient client;
    const HttpResponse notFound = client.get("https://status.example.com/404");
    EXPECT_FALSE(notFound.ok());
    EXPECT_EQ(notFound.status, 404);
    EXPECT_EQ(notFound.error, "HTTP 404");
    // fail-with-body: the error page is still there
    std::string body;
    EXPECT_TRUE(HttpClient::readBody(notFound, body));
    EXPECT_NE(body.find("missing"), std::string::npos);

    const HttpResponse broken = client.get("https://status.example.com/500");
    EXPECT_FALSE(broken.ok());
    EXPECT_EQ(broken.status, 500);
}

TEST(HttpClient, TimesOut){
    HttpClient client;
    client.setTimeoutSeconds(1);
    const auto started = std::chrono::steady_clock::now();
    const HttpResponse response = client.get("https://slow.example.com?s=4");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    EXPECT_FALSE(response.ok());
    EXPECT_EQ(response.status, 0);
    EXPECT_NE(lowerAscii(response.error).find("timed out"), std::string::npos) << response.error;
    EXPECT_LT(seconds, 3.5);
}

TEST(HttpClient, FollowsRedirects){
    HttpClient client;
    const HttpResponse response = client.get("https://redirect.example.com");
    ASSERT_TRUE(response.ok()) << response.error;
    EXPECT_EQ(response.contentType, "image/svg+xml");
    EXPECT_EQ(response.finalUrl, "https://images.example.com/placeholder");
}

TEST(HttpClient, ConnectionRefusedIsAnError){
    HttpClient client;
    client.setHostRewrite({{"https://nowhere.example.com", "http://127.0.0.1:1"}});
    const HttpResponse response = client.get("https://nowhere.example.com/x.png");
    EXPECT_FALSE(response.ok());
    EXPECT_EQ(response.status, 0);
    EXPECT_FALSE(response.error.empty());
}

TEST(HttpClient, UrlsWithQuotesAndSpacesNeverReachAShell){
    HttpClient client;
    // a quote, a $ and a backtick in the query: the config file carries them, no shell sees them
    const HttpResponse response = client.get("https://images.example.com/photo?w=800&q=\"$(touch /tmp/x)`id`");
    EXPECT_TRUE(response.ok()) << response.error;
}

TEST(HttpClient, UrlQuoteMatchesPython){
    // urllib.parse.quote(s, safe="")
    EXPECT_EQ(HttpClient::urlQuote("/css2?family=Karla:wght@400"), "%2Fcss2%3Ffamily%3DKarla%3Awght%40400");
    EXPECT_EQ(HttpClient::urlQuote("/a b/c~d_e.f-g"), "%2Fa%20b%2Fc~d_e.f-g");
    EXPECT_EQ(HttpClient::urlQuote("\xC3\xA9"), "%C3%A9");
    EXPECT_EQ(HttpClient::mirrorPath("https://fonts.gstatic.com/s/karla/v1/karla.ttf"), "fonts.gstatic.com/%2Fs%2Fkarla%2Fv1%2Fkarla.ttf");
    EXPECT_EQ(HttpClient::mirrorPath("HTTP://Fonts.GoogleAPIs.com/css2?family=Inter+Tight#x"),
              "fonts.googleapis.com/%2Fcss2%3Ffamily%3DInter%2BTight");
    EXPECT_EQ(HttpClient::mirrorPath("ui/local.png"), "");
}

TEST(HttpClient, MirrorServesFilesWithoutNetwork){
    const fs::path mirror = makeTempDir("jasper-mirror-");
    writeFile(mirror / "images.example.com" / HttpClient::urlQuote("/logo.svg?v=2"), "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");
    {
        ScopedEnv env("JASPERENGINE_IMPORT_MIRROR", mirror.string().c_str());
        sharedServer().reset();
        HttpClient client;
        const HttpResponse hit = client.get("https://images.example.com/logo.svg?v=2");
        ASSERT_TRUE(hit.ok()) << hit.error;
        EXPECT_EQ(hit.contentType, "image/svg+xml");
        std::string body;
        ASSERT_TRUE(HttpClient::readBody(hit, body));
        EXPECT_EQ(body, "<svg xmlns=\"http://www.w3.org/2000/svg\"/>");

        const HttpResponse miss = client.get("https://images.example.com/other.png");
        EXPECT_FALSE(miss.ok());
        EXPECT_NE(miss.error.find("not found"), std::string::npos);
        // nothing went to the network (the fixture server saw nothing)
        EXPECT_TRUE(sharedServer().requests().empty());
    }
    fs::remove_all(mirror);
}

TEST(HttpClient, MissingCurlIsReported){
    const fs::path empty = makeTempDir("jasper-nopath-");
    {
        ScopedEnv path("PATH", empty.string().c_str());
        EXPECT_FALSE(HttpClient::available());

        TempProject project(false);
        const ImportResult result = importUrl(project.root(), "https://images.example.com/placeholder", "");
        EXPECT_FALSE(result.ok);
        const auto missing = withCode(result.diagnostics, "HTMLUI-IMP-0005");
        ASSERT_EQ(missing.size(), 1u) << dump(result.diagnostics);
        EXPECT_EQ(missing[0].severity, compat::Severity::Error);
        EXPECT_FALSE(missing[0].suggestion.empty());
        EXPECT_TRUE(result.written.empty());
        EXPECT_FALSE(project.exists("assets/htmlui-imports/manifest.json"));
    }
    EXPECT_TRUE(HttpClient::available());
    fs::remove_all(empty);
}
