// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The settings registry (S25 R29, R30; AC21, AC22): every MCP key from before S25 is a wired row
// with a label and a page, an unwired row is refused, and the row-level search filters pages
// and rows.

#include "settings/SettingsRegistry.h"
#include "settings/SettingsSearch.h"
#include "AppSettings.h"
#include "LogCapture.h"

#include "support/EditorTestSupport.h"

#include <gtest/gtest.h>

#include <set>
#include <sstream>

using namespace jasperengine::editor;
using namespace jasperengine::editor::test;

namespace {

    std::vector<std::string> fixtureKeys() {
        std::istringstream in(readText(fixture("settings/mcp-keys-before-s25.txt")));
        std::vector<std::string> keys;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line[0] != '#') keys.push_back(line);
        }
        return keys;
    }

}

TEST(SettingsRegistry, CoversMcpKeys) {
    const SettingsRegistry& registry = SettingsRegistry::get();
    const std::vector<std::string> keys = fixtureKeys();
    ASSERT_GE(keys.size(), 100u);
    for (const std::string& key : keys) {
        const SettingRow* row = registry.find(key);
        ASSERT_NE(row, nullptr) << key;
        EXPECT_TRUE(row->mcp) << key;
        EXPECT_FALSE(row->label.empty()) << key;
        EXPECT_FALSE(row->type.empty()) << key;
        EXPECT_FALSE(row->description.empty()) << key;
        EXPECT_TRUE(row->get) << key;
        EXPECT_TRUE(row->set) << key;
        const SettingsPage* page = registry.findPage(row->page);
        ASSERT_NE(page, nullptr) << key << " on page " << row->page;
        EXPECT_EQ(page->scope, row->scope) << key;
        EXPECT_EQ(row->scope == SettingsScope::Preference, key.starts_with("preferences.")) << key;
    }
    // every row is wired, every page has rows
    std::set<std::string> pagesWithRows;
    for (const SettingRow& row : registry.rows()) {
        EXPECT_FALSE(row.label.empty()) << row.key;
        EXPECT_TRUE(row.get || row.draw || !row.mcp) << row.key;
        pagesWithRows.insert(row.page);
    }
    for (SettingsScope scope : {SettingsScope::Project, SettingsScope::Preference}) {
        for (const SettingsPage* page : registry.pages(scope)) {
            EXPECT_TRUE(pagesWithRows.count(page->name)) << page->name << " has no rows";
        }
    }
    const SettingRow* gravity = registry.find("physics.gravity");
    if (gravity) {
        EXPECT_EQ(registry.location(*gravity).rfind("Project Settings > ", 0), 0u);
    }
}

TEST(SettingsRegistry, PixelArtHighlightIsOneWritablePreference) {
    jasperengine::test::LogCapture log;
    SettingsRegistry registry;
    registerBuiltinSettings(registry);
    EXPECT_TRUE(log.errors().empty()) << log.describeErrors();

    const char* key = "preferences.pixel_art.agent_edit_highlight";
    const SettingRow* row = registry.find(key);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->scope, SettingsScope::Preference);
    EXPECT_EQ(row->page, "Pixel Art (Preferences)");
    EXPECT_EQ(row->label, "Highlight Agent Edits");
    EXPECT_EQ(row->type, "bool");
    EXPECT_TRUE(row->mcp);
    EXPECT_FALSE(row->userOnly);
    size_t matching = 0;
    for (const SettingRow& candidate : registry.rows()) matching += candidate.key == key;
    EXPECT_EQ(matching, 1u);
    ASSERT_TRUE(row->get);
    ASSERT_TRUE(row->set);
    SettingsContext context;
    const bool original = AppSettings::getPixelArtPreferences().agentEditHighlight;
    std::string error;
    EXPECT_TRUE(row->set(context, !original, false, error)) << error;
    EXPECT_EQ(row->get(context), original) << "validation must leave the preference alone";
    EXPECT_TRUE(row->set(context, !original, true, error)) << error;
    EXPECT_EQ(row->get(context), !original);
    EXPECT_TRUE(row->set(context, original, true, error)) << error;
    EXPECT_EQ(row->get(context), original);

    // EditorSettingsWindow dispatches its PreferencesCategory enum by this page order.
    std::vector<std::string> pages;
    for (const SettingsPage* page : registry.pages(SettingsScope::Preference)) pages.push_back(page->displayName());
    EXPECT_EQ(pages, (std::vector<std::string>{"General", "Code Editor", "Asset Pipeline", "Scripting", "Undo", "Lattice", "Scene View",
                                             "External Tools", "Logging", "Pixel Art", "Diagnostics", "Shader Cache", "Version Control"}));
}

TEST(SettingsRegistry, RefusesUnwiredRows) {
    SettingsRegistry registry;
    std::string why;
    ASSERT_TRUE(registry.addPage({"Test", SettingsScope::Project, 0, "", ""}, &why)) << why;
    EXPECT_FALSE(registry.addPage({"Test", SettingsScope::Project, 1, "", ""}, &why));   // a duplicate
    EXPECT_FALSE(registry.addPage({"", SettingsScope::Project, 1, "", ""}, &why));

    SettingRow unwired;
    unwired.key = "test.unwired";
    unwired.page = "Test";
    unwired.label = "Unwired";
    EXPECT_FALSE(registry.addRow(unwired, &why));   // neither get nor draw
    EXPECT_FALSE(why.empty());

    SettingRow wired = unwired;
    wired.key = "test.wired";
    wired.get = [](SettingsContext&) { return jasperengine::editor::mcp::Json(true); };
    SettingRow noLabel = wired;
    noLabel.label.clear();
    EXPECT_FALSE(registry.addRow(noLabel, &why));
    SettingRow noPage = wired;
    noPage.page = "Nowhere";
    EXPECT_FALSE(registry.addRow(noPage, &why));
    SettingRow mcpWithoutSet = wired;
    mcpWithoutSet.mcp = true;
    EXPECT_FALSE(registry.addRow(mcpWithoutSet, &why));
    EXPECT_TRUE(registry.addRow(wired, &why)) << why;
    EXPECT_FALSE(registry.addRow(wired, &why));     // the same key twice
    EXPECT_EQ(registry.rows().size(), 1u);
}

TEST(SettingsSearch, FiltersRows) {
    const SettingsRegistry& registry = SettingsRegistry::get();
    std::set<std::string> pages;
    std::vector<const SettingRow*> rows = registry.search("vsync");
    ASSERT_FALSE(rows.empty());
    for (const SettingRow* row : rows) {
        pages.insert(row->page);
        EXPECT_NE(row->label.find("VSync"), std::string::npos) << row->key << ": " << row->label << " on " << row->page;
    }
    // Quality (Project Settings) and General (Preferences)
    ASSERT_EQ(pages.size(), 2u);
    const std::vector<const SettingsPage*> projectPages = registry.pages(SettingsScope::Project);
    const std::vector<const SettingsPage*> preferencePages = registry.pages(SettingsScope::Preference);
    std::vector<std::string> matching;
    const std::string query = "vsync";
    for (const auto* list : {&projectPages, &preferencePages}) {
        for (const SettingsPage* page : *list) {
            if (registry.pageMatches(*page, query)) matching.push_back(page->displayName());
        }
    }
    EXPECT_EQ(matching, (std::vector<std::string>{"Quality", "General"}));

    // on a page, only the matching rows are drawn; with no search, every row
    settingsui::beginPage("Quality", "vsync");
    EXPECT_TRUE(settingsui::searching());
    EXPECT_TRUE(settingsui::beginRow("quality.vsync"));
    EXPECT_FALSE(settingsui::beginRow("quality.atmosphere_quality_desktop"));
    EXPECT_TRUE(settingsui::beginRow({"quality.atmosphere_quality_desktop", "quality.vsync"}));
    settingsui::endPage();
    settingsui::beginPage("Quality", "");
    EXPECT_FALSE(settingsui::searching());
    EXPECT_TRUE(settingsui::beginRow("quality.atmosphere_quality_desktop"));
    settingsui::endPage();
    // a query naming the page itself shows all of it
    settingsui::beginPage("Quality", "quality");
    EXPECT_TRUE(settingsui::beginRow("quality.atmosphere_quality_desktop"));
    settingsui::endPage();
    // typos are forgiven in Filter mode, bare subsequences are not
    EXPECT_FALSE(registry.search("vsnyc").empty());
    EXPECT_TRUE(registry.search("qzx").empty());
}
