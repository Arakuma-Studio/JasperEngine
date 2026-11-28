// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Player runtime archives (docs/specs/S30-shipping.md R8-R10, D5): an archive is installed only
// when it is the release's (its SHA-256 in the editor's table) and its binaries match its own
// player-info.json; nothing is left behind when it is refused; the editor finds only runtimes of
// its own version.

#include "support/TestTempDir.h"

#include "players/PlayerRuntimes.h"
#include "serialization/CookedScene.h"
#include "util/Sha256.h"
#include "util/ZipArchive.h"

#include <gtest/gtest.h>

namespace player_install_test {

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;


    const char* ID = "linux-x64-glcore-release";
    const std::string BINARY = "\x7f" "ELF player bytes";

    PlayerInfo info(const std::string& binarySha) {
        PlayerInfo p;
        p.id = ID;
        p.platform = "linux";
        p.arch = "x64";
        p.backend = "glcore";
        p.flavour = "release";
        p.engineVersion = PlayerRuntimes::editorVersion();
        p.contentFormatVersion = jasperengine::cook::contentFormatVersion();
        p.notices = {"lua"};
        p.executables["files/jasperengine-player"] = binarySha;
        p.executable = "jasperengine-player";
        return p;
    }

    // an archive as tools/players/package_player.py --zip writes it
    fs::path writeArchive(const fs::path& path, const std::string& binary, const PlayerInfo& p,
                          const std::string& extraName = "", const std::string& extra = "") {
        ZipWriter zip;
        std::string error;
        EXPECT_TRUE(zip.open(path, error)) << error;
        const std::string json = p.toJson();
        EXPECT_TRUE(zip.add("player-info.json", json.data(), json.size(), false, error)) << error;
        EXPECT_TRUE(zip.add("files/jasperengine-player", binary.data(), binary.size(), false, error)) << error;
        EXPECT_TRUE(zip.add("licenses/lua-LICENSE", "MIT", 3, false, error)) << error;
        if (!extraName.empty()) {
            EXPECT_TRUE(zip.add(extraName, extra.data(), extra.size(), false, error)) << error;
        }
        EXPECT_TRUE(zip.finish(error)) << error;
        return path;
    }

    std::string fileSha(const fs::path& path) {
        std::string hex;
        sha256File(path, hex);
        return hex;
    }

}

namespace player_install_test {

using namespace player_install_test;

TEST(PlayerInstall, TheReleasesArchiveInstallsAndIsFound) {
    TestTempDir dir("players");
    const fs::path archive = writeArchive(dir / "a.zip", BINARY, info(sha256Hex(BINARY)));
    const std::string sha = fileSha(archive);
    const std::vector<PlayerArchiveHash> table = {{ID, sha.c_str(), fs::file_size(archive)}};
    std::string installed, error;
    std::vector<std::string> warnings;
    ASSERT_TRUE(PlayerRuntimes::install(archive, dir / "root", installed, warnings, error, &table)) << error;
    EXPECT_EQ(installed, ID);
    EXPECT_TRUE(warnings.empty());
    const fs::path binary = PlayerRuntimes::installRoot(dir / "root") / ID / "files/jasperengine-player";
    ASSERT_TRUE(fs::is_regular_file(binary));
    EXPECT_NE(fs::status(binary).permissions() & fs::perms::owner_exec, fs::perms::none);

    const std::optional<PlayerInfo> found = PlayerRuntimes::find(ID, dir / "root", false, error);
    ASSERT_TRUE(found.has_value()) << error;
    EXPECT_EQ(found->notices, std::vector<std::string>{"lua"});
    EXPECT_TRUE(PlayerRuntimes::remove(ID, dir / "root", error)) << error;
    EXPECT_FALSE(PlayerRuntimes::find(ID, dir / "root", false, error).has_value());
}

TEST(PlayerInstall, RefusesAnotherArchiveADamagedOneAndEscapingEntries) {
    TestTempDir dir("players");
    const fs::path good = writeArchive(dir / "good.zip", BINARY, info(sha256Hex(BINARY)));
    const std::string sha = fileSha(good);
    const std::vector<PlayerArchiveHash> table = {{ID, sha.c_str(), fs::file_size(good)}};
    std::string installed, error;
    std::vector<std::string> warnings;

    // not the release's bytes
    const fs::path other = writeArchive(dir / "other.zip", BINARY, info(sha256Hex(BINARY)), "extra.txt", "x");
    EXPECT_FALSE(PlayerRuntimes::install(other, dir / "root", installed, warnings, error, &table));
    EXPECT_NE(error.find("not the release's archive"), std::string::npos) << error;

    // a development editor (no table) takes it with a warning, but checks its binaries
    const std::vector<PlayerArchiveHash> none;
    const fs::path damaged = writeArchive(dir / "damaged.zip", "other bytes", info(sha256Hex(BINARY)));
    EXPECT_FALSE(PlayerRuntimes::install(damaged, dir / "root", installed, warnings, error, &none));
    EXPECT_NE(error.find("does not match"), std::string::npos) << error;
    EXPECT_FALSE(warnings.empty());

    const fs::path escaping = writeArchive(dir / "escape.zip", BINARY, info(sha256Hex(BINARY)), "../outside.txt", "x");
    EXPECT_FALSE(PlayerRuntimes::install(escaping, dir / "root", installed, warnings, error, &none));
    EXPECT_FALSE(fs::exists(dir / "outside.txt"));
    EXPECT_FALSE(fs::exists(dir / "root" / "outside.txt"));

    // nothing half-installed
    std::error_code ec;
    const fs::path root = PlayerRuntimes::installRoot(dir / "root");
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        ADD_FAILURE() << "left behind: " << entry.path();
    }
}

TEST(PlayerInstall, AnotherEditorVersionIsNotUsed) {
    TestTempDir dir("players");
    PlayerInfo p = info(sha256Hex(BINARY));
    p.engineVersion = "v0.0.1-other";
    const fs::path archive = writeArchive(dir / "old.zip", BINARY, p);
    const std::vector<PlayerArchiveHash> none;
    std::string installed, error;
    std::vector<std::string> warnings;
    ASSERT_TRUE(PlayerRuntimes::install(archive, dir / "root", installed, warnings, error, &none)) << error;
    EXPECT_FALSE(PlayerRuntimes::find(ID, dir / "root", false, error).has_value());
    EXPECT_NE(error.find("v0.0.1-other"), std::string::npos) << error;
    EXPECT_TRUE(PlayerRuntimes::find(ID, dir / "root", true, error).has_value()) << error;
}

// The editor cooks its own format: a runtime that reads an older minor would skip what is newer,
// so it is not used unless --allow-player-mismatch asks for it (the error names the fix); a
// runtime of a newer minor is
TEST(PlayerInstall, APlayerOfAnOlderContentMinorIsNotUsed) {
    static_assert(jasperengine::cook::FORMAT_MINOR > 0, "an older minor exists");
    // what a build without a Build Profile writes: the cooked files' format
    const std::string writes = jasperengine::cook::buildContentFormatVersion(false);
    TestTempDir dir("players");
    const std::vector<PlayerArchiveHash> none;
    std::string installed, error;
    std::vector<std::string> warnings;
    const std::string major = std::to_string(jasperengine::cook::FORMAT_MAJOR) + ".";

    PlayerInfo old = info(sha256Hex(BINARY));
    old.contentFormatVersion = major + std::to_string(jasperengine::cook::FORMAT_MINOR - 1);
    ASSERT_TRUE(PlayerRuntimes::install(writeArchive(dir / "old.zip", BINARY, old), dir / "old", installed, warnings, error, &none)) << error;
    EXPECT_FALSE(PlayerRuntimes::find(ID, dir / "old", false, error).has_value());
    EXPECT_NE(error.find("reads content format " + old.contentFormatVersion + ", older than the " + writes +
                         " this editor writes: update the player runtime"), std::string::npos) << error;
    EXPECT_TRUE(PlayerRuntimes::find(ID, dir / "old", true, error).has_value()) << error;

    PlayerInfo newer = info(sha256Hex(BINARY));
    newer.contentFormatVersion = major + std::to_string(jasperengine::cook::FORMAT_MINOR + 1);
    ASSERT_TRUE(PlayerRuntimes::install(writeArchive(dir / "new.zip", BINARY, newer), dir / "new", installed, warnings, error, &none)) << error;
    EXPECT_TRUE(PlayerRuntimes::find(ID, dir / "new", false, error).has_value()) << error;

    PlayerInfo otherMajor = info(sha256Hex(BINARY));
    otherMajor.contentFormatVersion = std::to_string(jasperengine::cook::FORMAT_MAJOR + 1) + ".0";
    ASSERT_TRUE(PlayerRuntimes::install(writeArchive(dir / "major.zip", BINARY, otherMajor), dir / "major", installed, warnings, error, &none)) << error;
    EXPECT_FALSE(PlayerRuntimes::find(ID, dir / "major", true, error).has_value());
}

// Content of a build without a Build Profile is laid out as the cooked files' minor (FORMAT_MINOR):
// game.json's buildProfile and defines are the only thing BUILD_PROFILE_MINOR adds. A player runtime
// of that layout still serves such a build; a runtime older than BUILD_PROFILE_MINOR (every one
// released before Build Profiles) refuses a Build Profile's build. (Once a later cooked minor passed
// BUILD_PROFILE_MINOR, S95's 1.12 and 1.13, a runtime of the layout reads the profile too.)
TEST(PlayerInstall, APlayerOfTheContentBeforeBuildProfilesServesABuildWithoutOne) {
    TestTempDir dir("players");
    const std::vector<PlayerArchiveHash> none;
    std::string installed, error;
    std::vector<std::string> warnings;
    const std::string major = std::to_string(jasperengine::cook::FORMAT_MAJOR) + ".";
    PlayerInfo layoutInfo = info(sha256Hex(BINARY));
    layoutInfo.contentFormatVersion = jasperengine::cook::buildContentFormatVersion(false);
    const std::string layout = major + std::to_string(jasperengine::cook::FORMAT_MINOR);
    ASSERT_EQ(layoutInfo.contentFormatVersion, layout);
    ASSERT_TRUE(PlayerRuntimes::install(writeArchive(dir / "layout.zip", BINARY, layoutInfo), dir / "layout", installed, warnings, error, &none)) << error;
    EXPECT_TRUE(PlayerRuntimes::find(ID, dir / "layout", false, error).has_value()) << error;
    EXPECT_EQ(PlayerRuntimes::findVariants(ID, dir / "layout", false).size(), 1u);

    // a build of a Build Profile needs game.json's profile and defines read: a runtime before
    // BUILD_PROFILE_MINOR is refused, --allow-player-mismatch too (it would run a demo as the full
    // game), naming the fix
    const std::string profile = jasperengine::cook::buildContentFormatVersion(true);
    PlayerInfo before = info(sha256Hex(BINARY));
    before.contentFormatVersion = major + std::to_string(jasperengine::cook::BUILD_PROFILE_MINOR - 1);
    ASSERT_TRUE(PlayerRuntimes::install(writeArchive(dir / "before.zip", BINARY, before), dir / "before", installed, warnings, error, &none)) << error;
    for (bool allowMismatch : {false, true}) {
        EXPECT_FALSE(PlayerRuntimes::find(ID, dir / "before", allowMismatch, error, profile, true).has_value());
        EXPECT_NE(error.find("reads content format " + before.contentFormatVersion + ", older than the " + profile +
                             " a Build Profile's build needs (its game.json's profile and defines): update the player runtime"),
                  std::string::npos)
            << error;
        EXPECT_TRUE(PlayerRuntimes::findVariants(ID, dir / "before", allowMismatch, profile, true).empty());
    }
    // the layout's runtime serves a profile's build when the layout is no older than BUILD_PROFILE_MINOR
    EXPECT_EQ(PlayerRuntimes::find(ID, dir / "layout", false, error, profile, true).has_value(),
              jasperengine::cook::FORMAT_MINOR >= jasperengine::cook::BUILD_PROFILE_MINOR) << error;
    // and a runtime of this editor serves both
    PlayerInfo current = info(sha256Hex(BINARY));
    ASSERT_TRUE(PlayerRuntimes::install(writeArchive(dir / "current.zip", BINARY, current), dir / "current", installed, warnings, error, &none)) << error;
    EXPECT_TRUE(PlayerRuntimes::find(ID, dir / "current", false, error, profile, true).has_value()) << error;
    EXPECT_TRUE(PlayerRuntimes::find(ID, dir / "current", false, error).has_value()) << error;
}

// A nightly (nightly-YYYYMMDD, .github/workflows/build-release.yml) is a release like a stable
// v<major>.<minor>.<patch>: its editor downloads the players of its own tag, hash-checked; a
// git describe stamp or a two-part tag is a development editor
TEST(PlayerInstall, StableAndNightlyEditorsDownloadFromTheirOwnRelease) {
    EXPECT_TRUE(PlayerRuntimes::isReleaseVersion("v1.2.3"));
    EXPECT_TRUE(PlayerRuntimes::isReleaseVersion("nightly-20261112"));
    for (const char* development : {"v1.2", "v1.2.3-4-gabc1234", "v1.2.3-dirty", "nightly-2026111", "nightly-20261112-dirty",
                                    "nightly-2026-11-12", "abc1234", "unknown", ""})
        EXPECT_FALSE(PlayerRuntimes::isReleaseVersion(development)) << development;

    EXPECT_EQ(PlayerRuntimes::releaseUrl(ID, "nightly-20261112"),
              "https://github.com/Arakuma-Studio/JasperEngine/releases/download/nightly-20261112/"
              "jasperengine-player-linux-x64-glcore-release-nightly-20261112.zip");
    EXPECT_EQ(PlayerRuntimes::releaseUrl(ID, "v1.2.3"),
              "https://github.com/Arakuma-Studio/JasperEngine/releases/download/v1.2.3/jasperengine-player-linux-x64-glcore-release-v1.2.3.zip");
}

}  // namespace player_install_test
