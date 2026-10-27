// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Window > General > Bookmarks (S25 R28): assets, folders, scenes and GameObjects kept at hand,
// per user and project (organisation.yaml). Drag one in from the Project window or the
// Hierarchy (or use their Add to Bookmarks item), drag rows to reorder them, double-click to go
// there (the scene opens first when needed, then the selection and a ping). A bookmark dragged
// onto the Scene view or the Hierarchy is the asset itself. One that resolves nowhere shows
// dimmed as "(Missing)" with Locate and Remove. Adding, removing and reordering are undo steps
// of the project history. Its layout follows OpenEngine's BookmarksPanel (kinds, drag in,
// reorder, drop to navigate).

#include "organisation/Organisation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;

    class BookmarksWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Bookmarks";

        explicit BookmarksWindow(Project* project);

        void show();
        void setOpen(bool open);
        bool isOpen() const { return windowOpen; }

        // The undoable edits (the window, the Project and Hierarchy context items, MCP)
        bool add(const Bookmark& bookmark, std::string* error = nullptr);
        bool removeAt(size_t index, std::string* error = nullptr);
        bool move(size_t from, size_t to, std::string* error = nullptr);
        // Goes where it points (opens its scene first); false with *error when it is missing
        bool navigate(size_t index, std::string* error = nullptr);

    private:
        Project* project;
        bool windowOpen = false;
        bool focusRequested = false;

        bool commit(std::vector<Bookmark> list, const char* name, std::string* error);
        void acceptDrops();
        void locate(size_t index);
    };

}
