// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Crash backups: what makes one usable, stale or nothing (docs/specs/S03-data-safety.md
// R13, R16-R18, AC10). The editor's Restore/Discard/Keep on an opening scene runs in
// test_data_safety.py::test_crash_recovery.

#include "SceneBackup.h"
#include "DocumentFile.h"
#include "io/AtomicFile.h"
#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include "json.hpp"

#if !defined(_WIN32)
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;

namespace {

    int64_t deadPid() {
#if defined(_WIN32)
        return 4294966;
#else
        const pid_t child = ::fork();
        if (child == 0) ::_exit(0);
        int status = 0;
        ::waitpid(child, &status, 0);
        return (int64_t)child;
#endif
    }

    // A backup as a session with `pid` left it
    void plantBackup(const fs::path& project, const fs::path& source, const std::string& bytes, const std::string& baseSha1, int64_t pid) {
        ASSERT_TRUE(SceneBackups::write(project, BackupKind::Scene, source, 0, bytes, baseSha1, 1));
        const fs::path sidecar = SceneBackups::sidecarPath(SceneBackups::backupPath(project, BackupKind::Scene, source));
        nlohmann::json json = nlohmann::json::parse(TestTempDir::read(sidecar));
        json["pid"] = pid;
        TestTempDir::write(sidecar, json.dump(2));
    }

}

TEST(SceneBackup, ScanUsableStaleNone) {
    TestTempDir dir("backups");
    const fs::path project = dir.path();
    const fs::path source = "scenes/Level1.scene";
    const std::string saved = "name: Level1\n";
    const std::string edited = "name: Level1\nentities: []\n";
    TestTempDir::write(project / source, saved);
    const std::string base = DocumentFiles::sha1(saved);
    const int64_t gone = deadPid();

    // no backup
    EXPECT_EQ(SceneBackups::scan(project, source).status, BackupStatus::None);

    // made from these bytes by a session that is gone, and holding more: offered
    plantBackup(project, source, edited, base, gone);
    SceneBackupInfo usable = SceneBackups::scan(project, source);
    EXPECT_EQ(usable.status, BackupStatus::Usable);
    EXPECT_EQ(usable.baseSha1, base);
    EXPECT_FALSE(usable.savedAt.empty());
    std::string restored;
    ASSERT_TRUE(SceneBackups::readBackup(usable, restored));
    EXPECT_EQ(restored, edited);

    // Keep leaves it: offered again
    EXPECT_EQ(SceneBackups::scan(project, source).status, BackupStatus::Usable);

    // its writer still runs (this process): not offered, not touched
    plantBackup(project, source, edited, base, io::currentProcessId());
    EXPECT_EQ(SceneBackups::scan(project, source).status, BackupStatus::None);
    EXPECT_TRUE(fs::exists(SceneBackups::backupPath(project, BackupKind::Scene, source)));

    // nothing the file does not have: deleted
    plantBackup(project, source, saved, base, gone);
    EXPECT_EQ(SceneBackups::scan(project, source).status, BackupStatus::None);
    EXPECT_FALSE(fs::exists(SceneBackups::backupPath(project, BackupKind::Scene, source)));

    // the file changed since (a pull): set aside under stale/, never offered, never deleted
    plantBackup(project, source, edited, DocumentFiles::sha1("name: Before the pull\n"), gone);
    SceneBackupInfo stale = SceneBackups::scan(project, source);
    EXPECT_EQ(stale.status, BackupStatus::Stale);
    EXPECT_FALSE(fs::exists(SceneBackups::backupPath(project, BackupKind::Scene, source)));
    EXPECT_TRUE(fs::exists(stale.backup));
    EXPECT_EQ(stale.backup.lexically_relative(SceneBackups::root(project)).begin()->string(), "stale");
    EXPECT_EQ(TestTempDir::read(stale.backup), edited);
    EXPECT_TRUE(fs::exists(SceneBackups::sidecarPath(stale.backup)));
    EXPECT_EQ(SceneBackups::scan(project, source).status, BackupStatus::None);
}

TEST(SceneBackup, ResolveRestoreDiscardKeep) {
    TestTempDir dir("backups");
    const fs::path project = dir.path();
    const fs::path source = "Main.scene";
    TestTempDir::write(project / source, "a: 1\n");
    plantBackup(project, source, "a: 2\n", DocumentFiles::sha1("a: 1\n"), deadPid());

    // Restore reads the backup's content; the document stays at its path
    SceneBackupInfo backup = SceneBackups::scan(project, source);
    ASSERT_EQ(backup.status, BackupStatus::Usable);
    EXPECT_EQ(backup.source, source);
    std::string content;
    ASSERT_TRUE(SceneBackups::readBackup(backup, content));
    EXPECT_EQ(content, "a: 2\n");
    EXPECT_EQ(TestTempDir::read(project / source), "a: 1\n");

    // the listing shows it until it is discarded
    std::vector<SceneBackupInfo> listed = SceneBackups::list(project);
    ASSERT_EQ(listed.size(), 1u);
    EXPECT_EQ(listed[0].status, BackupStatus::Usable);

    EXPECT_TRUE(SceneBackups::discard(project, BackupKind::Scene, source));
    EXPECT_EQ(SceneBackups::scan(project, source).status, BackupStatus::None);
    EXPECT_TRUE(SceneBackups::list(project).empty());
}

TEST(SceneBackup, CleanCloseRemovesOwnBackupsOnly) {
    TestTempDir dir("backups");
    const fs::path project = dir.path();
    TestTempDir::write(project / "A.scene", "a\n");
    TestTempDir::write(project / "B.scene", "b\n");
    ASSERT_TRUE(SceneBackups::write(project, BackupKind::Scene, "A.scene", 0, "a2\n", DocumentFiles::sha1("a\n"), 1));
    ASSERT_TRUE(SceneBackups::write(project, BackupKind::Untitled, {}, 77, "u\n", "", 1));
    plantBackup(project, "B.scene", "b2\n", DocumentFiles::sha1("b\n"), deadPid());

    EXPECT_EQ(SceneBackups::removeOwnedBy(project, io::currentProcessId()), 2u);
    std::vector<SceneBackupInfo> left = SceneBackups::list(project);
    ASSERT_EQ(left.size(), 1u);
    EXPECT_EQ(left[0].source, fs::path("B.scene"));
}

TEST(SceneBackup, UntitledOfDeadSessionsAreOffered) {
    TestTempDir dir("backups");
    const fs::path project = dir.path();
    ASSERT_TRUE(SceneBackups::write(project, BackupKind::Untitled, {}, 42, "name: Untitled\n", "", 1));
    // this session's own untitled backup is not an orphan
    EXPECT_TRUE(SceneBackups::orphanedUntitled(project).empty());

    const fs::path sidecar = SceneBackups::sidecarPath(SceneBackups::backupPath(project, BackupKind::Untitled, {}, 42));
    nlohmann::json json = nlohmann::json::parse(TestTempDir::read(sidecar));
    json["pid"] = deadPid();
    TestTempDir::write(sidecar, json.dump());
    std::vector<SceneBackupInfo> orphans = SceneBackups::orphanedUntitled(project);
    ASSERT_EQ(orphans.size(), 1u);
    EXPECT_EQ(orphans[0].kind, BackupKind::Untitled);
    EXPECT_EQ(orphans[0].sceneId, 42u);
}
