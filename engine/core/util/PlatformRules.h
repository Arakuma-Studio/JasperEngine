// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PLATFORMRULES_H
#define PLATFORMRULES_H

// The platform-independent rules behind the platform services (System.openURL, the locale,
// Screen.orientation, Engine.targetFrameRate): pure functions every platform layer and the
// editor use, so the rules are the same everywhere and testable on any host.

#include "Export.h"
#include "System.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine {

    enum class Platform;

    namespace LocaleRules {

        // A platform locale name as a BCP 47 tag: "es_MX.UTF-8@euro" -> "es-MX",
        // "zh_hant_tw" -> "zh-Hant-TW", "C" / "POSIX" -> "en-US"; "" stays ""
        JASPERENGINE_API std::string normalize(const std::string& tag);
        // "es" of "es-MX" (lower case; "iw" stays "iw": toSystemLanguage knows the old codes)
        JASPERENGINE_API std::string languageCode(const std::string& tag);
        // "Hant" of "zh-Hant-TW", "" when the tag has no script subtag
        JASPERENGINE_API std::string scriptCode(const std::string& tag);
        // "MX" of "es-MX", "419" of "es-419", "" when the tag has no region subtag
        JASPERENGINE_API std::string regionCode(const std::string& tag);
        // Unity's SystemLanguage of a tag (case-insensitive, '_' taken as '-'): Chinese with the
        // Hant script or a TW/HK/MO region is ChineseTraditional, other Chinese
        // ChineseSimplified; sr/hr/bs/sh SerboCroatian, nb/nn/no Norwegian, the old iw/in codes
        // Hebrew/Indonesian; empty or not in Unity's list: Unknown
        JASPERENGINE_API SystemLanguage toSystemLanguage(const std::string& tag);
        // The LANGUAGE variable's list ("es_MX:es:en") as normalized tags, duplicates dropped
        JASPERENGINE_API std::vector<std::string> parseLanguageList(const std::string& list);
        // The region the platform reports apart from the language, or the tag's own region
        JASPERENGINE_API std::string resolveRegion(const std::string& regionOverride, const std::string& tag);
        // Unity's name of a SystemLanguage value ("Spanish"), "Unknown" for anything else
        JASPERENGINE_API const char* systemLanguageName(SystemLanguage language);

    }

    namespace OsRules {

        // Unity's SystemInfo.operatingSystem on Android:
        // "Android OS 14 / API-34 (UP1A.231005.007/10754064)"
        JASPERENGINE_API std::string androidOperatingSystem(const std::string& release, int sdkInt,
                                                            const std::string& display, const std::string& incremental);
        // The integer build of a version string: its first run of digits ("42" -> 42, "1.2" -> 1,
        // "b7" -> 7), 0 when there is none
        JASPERENGINE_API int64_t parseBuildNumber(const std::string& version);
        // "Windows 11 (10.0.22631) 64bit": builds from 22000 on are Windows 11
        JASPERENGINE_API std::string windowsOperatingSystem(unsigned major, unsigned minor, unsigned build, bool is64Bit);

    }

    namespace UrlRules {

        // Why System.openURL refuses the URL, "" when it may open: it needs a scheme of two
        // characters or more ("https:", "mailto:", "market:"; "C:" is a Windows drive), no control
        // characters, and not one of the schemes that run code or read local files (javascript:,
        // data:, file:, vbscript:) or that Windows hands to program launchers (ms-msdt:,
        // search-ms:, shell:, ms-appinstaller:, ms-officecmd: and similar)
        JASPERENGINE_API std::string check(const std::string& url);

    }

    namespace PlatformRules {

        // bit of an orientation in an allowed-orientations mask (Screen.autorotateTo*)
        constexpr uint8_t orientationBit(ScreenOrientation orientation){
            return (uint8_t)(1u << (unsigned)orientation);
        }
        constexpr uint8_t ALL_ORIENTATIONS = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4);
        // Portrait, LandscapeLeft and LandscapeRight: the player settings' default set
        constexpr uint8_t DEFAULT_ALLOWED_ORIENTATIONS = (1u << 1) | (1u << 3) | (1u << 4);

        // Portrait, PortraitUpsideDown, LandscapeLeft or LandscapeRight
        JASPERENGINE_API bool isFixedOrientation(ScreenOrientation orientation);
        // one of the five values
        JASPERENGINE_API bool isOrientation(int value);
        // "portrait", "portrait_upside_down", "landscape_left", "landscape_right", "auto_rotation"
        JASPERENGINE_API const char* orientationName(ScreenOrientation orientation);
        JASPERENGINE_API bool parseOrientationName(const std::string& name, ScreenOrientation& out);
        // The orientations a request allows: a fixed one alone, AutoRotation the mask
        JASPERENGINE_API uint8_t effectiveMask(ScreenOrientation requested, uint8_t allowedMask);
        // What a rotating device shows for a request: a fixed request; in AutoRotation the
        // current orientation while allowed, else the first allowed of Portrait, LandscapeLeft,
        // LandscapeRight, PortraitUpsideDown
        JASPERENGINE_API ScreenOrientation resolveOrientation(ScreenOrientation current, ScreenOrientation requested, uint8_t allowedMask);
        // a window's orientation: wider than high (or square) is LandscapeLeft, else Portrait
        JASPERENGINE_API ScreenOrientation orientationFromSize(int width, int height);
        // the DeviceOrientation of a device held so that it shows `orientation`
        JASPERENGINE_API DeviceOrientation deviceOrientationOf(ScreenOrientation orientation);

        // Android ActivityInfo.SCREEN_ORIENTATION_* constants
        constexpr int ANDROID_LANDSCAPE = 0;
        constexpr int ANDROID_PORTRAIT = 1;
        constexpr int ANDROID_USER = 2;
        constexpr int ANDROID_REVERSE_LANDSCAPE = 8;
        constexpr int ANDROID_REVERSE_PORTRAIT = 9;
        constexpr int ANDROID_USER_LANDSCAPE = 11;
        constexpr int ANDROID_USER_PORTRAIT = 12;
        constexpr int ANDROID_FULL_USER = 13;

        // The ActivityInfo constant of a request: a fixed orientation is its constant, AutoRotation
        // the "user" constant of the mask (the user's rotation lock is kept, as Unity's Auto Rotation
        // Behavior: User). A set Android cannot express gives FULL_USER and sets *unsupported.
        JASPERENGINE_API int androidRequestedOrientation(ScreenOrientation requested, uint8_t allowedMask, bool* unsupported = nullptr);
        // android:screenOrientation of a constant ("portrait", "userLandscape", "fullUser" ...)
        JASPERENGINE_API const char* androidManifestOrientation(int activityInfoOrientation);
        // The interface orientation of a display rotation (Display.getRotation in degrees):
        // devices whose natural orientation is landscape are turned a quarter first
        JASPERENGINE_API ScreenOrientation orientationFromRotation(int rotationDegrees, bool naturalPortrait);
        // OrientationEventListener degrees (0..359, -1 = flat/unknown) as a DeviceOrientation, staying
        // on `previous` until the angle is 10 degrees past a bucket's edge (20 degrees of hysteresis)
        JASPERENGINE_API DeviceOrientation deviceOrientationFromDegrees(int degrees, bool naturalPortrait, DeviceOrientation previous);

        // UIInterfaceOrientationMask bits (Portrait 2, PortraitUpsideDown 4, LandscapeRight 8,
        // LandscapeLeft 16): Unity LandscapeLeft is UIKit's LandscapeRight and the other way round
        JASPERENGINE_API unsigned iosOrientationMask(ScreenOrientation orientation);
        // the mask of a request: a fixed orientation alone, AutoRotation every allowed one
        JASPERENGINE_API unsigned iosSupportedMask(ScreenOrientation requested, uint8_t allowedMask);
        // UIInterfaceOrientation values (Portrait 1, PortraitUpsideDown 2, LandscapeRight 3,
        // LandscapeLeft 4) to the engine's, Portrait for anything else
        JASPERENGINE_API ScreenOrientation orientationFromIos(int interfaceOrientation);
        JASPERENGINE_API int iosInterfaceOrientation(ScreenOrientation orientation);
        // Info.plist names (UIInterfaceOrientationPortrait ...) of the allowed set, the default first
        JASPERENGINE_API std::vector<std::string> iosPlistOrientations(ScreenOrientation defaultOrientation, uint8_t allowedMask);

        // Unity's frame pacing (Application.targetFrameRate, QualitySettings.vSyncCount):
        // - Android, iOS: mobileFps = targetFrameRate (default 60), clamped to [1, refreshRate];
        //   vSyncCount is ignored
        // - Web: a limiter at targetFrameRate (none by default); vSyncCount is ignored
        // - desktops and the editor: vSyncCount > 0 presents every vSyncCount-th vblank and ignores
        //   targetFrameRate (past maxSwapInterval: interval 1 and a limiter at refreshRate /
        //   vSyncCount); vSyncCount 0 presents unsynchronized (synchronized where !canDisableVSync)
        //   with a limiter at targetFrameRate (none by default)
        // refreshRate <= 0 counts as 60.
        JASPERENGINE_API FramePacing computeFramePacing(Platform platform, int targetFrameRate, int vSyncCount,
                                                        double refreshRate, int maxSwapInterval, bool canDisableVSync);

        // The update rate of a desktop window nobody can see (minimized, hidden, covered): the game
        // updates without drawing at its own rate, vSyncCount first (refreshRate / vSyncCount), then
        // targetFrameRate, at most 60 and at least 1. refreshRate <= 0 counts as 60.
        JASPERENGINE_API double hiddenUpdateFps(int targetFrameRate, int vSyncCount, double refreshRate);

    }

}

#endif // PLATFORMRULES_H
