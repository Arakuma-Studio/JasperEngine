// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Project Hub's logic (docs/specs/S27-editor-tools.md R18-R23, R34, AC18-AC20): names and
// paths, Git URL rules, recent projects, templates and the zip import with crafted archives

#include "hub/HubPaths.h"
#include "hub/ProjectImporter.h"
#include "hub/RecentProjects.h"
#include "hub/TemplateCatalog.h"
#include "io/AtomicFile.h"

#include "support/TestTempDir.h"

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "libs/miniz/miniz.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <fstream>

#ifndef _WIN32
    #include <signal.h>
    #include <sys/resource.h>
#endif

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::hub;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

namespace {

    struct Entry {
        std::string name;
        std::string data;
        unsigned unixType = 0100000;   // a regular file
    };

    // a zip of `entries`; an entry with a type of its own (a link, a device) gets it in the central
    // directory's external attributes, the archive marked as made on Unix (as zip -y makes them)
    void writeZip(const fs::path& file, const std::vector<Entry>& entries) {
        mz_zip_archive zip{};
        ASSERT_TRUE(mz_zip_writer_init_file(&zip, file.string().c_str(), 0));
        // miniz refuses to write an absolute name: such an entry is written as "_..." and renamed in
        // the bytes below (local header and central directory)
        auto written = [](const std::string& name) { return !name.empty() && name[0] == '/' ? "_" + name.substr(1) : name; };
        for (const Entry& e : entries) {
            ASSERT_TRUE(mz_zip_writer_add_mem(&zip, written(e.name).c_str(), e.data.data(), e.data.size(), MZ_DEFAULT_COMPRESSION)) << e.name;
        }
        ASSERT_TRUE(mz_zip_writer_finalize_archive(&zip));
        ASSERT_TRUE(mz_zip_writer_end(&zip));
        std::string bytes;
        {
            std::ifstream in(file, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        for (const Entry& e : entries) {
            const std::string stored = written(e.name);
            if (stored == e.name) continue;
            for (size_t at = bytes.find(stored); at != std::string::npos; at = bytes.find(stored, at + stored.size())) {
                bytes.replace(at, stored.size(), e.name);
            }
        }
        for (size_t at = bytes.find("PK\x01\x02"); at != std::string::npos && at + 46 <= bytes.size(); at = bytes.find("PK\x01\x02", at + 4)) {
            const size_t nameLength = (uint8_t)bytes[at + 28] | ((size_t)(uint8_t)bytes[at + 29] << 8);
            const std::string name = bytes.substr(at + 46, nameLength);
            for (const Entry& e : entries) {
                if (e.name != name || e.unixType == 0100000) continue;
                const uint32_t attributes = (uint32_t)(e.unixType | 0644) << 16;
                bytes[at + 5] = 3;   // version made by: Unix
                for (int i = 0; i < 4; i++) bytes[at + 38 + i] = (char)((attributes >> (8 * i)) & 0xFF);
            }
        }
        std::ofstream(file, std::ios::binary | std::ios::trunc).write(bytes.data(), (std::streamsize)bytes.size());
    }

    bool emptyFolder(const fs::path& dir) {
        std::error_code ec;
        return fs::directory_iterator(dir, ec) == fs::directory_iterator();
    }

    // a project of a few files, the source of the template tests below
    fs::path writeGameProject(const fs::path& project) {
        for (const char* file : {"project.yaml", "Main.scene", "scripts/Rotator.lua", "scripts/Other.lua"}) {
            fs::create_directories((project / file).parent_path());
            std::ofstream(project / file) << file;
        }
        return project;
    }

    // the process ends at its `nth` publish of a file: a crash in the middle of a copy
    void dieAtPublish(int nth) {
        auto count = std::make_shared<int>(0);
        io::setAtomicFileFaultInjector([count, nth](std::string_view step, const fs::path&) -> std::error_code {
            if (step == "rename" && ++*count == nth) std::_Exit(42);
            return {};
        });
    }

}

TEST(HubPaths, CatalogIdsAndRelativePaths) {
    EXPECT_TRUE(isValidCatalogId("3d"));
    EXPECT_TRUE(isValidCatalogId("My_Template-2"));
    EXPECT_FALSE(isValidCatalogId(""));
    EXPECT_FALSE(isValidCatalogId("a b"));
    EXPECT_FALSE(isValidCatalogId("../x"));
    EXPECT_FALSE(isValidCatalogId(std::string(65, 'a')));
    EXPECT_TRUE(isSafeRelativePath("scenes/Main.scene"));
    EXPECT_FALSE(isSafeRelativePath("../evil"));
    EXPECT_FALSE(isSafeRelativePath("/abs"));
    EXPECT_FALSE(isSafeRelativePath("C:/x"));
    EXPECT_FALSE(isSafeRelativePath("a\\b"));
    EXPECT_FALSE(isSafeRelativePath("-flag"));
    EXPECT_FALSE(isSafeRelativePath("a//b"));
    EXPECT_EQ(slugify("My Racing Game!"), "my-racing-game");
    EXPECT_EQ(folderName(" New: Project. "), "New Project");
    std::string why;
    EXPECT_TRUE(isSafeArchiveEntry("Game/project.yaml", why));
    EXPECT_TRUE(isSafeArchiveEntry("Game/scenes/", why));
    EXPECT_FALSE(isSafeArchiveEntry("../evil", why));
    EXPECT_FALSE(isSafeArchiveEntry("/etc/passwd", why));
    EXPECT_FALSE(isSafeArchiveEntry("C:/x", why));
    EXPECT_FALSE(isSafeArchiveEntry("a\\b", why));
}

TEST(GitImport, UrlValidation) {
    std::string why;
    EXPECT_TRUE(isAcceptedGitUrl("https://github.com/team/game.git", why));
    EXPECT_TRUE(isAcceptedGitUrl("ssh://git@host:22/team/game.git", why));
    EXPECT_TRUE(isAcceptedGitUrl("git@github.com:team/game.git", why));
    EXPECT_FALSE(isAcceptedGitUrl("file:///srv/repo.git", why));
    EXPECT_FALSE(isAcceptedGitUrl("ext::sh -c touch% /tmp/pwned", why));
    EXPECT_FALSE(isAcceptedGitUrl("-oProxyCommand=touch /tmp/pwned", why));
    EXPECT_FALSE(isAcceptedGitUrl("--upload-pack=x", why));
    EXPECT_FALSE(isAcceptedGitUrl("http://plain.example/repo", why));
    EXPECT_FALSE(isAcceptedGitUrl("https://", why));
    EXPECT_FALSE(isAcceptedGitUrl("/srv/repo.git", why));
    EXPECT_FALSE(isAcceptedGitUrl("https://host/a b", why));
    EXPECT_EQ(importer::repositoryName("https://github.com/team/game.git"), "game");
    EXPECT_EQ(importer::repositoryName("git@host:team/My Game/"), "My Game");
    EXPECT_FLOAT_EQ(importer::gitProgress("Receiving objects:  45% (9/20)"), 0.45f);
    EXPECT_LT(importer::gitProgress("Cloning into 'x'..."), 0.0f);
}

TEST(RecentProjects, MigrationCapAndPins) {
    // the old list of paths reads as path-only records
    const YAML::Node old = YAML::Load("[/p/A, /p/B, /p/A]");
    std::vector<RecentProject> records = recent::fromYaml(old);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].name, "A");
    EXPECT_TRUE(records[0].lastOpened.empty());
    // round trip as records
    records[1].pinned = true;
    const std::vector<RecentProject> back = recent::fromYaml(recent::toYaml(records));
    EXPECT_EQ(back, records);
    // capped at 30, pinned ones never evicted
    std::vector<RecentProject> many;
    recent::touch(many, "/p/pinned", "Pinned", "2020-01-01T00:00:00Z");
    recent::setPinned(many, "/p/pinned", true);
    for (int i = 0; i < 40; i++) recent::touch(many, "/p/" + std::to_string(i), "", recent::iso(std::chrono::system_clock::now()));
    EXPECT_EQ(many.size(), recent::kCap);
    EXPECT_TRUE(std::any_of(many.begin(), many.end(), [](const RecentProject& p) { return p.path == "/p/pinned"; }));
    EXPECT_EQ(many.front().path, fs::path("/p/39"));
    // listed: pinned first, then by last opened; the filter
    const std::vector<RecentProject> listed = recent::listed(many, recent::Sort::LastOpened, "");
    EXPECT_EQ(listed.front().path, fs::path("/p/pinned"));
    // the paths with "/p/3": 30-39 (/p/3 itself was evicted: 11-39 and the pinned one are kept)
    EXPECT_EQ(recent::listed(many, recent::Sort::Name, "/p/3").size(), 10u);
    recent::relocate(many, "/p/pinned", "/q/moved");
    EXPECT_TRUE(std::any_of(many.begin(), many.end(), [](const RecentProject& p) { return p.path == "/q/moved" && p.pinned; }));
    recent::remove(many, "/q/moved");
    EXPECT_EQ(many.size(), recent::kCap - 1);
    // ISO 8601 UTC and the "ago" text
    std::chrono::system_clock::time_point t;
    ASSERT_TRUE(recent::parseIso("2026-10-27T12:00:00Z", t));
    EXPECT_EQ(recent::iso(t), "2026-10-27T12:00:00Z");
    EXPECT_EQ(recent::openedAgo("2026-10-27T12:00:00Z", t + std::chrono::hours(72)), "Opened 3 days ago");
    EXPECT_EQ(recent::openedAgo("", t), "Never opened");
    EXPECT_FALSE(recent::parseIso("yesterday", t));
}

TEST(ZipImport, NestedProject) {
    TestTempDir dir("zip-import");
    const fs::path zip = dir.path() / "Game.zip";
    writeZip(zip, {{"Download/Game/project.yaml", "name: Game\n"}, {"Download/Game/scenes/Main.scene", "scene\n"}, {"Download/readme.txt", "hi"}});
    const fs::path parent = dir.path() / "Projects";
    fs::create_directories(parent);
    fs::path projectDir;
    std::string error;
    ASSERT_TRUE(importer::importZip(zip, parent, ZipLimits(), nullptr, projectDir, error)) << error;
    EXPECT_EQ(projectDir, parent / "Game");
    EXPECT_TRUE(fs::exists(parent / "Game" / "scenes" / "Main.scene"));
    // nothing else left: the staging folder is gone
    size_t entries = 0;
    for (const auto& e : fs::directory_iterator(parent)) {
        (void)e;
        entries++;
    }
    EXPECT_EQ(entries, 1u);
    // the same again: the target exists
    EXPECT_FALSE(importer::importZip(zip, parent, ZipLimits(), nullptr, projectDir, error));
}

#ifndef _WIN32
// the disk fills up when an entry's last buffer is flushed: a failed import, not a project with a
// short file. The entry is under 1 KiB, which the stream holds back (it writes more than that at once)
TEST(ZipImport, AWriteThatFailsAtTheLastFlushFailsTheImport) {
    TestTempDir dir("zip-flush");
    const fs::path parent = dir.path() / "Projects";
    fs::create_directories(parent);
    const fs::path zip = dir.path() / "Game.zip";
    writeZip(zip, {{"Game/project.yaml", std::string(500, 'a')}});

    rlimit limit{};
    ASSERT_EQ(getrlimit(RLIMIT_FSIZE, &limit), 0);
    const rlimit unlimited = limit;
    limit.rlim_cur = 100;
    void (*const previous)(int) = signal(SIGXFSZ, SIG_IGN);
    ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &limit), 0);
    fs::path projectDir;
    std::string error;
    const bool imported = importer::importZip(zip, parent, ZipLimits(), nullptr, projectDir, error);
    setrlimit(RLIMIT_FSIZE, &unlimited);
    signal(SIGXFSZ, previous);

    EXPECT_FALSE(imported) << "imported to " << projectDir;
    EXPECT_NE(error.find("cannot write"), std::string::npos) << error;
    EXPECT_TRUE(emptyFolder(parent));
}
#endif

TEST(ZipImport, RefusedArchivesLeaveNothing) {
    TestTempDir dir("zip-refused");
    const fs::path parent = dir.path() / "Projects";
    fs::create_directories(parent);
    struct Case {
        const char* name;
        std::vector<Entry> entries;
        ZipLimits limits;
    };
    std::vector<Case> cases = {
        {"traversal", {{"Game/project.yaml", "x"}, {"../evil", "x"}}, ZipLimits()},
        {"absolute", {{"Game/project.yaml", "x"}, {"/abs/evil", "x"}}, ZipLimits()},
        {"backslash", {{"Game/project.yaml", "x"}, {"Game\\evil", "x"}}, ZipLimits()},
        {"symlink", {{"Game/project.yaml", "x"}, {"Game/link", "/etc/passwd", 0120000}}, ZipLimits()},
        {"device", {{"Game/project.yaml", "x"}, {"Game/dev", "", 0020000}}, ZipLimits()},
        {"oversize", {{"Game/project.yaml", std::string(4096, 'a')}}, ZipLimits{1024, 200000}},
        {"too many", {{"Game/project.yaml", "x"}, {"Game/a", "x"}, {"Game/b", "x"}}, ZipLimits{uint64_t(1) << 30, 2}},
        {"no project", {{"Game/readme.txt", "x"}}, ZipLimits()},
    };
    for (Case& c : cases) {
        const fs::path zip = dir.path() / (std::string(c.name) + ".zip");
        writeZip(zip, c.entries);
        fs::path projectDir;
        std::string error;
        EXPECT_FALSE(importer::importZip(zip, parent, c.limits, nullptr, projectDir, error)) << c.name;
        EXPECT_FALSE(error.empty()) << c.name;
        EXPECT_TRUE(emptyFolder(parent)) << c.name << ": " << error;
    }
    EXPECT_FALSE(fs::exists(dir.path() / "evil"));
    // not a zip at all
    std::ofstream(dir.path() / "fake.zip") << "not a zip";
    fs::path projectDir;
    std::string error;
    EXPECT_FALSE(importer::importZip(dir.path() / "fake.zip", parent, ZipLimits(), nullptr, projectDir, error));
    EXPECT_TRUE(emptyFolder(parent));
    // a cancel from the progress callback stops it and cleans up
    const fs::path big = dir.path() / "big.zip";
    writeZip(big, {{"Game/project.yaml", "x"}, {"Game/a", "x"}, {"Game/b", "x"}});
    EXPECT_FALSE(importer::importZip(big, parent, ZipLimits(), [](float, const std::string&) { return false; }, projectDir, error));
    EXPECT_EQ(error, "cancelled");
    EXPECT_TRUE(emptyFolder(parent));
}

// the copy dies midway, the editor with it: nothing half made at the target, and the same copy
// works afterwards. A built-in template and a user's; a new folder and an empty one the user made
TEST(ProjectTemplatesDeathTest, ACrashMidCopyLeavesTheTargetAsItWas) {
    TestTempDir dir("templates-crash");
    const fs::path project = writeGameProject(dir.path() / "Game");
    const fs::path user = templates::userDirectory(dir.path() / "settings");
    std::string id, error;

    EXPECT_EXIT({
        dieAtPublish(2);
        templates::saveProjectAsTemplate(project, user, "Saved", "", "Main", {}, id, error);
        std::_Exit(0);
    }, ::testing::ExitedWithCode(42), "");
    EXPECT_FALSE(fs::exists(user / "saved"));
    EXPECT_EQ(templates::find(templates::all(user), "saved"), nullptr);
    EXPECT_TRUE(templates::saveProjectAsTemplate(project, user, "Saved", "", "Main", {}, id, error)) << error;

    const std::vector<ProjectTemplate> list = templates::all(user);
    size_t saved = 0;
    for (const ProjectTemplate& t : list) saved += t.id == "saved" ? 1 : 0;
    EXPECT_EQ(saved, 1u) << "only the published template is in the catalog";
    for (const char* name : {"3d", "saved"}) {
        const ProjectTemplate* info = templates::find(list, name);
        ASSERT_NE(info, nullptr) << name;
        for (const bool existing : {false, true}) {
            const fs::path target = dir.path() / "Projects" / (std::string(name) + (existing ? "-existing" : "-new"));
            if (existing) fs::create_directories(target);
            EXPECT_EXIT({
                dieAtPublish(2);
                templates::materialize(*info, target, error);
                std::_Exit(0);
            }, ::testing::ExitedWithCode(42), "") << target;
            EXPECT_EQ(fs::exists(target), existing) << target;
            if (existing) EXPECT_TRUE(emptyFolder(target)) << target;
            EXPECT_TRUE(templates::materialize(*info, target, error)) << error;
            EXPECT_TRUE(fs::exists(target / "project.yaml")) << target;
        }
    }
}

// a failure midway: the folder keeps what the user put there (the copy never wrote in it), and none
// of the copy stays behind. The publish itself failing is one too
TEST(ProjectTemplates, AFailedCopyLeavesTheTargetAsItWas) {
    TestTempDir dir("templates-failed");
    const fs::path project = writeGameProject(dir.path() / "Game");
    const fs::path user = templates::userDirectory(dir.path() / "settings");
    std::string id, error;
    ASSERT_TRUE(templates::saveProjectAsTemplate(project, user, "Saved", "", "Main", {}, id, error)) << error;
    const std::vector<ProjectTemplate> list = templates::all(user);
    const fs::path parent = dir.path() / "Projects";
    const fs::path target = parent / "New";
    const std::error_code full = std::make_error_code(std::errc::no_space_on_device);
    struct InjectorOff {
        ~InjectorOff() { io::setAtomicFileFaultInjector(nullptr); }
    } injectorOff;

    for (const char* name : {"3d", "saved"}) {
        const ProjectTemplate* info = templates::find(list, name);
        ASSERT_NE(info, nullptr) << name;
        fs::remove_all(parent);
        fs::create_directories(target);

        // the second file fails, and the user dropped a file in the empty folder meanwhile
        auto renames = std::make_shared<int>(0);
        io::setAtomicFileFaultInjector([renames, full, target](std::string_view step, const fs::path&) -> std::error_code {
            if (step != "rename" || ++*renames < 2) return {};
            std::ofstream(target / "mine.txt") << "mine";
            return full;
        });
        EXPECT_FALSE(templates::materialize(*info, target, error)) << name;
        EXPECT_TRUE(fs::exists(target / "mine.txt")) << name;
        EXPECT_FALSE(fs::exists(target / "project.yaml")) << name;
        fs::remove(target / "mine.txt");

        // the whole copy's rename fails: the user's empty folder is back
        io::setAtomicFileFaultInjector([full, target](std::string_view step, const fs::path& path) -> std::error_code {
            return step == "rename" && path == target ? full : std::error_code();
        });
        EXPECT_FALSE(templates::materialize(*info, target, error)) << name;
        io::setAtomicFileFaultInjector(nullptr);
        EXPECT_TRUE(fs::is_directory(target)) << name;
        EXPECT_TRUE(emptyFolder(target)) << name;

        // a cancel
        const std::atomic<bool> cancelled{true};
        EXPECT_FALSE(templates::materialize(*info, target, error, &cancelled)) << name;
        EXPECT_EQ(error, "cancelled") << name;
        EXPECT_TRUE(emptyFolder(target)) << name;

        // nothing else in the location, and a path with a trailing separator is the same target
        size_t folders = 0;
        for (const fs::directory_entry& entry : fs::directory_iterator(parent)) folders += entry.exists() ? 1 : 0;
        EXPECT_EQ(folders, 1u) << name << ": a staging folder stayed behind";
        EXPECT_TRUE(templates::materialize(*info, target / "", error)) << error;
        EXPECT_TRUE(fs::exists(target / "project.yaml")) << name;
    }

    // saving a template: same, and the catalog's folder has nothing left
    auto renames = std::make_shared<int>(0);
    io::setAtomicFileFaultInjector([renames, full](std::string_view step, const fs::path&) -> std::error_code {
        return step == "rename" && ++*renames == 2 ? full : std::error_code();
    });
    EXPECT_FALSE(templates::saveProjectAsTemplate(project, user, "Other", "", "Main", {}, id, error));
    io::setAtomicFileFaultInjector(nullptr);
    EXPECT_FALSE(fs::exists(user / "other"));
    EXPECT_FALSE(fs::exists(user / ".staging"));
}

TEST(ProjectTemplates, CatalogAndSaveAsTemplate) {
    TestTempDir dir("templates");
    // template.json rules
    ProjectTemplate info;
    std::string error;
    EXPECT_TRUE(templates::parseInfo(R"({"schemaVersion": 1, "id": "3d", "name": "3D", "startScene": "Main"})", info, error)) << error;
    EXPECT_FALSE(templates::parseInfo(R"({"schemaVersion": 2, "id": "3d"})", info, error));
    EXPECT_FALSE(templates::parseInfo(R"({"schemaVersion": 1, "id": "../3d"})", info, error));
    EXPECT_FALSE(templates::parseInfo(R"({"schemaVersion": 1, "id": "x", "thumbnail": "../t.png"})", info, error));
    // a project saved as a template: no editor folders, then made into a new project
    const fs::path project = dir.path() / "Game";
    for (const char* file : {"project.yaml", "Main.scene", "scripts/Rotator.lua", ".jasperengine/user/workspace.yaml", "build/x.o",
                             "Recordings/Main_001.mp4", ".git/HEAD", ".trash/old.scene"}) {
        fs::create_directories((project / file).parent_path());
        std::ofstream(project / file) << file;
    }
    const fs::path user = templates::userDirectory(dir.path() / "settings");
    std::string id;
    ASSERT_TRUE(templates::saveProjectAsTemplate(project, user, "My Shooter", "Twin-stick", "Main", {0x89, 'P', 'N', 'G'}, id, error)) << error;
    EXPECT_EQ(id, "my-shooter");
    EXPECT_TRUE(fs::exists(user / id / "scripts" / "Rotator.lua"));
    for (const char* excluded : {".jasperengine", "build", "Recordings", ".git", ".trash"}) EXPECT_FALSE(fs::exists(user / id / excluded)) << excluded;
    EXPECT_FALSE(templates::saveProjectAsTemplate(project, user, "My Shooter", "", "", {}, id, error));
    const std::vector<ProjectTemplate> list = templates::all(user);
    const ProjectTemplate* mine = templates::find(list, "my-shooter");
    ASSERT_NE(mine, nullptr);
    EXPECT_FALSE(mine->builtIn);
    EXPECT_EQ(mine->thumbnailPng.size(), 4u);
    // the built-in ones are embedded: empty, 3d, 2d, ui
    for (const char* builtIn : {"empty", "3d", "2d", "ui"}) {
        const ProjectTemplate* t = templates::find(list, builtIn);
        ASSERT_NE(t, nullptr) << builtIn;
        EXPECT_TRUE(t->builtIn);
        EXPECT_FALSE(t->thumbnailPng.empty()) << builtIn;
    }
    // made into a project: template.json and the thumbnail stay out
    const fs::path made = dir.path() / "New";
    ASSERT_TRUE(templates::materialize(*mine, made, error)) << error;
    EXPECT_TRUE(fs::exists(made / "project.yaml"));
    EXPECT_FALSE(fs::exists(made / templates::kInfoFile));
    EXPECT_FALSE(fs::exists(made / "thumbnail.png"));
    // a target that is not empty is refused, and left as it was
    EXPECT_FALSE(templates::materialize(*mine, made, error));
    EXPECT_TRUE(fs::exists(made / "project.yaml"));
    const fs::path builtInMade = dir.path() / "From3D";
    ASSERT_TRUE(templates::materialize(*templates::find(list, "3d"), builtInMade, error)) << error;
    EXPECT_TRUE(fs::exists(builtInMade / "project.yaml"));
    EXPECT_TRUE(fs::exists(builtInMade / "Main.scene"));
}
