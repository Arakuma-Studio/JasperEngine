// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Shared by the jasperengine-htmlui-editor-tests files: the fixture server
// (tools/htmlui/fixture_server.py, started once for the run and per test where a test needs
// its own), temporary projects copied from fixtures/compat/net/project, file and TTF helpers.

#include "htmlui/HttpClient.h"
#include "htmlui/RemoteImporter.h"

#include "htmlui/compat/Diagnostics.h"
#include "htmlui/compat/ImportManifest.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <sys/types.h>

namespace jasperengine::editor::htmlui::test {

    namespace fs = std::filesystem;

    fs::path fixtureDir();                 // engine/tests/htmlui/fixtures/compat/net
    fs::path sourceDir();                  // the repository

    // A running fixture_server.py (killed by PID in the destructor)
    class FixtureServer {
    public:
        FixtureServer();
        ~FixtureServer();
        FixtureServer(const FixtureServer&) = delete;
        FixtureServer& operator=(const FixtureServer&) = delete;

        bool running() const { return pid > 0 && port > 0; }
        int getPort() const { return port; }
        std::string base() const;                                   // http://127.0.0.1:PORT
        // the host rewrites of phase-2 §5.4 onto this server
        std::map<std::string, std::string> rewrites() const;
        void stop();

        // GET of a test endpoint on the server (curl through HttpClient, no rewrite)
        std::string fetch(const std::string& path) const;
        // the requests the server saw ({path, ua}); reset() forgets them
        std::vector<std::pair<std::string, std::string>> requests() const;
        void reset() const;

    private:
        pid_t pid = -1;
        int port = 0;
    };

    // The server every test shares (started by the global environment)
    FixtureServer& sharedServer();

    // A copy of fixtures/compat/net/project in a fresh temporary directory, removed afterwards
    class TempProject {
    public:
        explicit TempProject(bool copyFixture = true);
        ~TempProject();
        TempProject(const TempProject&) = delete;
        TempProject& operator=(const TempProject&) = delete;

        const fs::path& root() const { return directory; }
        fs::path assets() const { return directory / "assets"; }
        ImportTarget target(const std::string& destDir = {}) const;
        void write(const std::string& projectRelative, const std::string& content) const;
        std::string read(const std::string& projectRelative) const;
        bool exists(const std::string& projectRelative) const;
        compat::ImportManifest manifest() const;

    private:
        fs::path directory;
    };

    // Environment variable set for the scope, restored afterwards
    class ScopedEnv {
    public:
        ScopedEnv(const char* name, const char* value);   // value nullptr: unset
        ~ScopedEnv();

    private:
        std::string name;
        std::string previous;
        bool hadPrevious = false;
    };

    // The process-wide host rewrite for the scope (empty map: none)
    class ScopedRewrite {
    public:
        explicit ScopedRewrite(std::map<std::string, std::string> rewrites);
        ~ScopedRewrite();

    private:
        std::map<std::string, std::string> previous;
    };

    fs::path makeTempDir(const std::string& prefix);
    std::string readFile(const fs::path& path);
    void writeFile(const fs::path& path, const std::string& content);

    // What a TrueType file says about itself
    struct TtfInfo {
        bool valid = false;
        std::vector<std::string> tables;
        int weight = 0;          // OS/2 usWeightClass
        bool italic = false;     // OS/2 fsSelection bit 0
        bool hasTable(const std::string& tag) const;
    };
    TtfInfo ttfInfo(const std::string& data);

    // diagnostics with this code
    std::vector<compat::Diagnostic> withCode(const std::vector<compat::Diagnostic>& diagnostics, const char* id);
    std::string dump(const std::vector<compat::Diagnostic>& diagnostics);

    // The references an .html declares, found the simple way the tests need until the HTML stage
    // (p2-html) collects them: <link rel=stylesheet href>, <img src/srcset>, <source src/srcset>,
    // <lottie src>, <svg src>, url() in <style> and style="" (CSS through the compat collector);
    // plus the CSS-side collection of linked local stylesheets. Installed as the discovery
    // collector while p2-html is missing (phase-2 §4 p2-importers).
    std::vector<compat::RemoteReference> minimalHtmlReferences(const std::string& htmlPath, const compat::FileAccess& files);
    bool htmlStageAvailable();

    // Runs a program, returns its exit code; output gathers stdout+stderr
    int runProgram(const std::vector<std::string>& args, std::string& output, const std::vector<std::string>& env = {});

}
