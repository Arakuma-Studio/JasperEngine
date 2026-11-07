// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The linked-folder walk of the assets, Lua and shader sources (Exporter::walkTree, S68): a folder
// it cannot list fails the export, named (export-pipeline 5.2), never a folder shipped in part.

#include "Exporter.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

namespace jasperengine::editor {

    struct ExporterTestAccess {
        static void walkTree(Exporter& exporter, const fs::path& root, const Exporter::TreeVisitor& visit, std::error_code& ec) {
            exporter.walkTree(root, visit, ec);
        }
        static fs::path treeListFailure(const Exporter& exporter) { return exporter.treeListFailure; }
        using TreeStep = Exporter::TreeStep;
    };

}

TEST(ExportTreeWalk, ASubfolderItCannotListFailsTheWalk) {
    TestTempDir dir("export-tree-walk");
    TestTempDir::write(dir / "assets/a/one.png", "png");
    TestTempDir::write(dir / "assets/b/two.png", "png");
    Exporter exporter;
    std::vector<std::string> visited;
    std::error_code ec;
    ExporterTestAccess::walkTree(exporter, dir / "assets",
        [&](const fs::directory_entry&, const fs::path& relPath, bool isDirectory, const fs::path&) {
            visited.push_back(relPath.generic_string());
            // listed, then a file put in its place before it is entered: ENOTDIR
            if (isDirectory && relPath == "a") {
                fs::remove_all(dir / "assets/a");
                TestTempDir::write(dir / "assets/a", "now a file");
            }
            return ExporterTestAccess::TreeStep::Enter;
        }, ec);
    EXPECT_TRUE(ec) << "the walk failed";
    EXPECT_EQ(ExporterTestAccess::treeListFailure(exporter).filename(), "a");
    ASSERT_FALSE(visited.empty());
    EXPECT_EQ(visited.back(), "a") << "nothing is visited after the folder it could not list";
}
