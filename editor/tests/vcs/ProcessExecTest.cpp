// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// ProcessExec (docs/specs/S29-version-control.md R2, R3, AC1-AC3). The cases of OpenEngine's
// VCSCommandExecutorTests (Tests/Editor/VCSCommandExecutorTests.cpp, MIT, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors; NOTICE.md) are ported: verbatim arguments, output
// written just before exit, environment overrides, the inherited environment.

#include "VcsTestSupport.h"

#include "jobs/JobSystem.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <sys/stat.h>
#endif

using namespace jasperengine;
using namespace jasperengine::editor::test;

namespace {

    fs::path helper() {
        return fs::path(JASPER_TEST_VCS_PROCESS_HELPER);
    }

    std::vector<std::string> nulFields(const std::string& out) {
        std::vector<std::string> fields;
        size_t start = 0;
        while (start < out.size()) {
            const size_t end = out.find('\0', start);
            if (end == std::string::npos) break;
            fields.push_back(out.substr(start, end - start));
            start = end + 1;
        }
        return fields;
    }

    ProcessSpec helperSpec(std::vector<std::string> args) {
        ProcessSpec spec;
        spec.exe = helper();
        spec.args = std::move(args);
        spec.timeoutMs = 20000;
        return spec;
    }

    bool processGone(long long pid) {
#ifdef _WIN32
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
        if (!process) return true;
        const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        CloseHandle(process);
        return exited;
#else
        if (kill(static_cast<pid_t>(pid), 0) != 0) return errno == ESRCH;
#if defined(__linux__)
        // killed, but not reaped yet by whoever adopted it (a container's init): a zombie runs nothing
        std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        if (!std::getline(stat, line)) return true;
        const size_t close = line.rfind(')');
        return close != std::string::npos && close + 2 < line.size() && line[close + 2] == 'Z';
#else
        return false;
#endif
#endif
    }

    // CommandLineToArgvW's rules (and the C runtime's): what a Windows child reads back
    std::vector<std::string> parseWindowsCommandLine(const std::string& line) {
        std::vector<std::string> args;
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
            if (i >= line.size()) break;
            std::string arg;
            bool quoted = false;
            while (i < line.size()) {
                size_t backslashes = 0;
                while (i < line.size() && line[i] == '\\') {
                    backslashes++;
                    i++;
                }
                if (i < line.size() && line[i] == '"') {
                    arg.append(backslashes / 2, '\\');
                    if (backslashes % 2 == 1) {
                        arg.push_back('"');
                        i++;
                        continue;
                    }
                    if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                        arg.push_back('"');
                        i += 2;
                        continue;
                    }
                    quoted = !quoted;
                    i++;
                    continue;
                }
                arg.append(backslashes, '\\');
                if (i >= line.size()) break;
                if (!quoted && (line[i] == ' ' || line[i] == '\t')) break;
                arg.push_back(line[i]);
                i++;
            }
            args.push_back(arg);
        }
        return args;
    }

    const std::vector<std::string> kTrickyArgs = {
        "a b", "\"q\"", "$HOME", "`x`", ";rm -rf ~", "-n", "trailing\\", "\xC3\xB1", "", "back\\\"slash", "tab\there", "a\\\\b c\\\\"};

}

TEST(ProcessExec, ArgumentsReachChildVerbatim) {
    std::vector<std::string> args = {"args"};
    args.insert(args.end(), kTrickyArgs.begin(), kTrickyArgs.end());
    const ProcessResult result = runOffMain(helperSpec(args));
    ASSERT_TRUE(result.ok()) << result.message();
    const std::vector<std::string> echoed = nulFields(result.out);
    ASSERT_EQ(echoed.size(), kTrickyArgs.size());
    for (size_t i = 0; i < kTrickyArgs.size(); i++) {
        EXPECT_EQ(echoed[i], kTrickyArgs[i]) << "argument " << i;
    }
}

TEST(ProcessExec, WindowsQuotingRoundTrips) {
    // what the Windows build puts on the command line parses back to the same arguments
    std::string line = "\"C:\\Program Files\\Git\\cmd\\git.exe\"";
    for (const std::string& arg : kTrickyArgs) line += " " + quoteWindowsArgument(arg);
    const std::vector<std::string> parsed = parseWindowsCommandLine(line);
    ASSERT_EQ(parsed.size(), kTrickyArgs.size() + 1);
    for (size_t i = 0; i < kTrickyArgs.size(); i++) {
        EXPECT_EQ(parsed[i + 1], kTrickyArgs[i]) << "argument " << i;
    }
    EXPECT_EQ(quoteWindowsArgument("plain"), "plain");
    EXPECT_EQ(quoteWindowsArgument(""), "\"\"");
}

TEST(ProcessExec, OutputWrittenJustBeforeExitIsCaptured) {
    const ProcessResult result = runOffMain(helperSpec({"late", "300000"}));
    ASSERT_TRUE(result.ok()) << result.message();
    EXPECT_EQ(result.out.size(), 300000u);
    EXPECT_EQ(result.out.find_first_not_of('L'), std::string::npos);
}

TEST(ProcessExec, EnvironmentOverridesReachTheChild) {
    ProcessSpec spec = helperSpec({"env", "JE_VCS_TEST_A", "JE_VCS_TEST_B", "PATH"});
    spec.env = {{"JE_VCS_TEST_A", "one two"}, {"JE_VCS_TEST_B", ""}};
    spec.unsetEnv = {"PATH"};
    const ProcessResult result = runOffMain(spec);
    ASSERT_TRUE(result.ok()) << result.message();
    const std::vector<std::string> values = nulFields(result.out);
    ASSERT_EQ(values.size(), 3u);
    EXPECT_EQ(values[0], "one two");
    EXPECT_EQ(values[1], "");
    EXPECT_EQ(values[2], "<unset>");
}

TEST(ProcessExec, NoOverridesLeaveTheInheritedEnvironment) {
    const char* path = std::getenv("PATH");
    ASSERT_NE(path, nullptr);
    const ProcessResult result = runOffMain(helperSpec({"env", "PATH"}));
    ASSERT_TRUE(result.ok()) << result.message();
    const std::vector<std::string> values = nulFields(result.out);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0], path);
}

TEST(ProcessExec, ExitCodeAndStderrAreKept) {
    const ProcessResult result = runOffMain(helperSpec({"exit", "3"}));
    EXPECT_FALSE(result.spawnFailed);
    EXPECT_EQ(result.exitCode, 3);
    EXPECT_FALSE(result.ok());
}

TEST(ProcessExec, TimeoutKillsTree) {
    jasperengine::test::TempDir dir;
    const fs::path pidFile = dir.path() / "pids.txt";
    ProcessSpec spec = helperSpec({"tree", pidFile.string()});
    spec.timeoutMs = 500;
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult result = runOffMain(spec);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_TRUE(result.timedOut);
    EXPECT_FALSE(result.ok());
    long long child = -1, grandchild = -1;
    {
        std::ifstream in(pidFile);
        in >> child >> grandchild;
    }
    ASSERT_GT(child, 0);
    ASSERT_GT(grandchild, 0);
    // both are gone within 3 s of the start (R2: SIGTERM to the group, SIGKILL after 2 s)
    const auto deadline = start + std::chrono::seconds(3);
    while ((!processGone(child) || !processGone(grandchild)) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(processGone(child)) << "child " << child;
    EXPECT_TRUE(processGone(grandchild)) << "grandchild " << grandchild;
    EXPECT_LT(seconds, 3.0);
}

TEST(ProcessExec, CancelStopsTheChild) {
    jobs::CancelToken cancel;
    ProcessSpec spec = helperSpec({"sleep", "20000"});
    spec.cancel = &cancel;
    std::thread canceller([&cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cancel.cancel();
    });
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult result = runOffMain(spec);
    canceller.join();
    EXPECT_TRUE(result.cancelled);
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 3.0);
}

TEST(ProcessExec, StdinIsNull) {
    ProcessSpec spec = helperSpec({"stdin"});
    spec.timeoutMs = 5000;
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult result = runOffMain(spec);
    ASSERT_TRUE(result.ok()) << result.message();
    EXPECT_EQ(result.out, "0");
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 2.0);
}

TEST(ProcessExec, StdinDataReachesTheChild) {
    ProcessSpec spec = helperSpec({"stdin"});
    spec.stdinData = std::string(3 * 1024 * 1024 + 7, 'i');
    const ProcessResult result = runOffMain(spec);
    ASSERT_TRUE(result.ok()) << result.message();
    EXPECT_EQ(result.out, std::to_string(spec.stdinData.size()));
}

TEST(ProcessExec, OutputCap) {
    const size_t total = size_t(100) << 20;
    ProcessSpec spec = helperSpec({"flood", std::to_string(total)});
    spec.timeoutMs = 60000;
    const ProcessResult result = runOffMain(spec);
    EXPECT_FALSE(result.timedOut);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_TRUE(result.truncated);
    EXPECT_EQ(result.out.size(), spec.maxOutputBytes);
    // what is held stays within the cap and a megabyte
    EXPECT_LE(result.out.capacity() + result.err.capacity(), spec.maxOutputBytes + (size_t(1) << 20));
}

TEST(ProcessExec, RelativeExecutableIsRefused) {
    ProcessSpec spec;
    spec.exe = "git";
    const uint64_t before = spawnCount(false) + spawnCount(true);
    const ProcessResult result = runOffMain(spec);
    EXPECT_TRUE(result.spawnFailed);
    EXPECT_EQ(spawnCount(false) + spawnCount(true), before);
}

TEST(ProcessExec, SpawnsAreCountedByThread) {
    const uint64_t main = spawnCount(true);
    const uint64_t other = spawnCount(false);
    runOffMain(helperSpec({"exit", "0"}));
    EXPECT_EQ(spawnCount(true), main);
    EXPECT_EQ(spawnCount(false), other + 1);
}

TEST(ProcessExec, NoProjectDirHijack) {
    jasperengine::test::TempDir dir;
    const fs::path project = dir.path() / "project";
    const fs::path marker = dir.path() / "marker.txt";
#ifdef _WIN32
    const std::string exeName = "git.exe";
#else
    const std::string exeName = "git";
#endif
    // a `git` in the project, below it, and in folders PATH names relatively
    for (const fs::path& at : {project, project / "tools", dir.path() / "rel"}) {
        fs::create_directories(at);
        fs::copy_file(helper(), at / exeName, fs::copy_options::overwrite_existing);
#ifndef _WIN32
        fs::permissions(at / exeName, fs::perms::owner_all, fs::perm_options::replace);
#endif
    }
    const char* realPath = std::getenv("PATH");
    ASSERT_NE(realPath, nullptr);
#ifdef _WIN32
    const char separator = ';';
#else
    const char separator = ':';
#endif
    ExecutableLookup lookup;
    lookup.projectDir = project;
    lookup.usePathOverride = true;
    lookup.pathOverride = std::string("rel") + separator + "." + separator + project.string() + separator + (project / "tools").string() +
                          separator + "" + separator + realPath;
    const fs::path found = findExecutable("git", lookup);
    ASSERT_FALSE(found.empty()) << "git must be on PATH (the build image has it)";
    EXPECT_TRUE(found.is_absolute());
    const fs::path relative = found.lexically_relative(project);
    EXPECT_TRUE(relative.empty() || *relative.begin() == "..") << found;
    EXPECT_NE(found.parent_path().filename(), "rel");

    // running what was found from the project folder never runs a planted copy
    ProcessSpec spec;
    spec.exe = found;
    spec.args = {"--version"};
    spec.cwd = project;
    spec.env = {{"JE_VCS_HELPER_MARKER", marker.string()},
                {"PATH", sanitizedPath(lookup.pathOverride, project)}};
    const ProcessResult result = runOffMain(spec);
    EXPECT_TRUE(result.ok()) << result.message();
    EXPECT_FALSE(fs::exists(marker));

    // with only relative and project entries there is nothing to run
    ExecutableLookup onlyPlanted = lookup;
    onlyPlanted.pathOverride = std::string("rel") + separator + project.string();
    onlyPlanted.probeMacLocations = false;
    std::string why;
    EXPECT_TRUE(findExecutable("git", onlyPlanted, &why).empty());
    EXPECT_FALSE(why.empty());
    EXPECT_EQ(sanitizedPath(onlyPlanted.pathOverride, project), "");
}

TEST(ProcessExec, ConfiguredExecutableWins) {
    ExecutableLookup lookup;
    lookup.configured = helper();
    EXPECT_EQ(findExecutable("git", lookup), helper().lexically_normal());
    lookup.configured = fs::relative(helper(), fs::current_path());
    std::string why;
    EXPECT_TRUE(findExecutable("git", lookup, &why).empty());
    EXPECT_NE(why.find("not absolute"), std::string::npos);
}

TEST(ProcessExec, VersionAtLeast) {
    std::string parsed;
    EXPECT_TRUE(versionAtLeast("git version 2.34.1\n", "2.30", &parsed));
    EXPECT_EQ(parsed, "2.34.1");
    EXPECT_TRUE(versionAtLeast("git version 2.43.0.windows.1", "2.30"));
    EXPECT_FALSE(versionAtLeast("git version 2.25.1", "2.30"));
    EXPECT_TRUE(versionAtLeast("git-lfs/3.0.2 (GitHub; linux amd64; go 1.18.1)", "3.0", &parsed));
    EXPECT_EQ(parsed, "3.0.2");
    EXPECT_FALSE(versionAtLeast("git-lfs/2.13.3 (GitHub; linux amd64)", "3.0"));
    EXPECT_TRUE(versionAtLeast("1.14.1\n", "1.10"));
    EXPECT_FALSE(versionAtLeast("1.9.7", "1.10"));
    EXPECT_FALSE(versionAtLeast("no version here", "1.0"));
}
