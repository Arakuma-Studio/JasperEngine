// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Git LFS file locks between two users (docs/specs/S29-version-control.md R5, R9, AC8), against
// tools/vcs-tests/fake_lfs_lock_server.py (D8) on 127.0.0.1: two clones of one local bare
// repository whose lfs.url names their user.

#include "VcsTestSupport.h"

#include "vcs/GitProvider.h"

#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor::test;

namespace {

    class FakeLockServer {
    public:
        explicit FakeLockServer(const VcsSandbox& box): portFile(box.path("lfs-port")), logFile(box.path("lfs-requests.log")) {
#ifdef _WIN32
            const fs::path python = toolPath("python");
#else
            const fs::path python = toolPath("python3");
#endif
            EXPECT_FALSE(python.empty()) << "python3 runs the fake lock server";
            ProcessSpec spec;
            spec.exe = python;
            spec.args = {(fs::path(JASPER_TEST_REPO_DIR) / "tools" / "vcs-tests" / "fake_lfs_lock_server.py").string(),
                         "--port-file", portFile.string(), "--log", logFile.string(), "--users", "alice,bob"};
            spec.timeoutMs = 0;
            spec.cancel = &stop;
            thread = std::thread([spec]() { runProcess(spec); });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            while (!fs::exists(portFile) && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            port = readFile(portFile);
        }
        ~FakeLockServer() {
            stop.cancel();
            thread.join();
        }
        std::string url(const std::string& user) const { return "http://127.0.0.1:" + port + "/" + user + "/repo.git/info/lfs"; }
        size_t requests() const {
            std::ifstream in(logFile);
            size_t lines = 0;
            std::string line;
            while (std::getline(in, line)) lines++;
            return lines;
        }

        std::string port;

    private:
        fs::path portFile;
        fs::path logFile;
        jobs::CancelToken stop;
        std::thread thread;
    };

    LockState lockOf(const SnapshotPtr& snapshot, const std::string& path, std::string* owner = nullptr) {
        if (!snapshot) return LockState::Unsupported;
        const VcsChange* change = snapshot->find(path);
        if (!change) return LockState::Unsupported;
        if (owner) *owner = change->lockOwner;
        return change->lock;
    }

}

TEST(GitLfsLocks, TwoUsers) {
    VcsSandbox box;
    FakeLockServer server(box);
    ASSERT_FALSE(server.port.empty()) << "the fake lock server did not start";

    const fs::path remote = box.path("remote.git");
    fs::create_directories(remote);
    box.gitOk(remote, {"init", "-q", "--bare"});
    const fs::path alice = box.gitRepo("alice", {{".gitattributes", "*.fbx lockable\n"}, {"hero.fbx", "fbx\n"}, {"notes.txt", "n\n"}});
    box.gitOk(alice, {"remote", "add", "origin", remote.string()});
    box.gitOk(alice, {"push", "-q", "-u", "origin", "main"});
    const fs::path bob = box.path("bob");
    box.gitOk(box.dir.path(), {"clone", "-q", remote.string(), bob.string()});
    box.gitOk(alice, {"config", "lfs.url", server.url("alice")});
    box.gitOk(bob, {"config", "lfs.url", server.url("bob")});

    VcsSettings settings = box.settings();
    settings.pollMilliseconds = 1000;
    settings.lockPollSeconds = 60;
    VcsService serviceA;
    serviceA.open(alice, alice / ".jasperengine", settings, false);
    ASSERT_TRUE(pumpUntil(serviceA, [&]() { return serviceA.state() != VcsState::Detecting; }));
    ASSERT_EQ(serviceA.state(), VcsState::Active) << serviceA.stateMessage();
    ASSERT_TRUE(serviceA.caps().locking) << serviceA.stateMessage() << " (git-lfs 3.0+ is in the build image)";

    // A locks hero.fbx and sees it as theirs
    const VcsResult locked = await(serviceA, [&](VcsService::Done done) { serviceA.lock({"hero.fbx"}, std::move(done)); });
    ASSERT_TRUE(locked.ok) << locked.message;
    EXPECT_EQ(lockOf(refreshed(serviceA, true), "hero.fbx"), LockState::LockedByMe);
    EXPECT_TRUE(serviceA.isLockable(alice / "hero.fbx"));
    EXPECT_FALSE(serviceA.isLockable(alice / "notes.txt"));

    // B's clone sees A's lock (its first poll reads the locks; refresh reads them again)
    VcsService serviceB;
    serviceB.open(bob, bob / ".jasperengine", settings, false);
    ASSERT_TRUE(pumpUntil(serviceB, [&]() { return serviceB.snapshot() != nullptr; }));
    std::string owner;
    EXPECT_EQ(lockOf(serviceB.snapshot(), "hero.fbx", &owner), LockState::LockedByOther);
    EXPECT_EQ(owner, "alice");
    EXPECT_EQ(lockOf(refreshed(serviceB, true), "hero.fbx", &owner), LockState::LockedByOther);

    // B cannot release it without force; with force it can
    const VcsResult plain = await(serviceB, [&](VcsService::Done done) { serviceB.unlock({"hero.fbx"}, false, std::move(done)); });
    EXPECT_FALSE(plain.ok);
    const VcsResult forced = await(serviceB, [&](VcsService::Done done) { serviceB.unlock({"hero.fbx"}, true, std::move(done)); });
    EXPECT_TRUE(forced.ok) << forced.message;
    EXPECT_EQ(lockOf(refreshed(serviceB, true), "hero.fbx"), LockState::Unlocked);
    EXPECT_EQ(lockOf(refreshed(serviceA, true), "hero.fbx"), LockState::Unlocked);

    // 30 s of idle fast polls ask the lock server nothing
    const size_t requestsBefore = server.requests();
    const uint64_t pollsBefore = serviceB.pollCount();
    const uint64_t lockPollsBefore = serviceB.lockPollCount();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < end) {
        serviceA.update();
        serviceB.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(server.requests(), requestsBefore);
    EXPECT_EQ(serviceB.lockPollCount(), lockPollsBefore);
    EXPECT_GE(serviceB.pollCount() - pollsBefore, 20u);
    serviceA.close();
    serviceB.close();
}
