// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/S82-title-designer.md R58, AC25: a system font's OS/2 fsType decides whether the
// picker asks before copying it into the project; every copy is one undo step into
// <assets>/TitleArt/Fonts/System/ with the system font note. Also the web font conversion and
// the free file names of the font import (R59).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "titleart/fonts/FontTestSupport.h"

#include "AppSettings.h"
#include "JasperEngine.h"
#include "Project.h"
#include "command/CommandHistory.h"
#include "titleart/fonts/FontImport.h"
#include "titleart/fonts/FontLicence.h"

#include <chrono>
#include <cstdlib>
#include <memory>

using jasperengine::Engine;
using jasperengine::editor::AppSettings;
using jasperengine::editor::CommandHistory;
using jasperengine::editor::Project;
using jasperengine::editor::UndoEntry;
using namespace jasperengine::editor::titlefonts;
using namespace jasperengine::editor::titlefonts::test;

namespace {

    void setEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    struct Headless {
        bool ready = false;
        fs::path dir;
        std::unique_ptr<Project> project;
    };

    Headless& headless() {
        static Headless h = []() {
            Headless out;
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            const fs::path root = fs::temp_directory_path() / ("jasper-title-font-import-test-" + std::to_string((long long)stamp));
            fs::remove_all(root);
            fs::create_directories(root / "config");
            setEnvironment("XDG_CONFIG_HOME", (root / "config").string());
            out.dir = root / "project";
            fs::create_directories(out.dir / ".jasperengine");
            fs::create_directories(out.dir / "assets");
            std::ofstream(out.dir / "project.yaml") << "name: titlefontimport\nassetsDir: assets\n";
            AppSettings::initialize();
            out.project.reset(new Project());
            jasperengine::editor::test::useProjectPaths(out.project.get());
            Engine::pauseGameEvents(true);
            Engine::setAsyncLoading(false);
            out.ready = out.project->loadProject(out.dir, false);
            jasperengine::editor::test::releaseAtExit(out.project.get());
            return out;
        }();
        if (h.project) jasperengine::editor::test::useProjectPaths(h.project.get());
        return h;
    }

    Project* project() { return headless().project.get(); }
    fs::path assets() { return headless().dir / "assets"; }
    CommandHistory* history() { return project()->getProjectCommandHistory(); }

    std::string topName() {
        const UndoEntry* top = history()->top();
        return top ? top->name() : std::string();
    }

    // Oswald with another fsType, as a "system" font file
    fs::path systemFontWithFsType(const ScratchDir& scratch, const std::string& name, uint16_t fsType) {
        std::vector<uint8_t> font = readBytes(oswaldPath());
        const size_t os2 = tableOffset(font, "OS/2");
        EXPECT_NE(os2, 0u);
        put16(font, os2 + 8, fsType);
        const fs::path path = scratch.path() / name;
        writeBytes(path, font);
        return path;
    }

    FontCopyRequest systemCopy(const fs::path& source) {
        FontCopyRequest request;
        request.source = source;
        request.assetsDir = assets();
        request.folder = SYSTEM_FONTS_FOLDER;
        request.from = FontImportSource::System;
        return request;
    }

    class FontLicenceTest : public ::testing::Test {
    protected:
        void SetUp() override { ASSERT_TRUE(headless().ready) << "the headless project did not open"; }
    };

}

// Red control: classifying by `fsType == 0x0002` only, or by the most restrictive bit, fails the
// mixed-bit rows; skipping the question (planning Restricted like the others) fails the import half
TEST_F(FontLicenceTest, FsType) {
    struct Row {
        uint16_t fsType;
        EmbeddingLicence expected;
    };
    for (const Row& row : {Row{0x0000, EmbeddingLicence::Installable}, Row{0x0002, EmbeddingLicence::Restricted},
                           Row{0x0004, EmbeddingLicence::PreviewAndPrint}, Row{0x0008, EmbeddingLicence::Editable},
                           Row{0x0006, EmbeddingLicence::PreviewAndPrint}, Row{0x000A, EmbeddingLicence::Editable},
                           Row{0x0102, EmbeddingLicence::Restricted}, Row{0x0200, EmbeddingLicence::Installable}}) {
        const FontLicenceInfo info = classifyFsType(row.fsType);
        EXPECT_EQ(info.embedding, row.expected) << std::hex << row.fsType;
        EXPECT_EQ(info.asksBeforeCopy(), row.expected == EmbeddingLicence::Restricted) << std::hex << row.fsType;
    }
    EXPECT_TRUE(classifyFsType(0x0102).noSubsetting);
    EXPECT_TRUE(classifyFsType(0x0200).bitmapOnly);
    EXPECT_STREQ(embeddingLicenceLabel(EmbeddingLicence::PreviewAndPrint), "Preview & Print");
    EXPECT_STREQ(systemFontLicenceNote(), "System font: check its licence before you ship a game with it");
    EXPECT_STREQ(restrictedLicenceConfirm(), "Copy Anyway");
    EXPECT_NE(std::string(restrictedLicenceQuestion()).find("does not allow embedding it in a game"), std::string::npos);

    // read from the font: OS/2 fsType of the face
    ScratchDir scratch("licence");
    const fs::path restricted = systemFontWithFsType(scratch, "OswaldRestricted.ttf", 0x0002);
    const std::vector<uint8_t> bytes = readBytes(restricted);
    EXPECT_EQ(readFontLicence(bytes.data(), bytes.size(), 0).embedding, EmbeddingLicence::Restricted);

    // Restricted: asks, writes nothing, until the request says Copy Anyway
    const size_t position = history()->position();
    FontImportPlan plan = planFontCopy(systemCopy(restricted));
    EXPECT_FALSE(plan.ok);
    EXPECT_TRUE(plan.needsLicenceConfirmation);
    EXPECT_EQ(plan.error, restrictedLicenceQuestion());
    EXPECT_TRUE(plan.items.empty());
    std::string error;
    EXPECT_FALSE(commitFontImport(project(), plan, error));
    EXPECT_EQ(history()->position(), position);
    EXPECT_FALSE(fs::exists(assets() / SYSTEM_FONTS_FOLDER / "OswaldRestricted.ttf"));

    FontCopyRequest confirmed = systemCopy(restricted);
    confirmed.licenceConfirmed = true;
    plan = planFontCopy(confirmed);
    ASSERT_TRUE(plan.ok) << plan.error;
    EXPECT_EQ(plan.licenceNote, systemFontLicenceNote());
    ASSERT_TRUE(commitFontImport(project(), plan, error)) << error;
    EXPECT_EQ(history()->position(), position + 1) << "one step";
    EXPECT_EQ(topName(), "Import Font Oswald");
    EXPECT_EQ(plan.assetPath, "TitleArt/Fonts/System/OswaldRestricted.ttf");
    EXPECT_EQ(readBytes(assets() / plan.assetPath), bytes);

    // Installable, Preview & Print and Editable: copied at once, each with the note, one step each
    struct Copied {
        const char* file;
        uint16_t fsType;
    };
    for (const Copied& copied : {Copied{"OswaldInstallable.ttf", 0x0000}, Copied{"OswaldPreview.ttf", 0x0004}, Copied{"OswaldEditable.ttf", 0x0008}}) {
        const fs::path source = systemFontWithFsType(scratch, copied.file, copied.fsType);
        const size_t before = history()->position();
        const FontImportPlan direct = planFontCopy(systemCopy(source));
        ASSERT_TRUE(direct.ok) << copied.file << ": " << direct.error;
        EXPECT_FALSE(direct.needsLicenceConfirmation);
        EXPECT_EQ(direct.licenceNote, systemFontLicenceNote());
        ASSERT_TRUE(commitFontImport(project(), direct, error)) << error;
        EXPECT_EQ(history()->position(), before + 1) << copied.file;
        EXPECT_TRUE(fs::is_regular_file(assets() / SYSTEM_FONTS_FOLDER / copied.file)) << copied.file;
    }

    // undo removes the copy
    ASSERT_TRUE(history()->undo());
    EXPECT_FALSE(fs::exists(assets() / SYSTEM_FONTS_FOLDER / "OswaldEditable.ttf"));
}

// Red control: always writing under the source's name replaces the project's other font
TEST_F(FontLicenceTest, ImportsTakeFreeNamesAndReuseTheSameFile) {
    ScratchDir scratch("names");
    const fs::path first = systemFontWithFsType(scratch, "a/Brand.ttf", 0x0000);
    const fs::path second = systemFontWithFsType(scratch, "b/Brand.ttf", 0x0008);
    FontCopyRequest request = systemCopy(first);
    request.folder = "TitleArt/Fonts/Names";
    std::string error;
    FontImportPlan plan = planFontCopy(request);
    ASSERT_TRUE(plan.ok) << plan.error;
    ASSERT_TRUE(commitFontImport(project(), plan, error)) << error;
    EXPECT_EQ(plan.assetPath, "TitleArt/Fonts/Names/Brand.ttf");

    // the same bytes again: the file that is there, nothing written, no step
    const size_t position = history()->position();
    plan = planFontCopy(request);
    ASSERT_TRUE(plan.ok) << plan.error;
    EXPECT_TRUE(plan.alreadyPresent);
    EXPECT_EQ(plan.assetPath, "TitleArt/Fonts/Names/Brand.ttf");
    ASSERT_TRUE(commitFontImport(project(), plan, error)) << error;
    EXPECT_EQ(history()->position(), position);

    // other bytes under the same name: "Brand 1.ttf"
    request.source = second;
    plan = planFontCopy(request);
    ASSERT_TRUE(plan.ok) << plan.error;
    EXPECT_EQ(plan.assetPath, "TitleArt/Fonts/Names/Brand 1.ttf");
}

// Red control: copying the .woff2 as it is (no decode) leaves a file Title Art cannot load and
// fails the TrueType check
TEST_F(FontLicenceTest, WebFontConversionWritesTheSfntAsOneStep) {
    WebFontConversionRequest request;
    request.source = webFontFixturesDir() / "Oswald-wght.woff2";
    request.assetsDir = assets();
    request.folder = FONTS_FOLDER;
    const FontImportPlan plan = planWebFontConversion(request);
    ASSERT_TRUE(plan.ok) << plan.error;
    EXPECT_EQ(plan.assetPath, "TitleArt/Fonts/Oswald-wght.ttf");
    EXPECT_EQ(plan.family, "Oswald");
    const size_t position = history()->position();
    std::string error;
    ASSERT_TRUE(commitFontImport(project(), plan, error)) << error;
    EXPECT_EQ(history()->position(), position + 1);
    EXPECT_EQ(topName(), "Import Font Oswald");
    const std::vector<uint8_t> written = readBytes(assets() / plan.assetPath);
    ASSERT_GE(written.size(), 4u);
    EXPECT_EQ(be32(written, 0), 0x00010000u) << "a TrueType sfnt";
    EXPECT_FALSE(fs::exists(assets() / FONTS_FOLDER / "Oswald-wght.woff2")) << "the web font itself is not copied";

    // a damaged web font is refused with the decoder's reason
    ScratchDir scratch("convert");
    std::vector<uint8_t> cut = readBytes(webFontFixturesDir() / "Oswald-wght.woff");
    cut.resize(cut.size() / 2);
    writeBytes(scratch.path() / "cut.woff", cut);
    request.source = scratch.path() / "cut.woff";
    const FontImportPlan refused = planWebFontConversion(request);
    EXPECT_FALSE(refused.ok);
    EXPECT_NE(refused.error.find("truncated"), std::string::npos) << refused.error;
}
