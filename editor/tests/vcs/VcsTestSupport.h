// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Shared by the version control tests (editor/tests/vcs, docs/specs/S29-version-control.md §7):
// processes run off the main thread (R24 asserts there), the tools found on PATH, an
// environment that keeps the user's git and svn configuration out (HOME and the global config
// in the test's temp dir, no system config), and temp repositories built with them.

#include "vcs/ProcessExec.h"
#include "vcs/VcsService.h"

#include "TempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace jasperengine::editor::test {

    namespace fs = std::filesystem;
    using namespace jasperengine::editor::vcs;

    // runProcess on a thread of its own (never the main one)
    inline ProcessResult runOffMain(const ProcessSpec& spec) {
        ProcessResult result;
        std::thread thread([&]() { result = runProcess(spec); });
        thread.join();
        return result;
    }

    inline fs::path toolPath(const char* name) {
        ExecutableLookup lookup;
        return findExecutable(name, lookup);
    }

    inline void writeFile(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }

    inline std::string readFile(const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // Repositories in a temp dir with tools that read nothing of the user's
    class VcsSandbox {
    public:
        jasperengine::test::TempDir dir;
        fs::path git = toolPath("git");
        fs::path svn = toolPath("svn");
        fs::path svnadmin = toolPath("svnadmin");
        std::vector<std::pair<std::string, std::string>> env;

        VcsSandbox() {
            const fs::path home = dir.path() / "home";
            fs::create_directories(home);
            writeFile(home / ".gitconfig", "[user]\n\tname = Tester\n\temail = tester@example.com\n[init]\n\tdefaultBranch = main\n"
                                           "[commit]\n\tgpgsign = false\n[advice]\n\tdetachedHead = false\n");
            env = {{"HOME", home.string()},
                   {"USERPROFILE", home.string()},
                   {"GIT_CONFIG_GLOBAL", (home / ".gitconfig").string()},
                   {"GIT_CONFIG_NOSYSTEM", "1"},
                   {"APPDATA", (home / "AppData").string()}};
        }

        fs::path path(const std::string& relative) const { return dir.path() / relative; }

        ProcessResult run(const fs::path& exe, const fs::path& cwd, std::vector<std::string> args, std::string stdinData = {}) const {
            ProcessSpec spec;
            spec.exe = exe;
            spec.cwd = cwd;
            spec.args = std::move(args);
            spec.env = env;
            spec.stdinData = std::move(stdinData);
            spec.timeoutMs = 60000;
            return runOffMain(spec);
        }
        // git in `cwd`; fails the test (with its output) when git does
        std::string gitOk(const fs::path& cwd, std::vector<std::string> args, std::string stdinData = {}) const {
            const ProcessResult result = run(git, cwd, args, std::move(stdinData));
            std::string command;
            for (const std::string& arg : args) command += " " + arg;
            EXPECT_TRUE(result.ok()) << "git" << command << ": " << result.message();
            return result.out;
        }
        std::string svnOk(const fs::path& cwd, std::vector<std::string> args) const {
            args.insert(args.begin(), "--non-interactive");
            const ProcessResult result = run(svn, cwd, args);
            std::string command;
            for (const std::string& arg : args) command += " " + arg;
            EXPECT_TRUE(result.ok()) << "svn" << command << ": " << result.message();
            return result.out;
        }

        // An empty repository at `relative` with one commit of `files`
        fs::path gitRepo(const std::string& relative, const std::vector<std::pair<std::string, std::string>>& files = {{"README.md", "readme\n"}}) const {
            const fs::path repo = path(relative);
            fs::create_directories(repo);
            gitOk(repo, {"init", "-q"});
            for (const auto& [name, content] : files) writeFile(repo / name, content);
            gitOk(repo, {"add", "-A"});
            gitOk(repo, {"commit", "-q", "-m", "initial"});
            return repo;
        }

        ProviderConfig config(const fs::path& repoRoot, const fs::path& projectDir) const {
            ProviderConfig cfg;
            cfg.repoRoot = repoRoot;
            cfg.projectDir = projectDir;
            cfg.internalDir = projectDir / ".jasperengine" / "vcs";
            cfg.executable = git;
            cfg.extraEnv = env;
            return cfg;
        }

        VcsSettings settings() const {
            VcsSettings s;
            s.extraEnv = env;
            return s;
        }
    };

    // Pumps service.update() on this (the main) thread until pred() holds or the time is up
    inline bool pumpUntil(VcsService& service, const std::function<bool()>& pred, std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end) {
            service.update();
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        service.update();
        return pred();
    }

    // Runs an asynchronous service operation and waits for its callback
    inline VcsResult await(VcsService& service, const std::function<void(VcsService::Done)>& start,
                           std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
        bool finished = false;
        VcsResult result = VcsResult::failure("timed out waiting for the operation");
        start([&](const VcsResult& r) {
            result = r;
            finished = true;
        });
        pumpUntil(service, [&]() { return finished; }, timeout);
        return result;
    }

    // A fresh snapshot (a poll requested now, with the locks when asked)
    inline SnapshotPtr refreshed(VcsService& service, bool withLocks = false) {
        const VcsResult result = await(service, [&](VcsService::Done done) { service.refresh(withLocks, std::move(done)); });
        EXPECT_TRUE(result.ok) << result.message;
        return service.snapshot();
    }

}
