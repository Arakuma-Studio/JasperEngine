// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Helpers of the phase-4 inspector and assertion tests (InspectorTest.cpp, AssertionsTest.cpp):
// each test writes its .html/.css into its own directory, which becomes the assets directory
// (paths in results are asset-relative, "ui/menu.css"), and loads them in a 1280x720 canvas.

#ifndef HTMLUI_TEST_INSPECTOR_COMMON_H
#define HTMLUI_TEST_INSPECTOR_COMMON_H

#include "harness/GpuEnvironment.h"
#include "harness/SceneFixture.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "Log.h"
#include "Scene.h"
#include "htmlui/HtmlUIAssertions.h"
#include "htmlui/HtmlUIInspector.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/compat/ImportManifest.h"
#include "subsystem/UIDocumentSystem.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace jasperengine { namespace test {

    class InspectorTestBase : public ::testing::Test {
    protected:
        std::filesystem::path dir;

        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            ASSERT_EQ(HtmlUI::getReferenceCount(), 0) << "an earlier test left RmlUi initialised";
            GpuEnvironment::setView(1280, 720, 1280, 720, Scaling::FITWIDTH);
            const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
            dir = std::filesystem::path(JASPER_HTMLUI_TEST_BINARY_DIR) / "p4-inspector" / info->test_suite_name() / info->name();
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir / "ui");
            TestSystem::get().setAssetPath(dir.generic_string());
            htmlui::compat::ImportManifest::reload();
            htmlui::compat::DocumentLoader::clearCache();
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            htmlui::compat::DocumentLoader::clearCache();
            TestSystem::get().setAssetPath("");
            htmlui::compat::ImportManifest::reload();
            GpuEnvironment::setView(640, 360, 640, 360, Scaling::FITWIDTH);
            EXPECT_EQ(HtmlUI::getReferenceCount(), 0);
            std::filesystem::remove_all(dir);
        }

        // a new content and a later modification time, whatever the file system's resolution
        void write(const std::string& name, const std::string& content){
            const std::filesystem::path file = dir / name;
            std::error_code error;
            std::filesystem::create_directories(file.parent_path(), error);
            const bool existed = std::filesystem::exists(file, error);
            const std::filesystem::file_time_type before = existed ? std::filesystem::last_write_time(file, error) : std::filesystem::file_time_type();
            {
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out << content;
            }
            if (existed)
                std::filesystem::last_write_time(file, before + std::chrono::seconds(2), error);
        }

        // ui/<name>.html linking ui/<name>.css (when css is not empty) around `body`
        void page(const std::string& name, const std::string& css, const std::string& body){
            std::string html = "<!DOCTYPE html>\n<html>\n<head>\n";
            if (!css.empty())
                html += "<link rel=\"stylesheet\" href=\"" + name + ".css\">\n";
            html += "</head>\n<body>\n" + body + "\n</body>\n</html>\n";
            write("ui/" + name + ".html", html);
            if (!css.empty())
                write("ui/" + name + ".css", css);
        }

        static std::string logText(int type){
            std::string text;
            for (const auto& line : TestSystem::get().getLogs()){
                if (line.type == type)
                    text += line.message + "\n";
            }
            return text;
        }
    };

    // the element of a query result by ref (nullptr when absent)
    inline const htmlui::InspectElement* findRef(const std::vector<htmlui::InspectElement>& elements, const std::string& ref){
        for (const htmlui::InspectElement& element : elements){
            if (element.ref == ref)
                return &element;
        }
        return nullptr;
    }

} }

#define EXPECT_RECT(rect, ex, ey, ew, eh) \
    do { \
        const ::jasperengine::htmlui::InspectRect rect_ = (rect); \
        EXPECT_NEAR(rect_.x, (ex), 0.01f); \
        EXPECT_NEAR(rect_.y, (ey), 0.01f); \
        EXPECT_NEAR(rect_.width, (ew), 0.01f); \
        EXPECT_NEAR(rect_.height, (eh), 0.01f); \
    } while (0)

#endif //HTMLUI_TEST_INSPECTOR_COMMON_H
