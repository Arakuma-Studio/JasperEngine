// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Row-level search of Preferences and Project Settings (S25 R30). A window begins a page with
// its search text; each row it draws asks beginRow(key) first and is drawn only when it
// matches (Filter mode over the registry row's label, keywords and key). Section headers ask
// sectionVisible. The label helpers draw the matched text highlighted, and a row the Search
// palette jumped to (requestReveal) is scrolled into view and flashes. Clearing the search
// restores the page exactly: with no search every row is drawn as before. In debug builds a
// drawn row that the registry does not have asserts.

#include "settings/SettingsRegistry.h"

#include <initializer_list>
#include <string>

namespace jasperengine::editor::settingsui {

    // The page a window draws now, and its search field's text
    void beginPage(const std::string& page, const char* search);
    void endPage();
    bool searching();

    // A row of the current page: false when the search hides it. Several keys: one drawn row
    // for several settings (visible when any of them matches)
    bool beginRow(const char* key);
    bool beginRow(std::initializer_list<const char*> keys);
    // A foldout or sub-heading of the current page: false when the search hides all its rows
    bool sectionVisible(const char* section);

    // The current row's label, the search's matches highlighted (a revealed row flashes)
    void rowLabel(const char* label);
    // Highlights for any text of the row (the matched part of a section title too)
    void highlightedText(const char* text);

    // The palette opened the window for this row: it scrolls there and flashes once drawn
    void requestReveal(const std::string& key);
    // The page of a pending reveal ("" when none), for the windows' page lists
    std::string pendingRevealPage();

}
