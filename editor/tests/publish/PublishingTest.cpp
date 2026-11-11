// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Releases (docs/specs/S30-shipping.md R22-R26, R30): the payload validator's findings, prepare
// then verify (and a tampered file caught), the SteamPipe scripts and the upload's SteamCMD
// command (a SteamCMD-shaped stub), and a Steam Deck deploy, rollback and smoke over ssh/scp
// stubs that run on this machine (the real Deck is not tested here).

#include "support/TestTempDir.h"

#include "AppSettings.h"
#include "player/ContentManifest.h"
#include "publish/BinaryInfo.h"
#include "publish/Notices.h"
#include "publish/PayloadValidator.h"
#include "publish/Publishing.h"
#include "publish/TreeManifest.h"
#include "serialization/CookedScene.h"
#include "util/ZipArchive.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::publish;
using jasperengine::test::TestTempDir;

namespace publishing_test {

    // an ELF header: 64-bit little-endian, e_machine `machine`, no segments or sections
    std::string elf(uint16_t machine) {
        std::string bytes(64, '\0');
        const unsigned char ident[] = {0x7f, 'E', 'L', 'F', 2, 1, 1};
        std::copy(ident, ident + sizeof(ident), bytes.begin());
        bytes[16] = 2;   // ET_EXEC
        bytes[18] = static_cast<char>(machine & 0xff);
        bytes[19] = static_cast<char>(machine >> 8);
        bytes[52] = 64;  // e_ehsize
        return bytes;
    }

    void makeExecutable(const fs::path& path) {
        fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);
    }

    // a prebuilt Linux build as the Exporter lays it out
    void writeLinuxBuild(const fs::path& root) {
        TestTempDir::write(root / "game", elf(0x3E));
        makeExecutable(root / "game");
        player::ContentManifest manifest;
        manifest.engineVersion = "v1.0.0";
        manifest.contentFormatVersion = jasperengine::cook::contentFormatVersion();
        manifest.playerId = "linux-x64-glcore-release";
        manifest.method = "prebuilt";
        manifest.backend = "glcore";
        manifest.startScene = 1;
        manifest.scenes = {"scenes/1.jscn"};
        manifest.notices = {"lua"};
        TestTempDir::write(root / "content-manifest.json", manifest.toJson().dump(2));
        TestTempDir::write(root / "game.json", "{\"schemaVersion\": 1}\n");
        TestTempDir::write(root / "scenes" / "1.jscn", "JSCN");
        TestTempDir::write(root / "licenses" / "lua-LICENSE", "MIT\n");
    }

    bool hasFinding(const std::vector<std::string>& findings, const std::string& part) {
        return std::any_of(findings.begin(), findings.end(), [&part](const std::string& f) { return f.find(part) != std::string::npos; });
    }

    std::string join(const std::vector<std::string>& lines) {
        std::string out;
        for (const std::string& line : lines) out += "\n  " + line;
        return out;
    }

    PublishingConfig linuxConfig(const std::string& buildDir) {
        PublishingConfig config;
        PublishTarget target;
        target.platform = "linux";
        target.arch = "x64";
        target.backend = "glcore";
        target.buildDir = buildDir;
        target.executable = "game";
        target.depotId = "1001";
        target.deviceProfile = "steamdeck";
        config.targets["linux"] = target;
        config.steam.appId = "480";
        return config;
    }

}

using namespace publishing_test;

TEST(PayloadValidator, AcceptsAPrebuiltLinuxBuild) {
    TestTempDir dir("payload");
    writeLinuxBuild(dir.path());
    ValidationTarget target;
    target.platform = BuildPlatform::Linux;
    target.executable = "game";
    std::vector<std::string> findings;
    EXPECT_TRUE(validatePayload(dir.path(), target, findings)) << join(findings);
}

TEST(PayloadValidator, FindsWhatCannotShip) {
    TestTempDir dir("payload");
    writeLinuxBuild(dir.path());
    // an arm64 binary in an x64 build, a missing exec bit, steam_appid.txt, a missing notice and scene
    TestTempDir::write(dir / "tools/helper", elf(0xB7));
    makeExecutable(dir / "tools/helper");
    fs::permissions(dir / "game", fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::remove);
    TestTempDir::write(dir / "steam_appid.txt", "480");
    fs::remove(dir / "licenses/lua-LICENSE");
    fs::remove(dir / "scenes/1.jscn");
    ValidationTarget target;
    target.platform = BuildPlatform::Linux;
    target.executable = "game";
    std::vector<std::string> findings;
    EXPECT_FALSE(validatePayload(dir.path(), target, findings));
    EXPECT_TRUE(hasFinding(findings, "tools/helper is arm64")) << join(findings);
    EXPECT_TRUE(hasFinding(findings, "game has no exec bit")) << join(findings);
    EXPECT_TRUE(hasFinding(findings, "steam_appid.txt")) << join(findings);
    EXPECT_TRUE(hasFinding(findings, "licenses/lua-LICENSE")) << join(findings);
    EXPECT_TRUE(hasFinding(findings, "scenes/1.jscn")) << join(findings);
}

// S34 R3: the content of a player variant names the modules it needs; one the variant leaves out
// is refused before the build ships (the player would exit 5)
TEST(PayloadValidator, RefusesContentNeedingAModuleTheVariantLeavesOut) {
    TestTempDir dir("payload");
    writeLinuxBuild(dir.path());
    auto withModules = [&](const std::string& playerId, const std::vector<std::string>& modules) {
        player::ContentManifest manifest;
        std::string error;
        EXPECT_TRUE(player::ContentManifest::parse(TestTempDir::read(dir / "content-manifest.json"), manifest, error)) << error;
        manifest.playerId = playerId;
        manifest.modules = modules;
        manifest.hasModules = true;
        TestTempDir::write(dir / "content-manifest.json", manifest.toJson().dump(2));
        ValidationTarget target;
        target.platform = BuildPlatform::Linux;
        target.executable = "game";
        std::vector<std::string> findings;
        validatePayload(dir.path(), target, findings);
        return findings;
    };
    EXPECT_TRUE(withModules("linux-x64-glcore-release-lite3d", {"physics3d", "navigation"}).empty());
    const std::vector<std::string> findings = withModules("linux-x64-glcore-release-lite3d", {"htmlui", "physics3d"});
    EXPECT_TRUE(hasFinding(findings, "needs the HTML UI (htmlui), which the player linux-x64-glcore-release-lite3d leaves out")) << join(findings);
    // the full player has every module
    EXPECT_TRUE(withModules("linux-x64-glcore-release", {"htmlui", "video", "runtimeshaders"}).empty());
    // T6 AC38: lite3d leaves Voxel Volumes out, the full player has them
    const std::vector<std::string> voxelFindings = withModules("linux-x64-glcore-release-lite3d", {"physics3d", "voxel"});
    EXPECT_TRUE(hasFinding(voxelFindings, "needs Voxel Volumes (voxel), which the player linux-x64-glcore-release-lite3d leaves out")) << join(voxelFindings);
    EXPECT_TRUE(withModules("linux-x64-glcore-release", {"voxel"}).empty());
    // T4 R55: lite3d leaves Terrain Worlds out, the full player has them
    const std::vector<std::string> worldFindings = withModules("linux-x64-glcore-release-lite3d", {"physics3d", "terrainworld"});
    EXPECT_TRUE(hasFinding(worldFindings, "needs Terrain World (terrainworld), which the player linux-x64-glcore-release-lite3d leaves out")) << join(worldFindings);
    EXPECT_TRUE(withModules("linux-x64-glcore-release", {"terrainworld"}).empty());
}

TEST(PayloadValidator, RefusesCaseCollisionsAndEscapingSymlinks) {
    TestTempDir dir("payload");
    writeLinuxBuild(dir.path());
    TestTempDir::write(dir / "assets/Logo.png", "a");
    TestTempDir::write(dir / "assets/logo.png", "b");
    std::error_code ec;
    fs::create_symlink("/etc/passwd", dir / "absolute", ec);
    fs::create_symlink("../../outside", dir / "assets/escape", ec);
    ValidationTarget target;
    target.platform = BuildPlatform::Linux;
    target.executable = "game";
    std::vector<std::string> findings;
    EXPECT_FALSE(validatePayload(dir.path(), target, findings));
    // a case-insensitive filesystem (macOS, Windows) has one file under both names: no collision
    // to find there
    size_t logos = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir / "assets"))
        logos += entry.path().filename() == "Logo.png" || entry.path().filename() == "logo.png";
    if (logos == 2) {
        EXPECT_TRUE(hasFinding(findings, "logo.png")) << join(findings);
    }
    if (!ec) {
        EXPECT_TRUE(hasFinding(findings, "absolute")) << join(findings);
        EXPECT_TRUE(hasFinding(findings, "escape")) << join(findings);
    }
}

TEST(PayloadValidator, WindowsNames) {
    EXPECT_EQ(windowsNameProblem("assets/a.png"), "");
    EXPECT_NE(windowsNameProblem("assets/CON.png"), "");
    EXPECT_NE(windowsNameProblem("assets/nul"), "");
    EXPECT_NE(windowsNameProblem("assets/name."), "");
    EXPECT_NE(windowsNameProblem("assets/a:b.png"), "");
    EXPECT_NE(windowsNameProblem("COM1/x.txt"), "");
}

TEST(PayloadValidator, AndroidLibrariesAreStoredAnd16KAligned) {
    TestTempDir dir("payload");
    // libraries with 4 KB pages, one compressed and one stored: both refused (a package stores
    // them all, or compresses them all); apksigner injected as accepting
    std::string library = elf(0xB7);
    library.resize(64 + 56, '\0');
    library[32] = 64;           // e_phoff
    library[54] = 56;           // e_phentsize
    library[56] = 1;            // e_phnum
    library[64] = 1;            // PT_LOAD
    library[64 + 48] = 0;       // p_align = 4096
    library[64 + 49] = 0x10;
    ZipWriter zip;
    std::string error;
    ASSERT_TRUE(zip.open(dir / "game.apk", error)) << error;
    ASSERT_TRUE(zip.add("lib/arm64-v8a/libjasperengine-android.so", library.data(), library.size(), false, error)) << error;
    ASSERT_TRUE(zip.add("lib/arm64-v8a/libc++_shared.so", library.data(), library.size(), true, error)) << error;
    ASSERT_TRUE(zip.add("AndroidManifest.xml", "x", 1, true, error)) << error;
    ASSERT_TRUE(zip.finish(error)) << error;
    ValidationTarget target;
    target.platform = BuildPlatform::Android;
    target.arch = "universal";
    target.verifyApk = [](const fs::path&, std::string& output) {
        output = "Verifies";
        return true;
    };
    std::vector<std::string> findings;
    EXPECT_FALSE(validatePayload(dir.path(), target, findings));
    EXPECT_TRUE(hasFinding(findings, "aligned to 4096")) << join(findings);
    EXPECT_TRUE(hasFinding(findings, "1 native library is compressed and 1 stored")) << join(findings);
}

// a bundle's libraries are bundletool's to pack: compressed in the .aab (it compresses every
// entry) is no finding; their 16 KB alignment still is checked
TEST(PayloadValidator, AndroidBundleLibrariesMayBeCompressed) {
    TestTempDir dir("payload-aab");
    std::string library = elf(0xB7);
    library.resize(64 + 56, '\0');
    library[32] = 64;           // e_phoff
    library[54] = 56;           // e_phentsize
    library[56] = 1;            // e_phnum
    library[64] = 1;            // PT_LOAD
    library[64 + 49] = 0x40;    // p_align = 16384
    ZipWriter zip;
    std::string error;
    ASSERT_TRUE(zip.open(dir / "game.aab", error)) << error;
    ASSERT_TRUE(zip.add("base/lib/arm64-v8a/libjasperengine-android.so", library.data(), library.size(), false, error)) << error;
    ASSERT_TRUE(zip.add("base/lib/arm64-v8a/libc++_shared.so", library.data(), library.size(), true, error)) << error;
    ASSERT_TRUE(zip.add("base/manifest/AndroidManifest.xml", "x", 1, false, error)) << error;
    ASSERT_TRUE(zip.finish(error)) << error;
    ValidationTarget target;
    target.platform = BuildPlatform::Android;
    target.arch = "universal";
    std::vector<std::string> findings;
    validatePayload(dir.path(), target, findings);
    EXPECT_FALSE(hasFinding(findings, "compressed")) << join(findings);
    EXPECT_FALSE(hasFinding(findings, "aligned to")) << join(findings);
}

// jarsigner's signature block is what makes a bundle one the store takes; an APK's apksigner verify
TEST(PayloadValidator, AndroidPackagesMustBeSigned) {
    TestTempDir dir("payload-signed");
    std::string error;
    const auto writeZip = [&error](const fs::path& path, const std::vector<std::string>& entries) {
        ZipWriter zip;
        ASSERT_TRUE(zip.open(path, error)) << error;
        for (const std::string& entry : entries) ASSERT_TRUE(zip.add(entry, "x", 1, false, error)) << error;
        ASSERT_TRUE(zip.finish(error)) << error;
    };
    ValidationTarget target;
    target.platform = BuildPlatform::Android;
    target.arch = "universal";
    std::vector<std::string> findings;

    writeZip(dir / "game.aab", {"base/manifest/AndroidManifest.xml", "META-INF/MANIFEST.MF"});
    validatePayload(dir.path(), target, findings);
    EXPECT_TRUE(hasFinding(findings, "the bundle is not signed")) << join(findings);

    for (const char* block : {"META-INF/ANDROIDD.RSA", "META-INF/KEY.EC"}) {
        writeZip(dir / "game.aab", {"base/manifest/AndroidManifest.xml", "META-INF/MANIFEST.MF", block});
        findings.clear();
        validatePayload(dir.path(), target, findings);
        EXPECT_FALSE(hasFinding(findings, "not signed")) << block << ": " << join(findings);
    }

    fs::remove(dir / "game.aab");
    writeZip(dir / "game.apk", {"AndroidManifest.xml"});
    target.verifyApk = [](const fs::path&, std::string& output) {
        output = "DOES NOT VERIFY";
        return false;
    };
    findings.clear();
    validatePayload(dir.path(), target, findings);
    EXPECT_TRUE(hasFinding(findings, "apksigner verify failed: DOES NOT VERIFY")) << join(findings);
}

// 16 KB pages are a 64-bit requirement: a 32-bit library with 4 KB pages is accepted (the NDK's
// flexible page sizes apply to arm64-v8a and x86_64 only)
TEST(PayloadValidator, Android32BitLibrariesKeep4KPages) {
    TestTempDir dir("payload-32");
    std::string library(52 + 32, '\0');
    const unsigned char ident[] = {0x7f, 'E', 'L', 'F', 1, 1, 1};   // ELFCLASS32, little endian
    std::copy(ident, ident + sizeof(ident), library.begin());
    library[16] = 3;            // ET_DYN
    library[18] = 0x28;         // EM_ARM
    library[28] = 52;           // e_phoff
    library[40] = 52;           // e_ehsize
    library[42] = 32;           // e_phentsize
    library[44] = 1;            // e_phnum
    library[52] = 1;            // PT_LOAD
    library[52 + 29] = 0x10;    // p_align = 4096
    ZipWriter zip;
    std::string error;
    ASSERT_TRUE(zip.open(dir / "game.apk", error)) << error;
    ASSERT_TRUE(zip.add("lib/armeabi-v7a/libjasperengine-android.so", library.data(), library.size(), false, error)) << error;
    ASSERT_TRUE(zip.add("AndroidManifest.xml", "x", 1, true, error)) << error;
    ASSERT_TRUE(zip.finish(error)) << error;
    ValidationTarget target;
    target.platform = BuildPlatform::Android;
    target.arch = "universal";
    target.verifyApk = [](const fs::path&, std::string& output) {
        output = "Verifies";
        return true;
    };
    std::vector<std::string> findings;
    validatePayload(dir.path(), target, findings);
    EXPECT_FALSE(hasFinding(findings, "aligned to")) << join(findings);
    EXPECT_FALSE(hasFinding(findings, "is arm")) << join(findings);
    EXPECT_FALSE(hasFinding(findings, "not an ELF")) << join(findings);
}

TEST(Publishing, ConfigRoundTripAndChecks) {
    PublishingConfig config = linuxConfig("build/linux");
    config.deck.host = "steamdeck.local";
    PublishingConfig again;
    std::string error;
    ASSERT_TRUE(PublishingConfig::parse(config.toJson(), again, error)) << error;
    EXPECT_EQ(again.toJson(), config.toJson());
    EXPECT_TRUE(config.check().empty()) << join(config.check());

    PublishTarget windows = config.targets["linux"];
    windows.platform = "windows";
    windows.deviceProfile.clear();
    config.targets["windows"] = windows;   // the same depot twice
    EXPECT_TRUE(hasFinding(config.check(), "depot 1001")) << join(config.check());
    EXPECT_FALSE(PublishingConfig::parse("{\"schemaVersion\": 2}", again, error));
    EXPECT_NE(error.find("schema 2"), std::string::npos) << error;
}

// a Compile build ships stripped like a prebuilt one: the symbols its link split off (the content
// manifest names them) go into the release's symbols/<target>/game, and a missing one is a warning
TEST(Publishing, PrepareCopiesACompileBuildsSymbols) {
    TestTempDir dir("release-compile");
    const fs::path project = dir / "Game";
    writeLinuxBuild(project / "build/linux");
    const fs::path kept = dir / "Game/.jasperengine/symbols/0123456789abcdef";
    TestTempDir::write(kept / "game.debug", "DWARF");
    auto describe = [&](const std::string& symbols) {
        player::ContentManifest manifest;
        std::string error;
        ASSERT_TRUE(player::ContentManifest::parse(TestTempDir::read(project / "build/linux/content-manifest.json"), manifest, error)) << error;
        manifest.method = "compile";
        manifest.playerId.clear();
        manifest.symbols = symbols;
        TestTempDir::write(project / "build/linux/content-manifest.json", manifest.toJson().dump(2));
    };
    describe(kept.generic_string());
    PrepareRequest request;
    request.projectDir = project;
    request.projectName = "Game";
    request.config = linuxConfig("build/linux");
    request.release = "1.0.0";
    PrepareResult result;
    std::vector<std::string> problems;
    ASSERT_TRUE(prepareRelease(request, {}, result, problems)) << join(problems);
    EXPECT_EQ(TestTempDir::read(result.releaseDir / "symbols/linux/game/game.debug"), "DWARF");
    EXPECT_FALSE(hasFinding(result.warnings, "W-BUILD-SYMBOLS")) << join(result.warnings);

    describe((dir / "gone").generic_string());
    request.release = "1.0.1";
    problems.clear();
    ASSERT_TRUE(prepareRelease(request, {}, result, problems)) << join(problems);
    EXPECT_TRUE(hasFinding(result.warnings, "W-BUILD-SYMBOLS")) << join(result.warnings);
}

TEST(Publishing, PrepareVerifyAndTamper) {
    TestTempDir dir("release");
    const fs::path project = dir / "Game";
    writeLinuxBuild(project / "build/linux");
    PrepareRequest request;
    request.projectDir = project;
    request.projectName = "Game";
    request.config = linuxConfig("build/linux");
    request.release = "1.0.0";
    PrepareResult result;
    std::vector<std::string> problems;
    ASSERT_TRUE(prepareRelease(request, {}, result, problems)) << join(problems);
    EXPECT_EQ(result.releaseDir, project / "releases/1.0.0");
    EXPECT_TRUE(fs::is_regular_file(result.releaseDir / "release.json"));
    EXPECT_TRUE(fs::is_regular_file(result.releaseDir / "content/linux/game"));

    ReleaseInfo info;
    problems.clear();
    ASSERT_TRUE(verifyRelease(result.releaseDir, problems, &info)) << join(problems);
    EXPECT_EQ(info.release, "1.0.0");
    EXPECT_EQ(info.targets["linux"].playerId, "linux-x64-glcore-release");
    EXPECT_EQ(info.targets["linux"].method, "prebuilt");
    EXPECT_EQ(info.targets["linux"].files.size(), 5u);

    // the same name again, and a release dir inside the build, are refused
    problems.clear();
    EXPECT_FALSE(prepareRelease(request, {}, result, problems));
    EXPECT_TRUE(hasFinding(problems, "exists already")) << join(problems);
    PrepareRequest overlap = request;
    overlap.release = "1.0.1";
    overlap.config.releasesDir = "build/linux/releases";
    problems.clear();
    EXPECT_FALSE(prepareRelease(overlap, {}, result, problems));
    EXPECT_TRUE(hasFinding(problems, "overlap")) << join(problems);

    // a change after prepare
    TestTempDir::write(project / "releases/1.0.0/content/linux/game.json", "{\"schemaVersion\": 1, \"x\": 1}\n");
    problems.clear();
    EXPECT_FALSE(verifyRelease(project / "releases/1.0.0", problems));
    EXPECT_TRUE(hasFinding(problems, "Release content changed after preparation: linux/game.json")) << join(problems);
}

TEST(PublishSteam, VdfScriptsAndRefusals) {
    TestTempDir dir("steam");
    const fs::path project = dir / "Game";
    writeLinuxBuild(project / "build/linux");
    PrepareRequest request;
    request.projectDir = project;
    request.config = linuxConfig("build/linux");
    request.release = "2.0.0";
    PrepareResult result;
    std::vector<std::string> problems;
    ASSERT_TRUE(prepareRelease(request, {}, result, problems)) << join(problems);

    fs::path app;
    std::vector<fs::path> depots;
    std::string error;
    ASSERT_TRUE(writeSteamScripts(result.releaseDir, true, "", app, depots, error)) << error;
    EXPECT_EQ(app.filename(), "app_build_480.vdf");
    ASSERT_EQ(depots.size(), 1u);
    EXPECT_EQ(depots[0].filename(), "depot_build_1001.vdf");
    const std::string appText = TestTempDir::read(app);
    EXPECT_NE(appText.find("\"AppID\" \"480\""), std::string::npos) << appText;
    EXPECT_NE(appText.find("\"Preview\" \"1\""), std::string::npos) << appText;
    EXPECT_NE(appText.find("\"1001\" \"depot_build_1001.vdf\""), std::string::npos) << appText;
    EXPECT_EQ(appText.find("SetLive"), std::string::npos) << appText;
    const std::string depotText = TestTempDir::read(depots[0]);
    EXPECT_NE(depotText.find("\"DepotID\" \"1001\""), std::string::npos) << depotText;
    EXPECT_NE(depotText.find("content/linux"), std::string::npos) << depotText;

    ASSERT_TRUE(writeSteamScripts(result.releaseDir, false, "beta", app, depots, error)) << error;
    EXPECT_NE(TestTempDir::read(app).find("\"SetLive\" \"beta\""), std::string::npos);
    EXPECT_FALSE(writeSteamScripts(result.releaseDir, false, "public", app, depots, error));
    EXPECT_NE(error.find("Steamworks"), std::string::npos) << error;
    EXPECT_FALSE(writeSteamScripts(result.releaseDir, false, "Default", app, depots, error));

    error.clear();
    EXPECT_EQ(vdfString("a \"quoted\" \\ path", error), "\"a \\\"quoted\\\" \\\\ path\"");
    EXPECT_TRUE(error.empty());
    vdfString("bad\nline", error);
    EXPECT_FALSE(error.empty());
}

#ifndef _WIN32

namespace publishing_test {

    // a script that answers like the tool it stands for
    fs::path writeScript(const fs::path& path, const std::string& body) {
        TestTempDir::write(path, "#!/bin/sh\n" + body);
        makeExecutable(path);
        return path;
    }

    // ssh and scp stand-ins: the "Deck" is a directory of this machine (FAKE_DECK_HOME)
    void installDeckStubs(const fs::path& bin) {
        writeScript(bin / "ssh",
                    "while [ $# -gt 0 ]; do case \"$1\" in -o|-p|-i) shift 2 ;; -*) shift ;; *) break ;; esac; done\n"
                    "shift\ncd \"$FAKE_DECK_HOME\" && exec sh -c \"$1\"\n");
        writeScript(bin / "scp",
                    "while [ $# -gt 0 ]; do case \"$1\" in -o|-P|-i) shift 2 ;; -*) shift ;; *) break ;; esac; done\n"
                    "src=\"$1\"; dst=\"${2#*:}\"\nexec cp -R \"$src\" \"$FAKE_DECK_HOME/$dst\"\n");
    }

    struct SettingsScope {
        explicit SettingsScope(const fs::path& file) { AppSettings::initializeWithFile(file); }
        ~SettingsScope() {
            for (const char* tool : {"ssh", "scp", "steamcmd"}) AppSettings::setExternalToolPath(tool, "");
        }
    };

}

TEST(PublishSteam, UploadRunsSteamcmdAfterVerify) {
    TestTempDir dir("steam-upload");
    const fs::path project = dir / "Game";
    writeLinuxBuild(project / "build/linux");
    PrepareRequest request;
    request.projectDir = project;
    request.config = linuxConfig("build/linux");
    request.release = "3.0.0";
    PrepareResult result;
    std::vector<std::string> problems;
    ASSERT_TRUE(prepareRelease(request, {}, result, problems)) << join(problems);
    const fs::path args = dir / "steamcmd-args.txt";
    const fs::path steamcmd = writeScript(dir / "bin/steamcmd", "printf '%s\\n' \"$@\" > '" + args.string() + "'\n");
    std::string error;
    EXPECT_FALSE(steamUpload(result.releaseDir, "", "", steamcmd, {}, error));
    EXPECT_NE(error.find("STEAM_BUILD_USER"), std::string::npos) << error;
    ASSERT_TRUE(steamUpload(result.releaseDir, "builder", "", steamcmd, {}, error)) << error;
    const std::string recorded = TestTempDir::read(args);
    EXPECT_EQ(recorded, "+login\nbuilder\n+run_app_build\n" + fs::absolute(result.releaseDir / "steampipe/app_build_480.vdf").string() + "\n+quit\n");
    // a changed release is not uploaded
    TestTempDir::write(result.releaseDir / "content/linux/scenes/1.jscn", "changed");
    fs::remove(args);
    EXPECT_FALSE(steamUpload(result.releaseDir, "builder", "", steamcmd, {}, error));
    EXPECT_FALSE(fs::exists(args));
}

TEST(PublishDeck, DeployRollbackSmoke) {
    TestTempDir dir("deck");
    const fs::path project = dir / "Game";
    const fs::path deckHome = dir / "deck-home";
    fs::create_directories(deckHome);
    SettingsScope settings(dir / "settings.yaml");
    installDeckStubs(dir / "bin");
    AppSettings::setExternalToolPath("ssh", (dir / "bin/ssh").string());
    AppSettings::setExternalToolPath("scp", (dir / "bin/scp").string());
    ::setenv("FAKE_DECK_HOME", deckHome.c_str(), 1);

    PublishingConfig config = linuxConfig("build/linux");
    config.deck.host = "steamdeck.local";
    config.deck.remoteDir = "Games/game";
    // a release of the build with a marker file that tells the deploys apart
    auto build = [&](const std::string& release, const std::string& marker) {
        writeLinuxBuild(project / "build/linux");
        TestTempDir::write(project / "build/linux/marker.txt", marker);
        PrepareRequest request;
        request.projectDir = project;
        request.config = config;
        request.release = release;
        PrepareResult result;
        std::vector<std::string> problems;
        EXPECT_TRUE(prepareRelease(request, {}, result, problems)) << join(problems);
        return result.releaseDir;
    };
    const fs::path first = build("1.0.0", "one");
    std::string output, error;
    ASSERT_TRUE(deckDeploy(first, "linux", config.deck, {}, output, error)) << error << "\n" << output;
    EXPECT_EQ(TestTempDir::read(deckHome / "Games/game/current/marker.txt"), "one");
    EXPECT_FALSE(fs::exists(deckHome / "Games/game/previous"));

    fs::remove_all(project / "build/linux");
    const fs::path second = build("1.1.0", "two");
    ASSERT_TRUE(deckDeploy(second, "linux", config.deck, {}, output, error)) << error << "\n" << output;
    EXPECT_EQ(TestTempDir::read(deckHome / "Games/game/current/marker.txt"), "two");
    EXPECT_EQ(TestTempDir::read(deckHome / "Games/game/previous/marker.txt"), "one");
    EXPECT_TRUE(fs::is_regular_file(deckHome / "Games/game/current/.jasper-release"));
    EXPECT_FALSE(fs::exists(deckHome / "Games/game/current/.deploy-sha256"));

    ASSERT_TRUE(deckRollback(config.deck, {}, output, error)) << error;
    EXPECT_EQ(TestTempDir::read(deckHome / "Games/game/current/marker.txt"), "one");
    EXPECT_EQ(TestTempDir::read(deckHome / "Games/game/previous/marker.txt"), "two");

    // the smoke run is the player's exit code and log tail: a stand-in player here
    writeScript(deckHome / "Games/game/current/game", "echo \"smoke $*\"\nexit 3\n");
    int code = 0;
    ASSERT_TRUE(deckSmoke(config.deck, "game", {}, code, output, error)) << error;
    EXPECT_EQ(code, 3);
    EXPECT_NE(output.find("smoke --quit-after 600 --fail-on-error"), std::string::npos) << output;

    // a non-linux target, and a remote dir that is not a plain relative path, are refused
    DeckSettings bad = config.deck;
    bad.remoteDir = "../escape";
    EXPECT_FALSE(deckRollback(bad, {}, output, error));
    EXPECT_NE(error.find("remoteDir"), std::string::npos) << error;
    ::unsetenv("FAKE_DECK_HOME");
}

#endif

TEST(Notices, ALicenceMayNameItselfInItsExtension) {
    // FFmpeg's COPYING.LGPLv2.1, which the exporter stages as tools/ci/check-notices.py does
    TestTempDir directory("notices");
    TestTempDir::write(directory.path() / "COPYING.LGPLv2.1", "LGPL\n");
    TestTempDir::write(directory.path() / "COPYING.bak", "no\n");
    TestTempDir::write(directory.path() / "README.md", "no\n");
    const std::vector<fs::path> files = licenceFiles(directory.path());
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].filename().string(), "COPYING.LGPLv2.1");
}

// S80 R40 / S81 R50: ThorVG's `when` is a list of player features (it ships with the HTML UI or
// with Title Art) and the repository's notices.json loads; HarfBuzz's subsetter is editor only
TEST(Notices, WhenListShipsWithAnyOfItsFeatures) {
    TestTempDir directory("notices-when");
    TestTempDir::write(directory.path() / "notices.json",
                       R"({"schemaVersion": 1, "libraries": {"sokol": {"dir": "engine/libs/sokol", "when": "always"},
                           "thorvg": {"dir": "engine/libs/thorvg", "when": ["htmlui", "titleart"]},
                           "harfbuzz-subset": {"dir": "engine/libs/harfbuzz/subset", "when": "editor"}}})");
    std::vector<NoticeLibrary> all;
    std::string error;
    ASSERT_TRUE(loadNoticeLibraries(directory.path() / "notices.json", all, error)) << error;
    auto names = [&](const std::vector<std::string>& features) {
        std::vector<std::string> out;
        for (const NoticeLibrary& library : shippedLibraries(all, features)) out.push_back(library.name);
        return out;
    };
    EXPECT_EQ(names({}), std::vector<std::string>({"sokol"}));
    EXPECT_EQ(names({"titleart"}), std::vector<std::string>({"sokol", "thorvg"}));
    EXPECT_EQ(names({"htmlui"}), std::vector<std::string>({"sokol", "thorvg"}));

    ASSERT_TRUE(loadNoticeLibraries(fs::path(JASPER_TEST_REPO_DIR) / "engine" / "notices.json", all, error)) << error;
    const std::vector<std::string> titleArt = names({"titleart"});
    EXPECT_NE(std::find(titleArt.begin(), titleArt.end(), "thorvg"), titleArt.end());
    EXPECT_EQ(std::find(titleArt.begin(), titleArt.end(), "harfbuzz-subset"), titleArt.end());
}
