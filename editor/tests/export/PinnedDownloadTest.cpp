// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The tool downloads a build makes (docs/specs/S47-packaging-tests.md R2): against a local server
// for what a network does wrong. The real bundletool URL is run by tools/ci/android_packaging.sh.

#ifndef _WIN32

#include "support/TestTempDir.h"
#include "util/TestHttpServer.h"

#include "platform/AndroidTools.h"
#include "platform/PinnedDownload.h"
#include "util/Sha256.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::platform;
namespace fs = std::filesystem;
using jasperengine::test::TestTempDir;
using jaspertest::TestHttpServer;

namespace pinned_download_test {

    std::string hashOf(const TestTempDir& dir, const std::string& bytes) {
        const fs::path file = dir / "hash-me";
        TestTempDir::write(file, bytes);
        std::string hex, error;
        EXPECT_TRUE(sha256File(file, hex, &error)) << error;
        fs::remove(file);
        return hex;
    }

    std::string contents(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    PinnedDownload request(const TestTempDir& dir, const std::string& url, const std::string& bytes) {
        PinnedDownload download;
        download.what = "the tool 1.0";
        download.url = url;
        download.sha256 = hashOf(dir, bytes);
        download.target = dir / "cache" / "tool.jar";
        download.attemptSeconds = 20;
        download.retryDelayMs = 1;
        return download;
    }

    fs::path partOf(const PinnedDownload& download) { return download.target.string() + ".part"; }

}

using namespace pinned_download_test;

TEST(PinnedDownload, DownloadsOnceAndKeepsIt) {
    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned");
    const std::string bytes = TestHttpServer::pattern(200000);
    const PinnedDownload download = request(dir, server.url("/bytes/200000"), bytes);
    std::string error;
    ASSERT_TRUE(fetchPinned(download, {}, error)) << error;
    EXPECT_EQ(contents(download.target), bytes);
    EXPECT_FALSE(fs::exists(partOf(download)));
    EXPECT_EQ(server.requests("/bytes/200000"), 1);
    ASSERT_TRUE(fetchPinned(download, {}, error)) << error;
    EXPECT_EQ(server.requests("/bytes/200000"), 1) << "the cached file is used";
}

TEST(PinnedDownload, ACachedFileWithAnotherHashIsDownloadedAgain) {
    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned-stale");
    const std::string bytes = TestHttpServer::pattern(5000);
    const PinnedDownload download = request(dir, server.url("/bytes/5000"), bytes);
    TestTempDir::write(download.target, "damaged");
    std::string error;
    EXPECT_FALSE(pinnedFileIsCached(download, error));
    EXPECT_FALSE(fs::exists(download.target)) << "one that does not match is removed";
    TestTempDir::write(download.target, "damaged");
    ASSERT_TRUE(fetchPinned(download, {}, error)) << error;
    EXPECT_EQ(contents(download.target), bytes);
    EXPECT_TRUE(pinnedFileIsCached(download, error));
}

TEST(PinnedDownload, AHashThatDiffersKeepsNothingAndSaysSo) {
    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned-hash");
    const PinnedDownload download = request(dir, server.url("/bytes/4000"), "other bytes");
    std::string error;
    EXPECT_FALSE(fetchPinned(download, {}, error));
    EXPECT_NE(error.find("the tool 1.0"), std::string::npos) << error;
    EXPECT_NE(error.find("pinned SHA-256"), std::string::npos) << error;
    EXPECT_NE(error.find(download.sha256), std::string::npos) << error;
    EXPECT_NE(error.find(hashOf(dir, TestHttpServer::pattern(4000))), std::string::npos) << "the hash it got: " << error;
    EXPECT_FALSE(fs::exists(download.target));
    EXPECT_FALSE(fs::exists(partOf(download)));
    EXPECT_EQ(server.requests("/bytes/4000"), 1) << "the same bytes would come again: no retry";
}

TEST(PinnedDownload, OfflineFailsWithWhatToDo) {
    std::string url;
    {
        TestHttpServer closed;   // its port, then nobody listens on it
        ASSERT_TRUE(closed.started());
        url = closed.url("/tool.jar");
    }
    TestTempDir dir("pinned-offline");
    PinnedDownload download = request(dir, url, "x");
    download.attempts = 2;
    std::string error;
    EXPECT_FALSE(fetchPinned(download, {}, error));
    EXPECT_NE(error.find("cannot download the tool 1.0"), std::string::npos) << error;
    EXPECT_NE(error.find(url), std::string::npos) << error;
    EXPECT_NE(error.find("Check this computer's network"), std::string::npos) << error;
    EXPECT_NE(error.find(download.target.string()), std::string::npos) << "where to put it by hand: " << error;
    EXPECT_NE(error.find(download.sha256), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(download.target));
}

TEST(PinnedDownload, ANotFoundIsNotRetriedAndATemporaryErrorIs) {
    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned-http");
    PinnedDownload download = request(dir, server.url("/status/404"), "x");
    std::string error;
    EXPECT_FALSE(fetchPinned(download, {}, error));
    EXPECT_NE(error.find("HTTP 404"), std::string::npos) << error;
    EXPECT_EQ(server.requests("/status/404"), 1);

    download.url = server.url("/status/503");
    download.attempts = 3;
    EXPECT_FALSE(fetchPinned(download, {}, error));
    EXPECT_NE(error.find("HTTP 503"), std::string::npos) << error;
    EXPECT_EQ(server.requests("/status/503"), 3);
}

TEST(PinnedDownload, ABrokenTransferResumesWhereItStopped) {
    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned-resume");
    const std::string bytes = TestHttpServer::pattern(100000);
    server.addFile("tool.jar", bytes, "");
    server.cutNext("tool.jar", 40960);
    const PinnedDownload download = request(dir, server.url("/file/tool.jar"), bytes);
    std::string error;
    ASSERT_TRUE(fetchPinned(download, {}, error)) << error;
    EXPECT_EQ(contents(download.target), bytes);
    EXPECT_EQ(server.requests("/file/tool.jar"), 2);
    EXPECT_EQ(server.lastHeader("/file/tool.jar", "range"), "bytes=40960-") << "only what is missing";
}

TEST(PinnedDownload, AnUnfinishedFileIsKeptForTheNextBuild) {
    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned-next");
    const std::string bytes = TestHttpServer::pattern(100000);
    server.addFile("tool.jar", bytes, "");
    server.cutNext("tool.jar", 40960);
    PinnedDownload download = request(dir, server.url("/file/tool.jar"), bytes);
    download.attempts = 1;
    std::string error;
    EXPECT_FALSE(fetchPinned(download, {}, error));
    EXPECT_FALSE(fs::exists(download.target));
    EXPECT_EQ(fs::file_size(partOf(download)), 40960u);
    ASSERT_TRUE(fetchPinned(download, {}, error)) << error;
    EXPECT_EQ(contents(download.target), bytes);
    EXPECT_EQ(server.lastHeader("/file/tool.jar", "range"), "bytes=40960-");
}

TEST(PinnedDownload, AnErrorPageIsNotAppendedToTheBytesKept) {
    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned-page");
    PinnedDownload download = request(dir, server.url("/status/503"), "x");
    download.attempts = 1;
    TestTempDir::write(partOf(download), "kept so far");
    std::string error;
    EXPECT_FALSE(fetchPinned(download, {}, error));
    EXPECT_EQ(contents(partOf(download)), "kept so far");
}

TEST(PinnedDownload, AServerThatCannotResumeStartsOver) {
    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned-norange");
    const std::string bytes = TestHttpServer::pattern(30000);
    // (chunked: this route answers 200 to any Range)
    const PinnedDownload download = request(dir, server.url("/chunked/30000"), bytes);
    TestTempDir::write(partOf(download), bytes.substr(0, 1000));
    std::string error;
    ASSERT_TRUE(fetchPinned(download, {}, error)) << error;
    EXPECT_EQ(contents(download.target), bytes);
    EXPECT_EQ(server.requests("/chunked/30000"), 2);
}

TEST(PinnedDownload, KeptBytesPastTheEndOrWrongStartOver) {
    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned-416");
    const std::string bytes = TestHttpServer::pattern(30000);
    server.addFile("tool.jar", bytes, "");
    const PinnedDownload download = request(dir, server.url("/file/tool.jar"), bytes);
    std::string error;
    // more than the file holds: the server answers 416
    TestTempDir::write(partOf(download), std::string(40000, 'x'));
    ASSERT_TRUE(fetchPinned(download, {}, error)) << error;
    EXPECT_EQ(contents(download.target), bytes);
    // bytes from something else: the resumed file fails the hash, and the whole one is fetched
    fs::remove(download.target);
    TestTempDir::write(partOf(download), std::string(1000, 'x'));
    ASSERT_TRUE(fetchPinned(download, {}, error)) << error;
    EXPECT_EQ(contents(download.target), bytes);
}

TEST(PinnedDownload, CancelStopsTheTransferAndKeepsItsBytes) {
    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned-cancel");
    PinnedDownload download = request(dir, server.url("/trickle/1000000/100"), "x");
    // cancelled once some of it has arrived
    const fs::path partial = partOf(download);
    const auto arrived = [&partial]() {
        std::error_code ec;
        const uintmax_t size = fs::file_size(partial, ec);
        return !ec && size > 0;
    };
    std::string error;
    const bool ok = fetchPinned(download, arrived, error);
    EXPECT_FALSE(ok);
    EXPECT_EQ(error, "cancelled");
    EXPECT_FALSE(fs::exists(download.target));
    EXPECT_GT(fs::file_size(partOf(download)), 0u);
}

// the real one's pins (with the cache somewhere else: this test does not touch the user's)
TEST(PinnedDownload, BundletoolIsPinned) {
    const PinnedDownload real = AndroidTools::bundletoolDownload();
    EXPECT_EQ(real.url, "https://github.com/google/bundletool/releases/download/1.17.2/bundletool-all-1.17.2.jar");
    EXPECT_EQ(real.sha256, "2d4ad908faea64047c1cc9cb747e6aa667c6ab192e09607bd16b67246a8cd6ae");
    EXPECT_EQ(real.target.filename().string(), "bundletool-all-1.17.2.jar");
    EXPECT_EQ(real.target.parent_path().filename().string(), "tools");

    TestHttpServer server;
    ASSERT_TRUE(server.started());
    TestTempDir dir("pinned-real");
    PinnedDownload download = real;
    download.url = server.url("/bytes/1000");
    download.target = dir / "bundletool.jar";
    std::string error;
    EXPECT_FALSE(fetchPinned(download, {}, error));
    EXPECT_NE(error.find("the downloaded bundletool 1.17.2 does not match its pinned SHA-256"), std::string::npos) << error;
    EXPECT_FALSE(fs::exists(download.target));
}

#endif
