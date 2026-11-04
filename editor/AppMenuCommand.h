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
        ToggleSpriteNormals,
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
        ToggleNavigation,
        TogglePlayModeChanges,
        FocusScene,
        FocusGame,
        StepFrame,
        ToggleDetachableWindows,
        ResetLayout,
        ProjectSettings,
        ClearUserSettings,
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
        ToggleVertexAnimationBaker,   // Window > Animation > Vertex Animation Baker
        AssetsCreatePoseAnimation,
        AssetsCreateAnimationClip,    // shown only once AnimationClipAppHooks::createClipAsset is set
        // S29: Assets > Version Control (the Project window's selection) and Window > Version Control
        VcsCommit,
        VcsRevert,
        VcsLock,
        VcsUnlock,
        VcsShowHistory,
        VcsDiffAgainstBaseline,
        VcsRefresh,
        ToggleVcsCommit,
        ToggleVcsHistory,
        ToggleVcsSceneDiff,
        ToggleVcsConflicts,
        ToggleAnimator,               // Window > Animation > Animator
        AssetsCreateAnimatorController,
        AssetsCreateAvatarMask,
        AssetsCreateAnimationMontage,
        // S27: File > Project Hub... and Save Project as Template..., Window > General > Color
        // Picker and Recorder, and the recording itself (F10)
        ProjectHub,
        SaveProjectAsTemplate,
        ToggleColorPicker,
        ToggleRecorder,
        ToggleRecording,
        // upstream (audio-mixer, native-bridge, camera-shake): after S27's
        ToggleAudioMixer,             // Window > Audio > Audio Mixer
        ResetLoudness,                // Audio Mixer > Master strip
        AssetsCreateAudioMixer,
        AssetsCreateNativePlugin,
        AssetsCreateShakePreset,
        AddCameraShakerToMainCamera   // GameObject > Add Camera Shaker to Main Camera
    };

}
