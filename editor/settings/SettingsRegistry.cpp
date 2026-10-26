// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SettingsRegistry.h"

#include "search/SearchScorer.h"

#include <algorithm>

using namespace jasperengine::editor;

SettingsRegistry& SettingsRegistry::get() {
    static SettingsRegistry registry = [] {
        SettingsRegistry built;
        registerBuiltinSettings(built);
        return built;
    }();
    return registry;
}

bool SettingsRegistry::addPage(SettingsPage page, std::string* why) {
    if (page.name.empty()) {
        if (why) *why = "a settings page needs a name";
        return false;
    }
    if (findPage(page.name)) {
        if (why) *why = "the page " + page.name + " exists";
        return false;
    }
    pageList.push_back(std::move(page));
    std::stable_sort(pageList.begin(), pageList.end(), [](const SettingsPage& a, const SettingsPage& b) {
        if (a.scope != b.scope) return a.scope < b.scope;
        return a.order < b.order;
    });
    return true;
}

bool SettingsRegistry::addRow(SettingRow row, std::string* why) {
    auto refuse = [&](const std::string& reason) {
        if (why) *why = (row.key.empty() ? std::string("a setting") : row.key) + ": " + reason;
        return false;
    };
    if (row.key.empty()) return refuse("no key");
    if (row.label.empty()) return refuse("no label");
    const SettingsPage* page = findPage(row.page);
    if (!page) return refuse("no page named \"" + row.page + "\"");
    if (!row.get && !row.draw) return refuse("neither get nor draw: nothing shows it");
    if (row.mcp && !row.set) return refuse("an MCP setting needs set");
    if (byKey.count(row.key)) return refuse("registered twice");
    row.scope = page->scope;
    byKey[row.key] = rowList.size();
    rowList.push_back(std::move(row));
    return true;
}

std::vector<const SettingsPage*> SettingsRegistry::pages(SettingsScope scope) const {
    std::vector<const SettingsPage*> out;
    for (const SettingsPage& page : pageList) {
        if (page.scope == scope) out.push_back(&page);
    }
    return out;
}

const SettingsPage* SettingsRegistry::findPage(std::string_view name) const {
    for (const SettingsPage& page : pageList) {
        if (page.name == name) return &page;
    }
    return nullptr;
}

const SettingRow* SettingsRegistry::find(std::string_view key) const {
    auto it = byKey.find(key);
    return it == byKey.end() ? nullptr : &rowList[it->second];
}

std::vector<const SettingRow*> SettingsRegistry::rowsOf(std::string_view page) const {
    std::vector<const SettingRow*> out;
    for (const SettingRow& row : rowList) {
        if (row.page == page) out.push_back(&row);
    }
    return out;
}

bool SettingsRegistry::rowMatches(const SettingRow& row, std::string_view normalizedQuery) {
    if (normalizedQuery.empty()) return true;
    for (const std::string* text : {&row.label, &row.keywords, &row.key, &row.section}) {
        if (!text->empty() && search::filterMatch(normalizedQuery, search::normalize(*text))) return true;
    }
    return false;
}

bool SettingsRegistry::pageMatches(const SettingsPage& page, std::string_view normalizedQuery) const {
    if (normalizedQuery.empty()) return true;
    if (search::filterMatch(normalizedQuery, search::normalize(page.displayName() + " " + page.keywords))) return true;
    for (const SettingRow& row : rowList) {
        if (row.page == page.name && rowMatches(row, normalizedQuery)) return true;
    }
    return false;
}

std::vector<const SettingRow*> SettingsRegistry::search(std::string_view query, std::optional<SettingsScope> scope) const {
    const std::string normalized = search::normalize(query);
    std::vector<const SettingRow*> out;
    for (const SettingRow& row : rowList) {
        if (scope && row.scope != *scope) continue;
        if (rowMatches(row, normalized)) out.push_back(&row);
    }
    return out;
}

std::string SettingsRegistry::location(const SettingRow& row) const {
    std::string text = row.scope == SettingsScope::Project ? "Project Settings > " : "Preferences > ";
    const SettingsPage* page = findPage(row.page);
    text += page ? page->displayName() : row.page;
    if (!row.section.empty()) text += " > " + row.section;
    return text;
}

const char* SettingsRegistry::scopeName(SettingsScope scope) {
    return scope == SettingsScope::Project ? "project" : "preference";
}
