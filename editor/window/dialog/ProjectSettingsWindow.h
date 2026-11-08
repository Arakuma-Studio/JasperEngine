// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Project.h"
#include "NativePluginDialog.h"
#include "imgui.h"
#include "util/ShaderGlobalsEdit.h"
#include "util/ShaderGlobalsUsage.h"

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
        // Editor > Asset Labels: the label being renamed, its new name
        std::string m_renamingLabel;
        // a label's colour while the Color window edits it (written once, when the window closes)
        std::unordered_map<std::string, Vector3> m_labelColorPreview;
        char m_labelNameBuffer[64] = {0};
        std::string m_projectNameOriginal;
        int m_canvasWidth = 0;
        int m_canvasHeight = 0;
        int m_scalingModeIndex = 0;
        int m_pixelsPerUnit = 1;
        CanvasScalerSettings m_uiScale;
        int m_textureStrategyIndex = 0;
        bool m_vsyncEnabled = true;
        // Quality > Atmosphere Quality (desktop, mobile, web) as AtmosphereQuality values
        int m_atmosphereQuality[3] = {3, 1, 1};
        // Quality > Foliage Distance (desktop, mobile, web)
        float m_foliageDistanceScale[3] = {1.0f, 1.0f, 1.0f};
        // Texture (S14 R24)
        TextureProjectSettings m_textureSettings;
        ModelProjectSettings m_modelSettings;
        PlayerLoggingSettings m_playerLogging;
        PlayerCrashSettings m_playerCrash;
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
        // Player > Resolution and Presentation
        int m_defaultOrientationIndex = 4;
        uint8_t m_allowedOrientations = PresentationSettings{}.allowedOrientations;
        bool m_keepScreenOn = true;
        int m_targetFrameRate = -1;
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
        ShaderCreationProjectSettings m_shaderCreation;
        HtmlUiRenderSettings m_htmlUiRender;
        DrawSubmissionSettings m_drawSubmission;
        // Quality > Device Tiers and the rest of the device settings (S56), and the platform
        // group the table shows
        DeviceSettings m_deviceSettings;
        int m_deviceTierGroup = 1;
        NavigationProjectSettings m_navigation;
        AudioSettings m_audioSettings;
        char m_audioMixerBuffer[512] = {0};
        char m_audioDefaultGroupBuffer[128] = {0};
        char m_audioMusicGroupBuffer[128] = {0};
        DebugSettings m_debugSettings;
        fs::path m_assetsDir;
        fs::path m_luaDir;
        // OK with other asset roots: what the change would do (the confirmation), why it did not
        AssetRootChange m_rootsPlan;
        bool m_rootsConfirmRequested = false;
        bool m_rootsCloseWindow = false;
        std::string m_rootsError;
        std::vector<fs::path> m_scriptDirs;
        int m_cxxStandardIndex = 0;
        bool m_physics2DEnabled = true;
        std::string m_spriteAtlasMode = "enabled";
        bool m_physics3DEnabled = true;
        // Input: the default actions asset (project-relative), the deadzone, and the
        // .inputactions files of the assets directory as the picker lists them
        fs::path m_inputActionsAsset;
        float m_inputDeadzone = 0.15f;
        std::vector<fs::path> m_inputActionsFiles;
        std::string m_inputStatus;
        // Pixel Art: the Default Palette (project-relative, or "builtin:<id>" until OK copies it
        // into the project), and the project's palettes as the picker lists them (path, name)
        std::string m_defaultPalette;
        std::vector<std::pair<std::string, std::string>> m_paletteFiles;
        Physics3DWorldSettings m_physics3DWorld;
        int m_particleWorldBudget = 0;
        ParticleQaSettings m_particleQa;
        procmat::ProcMatQaSettings m_procMatQa;
        bool m_runtimeShadersEnabled = true;
        bool m_dynamicBatching = true;
        int m_mobilePbrTierIndex = 1;
        int m_lightModeIndex = 0;
        bool m_packNativeResources = false;
        // S30: Build Settings' method (auto, prebuilt, compile) and the content pack
        int m_buildMethodIndex = 0;
        bool m_packContent = false;
        bool m_stripEngineCode = true;
        int m_androidPackageFormatIndex = 0;   // apk, aab
        fs::path m_androidKeystorePath;
        char m_androidKeystoreAliasBuffer[128] = {0};
        char m_iosTeamIdBuffer[32] = {0};
        int m_iosExportMethodIndex = 0;        // development, ad-hoc, app-store, enterprise
        bool m_allowInsecureHttp = false;
        bool m_versionControlMetadata = true;
        VcsProjectSettings m_vcs;
        char m_webApplicationNameBuffer[256] = {0};
        fs::path m_webFavicon;
        fs::path m_webCustomHtmlShell;
        char m_webHeadIncludeBuffer[2048] = {0};
        bool m_webResizeCanvasToWindow = true;
        bool m_webViewportFitCover = true;
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
        bool m_androidAbiArmeabiV7a = true;
        bool m_androidAbiArm64V8a = true;
        bool m_androidAbiX86 = true;
        bool m_androidAbiX86_64 = true;
        std::set<std::string> m_androidPermissions;
        int m_androidInternetAccessIndex = 0;
        bool m_androidAllowBackup = true;
        bool m_androidFullscreen = true;

        // Native Plugins being edited (Project Settings > Native Plugins); plist entries as rows
        std::vector<std::string> m_gradleDependencies;
        std::vector<std::string> m_gradleRepositories;
        std::vector<std::string> m_gradleProperties;
        std::vector<std::string> m_extraPermissions;
        std::string m_manifestXml;
        std::string m_manifestApplicationXml;
        std::vector<std::pair<std::string, NativePlistValue>> m_iosPlistEntries;
        std::string m_iosPlistXml;
        std::vector<std::string> m_iosFrameworks;
        std::vector<NativeCocoaPod> m_iosCocoapods;
        std::vector<NativeSwiftPackage> m_iosSwiftPackages;
        std::string m_iosEntitlementsXml;
        std::string m_iosDeploymentTarget;
        std::vector<std::pair<std::string, NativePlistValue>> m_macosPlistEntries;
        std::vector<std::string> m_macosFrameworks;
        std::vector<std::string> m_webScriptUrls;
        NativePluginDialog m_nativePluginDialog;
        // plugins/native as last scanned (open, Refresh, after Create Plugin): not every frame
        std::string m_nativeSummary;
        void refreshNativeSummary();

        // Player > iOS / macOS > Privacy Manifest (shared by both platforms)
        ApplePrivacy::Manifest m_applePrivacy;
        std::string m_privacyPreview;
        std::string m_privacyPreviewTitle;
        bool m_openPrivacyPreview = false;
        void drawApplePrivacySection(const char* platform);
        // The edited additions as OK saves them: empty rows dropped, domains trimmed, normalized
        ApplePrivacy::Manifest editedApplePrivacy() const;
        // Why the edited privacy manifest blocks OK, "" when it does not
        std::string applePrivacyError() const;

        // Tags and Layers being edited: user tags, and user layer names indexed by layer
        // (MAX_LAYERS entries, built-in ones empty)
        std::vector<std::string> m_tags;
        std::vector<std::string> m_layerNames;
        char m_newTagBuffer[128] = {0};

        // Layer Collision Matrices being edited (Physics and Physics 2D)
        LayerCollisionRows m_layerCollision3D = allLayersCollide();
        LayerCollisionRows m_layerCollision2D = allLayersCollide();

        // Project Settings > Input's Input Manager: the actions asset's "Input Manager" map (Unity's
        // defaults without an asset) being edited as Unity's virtual axes and buttons. The section
        // has its own undo (Ctrl+Z / Ctrl+Y while the page shows); OK makes the whole edit one
        // project history step (InputActionsCmd), which makes an asset when the project has none
        std::vector<InputManagerAction> m_inputActions;
        std::vector<InputManagerAction> m_inputOriginal;  // as the dialog opened (or the asset changed)
        std::filesystem::path m_inputActionsSource;       // the asset those came from
        bool m_inputManagerReadable = true;               // the asset could be read
        std::vector<InputManagerAction> m_inputCommitted;      // the state of the last section undo step
        std::string m_inputCommittedJson;
        std::vector<std::vector<InputManagerAction>> m_inputUndo;
        std::vector<std::vector<InputManagerAction>> m_inputRedo;
        int m_inputSelected = 0;
        char m_inputSearch[64] = {0};
        char m_inputControlFilter[64] = {0};
        // Listen: the next key, mouse button or gamepad control pressed goes to this binding
        // (list 0: positive, 1: negative, 2: the gamepad axis)
        struct InputListen {
            int action = -1;
            int binding = -1;
            int list = 0;
            int frames = 0;
        };
        InputListen m_inputListen;

        // Shader Globals being edited (Project Settings > Graphics). Every valid edit is declared to
        // the engine at once, so the Scene and Game views follow it; Cancel declares the project's
        // again, OK makes the whole edit one project history step (a renamed global takes the
        // material and component bindings of the open scenes along)
        ShaderGlobalsEdit m_globalsEdit;
        // "Used by", scanned when the page opens and on Refresh (not every frame)
        std::vector<ShaderGlobalsUsage::Use> m_shaderGlobalUses;
        bool m_shaderGlobalUsesScanned = false;
        int m_shaderGlobalRemove = -1;   // the row whose removal asks first (bindings use it)

        // Unity-style category list; indexes ProjectCategory in the .cpp.
        int m_category = 0;
        char m_categorySearch[64] = {0};

        void drawSettings();
        void drawPlayerSettings();
        // Default Orientation, Allowed Orientations, Keep Screen On, Target Frame Rate (rows of the open table)
        void drawPresentationSettings();
        void drawGraphicsSettings();
        void drawShaderCreationSettings();
        void drawShaderGlobals();
        // the default (or, in Play, the live value) of a global: a field per type; matrices and
        // arrays edit in a popup. True when `values` changed
        bool drawShaderGlobalValue(const char* id, ShaderGlobalType type, unsigned int arraySize, std::vector<Vector4>& values);
        bool drawShaderGlobalTexture(const char* id, ShaderGlobalType type, std::string& path);
        void refreshShaderGlobalUses();
        void drawQualitySettings();
        void drawDeviceTierSettings();
        void drawUploadBudgetSettings();
        void drawTextureSettings();
        void drawPhysicsSettings();
        void drawPhysics2DSettings();
        void drawInputSettings();
        void drawAudioSettings();
        void drawNavigationSettings();
        void drawMeshSettings();
        void draw2DSettings();
        void drawAnimationSettings();
        void drawPixelArtSettings();
        // Unity's triangular Layer Collision Matrix over the named layers (as typed in
        // Tags and Layers, not yet applied)
        void drawLayerCollisionMatrix(const char* id, LayerCollisionRows& rows);
        // the Input Manager section of the Input page
        void drawInputManagerSection();
        void loadInputManagerActions();
        void drawInputActionList();
        void drawInputActionDetails(InputManagerAction& action, int actionIndex);
        void drawInputBinding(InputManagerAction& action, int actionIndex, int bindingIndex);
        // a binding's control list: names to remove, a picker to add, Listen
        void drawInputControls(const char* id, std::vector<InputControl>& controls, int actionIndex, int bindingIndex, int list);
        void pollInputListen();
        void commitInputEdit();
        void undoInputEdit();
        void redoInputEdit();
        // First Input problem that blocks OK, "" when there is none
        std::string inputError() const;

        void drawScriptingSettings();
        // Debug: the on-device console and stats overlay, the log
        void drawDebugSettings();
        void drawTagsAndLayersSettings();
        // Why the New Tag Name field cannot be added ("" when it can)
        std::string newTagError() const;
        // Layer that already has the name (built-in or user, other than 'layer'), -1 when none
        int findLayerNameClash(int layer, const std::string& name) const;
        // First Tags and Layers problem that blocks OK, "" when there is none
        std::string tagsAndLayersError() const;
        void drawEditorSettings();
        void drawAssetLabelsSettings();
        void drawNativePluginsSettings();
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
        // the "Change Asset Directories" confirmation: Move and Apply runs the change, then the rest
        void drawRootsConfirmation();

    public:
        // category: a page to show, e.g. "Player"; nullptr keeps the last one.
        void open(Project* project, const char* category = nullptr);
        void show();
        bool isOpen() const { return m_isOpen; }
        // a native plugin's Lua mock the Create Plugin dialog wrote (to open in the code editor); once
        std::filesystem::path consumeCreatedFile() { return m_nativePluginDialog.consumeCreatedFile(); }
    };

}
