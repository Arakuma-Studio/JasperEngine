# S47 — Android and iOS packaging are tested end to end

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
- R5. These run as scripts in `tools/ci/`, and their CI jobs exist. They need no human.
- R6. Every bug found is fixed here with a red test where a unit test can catch it.

## 3. Acceptance
- AC1. Logs and screenshots of R1 and R4 under `je-evidence/s47`, listed in the final report.
- AC2. R2's failure-case tests in `editor/tests`. `PublishingTest` and `BuildMethodTest` extended.
- AC3. `tools/check-changed` green; docs for Android and iOS export corrected to what really works.
