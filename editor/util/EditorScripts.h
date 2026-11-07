// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Editor menus, context-menu items and toolbar buttons defined by the project's scripts
// (Unity's [MenuItem]), S23 R18-R20:
//
//   Lua, in a file under a folder named Editor that has at least one annotation:
//     ---@menu Tools/Snap To Grid [priority=10] [shortcut=Ctrl+Shift+K]
//     function M.snap(ctx) ... end
//     ---@toolbar Snap [icon=fa-magnet] [tooltip=Snap the selection]
//     ---@validate snap      (on a function returning whether the item is enabled)
//   C++, in a .cpp under Editor/:  JE_MENU_ITEM("Tools/Bake Lights", 100)
//                                  static void bakeLights(const jasperengine::EditorMenuContext& ctx) { ... }
//
// Paths follow Unity: Tools/... (any top-level name) is a main menu; GameObject/... also the
// Hierarchy's context menu, Assets/... the Project window's, CONTEXT/<Component>/... that
// component's Inspector header menu, SceneView/... the Scene view's.
//
// Discovery is a text scan: opening a project runs none of this code. A click runs the item
// (Lua in a sandboxed editor state with an Editor table: Editor.call runs an MCP tool, so every
// edit is undoable; C++ in a library built for it), as one undo step named after the item.

#include "mcp/McpJson.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::editor {

    class Project;
    namespace mcp { class McpCall; }

    struct EditorMenuItem {
        std::string id;              // "lua:Editor/Tools.lua#snap", "cpp:Editor/Bake.cpp#bakeLights"
        std::string path;            // "Tools/Snap To Grid"; a toolbar button: its label
        std::string kind;            // menu, toolbar
        std::string language;        // lua, cpp
        int priority = 1000;
        std::string shortcut;
        std::string icon;            // toolbar: a Font Awesome name ("magnet")
        const char* iconGlyph = nullptr;  // its glyph (nullptr: no such icon, the label shows)
        std::string tooltip;
        std::string file;            // project-relative
        int line = 0;
        std::string function;        // the module function (Lua) / the C++ function
        std::string validateFunction;// Lua: its ---@validate function, empty when none

        // the item's own name (the last part of its path): its undo step's name
        std::string name() const;
        // the menu targets its path reaches: main, gameobject, assets, component, sceneview
        std::vector<std::string> targets() const;
        // CONTEXT/<Component>/...: the component, else empty
        std::string contextComponent() const;
    };

    // What a run sees (the Lua function's ctx; the C++ function's EditorMenuContext JSON)
    struct EditorMenuRunContext {
        std::string menuPath;
        uint32_t sceneId = 0;
        std::vector<uint32_t> selection;     // GameObject ids of that scene
        std::vector<std::string> assets;     // project-relative paths (the Project window's selection)
        std::string component;               // CONTEXT/<Component>/... items
    };

    struct EditorMenuRunResult {
        bool ok = false;
        std::string error;                   // a Lua error with its location, or why it did not run
        std::string undoName;                // the undo step it made ("Raise Selected"), empty when none
        std::vector<std::string> log;        // Editor.log, print
        mcp::Json undo = nullptr;            // the batch undo group ({scene_ids, undo: {id, name}})
    };

    class EditorScripts {
    public:
        static EditorScripts& get();

        // Scans the project's Editor folders (text only); a file that changed again on its own
        void scan(Project* project);
        void rescanFile(const std::filesystem::path& absolute);
        const std::vector<EditorMenuItem>& items() const { return menuItems; }
        uint64_t revision() const { return revisionCounter; }
        const EditorMenuItem* find(const std::string& idOrPath) const;
        // the items a target shows (see EditorMenuItem::targets), by priority then path
        std::vector<const EditorMenuItem*> itemsFor(const std::string& target, const std::string& component = {}) const;

        // A Lua file is an editor script: it lies under a folder named Editor (any case) AND has
        // an annotation (runtime scripts in Editor/ folders stay runtime scripts)
        static bool isEditorFolderPath(const std::filesystem::path& projectRelative);
        static bool isLuaEditorScript(const std::filesystem::path& projectRelative, const std::string& code);
        // An editor-only script file (absolute): a Lua editor script, or a .cpp under Editor/
        // with a JE_MENU_ITEM. Never exported, never in the game's (or Play's) script build,
        // never require()d at runtime.
        static bool isEditorOnlyScript(const std::filesystem::path& absolute, const std::filesystem::path& projectRoot);
        // The annotations of a Lua editor script / the JE_MENU_ITEMs of a C++ file
        static std::vector<EditorMenuItem> parseLua(const std::string& code, const std::string& file);
        static std::vector<EditorMenuItem> parseCpp(const std::string& code, const std::string& file);
        // <internal>/editor-scripts of the C++ editor scripts `sources`: one generated source per
        // Editor .cpp (it includes the file, so the item functions, static in it, can be registered
        // after it) and the CMake project; libName is the library it builds
        static bool writeCppProject(Project* project, const std::vector<std::filesystem::path>& sources, const std::filesystem::path& dir,
                                    std::string& libName);

        // Runs an item. Editor.call goes through MCP tools nested in `parent` (the
        // run_editor_menu call; a menu click makes one), and the edits of the whole run become
        // one undo step named after the item. done runs on the main thread when the function and
        // the Editor.call callbacks it started have finished.
        void run(const EditorMenuItem& item, const EditorMenuRunContext& context, const std::shared_ptr<mcp::McpCall>& parent,
                 std::function<void(EditorMenuRunResult)> done);
        // A menu click (no MCP call): runs it through run_editor_menu in-process
        void runFromMenu(const std::string& id, const EditorMenuRunContext& context);
        // Inside an open ImGui popup: the items of a context menu target (gameobject, assets,
        // component (with its name), sceneview) after a separator, their submenus by path.
        // Their validate functions run when the popup appears. Returns whether it drew any.
        bool drawContextItems(const std::string& target, const std::string& component, const EditorMenuRunContext& context);

        // ---@validate: the enabled state of an item, from the last time a menu containing it
        // opened (true without a validate function or before one ran)
        bool isEnabled(const EditorMenuItem& item) const;
        // A menu whose path starts with prefix opened: its items' validate functions run (2 ms
        // budget each: over it, the item stays enabled and a warning is logged)
        void menuOpened(const std::string& prefix, const EditorMenuRunContext& context);

        static constexpr int VALIDATE_BUDGET_US = 2000;
        // A Font Awesome 6 glyph by name ("magnet", "fa-magnet"); nullptr when there is none
        static const char* fontAwesomeGlyph(const std::string& name);

    private:
        EditorScripts() = default;
        void scanFile(const std::filesystem::path& absolute, const std::string& relative, std::vector<EditorMenuItem>& out) const;
        void sortItems();

        Project* project = nullptr;
        std::vector<EditorMenuItem> menuItems;
        std::map<std::string, bool> validated;     // item id -> enabled
        uint64_t revisionCounter = 0;
        uint64_t invalidator = 0;                  // HotReloadService: script files that changed
    };

}
