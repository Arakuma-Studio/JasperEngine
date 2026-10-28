// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace jasperengine::editor {

    // The main menu's commands (PlatformMenuCommand::id), run by App::executeMenuCommand. The
    // command catalog (shortcuts/DefaultCommands.cpp) names each one with a stable id.
    enum class AppMenuCommand : uint32_t {
        NewProject = 1,
        NewScene3D,
        NewScene2D,
        NewSceneUI,
        OpenProject,
        OpenRecentProject,
        ClearRecentProjects,
        SaveProjectAs,
        Save,
        SaveAll,
        ExportProject,
        EditorSettings,
        Exit,
        Undo,
        Redo,
        SelectAll,
        CutSelection,
        CopySelection,
        PasteSelection,
        PasteAsChild,
        DuplicateSelection,
        DeleteSelection,
        RenameSelection,
        FrameSelected,
        ToggleHierarchy,
        ToggleInspector,
        ToggleProject,
        ToggleConsole,
        ToggleAnimation,
        ToggleTerrain,
        ToggleTextureSynthesis,
        TogglePhysicsPlacement,
        ToggleMcpActivity,
        ToggleSystems,
        ToggleMissingAssets,
        MigrateAssetReferences,
        ToggleAssetReferences,
        ToggleJobsMemory,
        ToggleProfiler,
        ToggleFrameDebugger,
        ToggleUndoHistory,
        ToggleInputActions,
        TogglePlayModeChanges,
        FocusScene,
        FocusGame,
        StepFrame,
        ToggleDetachableWindows,
        ResetLayout,
        ProjectSettings,
        ProjectScenes,
        ProjectPrefabs,
        ClearTrash,
        TogglePlay,
        TogglePause,
        StopScene,
        RemoveScene,
        CreateGameObject,     // payload: index into HierarchyWindow::getCreateMenuEntries()
        CreateEmptyChild,
        AddComponent,
        AssetsCreateFolder,
        AssetsCreateLuaScript,
        AssetsCreateHtmlDocument,
        AssetsCreateCppScript,
        AssetsShowInFileManager,
        AssetsOpen,
        AssetsRename,
        AssetsDelete,
        AssetsImport,
        AssetsRefresh,
        AssetsReimportTextures,    // S14: every texture cooked again (assets.reimportTextures)
        About,
        Documentation,
        KeyboardShortcuts,
        ReportIssue,
        ScriptsReload,        // S23 commands: scripts.reload, scripts.rebuildCpp, scripts.cancelBuild
        ScriptsRebuildCpp,
        ScriptsCancelBuild,
        ScriptMenu,           // payload: an editor script's menu item id (EditorScripts)
        SearchAll,            // S25: Edit > Search All... (the Search palette)
        Shortcuts,            // Edit > Shortcuts...
        ToggleBookmarks,      // Window > General > Bookmarks
        LayoutApply,          // payload: the preset's name (Window > Layouts)
        LayoutSave,
        LayoutRename,         // payload: the preset's name
        LayoutDelete,         // payload: the preset's name
        LayoutSaveToFile,
        LayoutLoadFromFile,
        TogglePoseAnimation,          // Window > Animation > Pose Animation
        AssetsCreatePoseAnimation,
        AssetsCreateAnimationClip     // shown only once AnimationClipAppHooks::createClipAsset is set
    };

}
