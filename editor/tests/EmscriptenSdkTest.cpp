// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Web build's Emscripten SDK without the emcmake wrapper (emcmake.bat would run through
// cmd.exe): Exporter::detectEmsdk reads an emsdk's layout and .emscripten config,
// emscriptenCMakeArgs adds what emcmake.py adds, emscriptenEnvironment what emsdk_env sets. A fake
// SDK layout here (no real emsdk needed). Also: HttpClient's curl only speaks http(s).

#include <gtest/gtest.h>

#include "Exporter.h"
#include "htmlui/HttpClient.h"
#include "util/CommandRunner.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace jasperengine::editor;
namespace fs = std::filesystem;

namespace {

    void writeFile(const fs::path& path, const std::string& text, bool executable = false) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
        if (executable) fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
    }

    struct FakeEmsdk {
        fs::path root, emscripten, node;
        explicit FakeEmsdk(const std::string& name) {
            root = fs::temp_directory_path() / ("jasper-emsdk-" + name + "-" + std::to_string(::getpid())) / "emsdk & co";
            fs::remove_all(root.parent_path());
            emscripten = root / "upstream" / "emscripten";
            writeFile(emscripten / "emcc", "#!/bin/sh\n", true);
            writeFile(emscripten / "emcmake", "#!/bin/sh\n", true);
            writeFile(emscripten / "cmake" / "Modules" / "Platform" / "Emscripten.cmake", "# toolchain\n");
            writeFile(root / "emsdk.py", "# emsdk\n");
            node = root / "node" / "20.18.0_64bit" / "bin" / "node";
            writeFile(node, "#!/bin/sh\n", true);
            writeFile(root / "node" / "18.0.0_64bit" / "bin" / "node", "#!/bin/sh\n", true);
        }
        ~FakeEmsdk() {
            std::error_code ec;
            fs::remove_all(root.parent_path(), ec);
        }
    };

    bool contains(const std::vector<std::string>& list, const std::string& item) {
        return std::find(list.begin(), list.end(), item) != list.end();
    }

}

TEST(EmscriptenSdk, DetectsTheLayoutAndConfigWithoutEmcmake) {
    FakeEmsdk sdk("layout");
    // the config emsdk writes: NODE_JS relative to emsdk_path (the config's folder)
    writeFile(sdk.root / ".emscripten", "import os\nemsdk_path = os.path.dirname(os.getenv('EM_CONFIG')).replace('\\\\', '/')\n"
                                       "NODE_JS = emsdk_path + '/node/20.18.0_64bit/bin/node'\nLLVM_ROOT = emsdk_path + '/upstream/bin'\n");
    ::unsetenv("EM_CONFIG");
    const EmsdkInfo info = Exporter::detectEmsdk(sdk.root.string());
    ASSERT_TRUE(info.found);
    EXPECT_EQ(fs::path(info.emscripten), sdk.emscripten);
    EXPECT_EQ(fs::path(info.toolchainFile), sdk.emscripten / "cmake" / "Modules" / "Platform" / "Emscripten.cmake");
    EXPECT_EQ(fs::path(info.emsdkRoot), sdk.root);
    EXPECT_EQ(fs::path(info.configFile), sdk.root / ".emscripten");
    EXPECT_EQ(fs::path(info.node), sdk.node);
    ASSERT_GE(info.pathDirs.size(), 3u);
    EXPECT_EQ(fs::path(info.pathDirs[0]), sdk.root);
    EXPECT_EQ(fs::path(info.pathDirs[1]), sdk.emscripten);
    EXPECT_EQ(fs::path(info.pathDirs[2]), sdk.node.parent_path());

    // the emscripten folder pointed at directly works too
    const EmsdkInfo direct = Exporter::detectEmsdk(sdk.emscripten.string());
    ASSERT_TRUE(direct.found);
    EXPECT_EQ(fs::path(direct.emsdkRoot), sdk.root);

    // what emcmake.py adds, as plain argv entries
    std::string error;
    const auto args = Exporter::emscriptenCMakeArgs(info, false, Exporter::emscriptenEnvironment(info), error);
    EXPECT_TRUE(error.empty()) << error;
    EXPECT_TRUE(contains(args, "-DCMAKE_TOOLCHAIN_FILE=" + info.toolchainFile)) << ::testing::PrintToString(args);
    EXPECT_TRUE(contains(args, "-DCMAKE_CROSSCOMPILING_EMULATOR=" + sdk.node.string())) << ::testing::PrintToString(args);

    // the SDK environment: its folders first on PATH, EMSDK / EM_CONFIG / EMSDK_NODE
    const auto env = Exporter::emscriptenEnvironment(info);
    const std::string path = CommandRunner::variable(env, "PATH");
    EXPECT_EQ(path.rfind(sdk.root.string() + ":" + sdk.emscripten.string() + ":", 0), 0u) << path;
    const char* ownPath = std::getenv("PATH");
    if (ownPath && *ownPath) { EXPECT_NE(path.find(ownPath), std::string::npos); }
    EXPECT_EQ(CommandRunner::variable(env, "EMSDK"), sdk.root.string());
    EXPECT_EQ(CommandRunner::variable(env, "EM_CONFIG"), (sdk.root / ".emscripten").string());
    EXPECT_EQ(CommandRunner::variable(env, "EMSDK_NODE"), sdk.node.string());
    EXPECT_EQ(CommandRunner::variable(env, "HOME"), std::string(std::getenv("HOME") ? std::getenv("HOME") : ""));
}

TEST(EmscriptenSdk, NodeFromTheNewestInstallWhenTheConfigSaysNothingUsable) {
    FakeEmsdk sdk("noconfig");
    writeFile(sdk.root / ".emscripten", "NODE_JS = os.path.expanduser(os.getenv('NODE', 'node'))\n");
    ::unsetenv("EM_CONFIG");
    const EmsdkInfo info = Exporter::detectEmsdk(sdk.root.string());
    ASSERT_TRUE(info.found);
    EXPECT_EQ(fs::path(info.node), sdk.node);   // 20.18.0 over 18.0.0

    // not an SDK: nothing found (a configured path alone decides)
    EXPECT_FALSE(Exporter::detectEmsdk((sdk.root / "node").string()).found);
}

TEST(HttpClientProtocols, OnlyHttpAndHttpsAreFetched) {
    if (!htmlui::HttpClient::available()) GTEST_SKIP() << "no curl";
    ::unsetenv("JASPERENGINE_IMPORT_MIRROR");
    const fs::path secret = fs::temp_directory_path() / ("jasper-curl-secret-" + std::to_string(::getpid()));
    writeFile(secret, "TOP-SECRET-CONTENT");
    htmlui::HttpClient client;
    const std::string urls[] = {"file://" + secret.string(), "FILE://" + secret.string(), "ftp://127.0.0.1:1/x",
                                "dict://127.0.0.1:1/x", "gopher://127.0.0.1:1/x"};
    for (const std::string& url : urls) {
        const htmlui::HttpResponse response = client.get(url);
        EXPECT_FALSE(response.ok()) << url;
        EXPECT_FALSE(response.error.empty()) << url;
        std::string body;
        if (htmlui::HttpClient::readBody(response, body)) {
            EXPECT_EQ(body.find("TOP-SECRET"), std::string::npos) << url;
        }
    }
    std::error_code ec;
    fs::remove(secret, ec);
}

TEST(EmscriptenSdk, WebBuildsRefuseFoldersCmdWouldExpand) {
    // the check itself (platform-independent): '%' or '"' anywhere in the project, build tree or output path
    EXPECT_EQ(Exporter::webPathRefusal({"/home/dev/My Game & Co (2)", "/home/dev/My Game & Co (2)/.jasperengine/export/web",
                                        "/srv/out/web!build^x"}), "");
    EXPECT_EQ(Exporter::webPathRefusal({fs::path(), "/plain"}), "");
    const std::string refusal = Exporter::webPathRefusal({"/home/dev/game", "/home/dev/game/.jasperengine/export/web", "/srv/100%/web"});
    EXPECT_NE(refusal.find("Web builds on Windows can't use a folder whose path contains %"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("rename the folder"), std::string::npos);
    EXPECT_NE(refusal.find("/srv/100%/web"), std::string::npos);
    EXPECT_NE(Exporter::webPathRefusal({"/home/%USERNAME%/game"}).find("/home/%USERNAME%/game"), std::string::npos);
    EXPECT_NE(Exporter::webPathRefusal({"/home/dev/\"quoted\""}), "");
    // applied on Windows only (elsewhere no shell runs the compiler driver)
#ifdef _WIN32
    EXPECT_NE(Exporter::webBuildRefusal({"C:\\100%\\web"}), "");
#else
    EXPECT_EQ(Exporter::webBuildRefusal({"/srv/100%/web"}), "");
#endif
}

TEST(EmscriptenSdk, WindowsGeneratorFollowsEmcmake) {
    const fs::path base = fs::temp_directory_path() / ("jasper-emgen-" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path ninjaDir = base / "ninja dir", makeDir = base / "mingw", batDir = base / "bat";
    writeFile(ninjaDir / "ninja.exe", "x");
    writeFile(makeDir / "mingw32-make.exe", "x");
    writeFile(batDir / "ninja.bat", "x");
    std::string error;
    using Env = CommandRunner::Environment;
    // a generator given, or CMAKE_GENERATOR in the environment: nothing added (CMake reads it)
    EXPECT_TRUE(Exporter::emscriptenWindowsGeneratorArgs(Env{{"PATH", ninjaDir.string()}}, true, error).empty());
    EXPECT_TRUE(Exporter::emscriptenWindowsGeneratorArgs(Env{{"CMAKE_GENERATOR", "Ninja Multi-Config"}, {"PATH", ""}}, false, error).empty());
    EXPECT_TRUE(error.empty());
    // looked up on the SDK environment's PATH: MinGW Makefiles first, then Ninja
    EXPECT_EQ(Exporter::emscriptenWindowsGeneratorArgs(Env{{"PATH", "\"" + ninjaDir.string() + "\";" + makeDir.string()}}, false, error),
              (std::vector<std::string>{"-G", "MinGW Makefiles"}));
    EXPECT_EQ(Exporter::emscriptenWindowsGeneratorArgs(Env{{"PATH", batDir.string() + ";" + ninjaDir.string()}}, false, error),
              (std::vector<std::string>{"-G", "Ninja"}));
    // a ninja.bat is no ninja; nothing found: the error says what to install
    EXPECT_TRUE(Exporter::emscriptenWindowsGeneratorArgs(Env{{"PATH", batDir.string()}}, false, error).empty());
    EXPECT_NE(error.find("Ninja"), std::string::npos);
    std::error_code ec;
    fs::remove_all(base, ec);
}

TEST(EmscriptenSdk, WebBuildsRefuseFileNamesCmdWouldExpand) {
    const fs::path base = fs::temp_directory_path() / ("jasper-emtree-" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path assets = base / "assets", lua = base / "lua";
    writeFile(assets / "ok & fine (1)" / "hero!.png", "x");
    writeFile(lua / "main.lua", "x");
    EXPECT_EQ(Exporter::webTreePathRefusal({assets, lua, base / "missing"}), "");
    writeFile(lua / "deep" / "er" / "50%off.lua", "x");
    const std::string refusal = Exporter::webTreePathRefusal({assets, lua});
    EXPECT_NE(refusal.find("rename"), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("50%off.lua"), std::string::npos) << refusal;
    // a folder name too
    fs::remove(lua / "deep" / "er" / "50%off.lua");
    fs::create_directories(assets / "%TEMP%");
    EXPECT_NE(Exporter::webTreePathRefusal({assets, lua}).find("%TEMP%"), std::string::npos);
    fs::remove(assets / "%TEMP%");
    // bounded: more entries than allowed is refused (it can't be checked)
    EXPECT_NE(Exporter::webTreePathRefusal({assets, lua}, 2).find("too many"), std::string::npos);
    EXPECT_EQ(Exporter::webTreePathRefusal({assets, lua}, 100), "");
#ifndef _WIN32
    EXPECT_EQ(Exporter::webTreeRefusal({assets}), "");   // applied on Windows only
#endif
    std::error_code ec;
    fs::remove_all(base, ec);
}

// Emscripten 6 on Windows has emcc.exe (and emcmake.exe) launchers where earlier versions had .bat
TEST(EmscriptenSdk, AcceptsTheEmscripten6WindowsLayout) {
    const fs::path base = fs::temp_directory_path() / ("jasper-emsdk6-" + std::to_string(::getpid()));
    fs::remove_all(base);
    const fs::path emscripten = base / "emsdk" / "upstream" / "emscripten";
    writeFile(emscripten / "emcc.exe", "MZ");
    writeFile(emscripten / "emcmake.exe", "MZ");
    writeFile(emscripten / "cmake" / "Modules" / "Platform" / "Emscripten.cmake", "# toolchain\n");
    writeFile(base / "emsdk" / "emsdk.py", "# emsdk\n");
    const EmsdkInfo info = Exporter::detectEmsdk((base / "emsdk").string());
    EXPECT_TRUE(info.found);
    EXPECT_EQ(fs::path(info.emscripten), emscripten);
    std::error_code ec;
    fs::remove_all(base, ec);
}
