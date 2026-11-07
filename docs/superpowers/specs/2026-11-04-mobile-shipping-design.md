# Mobile shipping: no built-in AdMob, an Apple privacy manifest in every build, no native plugin silently dropped (design)

Date 2026-11-04. Branch `dc/mobile-shipping`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md`
(feature #2). Dossier: `dc-work/dossiers/mobile-shipping.json`; owner decisions: `dc-work/dossiers/EXTRA-FINDINGS.md`.

Evidence labels used below: **verified** (I ran a command or read the file today), **inferred** (from reading code),
**assumed**. Everything in this spec was read statically; nothing was built or run.

## 1. Goals

1. **Retire built-in AdMob completely** (owner decision 2026-11-04: no compatibility, no deprecation path, nothing
   kept). No Google Mobile Ads SDK in the repository, no AdMob code, settings, Lua API, Gradle/Xcode entries, tests
   or doc sections. Ads (and purchases) go through the native bridge, and the docs show a complete worked example.
2. **Every Apple build ships a privacy manifest** (`PrivacyInfo.xcprivacy`): the exported Xcode project (iOS and
   macOS targets), the CMake iOS app and the CMake macOS bundle, the prebuilt iOS player project and the prebuilt
   macOS bundle. It holds the engine's own required-reason API declarations, the game's additions from Project
   Settings (tracking, tracking domains, collected data, extra API reasons) and the manifests of the game's static
   native plugins. Humans edit it in Project Settings with undo; agents set it with `set_project_settings` and read
   the composed result with `native_plugins`.
3. **A prebuilt build never silently drops a native plugin or a native setting.** For every target platform each
   piece of `plugins/native` and each native setting is either *carried* into the prebuilt build (and listed as
   such) or makes the build *Compile* with a reason that names it. A Lua mock that would answer in a shipped build
   in place of real native code is a build warning and a runtime warning.
4. **The small Apple build fixes**: the CMake iOS app links the engine's system frameworks (AudioToolbox was
   missing, Doriax `458bad1a3`); the zstd targets of the Xcode template use the iOS 15 / macOS 12 minimums (Doriax
   `9b95b353b`); the engine API sync never walks vendored SDK bundles (Doriax `323fc511e`).

## 2. Non-goals

- Updating AdMob to the current Google SDKs, more ad formats, consent API, paid events, Google Play Billing or
  StoreKit purchases (Doriax `3889ddf10`, `21eb2f4f9`): rejected by the owner's retirement decision. Games use the
  native bridge (the project's own game uses Wortise and RevenueCat that way: `docs/engine-gaps/GAP-REPORT.md`).
- The built-in CrazyGames web SDK calls (`System.initializeCrazyGamesSDK` ...): not AdMob, not in this mandate.
- A scaffold template that writes an AdMob plugin (`native_plugins create template: admob`): it would put
  AdMob-specific code back into the editor, against the owner's decision. The worked example lives in the docs.
- Signing, notarization, App Store Connect upload, the App Privacy "nutrition label" form: Apple's tools.
- Carrying Java/Kotlin code, AARs, Gradle dependencies or C++ plugins into prebuilt builds: they need Gradle, d8 or
  a C++ compiler, which a prebuilt build by definition does not run. They make the build Compile, with a reason.
- Other gaps of the prebuilt iOS template found while reading (App Transport Security from
  `player.allow_insecure_http`, `UIRequiredDeviceCapabilities armv7` in the source-export Info.plist): reported to
  the lead in section 13, not fixed here.

## 3. Users and stories

- **Human shipping an iOS game with an ads SDK.** Writes the SDK wrapper as a native plugin (Objective-C in
  `plugins/native/ios`, the SDK by Swift Package), sets Project Settings > Player > iOS > Privacy Manifest >
  Tracking on, adds the SDK's tracking domains, ticks "Device ID: Third-Party Advertising, linked, tracking", and
  presses Build. The prebuilt iOS build carries the plugin and the package into the Xcode project; the Build
  Settings window lists "Carried: iOS plugins (Ads.m), Swift packages (1), privacy manifest additions (3)". The
  archive passes App Store Connect's required-reason check because the engine's own reasons are declared.
- **Human with a Java ads plugin building for Android with Auto.** The Build Settings window says "Compiled from
  source: Java/Kotlin plugins (plugins/native/android/java/com/me/Ads.java)" and, because Android native code is
  built from the source project, Build is blocked with "Export source, then Android Studio". Today the prebuilt
  APK silently ships without the plugin and the shipped game's Lua mock answers `showRewarded` with a fake reward.
- **Human who enabled AdMob in an older build of Jasper.** The rows are gone; `System.initializeAdMob` is nil; the
  docs' "Ads through the native bridge" section is the replacement. (Owner: no project uses AdMob.)
- **Agent.** `set_project_settings {values: {"apple.privacy_tracking": true, "apple.privacy_tracking_domains":
  ["ads.example.com"], "apple.privacy_collected_data": [{"type": "DeviceID", "linked": true, "tracking": true,
  "purposes": ["ThirdPartyAdvertising"]}]}}`, then `native_plugins {action: "privacy_manifest", platform: "ios"}`
  to read the composed XML and where each entry came from, then `get_build_info` to see, per platform, the method,
  `compile_reasons` and `carried`. `undo {scope: "project"}` reverts the privacy edit.
- **Agent porting a Unity game with `Assets/Plugins/iOS/PrivacyInfo.xcprivacy`.** Copies it to
  `plugins/native/ios/PrivacyInfo.xcprivacy`; the export merges it (Unity's behaviour for plugin manifests).

## 4. Findings

### 4.1 AdMob (Doriax `3889ddf10`, confirmed; owner: retire)

- Jasper links an iOS SDK past its sunset: `engine/platform/apple/GoogleMobileAdsSdkiOS-10.14.0` (73 MB, 637
  files, verified `du`, `git ls-files`), CMake links only its simulator slices (`engine/CMakeLists.txt:830-847`).
  Android pins `play-services-ads:22.6.0` (`engine/workspaces/androidstudio/app/build.gradle:137`). Google lists
  both majors as sunset on 2026-06-30 (dossier verdict, fetched 2026-11-04).
- Every export copies the 73 MB SDK, AdMob or not (`Exporter::copyEngine` copies all of `platform/`,
  `editor/Exporter.cpp:3429`; inferred).
- AdMob surfaces, all to delete (verified by `git grep -il admob`, 91 files incl. the SDK; the non-SDK list):
  - engine: `engine/core/System.{h,cpp}` (`AdMobRating` enum `System.h:24`, virtuals `System.h:454-459`, stubs
    `System.cpp:568-586`); `engine/core/script/LuaBridgeAddon.h:106`; `engine/core/script/binding/CoreClassesLua.cpp:1749-1753`
    (`AdMobRating` namespace) and `:3804-3808` (5 `System` statics);
    `engine/platform/android/{JasperEngineAndroid.{h,cpp}:389-453, NativeEngine.{h,cpp}}` (JNI fields and lookups
    `NativeEngine.cpp:978-1018`), `engine/platform/android/java-admob/` (AdMobWrapper.java),
    `engine/platform/android/java/.../MainActivity.java:31-32,75-92,193-195` (`createOptionalWrapper` exists only
    for AdMob); `engine/platform/apple/JasperEngineApple.{h,mm}` (`:39-44,77-103,582-630`),
    `engine/platform/apple/ios/AdMobAdapter.{h,m}`; `engine/CMakeLists.txt:24-26,830-847` (`JASPERENGINE_ADMOB`).
  - templates: `engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj` (36 lines: build files `:554-561`,
    file refs `:5422-5430`, Frameworks phase `:8661-8667`, groups `:9810-9811,:11087-11095`, Sources `:17762`,
    `JASPERENGINE_ADMOB` defines `:21185,:21251`); `engine/workspaces/xcode/ios/Info.plist:23-24`
    (`GADApplicationIdentifier` = Google's sample id) and `:27-228` (`SKAdNetworkItems`, 50 ad-network ids in every
    iOS game); `engine/workspaces/androidstudio/app/src/main/AndroidManifest.xml:13-15` (sample APPLICATION_ID);
    `build.gradle:114-121,136-138`.
  - editor: `editor/Project.{h,cpp}` (`admobEnabled/admobAppId` `Project.h:714-715,756-757`, sample ids
    `:774-779`, `isAdMobAppIdValid` `Project.cpp:2915`), `editor/Stream.cpp:2796-2797,2870-2871,3510-3511,3589-3590`,
    `editor/settings/SettingsRows.cpp:1110-1114,1346-1350,2707-2708,2723-2724`,
    `editor/window/dialog/ProjectSettingsWindow.{h,cpp}` (`.h:135-136,154-155`; `.cpp:1032-1033,1059-1060,1680,
    3138-3157,3314-3333,4473-4474,4507-4508`), `editor/Exporter.cpp:529-544,3617-3619,4109-4116,4248-4252,
    4589-4606,4695-4699,5253` (comment), `editor/ExporterPrebuilt.cpp:179-183`.
  - tests: `editor/tests/AdMobSettingsTest.cpp`, `editor/tests/export/BuildMethodTest.cpp:102-118`,
    `tools/mcp/tests/test_build.py:229-305` (`test_admob_export`), comment `tools/mcp/tests/test_native_bridge.py:201`,
    `tools/lua_api/tests/fixtures/regex_symbols.txt:1950,2030-2033,5035,5038,5042,5055`.
  - tools: `tools/check-cxx-subset.py:24` (`SKIPPED_PREFIXES`).
  - docs: `docs/shipping.md:20`, `docs/mcp/guide/shipping.md:14`, `docs/mcp/guide/lua-api.md:869-872,3077`,
    `docs/mcp/guide/native-bridge.md:392` (`GADApplicationIdentifier` example), `docs/testing.md:210`,
    `docs/specs/S30-shipping.md:91`, `docs/specs/S25-editor-search-organisation.md:685`; the
    `ProjectSettingsWindow` plist hints name `GADApplicationIdentifier` (`.cpp:3618`).

### 4.2 No privacy manifest (EXTRA-FINDINGS; no Doriax commit: Doriax has none either, verified `git grep xcprivacy`)

- No `PrivacyInfo.xcprivacy` anywhere in Jasper (verified `git grep -n xcprivacy`: nothing). Apple: "Starting May
  1, 2024, apps that don't describe their use of required reason API in their privacy manifest file aren't accepted
  by App Store Connect" (fetched today). The page names iOS, iPadOS, tvOS, visionOS and watchOS, not macOS.
- Engine uses of required-reason APIs on Apple (verified by grep, excluding the Google SDK):
  - **UserDefaults**: `NSUserDefaults standardUserDefaults` in `engine/platform/apple/JasperEngineApple.mm:366-400`
    (UserSettings). Reason `CA92.1` (only the app reads what it writes).
  - **FileTimestamp**: `stat`/`fstat`/`st_mtimespec`/`last_write_time` in `engine/core/io/watch/FileIdentity.cpp:56`,
    `FileWatcher.cpp:112-114`, `SharedFileReader.cpp:161`, `engine/core/io/AtomicFile.cpp:84,161,190`,
    `engine/core/log/sinks/RotatingFileSink.cpp:47,103`, `engine/core/media/MediaCache.cpp:498`,
    `engine/core/diag/CrashHandler.cpp:144`, plus vendored code (tinygltf, miniaudio, harfbuzz, plutovg). All on
    files in the app's container. Reason `C617.1`.
  - **SystemBootTime**: `mach_absolute_time` in `engine/libs/sokol/sokol_time.h:219,237`, `sokol_app.h:2606`,
    `engine/libs/box2d/src/timer.c:156-170`, miniaudio `:20020`. Reason `35F9.1` (elapsed time between events).
  - **DiskSpace**, **ActiveKeyboards**: no use found (`statfs`, `NSFileSystemFreeSize`, `volumeAvailableCapacity`,
    `std::filesystem::space`, `activeInputModes`: none).
- The three iOS build paths and two macOS bundle paths have no place for one:
  - exported Xcode project: Resources phases `project.pbxproj:16578-16600` (iOS target `7162F8CC...`, macOS
    `7162F8E2...`);
  - CMake iOS app (`engine/CMakeLists.txt:817-821` `PLATFORM_RESOURCES`), CMake macOS Xcode-generator app
    (`:680-683`), CMake macOS player bundle (`:773-779`);
  - prebuilt iOS: `engine/platform/apple/ios/player-template` (Resources phase has Content, storyboards, assets),
    staged by `editor/platform/IosProject.cpp:55-75`, packaged by `tools/players/ios_player.sh:49-51`;
  - prebuilt macOS: `editor/platform/MacBundle.cpp:81-140` writes `Contents/Info.plist` and signs, no manifest.
- Games cannot add entries (ad SDK tracking, domains, collected data). Static native plugins cannot carry their
  own manifest (no bundle at run time); only dynamic frameworks embedded in the app do.

### 4.3 Prebuilt builds drop native plugins silently (EXTRA-FINDINGS)

- `Exporter::chooseBuildMethod` (`editor/ExporterPrebuilt.cpp:124-187`) knows C++ scripts, `ProjectBuild.cmake`,
  capacities, AdMob, forced Compile. It has no native-plugin reason, and its C++ walk uses `getScriptDirs()`
  (`:128`), so `plugins/native/cpp` (part of `getBuildScriptDirs()`, `editor/Project.cpp:1861-1876`) is missed:
  a Linux prebuilt build of a project whose only C++ is a `JASPERENGINE_NATIVE_PLUGIN` ships without it.
- `copyNativePlugins` runs only in `runExport` (`editor/Exporter.cpp:1124`); `runPrebuilt`
  (`editor/ExporterPrebuilt.cpp:418-519`) never looks at `plugins/native`:
  - Android prebuilt (`finalizeAndroid` `:1101-1159`, `platform::buildAndroidPackage`): no Java/Kotlin, AARs,
    resources, Gradle dependencies; `writeAndroidManifest` (`editor/platform/AndroidPackager.cpp:85-127`) reads
    only `settings.permissions` of Android's settings, not `extraPermissions`, `manifestXml`,
    `manifestApplicationXml` (verified by grep of `settings.` uses).
  - iOS prebuilt (`finalizeIOS` `:1161-1192`, `IosProjectSettings` `editor/platform/IosProject.h:21-34`): no
    plugin sources, SDKs, `ios.plist_entries`, `ios.plist_xml`, `ios.frameworks`, `ios.swift_packages`,
    `ios.entitlements_xml`, `ios.deployment_target`, CocoaPods.
  - macOS prebuilt (`finalizeMacOS` `:960-985`, `MacBundle.cpp:27-45`): no `macos.plist_entries`, plugins or
    frameworks.
  - Web prebuilt (`finalizeWeb` `:1019-1098`, `composeWebPage` `:990-1017`): no `plugins/native/web/*.js`, no
    `web.script_urls`; the Compile build has both (`Exporter.cpp:3689-3770`).
- The prebuilt player never calls `NativeBridge::setPlatformOnlyPlugins` (the Compile build's generated `main`
  does, `editor/Generator.cpp:2625-2650`; `engine/player/PlayerMain.cpp` and `engine/core/player/PlayerConfig.h`
  have nothing native, verified grep), so a call to an Android-only plugin in a prebuilt Linux game fails with
  `not_found` instead of `unavailable`, and a shipped `lua/native_mocks/<Plugin>.lua` (mocks ship by design,
  native-bridge spec decision 6) answers as if it were the SDK (inferred).
- The Compile build for macOS through CMake leaves Swift plugins out by design (`NativePlugins::macosCMake`,
  `editor/util/NativePlugins.h:84-88`) and says so only at run time (`unavailable`).

### 4.4 Small Apple build fixes

- **458bad1a3** (Doriax, could): Jasper's Xcode export links AudioToolbox explicitly (`editor/Exporter.cpp:4951-4958`,
  `5252-5260`), but the CMake iOS app's `PLATFORM_LIBS` are `-ObjC` and `GameController` only
  (`engine/CMakeLists.txt:823-828`); the engine library links `Foundation AVFoundation CoreMedia CoreVideo`
  (`:2089`). AudioToolbox (miniaudio's AudioUnit calls, compiled as C++ so not autolinked by `-fmodules`) is named
  nowhere in `engine/CMakeLists.txt` (verified grep). The ARC part of the Doriax commit does not apply (no such
  switch in Jasper's adapter, and the adapter is deleted anyway).
- **9b95b353b** (Doriax, could): the Xcode template has 44 entries at iOS 15.0 and 44 at macOS 12.0, but the zstd
  targets added by `tools/textures/xcode_add_texture_libs.py` (cloned from an older lunasvg configuration) keep
  `IPHONEOS_DEPLOYMENT_TARGET = 13.0` (`project.pbxproj:23210,23237`) and `MACOSX_DEPLOYMENT_TARGET = 10.15`
  (`:23267,23294`) (verified). `ios.deployment_target` rewrites only `= 15.0;` (`editor/Exporter.cpp:5114`), so a
  game at 16.0 still builds zstd for 13.0.
- **323fc511e** (Doriax, could): `Project::copyEngineApiToProject` (`editor/Project.cpp:5880` area) walks the whole
  engine folder; the only `.xcframework`/`.framework` trees in `engine/` are the Google SDK's (verified
  `git ls-files engine | grep -E '\.(xc)?framework/'`: none other). Deleting the SDK makes it moot; a skip of SDK
  bundles stays as a guard (decision 9).

## 5. Product decisions

1. **AdMob goes entirely; the native bridge is the one way to integrate ads and purchases.** Owner decision. Every
   surface of 4.1 is deleted, not hidden. No load-time warning for old `admobEnabled` keys (owner: no deprecation
   path; nested unknown keys of `project.yaml` are not preserved by the file overlay, so they vanish at the next
   save, inferred from `DocumentFiles::overlayUnknownRootKeys` being root-only, `editor/Project.cpp:6492`).
   Reason: a sunset SDK serving no ads, 73 MB copied into every export, and a second, weaker ads path beside the
   bridge.
2. **One privacy manifest per Apple app, composed at export** from three layers, in this order: the engine's
   declarations (fixed, always present, not removable), the project's additions (Project Settings), and the
   `PrivacyInfo.xcprivacy` files of the game's static native plugins. Unity copies plugin manifests into the
   Xcode project; Godot exposes tracking, domains, collected data and API reasons in the iOS export preset. We do
   both, so a human fills a form and a plugin author ships a file.
3. **The project's additions are shared by iOS and macOS** (`apple.privacy_*`, one YAML block `apple.privacy`),
   because the game's data use does not change with the platform; platform-specific SDKs declare theirs through
   plugin manifests in `plugins/native/ios` or `plugins/native/macos`, which merge only into that platform. The UI
   section appears in Player > iOS and Player > macOS, both editing the same data, titled "Privacy Manifest (iOS
   and macOS)". Reason: one place to keep right, no duplicated form; per-platform precision where it is real.
4. **macOS gets the manifest too** (`Contents/Resources/PrivacyInfo.xcprivacy`), although Apple's required-reason
   page names only iOS, iPadOS, tvOS, visionOS and watchOS. Reason: Xcode's privacy report reads it on every
   platform, the mandate asks for it, it costs one small file, and declaring what the code does is never wrong.
5. **Merging rules are unions**: tracking = any layer says true; domains, purposes, reasons = sorted unions;
   collected data merged per type (linked and tracking = any). Output is deterministic (keys sorted, engine entries
   first in their arrays only through sorting), so re-exports write identical files.
6. **Per platform, every native input is either carried or a Compile reason**, decided by what the prebuilt path can
   do with the same result as the source path:

   | Target | Carried into the prebuilt build | Compile reason |
   |---|---|---|
   | Linux, Windows | (nothing native applies) | `plugins/native/cpp` sources |
   | macOS | `macos.plist_entries`, privacy manifest | `cpp`; `apple/` or `macos/` sources or SDKs; `macos.frameworks` |
   | iOS | `apple/` + `ios/` Objective-C/Swift sources and SDKs (`.framework`, `.xcframework`, `.a`), `ios.frameworks`, `ios.swift_packages`, `ios.plist_entries`, `ios.plist_xml`, `ios.entitlements_xml`, `ios.deployment_target`, privacy manifest | `cpp`; `ios.cocoapods` (needs `pod install` and a workspace); a runtime without the `nativeplugins` feature when sources exist |
   | Android | `android.extra_permissions`, `android.manifest_xml`, `android.manifest_application_xml` (without `tools:` merge rules) | `cpp`; `android/` sources, `libs/`, `res/`, plugin `AndroidManifest.xml`, `proguard-rules.pro`; `android.gradle_dependencies` / `_repositories` / `_properties`; `tools:` attributes in the manifest XML |
   | Web | `plugins/native/web/*.js`, `web.script_urls` | `cpp` |

   iOS carries code because the prebuilt iOS build *is* an Xcode project that `xcodebuild` compiles (or that the
   user opens on a Mac): adding Objective-C/Swift files to it is what the source export does, without compiling
   the engine. This is the case of the project's own game (Wortise, RevenueCat on iOS). Android cannot: the prebuilt
   APK is linked by `aapt2` from a prebuilt dex; Java needs Gradle and d8.
7. **The reason names the files.** Reason and carried strings name up to three project-relative paths and "+N more",
   like the existing C++ script reason (`ExporterPrebuilt.cpp:152-156`). The Auto-to-Compile error on a platform
   this computer cannot compile for says what to do and no longer says "C++ toolchain" when the code is Java:
   "needs to compile native code (Java/Kotlin plugins (...)): an Android build with native code is made from the
   source project (File > Build Settings > Export source, then Android Studio)".
8. **A mock answering in a shipped build is visible, not forbidden.** Mocks keep shipping (native-bridge spec
   decision: desktop builds of a mobile game keep working). The build warns `W-NATIVE-MOCK` for each plugin whose
   only answer on the target platform is a Lua mock, and the shipped game logs one warning per plugin the first
   time its mock answers. The prebuilt player learns the platform-only plugins from `game.json`, so its errors
   match the Compile build's (`unavailable` with where it runs).
9. **The engine API sync skips bundle directories** (`.framework`, `.xcframework`, `.bundle`) even after the SDK is
   gone: cheap, and the next vendored SDK will not reintroduce the bug.
10. **The CMake iOS app links the same engine framework list as the Xcode export**, one list kept in two places
    and checked equal by a test (not a shared file: the CMake file must stay standalone for engine builds).
11. **The privacy manifest is checked at build end.** The payload check of prebuilt builds
    (`editor/publish/PayloadValidator.cpp`) requires a parseable `PrivacyInfo.xcprivacy` in iOS and macOS payloads,
    so a regression fails the build instead of App Review. The export warns `W-PRIVACY-ATT` when tracking is on
    and `ios.plist_entries` has no `NSUserTrackingUsageDescription` (App Tracking Transparency needs it), and
    `W-PRIVACY-DOMAINS` when tracking is on with no tracking domain.

## 6. Design

### 6.1 Remove AdMob (engine, platforms, templates)

- Delete `engine/platform/apple/GoogleMobileAdsSdkiOS-10.14.0/`, `engine/platform/apple/ios/AdMobAdapter.{h,m}`,
  `engine/platform/android/java-admob/`.
- `engine/core/System.h/.cpp`: delete `AdMobRating`, `initializeAdMob`, `setMaxAdContentRating`,
  `loadInterstitialAd`, `isInterstitialAdLoaded`, `showInterstitialAd`. `LuaBridgeAddon.h`: delete
  `Stack<AdMobRating>`. `CoreClassesLua.cpp`: delete the `AdMobRating` namespace and the five statics.
- Android: delete the overrides and `hasAdMob` (`JasperEngineAndroid.{h,cpp}`), the JNI fields and lookups
  (`NativeEngine.{h,cpp}`: `getAdMobWrapperRef`, `adMobWrapperObjRef/ClsRef`, the five method ids),
  `MainActivity.admobWrapper`, `createOptionalWrapper`, `getAdMobWrapper`.
- Apple: delete the `JASPERENGINE_ADMOB` block, `warnNoAdMob`, the overrides (`JasperEngineApple.{h,mm}`). Keep
  `isForeignKey`'s prefixes (consent strings of any ads SDK still live in `standardUserDefaults`); reword the comment
  at `:366` to "the app's SDKs (ads consent: IABTCF_*, UMP ...)".
- `engine/CMakeLists.txt`: delete the option (`:24-26`) and the `if(JASPERENGINE_ADMOB)` block (`:830-847`).
- Xcode template: remove all 36 AdMob/Google SDK lines listed in 4.1 (build files, file refs, phase entries, group
  children and the now-empty SDK group if one exists, `JASPERENGINE_ADMOB,` defines, and any `FRAMEWORK_SEARCH_PATHS`
  entry naming `GoogleMobileAdsSdkiOS`). Edit with the repo's pbxproj tooling style (`tools/xcode_sync_*.py` keep
  ids stable); the result must parse with `tools/mcp/tests`' `pbxproj` module.
- `engine/workspaces/xcode/ios/Info.plist`: delete `GADApplicationIdentifier` and `SKAdNetworkItems`.
  `AndroidManifest.xml`: delete the sample `APPLICATION_ID` meta-data. `build.gradle`: delete the `java-admob`
  source dir and its comment, the `play-services-ads` dependency and the `if (!jasperPlayer)` around it, and
  "a player has no ads (a project with AdMob compiles)" in the player comment.
- `tools/check-cxx-subset.py`: drop the `GoogleMobileAds` prefix (the tuple becomes empty; keep the variable).

### 6.2 Remove AdMob (editor, settings, MCP, tests, docs)

- `editor/Project.h/.cpp`: delete the four fields, the two sample ids, `isAdMobAppIdValid`.
- `editor/Stream.cpp`: delete the four encode and four decode lines (old keys are ignored on load; decision 1).
- `editor/settings/SettingsRows.cpp`: delete the four rows and the four search entries. `set_project_settings` then
  refuses `android.admob_enabled` with its unknown-key message (existing behaviour of the registry).
- `editor/window/dialog/ProjectSettingsWindow.{h,cpp}`: delete the members, the rows, the apply lines; replace "AdMob"
  in the build-method note (`.cpp:1680`) by "native plugins the prebuilt player cannot carry"; replace
  `GADApplicationIdentifier` in the plist hint (`:3618`) by `NSUserTrackingUsageDescription, ITSAppUsesNonExemptEncryption`.
- `editor/Exporter.cpp`: delete `isAdMobIncluded`, the CMake `set(JASPERENGINE_ADMOB ...)` line, the Gradle line
  removal, the manifest meta-data, the Xcode build-file removal loop, the plist value handling; reword the comment
  at `:5253` ("an iOS export once linked AVFoundation only through an ads SDK's module").
- `editor/ExporterPrebuilt.cpp`: delete reason (d) and the `"ads"` feature check.
- Tests: delete `editor/tests/AdMobSettingsTest.cpp` (and its `EditorTests.cmake` entry); `BuildMethodTest`
  `AdMobAndForcedCompile` becomes `ForcedCompile`; `test_build.py::test_admob_export` becomes `test_no_admob`
  (10.2); regenerate `tools/lua_api/tests/fixtures/regex_symbols.txt` (the build regenerates the Lua API data;
  the fixture is refreshed with the generator the test uses).
- Docs: 9 files of 4.1. `lua-api.md`: delete the ads clause of the System paragraph and the `AdMobRating` row, and
  add one sentence: "Ads and purchases: the native bridge (`jasper://docs/native-bridge`, section 10)".
  `native-bridge.md`: section 10 gains the worked example (6.9). Historical spec text (`S25`, `S30`) gets a one-line
  edit each ("AdMob" -> "native plugins") so no doc tells a reader the feature exists.

### 6.3 Privacy manifest: data model and composition (editor)

New `editor/util/ApplePrivacy.{h,cpp}` (namespace `jasperengine::editor::ApplePrivacy`):

```cpp
struct AccessedApi { std::string type; std::vector<std::string> reasons; };   // type: short name "FileTimestamp"
struct CollectedData { std::string type; bool linked = false; bool tracking = false;
                       std::vector<std::string> purposes; };                   // type "DeviceID", purposes "Analytics"
struct Manifest {
    bool tracking = false;
    std::vector<std::string> trackingDomains;
    std::vector<CollectedData> collectedData;
    std::vector<AccessedApi> accessedApis;
    bool operator==(const Manifest&) const = default;
    bool empty() const;
};
struct Source { std::string what; std::string origin; };   // "UserDefaults CA92.1", "engine" | "project" | "plugins/native/ios/Ads/PrivacyInfo.xcprivacy"
struct Composed { Manifest manifest; std::vector<Source> sources; std::vector<std::string> warnings; };

const Manifest& engineManifest();     // UserDefaults CA92.1, FileTimestamp C617.1, SystemBootTime 35F9.1; tracking false
// the names Apple accepts (short form, without the NSPrivacy... prefix) and each API type's reasons
const std::vector<std::string>& apiTypes();             // FileTimestamp SystemBootTime DiskSpace ActiveKeyboards UserDefaults
const std::vector<std::string>& reasonsOf(const std::string& apiType);
const std::vector<std::string>& dataTypes();            // the 35 of Apple's list
const std::vector<std::string>& purposes();             // ThirdPartyAdvertising DeveloperAdvertising Analytics ProductPersonalization AppFunctionality Other
std::string validate(const Manifest&);                  // "" or the first problem, naming the entry
Manifest merge(const Manifest& a, const Manifest& b);   // decision 5
std::string toXml(const Manifest&);                     // a full plist document, tabs, sorted, trailing newline
bool parse(const std::string& xml, Manifest& out, std::string& error);   // tinyxml2; long or short names; unknown keys -> error text
// engine + project + the static plugin manifests of plugins/native/{apple, ios|macos}
Composed compose(const fs::path& projectPath, const Manifest& project, const std::string& platform /* "ios" | "macos" */);
}
```

Apple's identifiers (fetched today from developer.apple.com JSON docs, and EXTRA-FINDINGS):

- API types `NSPrivacyAccessedAPICategory{FileTimestamp, SystemBootTime, DiskSpace, ActiveKeyboards, UserDefaults}`;
  reasons FileTimestamp `DDA9.1 C617.1 3B52.1 0A2A.1`, SystemBootTime `35F9.1 8FFB.1 3D61.1`, DiskSpace
  `85F4.1 E174.1 7D9E.1 B728.1`, ActiveKeyboards `3EC4.1 54BD.1`, UserDefaults `CA92.1 1C8F.1 C56D.1 AC6B.1`.
- Data types `NSPrivacyCollectedDataType` + `Name EmailAddress PhoneNumber PhysicalAddress OtherUserContactInfo Health
  Fitness PaymentInfo CreditInfo OtherFinancialInfo PreciseLocation CoarseLocation SensitiveInfo Contacts
  EmailsOrTextMessages PhotosorVideos AudioData GameplayContent CustomerSupport OtherUserContent BrowsingHistory
  SearchHistory UserID DeviceID PurchaseHistory ProductInteraction AdvertisingData OtherUsageData CrashData
  PerformanceData OtherDiagnosticData EnvironmentScanning Hands Head OtherDataTypes` (note Apple's `PhotosorVideos`).
- Purposes `NSPrivacyCollectedDataTypePurpose` + `ThirdPartyAdvertising DeveloperAdvertising Analytics
  ProductPersonalization AppFunctionality Other`.

`compose` collects plugin manifests: every file named `PrivacyInfo.xcprivacy` below `plugins/native/apple` and
`plugins/native/<platform>`, outside bundles; and the one at the root of a `.framework` (or of each library folder
of an `.xcframework`, first match) whose binary is a static archive (`isStaticArchive`, moved from `Exporter.cpp`
to `editor/util/AppleProjectText` with the other shared helpers, 6.6). Dynamic frameworks are embedded and keep
their own. A plugin manifest that does not parse is a warning naming it, and is skipped (the build goes on: the
engine's own entries still make the app acceptable); an unknown API type or reason in it is a warning too.

The project's data: `ApplePrivacySettings` in `editor/Project.h` holding one `ApplePrivacy::Manifest privacy`, with
`getApplePrivacySettings()`; YAML root key `apple`:

```yaml
apple:
  privacy:
    tracking: true
    trackingDomains: [ads.example.com]
    collectedData:
      - {type: DeviceID, linked: true, tracking: true, purposes: [ThirdPartyAdvertising]}
    accessedApis:
      - {type: DiskSpace, reasons: [E174.1]}
```

Written only when not empty; short names stored; long names accepted on read. Add `apple` to the project root keys
of `editor/StreamSchema.cpp:72` (and whatever `tools/check_stream_keys.py` reads).

### 6.4 Privacy manifest: editor UI and undo

- `ProjectSettingsWindow::drawApplePrivacySection()` called from `drawIOSSettings()` and `drawMacOSSettings()`,
  foldout "Privacy Manifest (iOS and macOS)", buffered in `m_applePrivacy` like the other platform fields:
  - "Engine (always included)": read-only rows from `engineManifest()` with their reason text ("UserDefaults: CA92.1
    (only this app reads what it saves)").
  - "Tracking" checkbox (`NSPrivacyTracking`), "Tracking Domains" (`drawStringList`, hostname check per row, red
    note on invalid), "Collected Data" table: Type combo (Apple's names spaced: "Device ID"), Linked, Tracking,
    Purposes (multi-select combo), remove button; "+ Add". "Required Reason APIs" table: API combo, Reasons
    multi-select with each code's one-line meaning in the tooltip.
  - "Plugin manifests" line: count and paths found by `compose` (refreshed with the Native Plugins summary).
  - Buttons "Preview iOS" / "Preview macOS": a read-only popup with the composed XML, its sources and warnings,
    and "Copy".
  - Warnings inline: tracking without domains, tracking without `NSUserTrackingUsageDescription` (iOS).
- Apply (OK): when `m_applePrivacy != project` push `ApplePrivacySettingsCmd(project, newValue)` to
  `getProjectCommandHistory()` (new `editor/command/type/ApplePrivacySettingsCmd.{h,cpp}`, modelled on
  `DebugSettingsCmd`: stores old and new, `execute`/`undo` set the struct, `markGeneratedSourcesDirty`, save
  project file; name "Change Privacy Manifest"). Edit > Undo with the project scope reverts it.
- Settings search entries: `apple.privacy_tracking` ("Player", "iOS", "Privacy Tracking", "privacy manifest
  xcprivacy tracking att app store"), and one each for the other three keys.

### 6.5 Privacy manifest: MCP

- `editor/settings/SettingsRows.cpp`: four rows, each `set` validating through `ApplePrivacy::validate` and applying
  one `ApplePrivacySettingsCmd` (`apply` true), description ending "Undoable (undo scope project). Details:
  jasper://docs/shipping#privacy-manifest.":
  - `apple.privacy_tracking` bool;
  - `apple.privacy_tracking_domains` list of hostnames (max 64, each a valid DNS name, no scheme);
  - `apple.privacy_collected_data` list of `{type, linked?, tracking?, purposes}` (closed objects; types and purposes
    in short or long form; duplicate types refused);
  - `apple.privacy_accessed_apis` list of `{type, reasons}` (reasons must belong to the type; the engine's own
    entries need not be repeated and are reported as already included, not stored twice).
  Refusals name the bad entry and list the valid values (`must be one of: FileTimestamp, ...`).
- `native_plugins` gains `action: "privacy_manifest"` with `platform: "ios" | "macos"` (default `ios`), read-only:
  structured `{platform, xml, tracking, tracking_domains, collected_data, accessed_apis, sources: [{what, origin}],
  plugin_manifests: [paths], warnings: [...]}`, text summary "iOS privacy manifest: 3 API types (engine 3), tracking
  on (1 domain), 1 collected data type; 1 plugin manifest". Schema via `McpSchema.h`, enum for `platform`.
  Regenerate `docs/mcp/TOOLS.md` and run `tool_budget.py --check`.

### 6.6 Privacy manifest: every Apple build path

Shared text helpers move out of `Exporter.cpp`'s anonymous namespace into `editor/util/AppleProjectText.{h,cpp}`
(namespace `editor::appletext`): `replacePlistStringValue`, `removePlistStringValue`, `writeNativePlistEntries`,
`replacePlistBoolValue`, the pbx helpers (`PbxRange`, `findPbxObject*`, `pbxListIds`, `addPbxListEntries`,
`setPbxBuildSetting`, `addPbxBuildSettingList`, `addPbxObjectKey`, `pbxString`, `xcodeObjectId`, `findXcodeTarget`,
`leadingObjectId`), `isStaticArchive`, `frameworkBinary`, `xcframeworkBinary`, and `appleEngineFrameworksIOS/MacOS`
(declared in the header, so the tests of 10.1 read the list). `Exporter.cpp`, `IosProject.cpp`, `MacBundle.cpp` and
`ApplePrivacy.cpp` include it. Pure moves; no behaviour change in this step (commit it alone).

- **Committed engine defaults.** `engine/workspaces/xcode/ios/PrivacyInfo.xcprivacy` and
  `engine/workspaces/xcode/macos/PrivacyInfo.xcprivacy`, each exactly `toXml(engineManifest())` (a gtest keeps
  them equal, 10.1). They make the engine's own Xcode workspace and CMake builds correct without the editor.
- **Exported Xcode project.** Template pbxproj: two file references (`path = ios/PrivacyInfo.xcprivacy` and
  `path = macos/PrivacyInfo.xcprivacy`, `lastKnownFileType = text.xml`, in the `ios` and `macos` groups next to
  `Info.plist`) and one build file in each app target's Resources phase (`7162F8CC...` iOS, `7162F8E2...` macOS).
  `writeAppleProjectSettings` (`Exporter.cpp:4373`) writes `compose(..., "ios")` and `compose(..., "macos")` over
  the copied files with `FileUtils::writeIfChanged`, and emits the composed warnings as build warnings.
- **CMake iOS app** (`engine/CMakeLists.txt:817-821`): append `${JASPERENGINE_ROOT}/workspaces/xcode/ios/PrivacyInfo.xcprivacy`
  to `PLATFORM_RESOURCES` (the bundle root, where `BUNDLE_RESOURCES_DIR` points). The export's copy of
  `workspaces/` already holds the composed file, so the CMake source export needs no new variable.
- **CMake macOS Xcode-generator app** (`:680-683`) and **CMake macOS player bundle** (`:773-779`, inside
  `if(JASPERENGINE_PLAYER)`): append `.../workspaces/xcode/macos/PrivacyInfo.xcprivacy` to `PLATFORM_RESOURCES`;
  the `RESOURCE` target property (`:2406`) places it in `Contents/Resources` for bundle targets. Bare non-bundle
  macOS executables get none (they cannot be submitted to the App Store; no bundle to hold it).
- **Prebuilt iOS.** `engine/platform/apple/ios/player-template`: add `PrivacyInfo.xcprivacy` (file ref + Resources
  build file, ids stable). `tools/players/ios_player.sh` copies `engine/workspaces/xcode/ios/PrivacyInfo.xcprivacy`
  into `$OUT/template/` (and, for 6.7, `engine/platform/apple/JasperNative.h` into `$OUT/template/include/`).
  `IosProjectSettings` gains `std::string privacyManifest;` written to `projectDir/PrivacyInfo.xcprivacy` after the
  template copy (`IosProject.cpp:73`), so a runtime packaged before this change still gets the file (the template's
  Resources phase lacking it is then a warning `W-IOS-PRIVACY-RUNTIME`: "the iOS player runtime predates privacy
  manifests: reinstall it"; the build adds the build file itself when missing, using `appletext` helpers, so the
  manifest ships either way).
- **Prebuilt macOS.** `MacBundleSettings` gains `privacyManifest` and `plistEntries` (+ raw XML none on macOS);
  `finishMacBundle` writes `Contents/Resources/PrivacyInfo.xcprivacy` and merges the plist entries into
  `infoPlist(s)` before signing. The macOS runtime bundle already holds the engine default from CMake.
- **Payload check** (`editor/publish/PayloadValidator.cpp`, the iOS and macOS content roots at `:313-319`): finding
  "no PrivacyInfo.xcprivacy" / "PrivacyInfo.xcprivacy does not parse: ..." for iOS (`<app>/PrivacyInfo.xcprivacy`)
  and macOS (`<app>.app/Contents/Resources/PrivacyInfo.xcprivacy`). Publish verify gets it for free.

### 6.7 Prebuilt builds carry or compile (editor)

`editor/Exporter.h`: `BuildMethodChoice` gains `std::vector<std::string> carried;`, `ExportResult` gains
`std::vector<std::string> carried;`. New `editor/export/NativeNeeds.{h,cpp}` (pure, testable, no Exporter state):

```cpp
struct NativeNeeds {
    std::vector<std::string> compileReasons;   // section 5 decision 6, platform by platform
    std::vector<std::string> carried;
    std::vector<std::string> warnings;         // W-NATIVE-MOCK texts
};
NativeNeeds nativeNeedsFor(const Project&, BuildPlatform, const PlayerInfo* player);
```

It reads `NativePlugins::scan(projectPath)`, `listPlugins`, `listMocks(luaDir)`, `platformOnlyPlugins` and the
settings structs. `chooseBuildMethod` replaces reason (d) by `nativeNeedsFor(...)` and walks `getBuildScriptDirs()`
for the C++ scan (a `plugins/native/cpp` source is reported as "C++ native plugins (plugins/native/cpp/X.cpp)",
not as a C++ script). The iOS source rule: sources or SDKs present and `player` known without feature
`"nativeplugins"` -> reason "the installed ios player runtime cannot take native plugins (update it)"; `player`
unknown -> carried (this editor's runtimes have it). `tools/players/package_player.py` adds `"nativeplugins"` to
the iOS runtime's `features`.

`runPlayerBuild`: the Auto-to-Compile error for a non-host platform names "native code" (decision 7). `runPrebuilt`
puts `choice.carried` into the result and calls, per platform:

- **Web** (`finalizeWeb`): copy `plugins/native/web/*.js` to `stagingDir/plugins/` (name order, the Compile build's
  rule at `Exporter.cpp:3689-3770`), and `composeWebPage` gains `scriptUrls` and `pluginScripts` parameters that it
  inserts as `<script src>` tags in the head, SDK URLs first, then the bridge stub and the plugin files, before the
  loader, exactly as the Compile build's page orders them (read `Exporter.cpp:3689-3700` and reuse its function for
  the stub; extract it into `NativeNeeds.cpp` or `NativePlugins` if it is a lambda).
- **Android** (`AndroidPackage` gains `extraPermissions`, `manifestXml`, `manifestApplicationXml`):
  `writeAndroidManifest` fills two new markers of `engine/platform/android/player/AndroidManifest.template.xml`,
  `{{NATIVE_MANIFEST_XML}}` (after `{{PERMISSIONS}}`) and `{{NATIVE_APPLICATION_XML}}` (last child of
  `<application>`), declares `xmlns:tools` only when used (never, since `tools:` makes the build Compile), and
  validates the fragments with `NativePlugins::xmlFragmentError` (an error fails the build naming the setting).
- **iOS** (`finalizeIOS`): copy `plugins/native/{apple,ios}` into `settings.projectDir/plugins/native/` with the
  same link rules as `copyNativePlugins` (refactor it to take a source and destination); fill `IosProjectSettings`
  with `privacyManifest`, `plistEntries`, `plistXml`, `entitlementsXml`, `deploymentTarget`; and run the native
  Xcode step on the staged template. That step is `addNativePluginsToXcode`, generalized: it becomes a free function
  in `editor/util/AppleNativeXcode.{h,cpp}`:

  ```cpp
  struct XcodeNativeTarget {
      std::string name;                         // "JasperEngine iOS", "JasperEngine macOS", "JasperEnginePlayer"
      std::vector<std::string> folders;         // {"apple", "ios"}
      const std::vector<std::string>* systemFrameworks;
      std::string runpath, bridgingHeader;
      bool engineFrameworks;                    // the source export links appleEngineFrameworks*; the template has its own
  };
  struct XcodeNativeInput {
      fs::path nativeRoot;                      // the copied plugins/native
      std::string pathPrefix;                   // "$(SRCROOT)/../../project/plugins/native" or "$(SRCROOT)/plugins/native"
      std::string headerDir;                    // "$(SRCROOT)/../../platform/apple" or "$(SRCROOT)/include" (JasperNative.h)
      std::vector<XcodeNativeTarget> targets;
      const IOSProjectSettings* ios; const MacOSProjectSettings* macos;
  };
  bool addNativePluginsToXcode(std::string& pbx, const XcodeNativeInput&, std::vector<std::string>& warnings, std::string& error);
  bool writeNativeAppleFiles(const XcodeNativeInput&, std::string& error);   // bridging headers, entitlements
  ```

  The source export calls it with its two targets (byte-identical output to today: the existing
  `test_native_bridge.py::test_source_export` is the guard); `IosProject` calls it with the one template target.
  Template additions: `HEADER_SEARCH_PATHS = "$(SRCROOT)/include"` is set by the step only when plugins exist.
- **macOS**: `macos.plist_entries` go to `MacBundleSettings.plistEntries` (6.6).
- **Compile path on macOS through CMake** (`runExport`, desktop mode, macOS host): Swift files in `apple/` or
  `macos/` -> warning `W-NATIVE-SWIFT-CMAKE` "Swift plugins (X.swift) build only in the Xcode project: File > Build
  Settings > Export source, then Xcode; this build answers them with `unavailable`".
- **Mock warnings**: `nativeNeedsFor` returns, for each plugin of `platformOnlyPlugins(plugins, target)` that has a
  `native_mocks/<Name>.lua`, `W-NATIVE-MOCK` "Native plugin \"Ads\" has no code for Windows: its Lua mock
  (lua/native_mocks/Ads.lua) answers in this build". Emitted by both methods (`warn(...)` in `runPlayerBuild`, and
  `Out::warning` in `runExport`).

UI: `ExportWindowShipping.cpp` shows under the Build Method row "Carried into the prebuilt build:" with the list (a
tooltip lists all), and the reasons as today. Project Settings > Native Plugins summary (`refreshNativeSummary`)
appends per platform "prebuilt: carried" or "prebuilt: compiles (reason)". MCP: `get_build_info` `platforms[*]`
gains `carried`; `build_project` result gains `carried`; CLI `ShippingCommands.cpp` prints `carried:` lines and
`report["carried"]`.

### 6.8 Prebuilt player: platform-only plugins and the mock warning (engine)

- `engine/core/player/PlayerConfig.{h,cpp}`: `std::map<std::string, std::string> nativePlatformOnly;` (JSON
  `"nativePlatformOnly": {"Ads": "Java/Kotlin plugins run only on Android"}`, omitted when empty; schema stays 1:
  an optional key, and players never run another editor's game.json).
  `writeGameConfig` fills it with `platformOnlyPlugins(plugins, runningOf(platform))` (`"desktop"` for
  Linux/Windows, `"macos-cmake"` for the prebuilt macOS player, which has no Swift, `"ios"`, `"android"`, `"web"`).
  `PlayerMain.cpp` calls `NativeBridge::setPlatformOnlyPlugins` before the first scene.
- `NativeBridge::setMockWarnings(bool)` (default false): when on, the first answer of a mock for a plugin listed
  as platform-only logs `Log::warn("Native plugin \"%s\" is answered by its Lua mock on %s: its native code runs on
  %s", ...)` once per plugin (a `std::set` beside `warnedUnavailable`). Turned on by `PlayerMain.cpp` and by the
  generated `main` of Compile builds (`Generator.cpp`, next to `setPlatformOnlyPlugins`), never by the editor.

### 6.9 Docs and agent guide

- `docs/shipping.md`: the Compile list loses AdMob and gains "native plugins the prebuilt player cannot carry"
  with the table of decision 6; new section "Privacy manifest (iOS and macOS)": what the engine declares and why,
  the settings, plugin manifests, dynamic frameworks, the warnings, where the file lands per build path.
- `docs/mcp/guide/shipping.md`: same in agent form, with the four keys, `native_plugins privacy_manifest`,
  `carried` and the warning codes.
- `docs/mcp/guide/native-bridge.md`: section 8 "Export" states what prebuilt builds carry per platform; section 9
  gains the `apple.privacy_*` keys and plugin manifests; section 10 gains "Worked example: interstitial ads with
  Google Mobile Ads": Android (Java plugin with `onJasperInit` + UMP consent + `InterstitialAd.load` and
  `show` completing a `NativeCall`, `pauseEngine`/`resumeEngine`, `android.gradle_dependencies`
  `com.google.android.gms:play-services-ads:<current>` with the note to take the current version from Google's
  release notes, `android.manifest_application_xml` with the APPLICATION_ID meta-data, `android.extra_permissions`
  AD_ID), iOS (Objective-C plugin, `ios.swift_packages` for Google's package, `ios.plist_entries`
  `GADApplicationIdentifier` and `NSUserTrackingUsageDescription`, `ios.plist_xml` `SKAdNetworkItems`, privacy
  tracking on with Google's domains left to the SDK's own manifest since its framework is dynamic), the Lua side
  (`Native.callAsync("Ads", "showInterstitial", {unit}, cb, {owner = self, timeout = 60})`) and the mock. Lua blocks
  pass the docs' Lua block test.
- `docs/mcp/guide/lua-api.md`, `docs/testing.md`, `S25`, `S30`: 6.2.
- Agent guide (`editor/mcp/McpInstructions.cpp`, budget): no new text is needed; if the shipping line has room,
  append "Apple privacy: native_plugins privacy_manifest" (check `tool_budget.py --check`).

## 7. Behaviour rules and edge cases

1. A project with no `apple.privacy` block and no plugin manifests ships exactly the engine's manifest, byte-equal
   to the committed `PrivacyInfo.xcprivacy` files.
2. Project entries that repeat an engine entry are accepted and stored once (MCP reports "already included by the
   engine"); the composed manifest never lists a type twice.
3. `apple.privacy_accessed_apis` with a reason not of its type is refused (MCP error; the window shows the row red
   and OK is still allowed only after it is fixed, like other invalid overrides there).
4. Plugin manifest inside a dynamic `.framework`/`.xcframework`: not merged (embedded with its framework). Inside a
   static one: merged. A `.a` beside a loose `PrivacyInfo.xcprivacy` in the same folder: merged (loose file rule).
5. A plugin manifest in `plugins/native/macos` never reaches iOS and vice versa; `apple/` reaches both.
6. `NSPrivacyTracking` true with no domains: warning `W-PRIVACY-DOMAINS`, build continues. Tracking true on iOS
   without `NSUserTrackingUsageDescription` in `ios.plist_entries`: warning `W-PRIVACY-ATT`.
7. A plugin manifest that does not parse: warning naming the path, skipped; the composed manifest still holds the
   engine's and the project's entries.
8. A prebuilt iOS build with plugins on a computer without Xcode: the staged project (with the plugins) is the
   result and the existing "open on a Mac" note stands.
9. Swift plugins in the prebuilt iOS template: the step sets `SWIFT_VERSION 5.0` and the bridging header exactly as
   the source export does; the bridging header imports `JasperNative.h` from `include/`.
10. Android `manifest_xml`/`manifest_application_xml` with `tools:` attributes -> Compile reason; without -> carried.
11. A forced Prebuilt method with any Compile reason fails listing every reason (existing behaviour, now with the
    native ones).
12. Linux/Windows builds of a project whose plugins are mobile-only stay prebuilt (nothing applies there) and warn
    `W-NATIVE-MOCK` only for plugins that have a mock; without a mock, calls fail with `unavailable` (prebuilt now
    too, via `game.json`).
13. Re-export with the same inputs writes byte-identical manifests, Info.plists and pbxproj files.
14. The engine API sync ignores directories with extensions `.framework`, `.xcframework`, `.bundle` anywhere below
    the engine folder (`disable_recursion_pending`).

## 8. Performance and cost

- Runtime: nothing in the frame. The privacy manifest is a ~1.5 KB file read only by the OS and App Store tools.
  `setMockWarnings` adds one `std::set` lookup on the mock path only (mocks are Lua calls already); off in the
  editor. `nativePlatformOnly` is a small map read once at start.
- Build: composing manifests scans `plugins/native/{apple,ios|macos}` once per Apple build (the same scan the
  exporter does already). `nativeNeedsFor` is one `NativePlugins::scan` per method decision; the Build Settings
  window caches it in `refreshPlayerState` as today.
- Removal wins: every export stops copying 73 MB (637 files) of SDK; the repository loses it; iOS/Android games no
  longer risk carrying a sunset SDK.

## 9. Compatibility

- AdMob: owner decision, no compatibility. Old `ios.admobEnabled`/`admobAppId` and `android.*` keys are ignored on
  load and dropped at the next save; `System.initializeAdMob` and the other four are nil in Lua (a script calling
  them errors with Lua's own message). No format version bump (a removal of optional keys).
- `apple.privacy`: a new optional root block with defaults; old projects load unchanged and do not write it until
  set. No version bump (new fields get defaults).
- `game.json`: an optional key, schema 1 unchanged.
- Player runtimes: an iOS runtime without `nativeplugins` makes builds with Apple plugin sources Compile (named);
  without the privacy manifest in its template it still gets one (6.6). The Android template marker change ships
  with the editor's own runtimes (editor and runtime versions must match, `PlayerRuntimes`).
- The exported Xcode project of a project with no plugins differs only by the two `PrivacyInfo.xcprivacy`
  resources and the removed AdMob entries.

## 10. Tests

Red controls are marked **(red)**: they fail on `main` today and pass with the change. Run the red ones against
`main` first and record the failure in the PR.

### 10.1 gtest, `jasperengine-editor-tests`

- `editor/tests/export/BuildMethodTest.cpp`:
  - `BuildMethod.NativeCppPluginsCompile` **(red)**: `plugins/native/cpp/Steam.cpp` -> Linux choice is Compile with
    reason containing "plugins/native/cpp/Steam.cpp" (today: Prebuilt, no reason).
  - `BuildMethod.AndroidCodePluginsCompile` **(red)**: `plugins/native/android/java/com/me/Ads.java`, and separately
    `android.gradle_dependencies = {"a:b:1"}`, each -> Android Compile with the reason naming it; Linux stays
    Prebuilt.
  - `BuildMethod.AndroidManifestSettingsAreCarried` **(red)**: `extraPermissions = {"com.google.android.gms.permission.AD_ID"}`
    -> Android Prebuilt, `carried` names it; `manifestXml` with `tools:node` -> Compile.
  - `BuildMethod.MacObjectiveCPluginsCompile` **(red)**: `plugins/native/macos/Store.m` -> macOS Compile.
  - `BuildMethod.IosPluginsAreCarried`: `plugins/native/ios/Device.m` + `ios.swift_packages` -> iOS Prebuilt with
    both in `carried`; with a `PlayerInfo` lacking `nativeplugins` -> Compile naming the runtime; `ios.cocoapods`
    -> Compile.
  - `BuildMethod.WebPluginsAreCarried`: `plugins/native/web/poki.js` + `web.script_urls` -> Web Prebuilt, both carried.
  - `BuildMethod.MockOnlyPluginsWarn`: Android-only plugin + `lua/native_mocks/Ads.lua` -> Windows `warnings` has
    the `W-NATIVE-MOCK` text; without the mock file, none.
  - `BuildMethod.ForcedCompile` (rewritten from `AdMobAndForcedCompile`).
  - `AndroidPackage.CarriesNativeManifestSettings` **(red)**: `writeAndroidManifest` with extra permissions and both
    XML fragments puts them in the right places of the real template (`JASPER_TEST_REPO_DIR`).
- `editor/tests/export/ApplePrivacyTest.cpp` (new):
  - `EngineManifestDeclaresTheEngineApis`: UserDefaults CA92.1, FileTimestamp C617.1, SystemBootTime 35F9.1,
    tracking false, no data; `parse(toXml(m)) == m`.
  - `CommittedManifestsMatchTheEngine` **(red: the files do not exist)**: both repo files equal
    `toXml(engineManifest())`.
  - `ProjectAndPluginLayersMerge`: decision 5 on a temp project with `plugins/native/ios/A/PrivacyInfo.xcprivacy`
    (merged into ios, not macos), a static `.framework` with one (merged), a dynamic one (not), a broken one
    (warning, skipped); sources name each origin.
  - `ValidateRefusesWrongReasonsAndTypes`: CA92.1 under FileTimestamp, `DeviceId` misspelt, purpose "Ads".
  - `ToXmlIsDeterministic`: two composes of the same input are byte-equal; keys sorted.
- `editor/tests/export/AppleTemplatesTest.cpp` (new; reads repo files via `JASPER_TEST_REPO_DIR`):
  - `XcodeDeploymentTargetsAreTheMinimums` **(red)**: every `IPHONEOS_DEPLOYMENT_TARGET` is 15.0 and every
    `MACOSX_DEPLOYMENT_TARGET` 12.0 in `JasperEngine.xcodeproj` and the player template.
  - `CMakeIosLinksTheEngineFrameworks` **(red)**: the iOS `PLATFORM_LIBS` block of `engine/CMakeLists.txt` names
    every `appletext::appleEngineFrameworksIOS()` framework (today AudioToolbox, CoreGraphics, Metal, ... absent).
  - `EveryAppleBundleShipsThePrivacyManifest` **(red)**: both app targets' Resources phases of the Xcode template
    and the player template's reference `PrivacyInfo.xcprivacy`; the CMake iOS and macOS bundle blocks list it in
    `PLATFORM_RESOURCES`.
  - `NoAdMobLeft` **(red)**: no file below `engine/` or `editor/` (git-tracked list from the repo walk, skipping
    `docs/superpowers`) contains `AdMob`, `GoogleMobileAds`, `GADApplicationIdentifier`, `play-services-ads`,
    `JASPERENGINE_ADMOB` (case-insensitive for admob).
- `editor/tests/project/ApplePrivacySettingsTest.cpp` (new): YAML round trip (short names written, long accepted,
  empty block not written); `ApplePrivacySettingsCmd` execute/undo restores the struct.
- `editor/tests/NativeBridgeTest.cpp`: `MockInAShippedBuildWarnsOnce` **(red: no API)**: with
  `setMockWarnings(true)` and `{Ads: "..."}` platform-only, two mock calls log one warning; with the flag off, none.
- `editor/tests/players/PlayerConfigNativeTest.cpp` (or the existing PlayerConfig test file if one exists):
  `nativePlatformOnly` round trip and omission when empty.
- `editor/tests/export/MacBundleTest.cpp` (new or appended): `finishMacBundle` with a temp bundle (no codesign tool:
  the function's existing unsigned path) writes `Contents/Resources/PrivacyInfo.xcprivacy` and the plist entries.
- `editor/tests/project/EngineApiSyncTest.cpp`: the extracted predicate `isEngineApiSkippedDir` skips `X.xcframework`,
  `Y.framework`, `Z.bundle` and keeps `core`.

### 10.2 MCP end to end, `tools/mcp/tests/`

- `test_build.py::test_no_admob` (replaces `test_admob_export`) **(red)**: a source export has no `java-admob`,
  `play-services-ads`, `APPLICATION_ID` meta-data, `GADApplicationIdentifier`, `SKAdNetworkItems`,
  `JASPERENGINE_ADMOB`, `GoogleMobileAds`, `AdMobAdapter`; `set_project_settings {"android.admob_enabled": true}`
  is refused as unknown; `eval_lua "return System.initializeAdMob == nil and AdMobRating == nil"` in Play is true.
- `test_build.py::test_privacy_manifest_export` **(red)**: set the four `apple.privacy_*` keys; a source export's
  `workspaces/xcode/ios/PrivacyInfo.xcprivacy` (parsed with `plistlib`) has the engine's three API types, the project
  additions and tracking; macOS's too; both targets' Resources phases list it (`pbxproj` module); a
  `plugins/native/ios/X/PrivacyInfo.xcprivacy` reaches only iOS; `undo {scope: "project"}` restores the settings
  (`get_project_settings`); a bad reason is refused with the valid list.
- `test_native_bridge.py::test_privacy_manifest_tool`: `native_plugins {action: "privacy_manifest"}` returns the
  composed XML, sources and the two warnings of rule 6.
- `test_native_bridge.py::test_source_export` (existing, unchanged assertions): guards the generalized
  `addNativePluginsToXcode`; extend with the template's privacy resource.
- `test_build_method.py` (fake runtimes made by the test, as `runtime_archive` does for Linux):
  - `test_prebuilt_refuses_to_drop_cpp_plugins` **(red)**: Linux, `plugins/native/cpp/Steam.cpp`; `get_build_info`
    shows Compile with the reason; `build_project method: prebuilt` fails naming it.
  - `test_prebuilt_web_carries_plugins` **(red)**: a fake `web-wasm32-gles3-release` runtime (`files/index.html`
    with the loader marker, `player-loader.js`, `jasperengine-player-st.js`, `.wasm` starting with `\0asm`);
    `plugins/native/web/poki.js` and `web.script_urls`; the built `index.html` has the SDK URL then
    `plugins/poki.js` before `player-loader.js`; `plugins/poki.js` exists; the result's `carried` lists both.
  - `test_prebuilt_ios_carries_plugins` **(red)**: a fake `ios-arm64-metal-release` runtime made from
    `engine/platform/apple/ios/player-template` (+ storyboards, `include/JasperNative.h`, an empty
    `JasperEnginePlayer.xcframework/Info.plist`, features with `nativeplugins`); `plugins/native/ios/Device.m`,
    `ios.plist_entries {"NSCameraUsageDescription": "x"}`, privacy tracking; on Linux the result is the staged
    project: its pbxproj compiles `Device.m`, has `PrivacyInfo.xcprivacy` in Resources, its `Info.plist` has the
    entry, its manifest has the tracking flag.
  - `test_prebuilt_mock_warning`: Linux build of a project with an Android-only plugin and its mock: result
    `warnings` has `W-NATIVE-MOCK`; `content/game.json` has `nativePlatformOnly.Ads`.
- Tool-group, budget and docs tests (`test_tool_budget.py`, `test_docs.py`, `test_tool_coverage.py`) pass after
  `gen_tool_docs.py` and `tool_budget.py --check`.

### 10.3 Static and other suites

- `tools/check_apple_privacy.py` (new, run by `tools/check-changed` when `engine/` changes): scans
  `engine/core`, `engine/platform/{apple,mac}` and the libraries compiled on Apple for the required-reason API
  patterns (FileTimestamp: `stat(`, `fstat(`, `lstat(`, `getattrlist`, `st_mtime`, `last_write_time`,
  `NSFileModificationDate`, `creationDate`, `modificationDate`; SystemBootTime: `mach_absolute_time`,
  `systemUptime`, `kern.boottime`; DiskSpace: `statfs`, `fstatfs`, `statvfs`, `filesystem::space`,
  `NSFileSystemFreeSize`, `volumeAvailableCapacity`; ActiveKeyboards: `activeInputModes`; UserDefaults:
  `NSUserDefaults`, `UserDefaults`) and fails when a category is used but not in `engine/workspaces/xcode/ios/PrivacyInfo.xcprivacy`.
  Red control: run it against a temp copy with a `statfs(` line added; it must fail naming DiskSpace and the file.
- Lua API fixture test (`tools/lua_api/tests`) passes with the regenerated `regex_symbols.txt`.
- `tools/check-cxx-subset.py` passes with the empty skip list.
- Builds: `tools/build.sh linux`, `web`, `android` (the Android Gradle template without the ads SDK), and the
  unit tests, through `tools/check-changed`. iOS/macOS code (Objective-C removals, CMake resources, template
  pbxproj) cannot be built on this host: keep it compiling by reading, by the existing patterns, and by the pbxproj
  parse in the MCP tests; say so in the PR.

## 11. Acceptance criteria

1. `git ls-files | xargs grep -il -E 'admob|GoogleMobileAds|GADApplicationIdentifier|play-services-ads|Interstitial'`
   lists only files under `docs/superpowers/`, `docs/openengine-analysis/`, `docs/engine-gaps/`, `docs/test-cleanup/`,
   `docs/doriax-catchup/` and the native-bridge worked example of `docs/mcp/guide/native-bridge.md`.
2. `engine/platform/apple/GoogleMobileAdsSdkiOS-10.14.0` and `engine/platform/android/java-admob` do not exist.
3. In Lua, `System.initializeAdMob`, `System.showInterstitialAd` and `AdMobRating` are nil (10.2 `test_no_admob`).
4. A source export of a plain project contains `workspaces/xcode/{ios,macos}/PrivacyInfo.xcprivacy` equal to the
   committed engine defaults, referenced by both app targets' Resources phases.
5. The CMake iOS app block, the CMake macOS bundle blocks and the iOS player template ship `PrivacyInfo.xcprivacy`
   (`AppleTemplatesTest`); `ios_player.sh` copies it and `JasperNative.h` into the template.
6. Prebuilt macOS bundles contain `Contents/Resources/PrivacyInfo.xcprivacy`; prebuilt iOS projects contain
   `PrivacyInfo.xcprivacy` in their Resources phase; the payload check fails a build without one.
7. The four `apple.privacy_*` keys set, validate (refusals list valid values), save to `project.yaml` `apple.privacy`,
   undo in one project-scope step, and show in Project Settings > Player > iOS and > macOS with the engine rows
   read-only; Preview shows the composed XML.
8. `native_plugins {action: "privacy_manifest", platform}` returns XML, sources, plugin manifests and warnings.
9. For each row of decision 6, `chooseBuildMethod` reports the input either in `carried` or in `reasons`, with
   project-relative names (BuildMethodTest).
10. A prebuilt Web build's page loads `web.script_urls` and `plugins/native/web/*.js` in the Compile build's order.
11. A prebuilt iOS build compiles `plugins/native/{apple,ios}` sources, links their SDKs, Swift packages and
    `ios.frameworks`, and applies the Info.plist entries, entitlements and deployment target (staged pbxproj
    checked on Linux; on a Mac, `xcodebuild` builds the simulator app: manual check by whoever has a Mac).
12. A prebuilt Android build's manifest holds `android.extra_permissions` and both XML fragments.
13. `get_build_info` platforms and `build_project` results carry `carried`; the Build Settings window lists it.
14. Builds warn `W-NATIVE-MOCK` for mock-only plugins; shipped games (both methods) log one warning per such plugin
    when its mock first answers; prebuilt games answer platform-only plugins with `unavailable`.
15. The Xcode template has no deployment target below 15.0 (iOS) / 12.0 (macOS).
16. The engine API sync skips bundle directories (unit test).
17. Every red control of section 10 was run red on `main` and green on the branch; `tools/check-changed` is green;
    `gen_tool_docs.py --check` and `tool_budget.py --check tools/mcp/budget.json` pass.
18. Docs updated as in 6.2 and 6.9; the docs' Lua block test passes.

## 12. Files likely touched

Deleted: `engine/platform/apple/GoogleMobileAdsSdkiOS-10.14.0/**`, `engine/platform/apple/ios/AdMobAdapter.{h,m}`,
`engine/platform/android/java-admob/**`, `editor/tests/AdMobSettingsTest.cpp`.

Engine: `engine/CMakeLists.txt`, `engine/core/System.{h,cpp}`, `engine/core/script/LuaBridgeAddon.h`,
`engine/core/script/binding/CoreClassesLua.cpp`, `engine/core/native/NativeBridge.{h,cpp}`,
`engine/core/player/PlayerConfig.{h,cpp}`, `engine/player/PlayerMain.cpp`,
`engine/platform/android/{JasperEngineAndroid.{h,cpp},NativeEngine.{h,cpp}}`,
`engine/platform/android/java/com/jaspergameengine/jasperengine/MainActivity.java`,
`engine/platform/android/player/AndroidManifest.template.xml`,
`engine/platform/apple/JasperEngineApple.{h,mm}`, `engine/platform/apple/ios/player-template/**` (pbxproj,
`PrivacyInfo.xcprivacy`), `engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj`,
`engine/workspaces/xcode/{ios,macos}/PrivacyInfo.xcprivacy` (new), `engine/workspaces/xcode/ios/Info.plist`,
`engine/workspaces/androidstudio/app/{build.gradle,src/main/AndroidManifest.xml}`.

Editor: `editor/Exporter.{h,cpp}`, `editor/ExporterPrebuilt.cpp`, `editor/Generator.cpp`, `editor/Project.{h,cpp}`,
`editor/Stream.cpp`, `editor/StreamSchema.cpp`, `editor/settings/SettingsRows.cpp`,
`editor/window/dialog/ProjectSettingsWindow.{h,cpp}`, `editor/window/dialog/ExportWindowShipping.cpp`,
`editor/window/dialog/ExportWindow.{h,cpp}` (carried line), `editor/platform/{IosProject,MacBundle,AndroidPackager}.{h,cpp}`,
`editor/publish/PayloadValidator.cpp`, `editor/mcp/tools/{BuildTools,ProjectTools}.cpp` (`native_plugins` is at `ProjectTools.cpp:1473`),
`editor/cli/ShippingCommands.cpp`, new `editor/util/{ApplePrivacy,AppleProjectText,AppleNativeXcode}.{h,cpp}`,
`editor/export/NativeNeeds.{h,cpp}`, `editor/command/type/ApplePrivacySettingsCmd.{h,cpp}`, the editor CMake source
list, `editor/tests/EditorTests.cmake` and the tests of section 10.

Tools and docs: `tools/players/{ios_player.sh,package_player.py}`, `tools/check-cxx-subset.py`,
`tools/check_apple_privacy.py` (new), `tools/check_changed.py` (wire it), `tools/lua_api/tests/fixtures/regex_symbols.txt`,
`tools/mcp/tests/{test_build.py,test_build_method.py,test_native_bridge.py}`, `docs/shipping.md`,
`docs/mcp/guide/{shipping,native-bridge,lua-api}.md`, `docs/mcp/TOOLS.md` (generated), `docs/testing.md`,
`docs/specs/{S25-editor-search-organisation,S30-shipping}.md`, `NOTICE.md` (only if it ever listed the SDK: it does not
today, verified).

Hot spots: `editor/Exporter.cpp` (AdMob deletions, helper extraction, `writeAppleProjectSettings`,
`addNativePluginsToXcode` move), `editor/ExporterPrebuilt.cpp` (`chooseBuildMethod`, `finalizeWeb/IOS/Android/MacOS`),
`engine/workspaces/xcode/JasperEngine.xcodeproj/project.pbxproj`, `ProjectSettingsWindow.cpp`, `SettingsRows.cpp`,
`CoreClassesLua.cpp`, `engine/CMakeLists.txt`, `build.gradle`.

## 13. Cross-feature dependencies

- **export-pipeline**: also edits `ExporterPrebuilt.cpp` (`finalizeWeb`: index.html and favicon for every web export;
  failed copies fail the export, which applies to the plugin copies of 6.7; capacity counting for script-only prefabs
  in `chooseBuildMethod`) and `Exporter.cpp` (copy helpers, visible build errors). Merge order: whichever lands
  second rebases; I take their copy-failure helper for the plugin copies if it lands first. They may want the two
  gaps I found but leave out (prebuilt iOS lacks App Transport Security from `player.allow_insecure_http`; the
  source-export iOS `Info.plist` has `UIRequiredDeviceCapabilities armv7`).
- **build-toolchain**: edits `Exporter.cpp` Android/Gradle paths and generated project files; my AdMob deletions
  in `writeAndroidProjectSettings` (`:4109-4116`, `:4248-4252`) sit next to theirs.
- **platform-lifecycle**: Android Gradle deprecations in the same `build.gradle` (my edits at `:114-138`),
  `MainActivity.java`, `NativeEngine.cpp`, `JasperEngineApple.mm` (Metal waits when hidden). Small textual conflicts.
- **unicode-text**: `JasperEngineApple.mm` (Apple bundle paths as UTF-8) and the Windows manifest; `PlayerMain.cpp`.
- **scripting**: `CoreClassesLua.cpp` (subsystem bindings) and the Lua API fixture `regex_symbols.txt`: regenerate the
  fixture after merging.
- **agent-authoring**: agent guide (`McpInstructions.cpp`) budget; I add at most one short line.
- No dependency on the rendering, water, mesh or UI features.

## Deviations

Recorded during implementation (branch `dc/mobile-shipping`).

1. **No `AppleProjectText` / `AppleNativeXcode` modules.** The plist and pbx helpers stay in
   `Exporter.cpp`'s anonymous namespace; `addNativePluginsToXcode` is generalized in place with an
   `Exporter::XcodeNativeLayout` (source export: two targets; prebuilt iOS template: one target,
   `$(SRCROOT)/plugins/native`, `$(SRCROOT)/include`). `platform::buildIosProject` is split into
   `stageIosProject` and `buildIosProject`, and the Exporter edits the staged project in between
   (`addNativeToIosPlayer`); `MacBundleSettings` gets `privacyManifest` and a `customizePlist`
   hook that applies `Exporter::applyNativePlistEntries`. Same behaviour, far less code moved
   between hot files. Only `isStaticArchive`, `frameworkBinary` and `xcframeworkBinary` moved, to
   `editor/util/NativePlugins`, because `ApplePrivacy` needs them.
2. **`NativeNeeds` lives in `editor/util/`** (there is no `editor/export/` folder).
3. **C++ plugins keep their own reason.** `chooseBuildMethod` still scans `getScriptDirs()` for
   "C++ scripts"; `plugins/native/cpp` is reported by `NativeNeeds` as "C++ native plugins (...)",
   which says better what to change. The fix of finding 4.3 is the same: such a project compiles.
4. **The Lua API fixture is not regenerated.** `regex_symbols.txt` is a snapshot of a deleted
   parser; `test_lua_api.py` lists the AdMob symbols under `removed_on_purpose` like the other
   later API changes.
5. **A third committed manifest**: `engine/platform/apple/ios/player-template/PrivacyInfo.xcprivacy`
   (the template is copied whole by `ios_player.sh`); `ApplePrivacyTest.CommittedManifestsMatchTheEngine`
   keeps all three equal to `toXml(engineManifest())`.
6. **Rule 7.2's message** ("already included by the engine") is not given: settings rows have no
   success-message channel. Repeating an engine entry is accepted and the composed manifest holds
   it once.
7. **AC1's grep** also lists the tests and the Lua API test that assert AdMob's absence
   (`test_build.py::test_no_admob`, `AppleTemplatesTest.NoAdMobLeft`, `BuildMethodTest.cpp`,
   `test_lua_api.py` and its `regex_symbols.txt` fixture, deviation 4), the glossary row pointing
   to the native bridge, the native-bridge worked example, the lead's `FOLLOWUPS-LEDGER.md`, and
   `docs/specs/S01-cxx20.md`, a historical spec that is not rewritten. `NoAdMobLeft` walks every
   source and template file below `engine/` and `editor/` (spec 10.1) with the two tests as its
   allowlist.
8. **No agent-guide line** (lead decision: only agent-authoring restructures the guide). The
   detail is in `docs/mcp/guide/shipping.md` and `native-bridge.md`; the new tool text has the
   eval case `bad_privacy_manifest_by_hand`.
9. **Prebuilt macOS players use the running name `macos-cmake`** for `game.json`'s
   `nativePlatformOnly`: a prebuilt macOS build exists only when the project has no Apple plugin
   code, so the two names give the same map.
10. **The Android template markers** are new (`{{NATIVE_MANIFEST_XML}}`, `{{NATIVE_APPLICATION_XML}}`):
    editor and runtime versions must match, so no fallback for older templates is kept.
11. **No CI step for `tools/check_apple_privacy.py`**: `main` removed the GitHub workflows while
    this was implemented. `tools/check-changed` runs the check for every change to engine C/C++
    or to the engine's manifest.
12. **S67 (packaging tests, merged from main) adapted**: `tools/players/ios_player.sh` no longer passes
    the removed `JASPERENGINE_ADMOB` option, `IosBuild.ThePlayerTemplateLinksWhatTheEngineNeeds`
    checks that the option is gone and that the template ships `PrivacyInfo.xcprivacy`,
    `IosBuild.WithoutAMacTheXcodeProjectIsTheResult` stages before it builds (the split of
    deviation 1), and `tools/ci/ios_packaging.sh` checks the manifest in the simulator and device apps.
13. **Review round.** An SDK's resource `.bundle` in `plugins/native/{apple,ios,macos}` is copied
    whole into the app (a Resources build phase entry in the source export's targets and the
    prebuilt iOS template), and its `PrivacyInfo.xcprivacy` joins the composed manifest; the
    macOS CMake build still copies no plugin bundle (it builds Objective-C sources only, as
    before). Merging one call's `apple.privacy_*` keys into one undo step is limited to agent
    calls: the dialog's OK is one command already, and two user edits stay two steps. The
    privacy rows show on Player > macOS in search through display-only rows `macos.privacy_*`
    (MCP sets the `apple.privacy_*` keys). An Android plugin folder forces a compile only for
    what the export's Gradle module reads (`.java`, `.kt`, `.aar`, `.jar`, `res/`,
    `AndroidManifest.xml`, `proguard-rules.pro`), so notes and READMEs ride along.
