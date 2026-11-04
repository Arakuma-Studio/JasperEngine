// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Every setting of Preferences and Project Settings, one row each (S25 R29-R30), lifted from
// the MCP settings table (get_project_settings / set_project_settings). A row keeps the MCP
// key, type, get and set it had, and gains its page, section, label, tooltip and search
// keywords. The two windows list their pages from here; their search (Filter mode over label,
// keywords and key) shows the pages with a matching row and, on a page, only those rows
// (settingsui::beginRow); the Search palette's ":" scope lists the rows. A row is wired: it has
// a get (its value) or a draw; registering one without a label, or with neither, is refused
// (OpenEngine's EditorSettingsRegistry::RegisterCategory rule, Apps/Editor/Source/Editor/
// Settings/EditorSettingsRegistry.cpp, MIT, Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors; NOTICE.md).

#include "mcp/McpJson.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::editor {

    class Project;
    class ConsoleWindow;

    // What a row's get and set read and write
    struct SettingsContext {
        Project* projectPtr = nullptr;
        ConsoleWindow* consoleWindowPtr = nullptr;
        bool playing = false;
        // what a setting did beyond its value (MCP set_project_settings: the asset roots' relocation
        // under "relocation"); null when nobody reads it
        mcp::Json* report = nullptr;

        Project* project() const { return projectPtr; }
        ConsoleWindow* consoleWindow() const { return consoleWindowPtr; }
        bool isPlaying() const { return playing; }
    };

    enum class SettingsScope { Project, Preference };

    struct SettingsPage {
        std::string name;          // unique: "Physics", "Code Editor", "Scripting (Preferences)"
        SettingsScope scope = SettingsScope::Project;
        int order = 0;
        std::string keywords;      // more words that find the page itself
        std::string title;         // as the window lists it ("Scripting"); the name when empty

        const std::string& displayName() const { return title.empty() ? name : title; }
    };

    struct SettingRow {
        std::string key;           // "physics2d.enabled" (an MCP key), or the window's own row key
        SettingsScope scope = SettingsScope::Project;
        std::string page;          // the page's name
        std::string section;       // a foldout or sub-heading on the page ("" at its top)
        std::string label;         // as the window shows it
        std::string tooltip;
        std::string keywords;
        // MCP: get_project_settings lists the row (with type, description, values)
        bool mcp = false;
        std::string type;          // bool, int, number, string, enum, path, list, object, scene
        std::string description;
        std::vector<std::string> values;
        std::function<mcp::Json(SettingsContext&)> get;
        // Validates `value`, applies it when apply is true; false with error
        std::function<bool(SettingsContext&, const mcp::Json& value, bool apply, std::string& error)> set;
        // custom drawing of a row the windows do not draw themselves
        std::function<void()> draw;
        // A machine-level preference naming a program to run or a folder outside the project: only
        // the user changes it (Edit > Preferences); over MCP it is read-only (the sandbox hardening)
        bool userOnly = false;
    };

    class SettingsRegistry {
    public:
        // Every built-in page and row (SettingsRows.cpp), registered on first use
        static SettingsRegistry& get();

        // False with *why: a page without a name, a duplicate
        bool addPage(SettingsPage page, std::string* why = nullptr);
        // False with *why: no key, no label, no page of that name, neither get nor draw, a
        // duplicate key, an MCP row without set
        bool addRow(SettingRow row, std::string* why = nullptr);

        // In order
        std::vector<const SettingsPage*> pages(SettingsScope scope) const;
        const SettingsPage* findPage(std::string_view name) const;
        const std::vector<SettingRow>& rows() const { return rowList; }
        const SettingRow* find(std::string_view key) const;
        std::vector<const SettingRow*> rowsOf(std::string_view page) const;

        // Filter mode over label, keywords, key, section and page; an empty query matches all
        static bool rowMatches(const SettingRow& row, std::string_view normalizedQuery);
        std::vector<const SettingRow*> search(std::string_view query, std::optional<SettingsScope> scope = std::nullopt) const;
        // Pages with a matching row (or whose own name and keywords match)
        bool pageMatches(const SettingsPage& page, std::string_view normalizedQuery) const;

        // "Project Settings > Physics > Gravity", "Preferences > General"
        std::string location(const SettingRow& row) const;
        static const char* scopeName(SettingsScope scope);   // "project", "preference"

    private:
        std::vector<SettingsPage> pageList;
        std::vector<SettingRow> rowList;
        std::map<std::string, size_t, std::less<>> byKey;
    };

    // The built-in rows (SettingsRows.cpp)
    void registerBuiltinSettings(SettingsRegistry& registry);

}
