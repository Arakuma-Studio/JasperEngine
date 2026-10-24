// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Files written by today's editor load with zero issues, and saving them changes nothing but
// the added format version; a second round trip is byte-identical (docs/specs/S03-data-safety.md
// R41, AC18)

#include "support/EditorTestSupport.h"

#include "DocumentFile.h"
#include "Stream.h"
#include "StreamPreserved.h"
#include "util/Prefab.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    std::vector<fs::path> corpus(const std::vector<std::string>& extensions) {
        std::vector<fs::path> files;
        const fs::path root = fs::path(JASPER_TEST_REPO_DIR) / "tools" / "fixtures";
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            const std::string extension = entry.path().extension().string();
            if (std::find(extensions.begin(), extensions.end(), extension) != extensions.end() ||
                std::find(extensions.begin(), extensions.end(), name) != extensions.end())
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    std::string problems(const LoadReport& report) {
        std::string text;
        for (const LoadIssue& issue : report.issues) {
            if (issue.fate != LoadIssueFate::Info) text += "\n  " + issue.consoleLine();
        }
        return text;
    }

    // Where two texts first differ, a few lines of each
    std::string firstDifference(const std::string& a, const std::string& b) {
        std::istringstream ia(a), ib(b);
        std::string la, lb;
        for (int line = 1;; line++) {
            const bool ha = (bool)std::getline(ia, la);
            const bool hb = (bool)std::getline(ib, lb);
            if (!ha && !hb) return "";
            if (!ha || !hb || la != lb) {
                return "line " + std::to_string(line) + ":\n  saved:    " + (ha ? la : "<end>") + "\n  original: " + (hb ? lb : "<end>");
            }
        }
    }

    // The text without its shaderKeys block (top-level key to the next top-level key)
    std::string withoutShaderKeys(const std::string& text) {
        std::istringstream in(text);
        std::string line, out;
        bool skipping = false;
        while (std::getline(in, line)) {
            const bool topLevel = !line.empty() && line[0] != ' ' && line[0] != '-';
            if (topLevel) skipping = line.rfind("shaderKeys:", 0) == 0;
            if (!skipping) out += line + "\n";
        }
        return out;
    }

    // shaderKeys as a sorted list: the set is ordered by keys holding custom shader ids, which
    // each session assigns in its own order
    std::vector<std::string> shaderKeySet(const std::string& text) {
        std::vector<std::string> keys;
        const YAML::Node root = YAML::Load(text);
        for (const YAML::Node& key : root["shaderKeys"]) keys.push_back(YAML::Dump(key));
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    // The lines the scene 1 -> 2 migration rewrites (S10: the shadow quality Low is Auto, a
    // light's shadow bias in texels) left out of both texts
    std::string withoutShadowMigration(const std::string& text) {
        std::istringstream in(text);
        std::string line, out;
        while (std::getline(in, line)) {
            const size_t start = line.find_first_not_of(' ');
            if (start != std::string::npos && (line.compare(start, 14, "shadowQuality:") == 0 || line.compare(start, 11, "shadowBias:") == 0))
                continue;
            out += line + "\n";
        }
        return out;
    }

    // The input's lines without the format version line
    std::string withoutVersionLine(const std::string& text) {
        std::istringstream in(text);
        std::string line, out;
        while (std::getline(in, line)) {
            if (line.rfind("formatVersion:", 0) == 0) continue;
            out += line + "\n";
        }
        return out;
    }

}

TEST(Compat, CorpusRoundTrip) {
    const std::vector<fs::path> scenes = corpus({".scene"});
    ASSERT_FALSE(scenes.empty());
    for (const fs::path& file : scenes) {
        SCOPED_TRACE(file.string());
        std::printf("[ corpus   ] %s\n", file.lexically_relative(JASPER_TEST_REPO_DIR).generic_string().c_str());
        std::fflush(stdout);
        const std::string original = readText(file);
        DecodedScene decoded = decodeSceneFile(file);
        EXPECT_TRUE(decoded.report.empty()) << problems(decoded.report);

        const std::string first = encodeSceneText(decoded);
        DecodedScene again = decodeSceneText(first, file.filename().string());
        EXPECT_TRUE(again.report.empty()) << problems(again.report);
        EXPECT_EQ(encodeSceneText(again), first) << "a second round trip changed the file";

        // written by the editor (a scene saved with its build data): only the version line is new.
        // Hand-written fixtures and scenes holding prefab instances (their members come from the
        // prefab files, which this test does not load) are normalised by their first save.
        const bool editorWritten = original.find("\nmaxValues:") != std::string::npos &&
                                   original.find("prefabInstance") == std::string::npos && original.find("\n      prefab:") == std::string::npos;
        if (editorWritten) {
            const std::string difference = firstDifference(withoutShadowMigration(withoutShaderKeys(withoutVersionLine(first))),
                                                           withoutShadowMigration(withoutShaderKeys(original)));
            EXPECT_TRUE(difference.empty()) << "the save changed more than the version line, at " << difference;
            EXPECT_EQ(shaderKeySet(first), shaderKeySet(original)) << "the shader keys";
            EXPECT_EQ(first.rfind("formatVersion: 2\n", 0), 0u);
        }
    }

    for (const fs::path& file : corpus({".material"})) {
        SCOPED_TRACE(file.string());
        const LoadReport report = DocumentFiles::materialFileReport(file);
        EXPECT_TRUE(report.empty()) << problems(report);
        const YAML::Node encoded = Stream::encodeMaterial(Stream::decodeMaterial(DocumentFiles::read(file, DocumentKind::Material).root));
        const std::string first = DocumentFiles::materialText(encoded, file);
        EXPECT_EQ(first.rfind("formatVersion: 1\n", 0), 0u);
    }

    for (const fs::path& file : corpus({".prefab"})) {
        SCOPED_TRACE(file.string());
        std::printf("[ corpus   ] %s\n", file.lexically_relative(JASPER_TEST_REPO_DIR).generic_string().c_str());
        std::fflush(stdout);
        ensureEngine();
        LoadReport report;
        Prefab prefab;
        {
            ScopedLoadReport scope(report);
            prefab.loadRegistry(DocumentFiles::read(file, DocumentKind::Prefab).root, file.stem().string());
        }
        EXPECT_TRUE(report.empty()) << problems(report);
    }

    for (const fs::path& file : corpus({"project.yaml"})) {
        SCOPED_TRACE(file.string());
        LoadReport report;
        {
            ScopedLoadReport scope(report);
            StreamLoad::reportRootKeys(DocumentFiles::read(file, DocumentKind::Project).root, RootKind::Project);
        }
        EXPECT_TRUE(report.empty()) << problems(report);
    }
}
