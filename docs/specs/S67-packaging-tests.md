# S67 — Android and iOS packaging are tested end to end

**Depends on.** S30. Touches `editor/platform/AndroidPackager.*`, `editor/platform/AndroidTools.*`, iOS export, `tools/ci/`, `tools/android-emulator-test.sh`.
Android work runs on server A (KVM), iOS on the Mac (M).

## 1. Goal
BF21 found no test exercising the Android and iOS packaging paths, and the bundletool download was never run. A user
pressing Build for those targets must get a working app. Everything in the path gets run for real, and what is broken gets fixed.

## 2. Requirements
- R1. Android: prebuilt or source export produces a signed APK and an AAB. The APK installs and runs on the emulator; a
  screenshot of the game is compared. The AAB goes through bundletool `build-apks` and `install-apks` on the emulator.
- R2. The bundletool download really runs: version pinned, SHA-256 verified, cached, resumes or retries, and a clear error
  when offline or the hash differs. Test with a local HTTP server for failure cases and the real URL once.
- R3. Signing: debug key by default; a release keystore path works; a wrong password gives a clear error, and passwords are
  never logged.
- R4. iOS: an export produces an Xcode project that builds with `xcodebuild` for the simulator, installs, launches and shows the
  game on the Mac. A device build gets as far as signing, with a clear message when no team is set. The IPA path is exercised
  as far as possible without a developer account, and the rest is stated.
- R5. These run as scripts in `tools/ci/` and need no human. There is no CI (workflows removed on 2026-11-04), so the scripts are run by hand.
- R6. Every bug found is fixed here with a red test where a unit test can catch it.

## 3. Acceptance
- AC1. Logs and screenshots of R1 and R4 under `je-evidence/s47`, listed in the final report.
- AC2. R2's failure-case tests in `editor/tests`. `PublishingTest` and `BuildMethodTest` extended.
- AC3. `tools/check-changed` green; docs for Android and iOS export corrected to what really works.

## 4. Deviations

- R1 ("prebuilt or source export"): both are run (`ANDROID_PACKAGING_SOURCE=1` adds the source export). Build itself takes
  the prebuilt path: a project with native code is refused with a pointer to the source export. Gradle signs a source
  export with the debug key: its Gradle project has no signing config for the project's keystore, so a source export's
  release key is set up in Android Studio. R3's release keystore is the prebuilt path's.
- R4: with no team, a device build is a success with the warning `W-IOS-NO-TEAM`, not a failure: the output folder
  is replaced only on success, and the Xcode project and the unsigned device `.app` are what such a user needs.
- Added beyond the spec, because R4 needs them from the editor: `build_project ios_simulator` and a Simulator switch in Build
  Settings (only the CLI could make a simulator build), and the editor finds an Xcode in `/Applications` when
  `xcode-select` is on the Command Line Tools, through `DEVELOPER_DIR` for its own tools.
- `publish` now also refuses an unsigned App Bundle (no signature block in `META-INF`).
- Android's Vulkan player is withdrawn: S30's table lists it, but `platform/android` makes only an EGL context, the
  engine's CMake set no sokol backend for it, and `-PjasperBackend=vulkan` never compiled, which also failed `tools/build.sh
  players` for Android. Build Settings, `get_build_info` and `--backend` offer gles3 only, and CMake
  says why. A real Android Vulkan backend is a feature of its own.
- Open, not fixed here: a source export's debug APK fails `android-emulator-test.sh`'s GLES check on the emulator, with
  `GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE` from sokol's debug-only check, right after the emulator's own
  `eglCodecCommon: depth attachment not complete`. Its screen still matches the golden, and the prebuilt builds compile that check
  out. Whether a device reports it too is not known.
