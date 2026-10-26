// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "imgui.h"

#include <string>
#include <filesystem>
#include <set>
#include <unordered_map>

namespace jasperengine::editor {

    namespace fs = std::filesystem;

    class ProjectSettingsWindow {
    private:
        bool m_isOpen = false;
        Project* m_project = nullptr;

        // UI state
        char m_projectNameBuffer[256] = {0};
        std::string m_projectNameOriginal;
        int m_canvasWidth = 0;
        int m_canvasHeight = 0;
        int m_scalingModeIndex = 0;
        int m_textureStrategyIndex = 0;
        bool m_vsyncEnabled = true;
        // Quality > Atmosphere Quality (desktop, mobile, web) as AtmosphereQuality values
        int m_atmosphereQuality[3] = {3, 1, 1};
        // Texture (S14 R24)
        TextureProjectSettings m_textureSettings;
        PlayerLoggingSettings m_playerLogging;
        int m_htmlUiMsaaIndex = 2;   // htmlUiMsaaValues: 1, 2, 4, 8
        // the shadow quality tier of each platform (Engine::Platform order), as ShadowTier ints
        int m_shadowTiers[6] = {2, 0, 1, 0, 2, 2};
        int m_windowModeIndex = 0;
        int m_windowWidth = 0;
        int m_windowHeight = 0;
        bool m_windowResizable = true;
        char m_windowTitleBuffer[256] = {0};
        std::string m_windowTitleOriginal;
        fs::path m_windowIcon;
        char m_applicationNameBuffer[256] = {0};
        char m_applicationIdentifierBuffer[256] = {0};
        char m_applicationVersionBuffer[64] = {0};
        int m_applicationBuild = 1;

        // Per-frame preview textures over the pool-cached thumbnails, cleared
        // at the end of each draw (same lifecycle as Properties).
        std::unordered_map<std::string, Texture> m_thumbnailTextures;
        uint32_t m_startSceneId = NULL_PROJECT_SCENE;
        LoadingSettings m_loadingSettings;
        ThreadingProjectSettings m_threading;
        fs::path m_assetsDir;
        fs::path m_luaDir;
        std::vector<fs::path> m_scriptDirs;
        int m_cxxStandardIndex = 0;
        bool m_physics2DEnabled = true;
        bool m_physics3DEnabled = true;
        bool m_runtimeShadersEnabled = true;
        int m_mobilePbrTierIndex = 1;
        bool m_packNativeResources = false;
        bool m_versionControlMetadata = true;
        char m_webApplicationNameBuffer[256] = {0};
        fs::path m_webFavicon;
        fs::path m_webCustomHtmlShell;
        char m_webHeadIncludeBuffer[2048] = {0};
        bool m_webResizeCanvasToWindow = true;
        bool m_webHideEmscriptenUI = false;
        char m_linuxApplicationNameBuffer[256] = {0};
        char m_linuxCommentBuffer[512] = {0};
        char m_linuxCategoriesBuffer[256] = {0};
        char m_windowsProductNameBuffer[256] = {0};
        char m_windowsCompanyNameBuffer[256] = {0};
        char m_windowsFileVersionBuffer[64] = {0};
        char m_windowsProductVersionBuffer[64] = {0};
        char m_macOSApplicationNameBuffer[256] = {0};
        char m_macOSBundleIdentifierBuffer[256] = {0};
        char m_macOSVersionNameBuffer[64] = {0};
        char m_macOSBuildNumberBuffer[64] = {0};
        fs::path m_macOSIcon;
        bool m_macOSHighDpi = true;
        char m_iosApplicationNameBuffer[256] = {0};
        char m_iosBundleIdentifierBuffer[256] = {0};
        char m_iosVersionNameBuffer[64] = {0};
        char m_iosBuildNumberBuffer[64] = {0};
        fs::path m_iosIcon;
        bool m_iosHideStatusBar = true;
        bool m_iosHideHomeIndicator = true;
        bool m_iosSupportsHighRefreshRate = true;
        char m_androidApplicationNameBuffer[256] = {0};
        char m_androidPackageNameBuffer[256] = {0};
        char m_androidVersionNameBuffer[64] = {0};
        fs::path m_androidLauncherIcon;
        fs::path m_androidAdaptiveIconForeground;
        fs::path m_androidAdaptiveIconBackground;
        char m_androidVersionCodeBuffer[16] = {0};
        int m_androidMinSdk = 21;
        int m_androidTargetSdk = 33;
        int m_androidOrientationIndex = 0;
        bool m_androidAbiArmeabiV7a = true;
        bool m_androidAbiArm64V8a = true;
        bool m_androidAbiX86 = true;
        bool m_androidAbiX86_64 = true;
        std::set<std::string> m_androidPermissions;
        bool m_androidAllowBackup = true;
        bool m_androidFullscreen = true;
        bool m_androidKeepScreenOn = false;

        // Tags and Layers being edited: user tags, and user layer names indexed by layer
        // (MAX_LAYERS entries, built-in ones empty)
        std::vector<std::string> m_tags;
        std::vector<std::string> m_layerNames;
        char m_newTagBuffer[128] = {0};

        // Layer Collision Matrices being edited (Physics and Physics 2D)
        LayerCollisionRows m_layerCollision3D = allLayersCollide();
        LayerCollisionRows m_layerCollision2D = allLayersCollide();

        // Unity-style category list; indexes ProjectCategory in the .cpp.
        int m_category = 0;
        char m_categorySearch[64] = {0};

        void drawSettings();
        void drawPlayerSettings();
        void drawGraphicsSettings();
        void drawQualitySettings();
        void drawTextureSettings();
        void drawPhysicsSettings();
        void drawPhysics2DSettings();
        // Unity's triangular Layer Collision Matrix over the named layers (as typed in
        // Tags and Layers, not yet applied)
        void drawLayerCollisionMatrix(const char* id, LayerCollisionRows& rows);
        void drawScriptingSettings();
        void drawTagsAndLayersSettings();
        // Why the New Tag Name field cannot be added ("" when it can)
        std::string newTagError() const;
        // Layer that already has the name (built-in or user, other than 'layer'), -1 when none
        int findLayerNameClash(int layer, const std::string& name) const;
        // First Tags and Layers problem that blocks OK, "" when there is none
        std::string tagsAndLayersError() const;
        void drawEditorSettings();
        Texture* findThumbnail(const std::string& path);

        // Player's per-platform tabs; each platform draws rows into the table its tab opens.
        void drawPlatformsSettings();
        void drawWebSettings();
        void drawLinuxSettings();
        void drawWindowsSettings();
        void drawMacOSSettings();
        void drawIOSSettings();
        void drawAndroidSettings();

        // What an empty override resolves to, read from the buffers so hints follow typing.
        std::string inheritedApplicationName() const;
        std::string inheritedApplicationIdentifier() const;
        std::string inheritedApplicationVersion() const;
        std::string inheritedApplicationBuild() const;

        bool applySettings();

    public:
        // category: a page to show, e.g. "Player"; nullptr keeps the last one.
        void open(Project* project, const char* category = nullptr);
        void show();
        bool isOpen() const { return m_isOpen; }
    };

}
