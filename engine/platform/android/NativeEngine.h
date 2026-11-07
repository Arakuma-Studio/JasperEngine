// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NativeEngine_H_
#define NativeEngine_H_

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <android/asset_manager.h>
#include <game-activity/GameActivity.h>
#include <string>
#include "util/FrameLimiter.h"

struct NativeEngineSavedState {
    bool mHasFocus;
};

struct JniData{
    jobject gameActivityObjRef;
    jclass gameActivityClsRef;

    jmethodID getUserSettingsRef;

    jobject userSettingsObjRef;
    jclass userSettingsClsRef;

    jmethodID getBoolForKeyRef;
    jmethodID getIntegerForKeyRef;
    jmethodID getLongForKeyRef;
    jmethodID getFloatForKeyRef;
    jmethodID getDoubleForKeyRef;
    jmethodID getStringForKeyRef;

    jmethodID setBoolForKeyRef;
    jmethodID setIntegerForKeyRef;
    jmethodID setLongForKeyRef;
    jmethodID setFloatForKeyRef;
    jmethodID setDoubleForKeyRef;
    jmethodID setStringForKeyRef;

    jmethodID removeKeyRef;
    jmethodID hasKeyRef;
    jmethodID removeAllKeysRef;
    jmethodID saveUserSettingsRef;

    // null when the activity does not provide them (the System base clipboard is used)
    jmethodID setClipboardText;
    jmethodID getClipboardText;

    // JasperPlatform's static helpers (platform services); null when the class is missing
    jclass platformClsRef;
    jmethodID platformOpenURL;
    jmethodID platformGetPreferredLocales;
    jmethodID platformGetRegion;
    jmethodID platformGetAppVersion;
    jmethodID platformGetAppBuild;
    jmethodID platformGetAppIdentifier;
    jmethodID platformGetAppName;
    jmethodID platformGetOsRelease;
    jmethodID platformGetApiLevel;
    jmethodID platformGetBuildDisplay;
    jmethodID platformGetBuildIncremental;
    jmethodID platformGetDeviceModel;
    jmethodID platformGetSystemMemorySize;
    jmethodID platformGetInternetReachability;
    jmethodID platformVibrate;
    jmethodID platformHapticFeedback;
    jmethodID platformGetScreenDpi;
    jmethodID platformGetRefreshRate;
    jmethodID platformIsNaturalPortrait;
    jmethodID platformSetOrientationPolicy;
    // the volatile fields its listeners keep (display rotation, device orientation degrees)
    jfieldID platformDisplayRotation;
    jfieldID platformDeviceDegrees;
    // Screen.safeArea: where the game's view lies in the window and the cutout rectangles
    jfieldID platformSafeAreaRevision;
    jfieldID platformSafeAreaInfo;
    // Device's thermal state and memory warnings (spec S56): the listener's state, the onTrimMemory
    // count, and the 2 s poll of the status and the headroom forecast
    jfieldID platformThermalState;
    jfieldID platformTrimMemoryCount;
    jmethodID platformGetThermalState;
};

class NativeEngine {
private:
    static const int MAX_GAMEPADS = 8;

    struct GamepadDevice {
        int32_t deviceId = -1;
        float axes[6] = {0.0f};
        int hatX = 0;
        int hatY = 0;
    };

    struct android_app *mApp;
    bool mHasFocus, mIsVisible, mHasWindow;
    bool mHasGLObjects;

    GamepadDevice mGamepads[MAX_GAMEPADS];

    EGLDisplay mEglDisplay;
    EGLSurface mEglSurface;
    // makeContextCurrent's surface while there is no window (GL objects released then)
    EGLSurface mEglReleaseSurface;
    EGLContext mEglContext;
    EGLConfig mEglConfig;

    int mSurfWidth, mSurfHeight;
    std::string mInternalDataPath;

    // Display.getRotation() in degrees as last read, and whether rotation 0 is portrait
    int mDisplayRotation;
    bool mNaturalPortrait;

    // the last thermal poll (steady clock seconds) and onTrimMemory count the game thread saw
    double mLastThermalPoll = -1.0e9;
    int mTrimMemoryCount = 0;

    // Screen.safeArea (safe-area spec 6.5): MainActivity.isFullscreenGame() (read once; a
    // windowed game includes the system bars), and what the last push was computed from
    bool mFullscreenGame;
    bool mSafeAreaKnown;
    ARect mLastCutoutInsets;
    ARect mLastBarInsets;
    int mLastSafeAreaRevision;
    int mLastSafeAreaWidth;
    int mLastSafeAreaHeight;

    // Engine.targetFrameRate where Swappy is not enabled: the EGL swap interval and a limiter
    // (0: none) after each swap
    int mFallbackSwapInterval;
    double mFallbackLimiterFps;
    int mAppliedSwapInterval;
    jasperengine::FrameLimiter mFrameLimiter;

    JNIEnv *mJniEnv;
    JniData mJniData;

    struct NativeEngineSavedState mState;

    bool initDisplay();
    bool initSurface();
    bool initContext();

    void killContext();
    // the context current with the window surface, or without the window (a 1x1 pbuffer, or
    // no surface: EGL_KHR_surfaceless_context) to release GL objects; false when it cannot be
    bool makeContextCurrent();
    void killSurface();
    void killDisplay();

    void killGLObjects();

    int getGamepadSlot(int32_t deviceId);
    void handleGamepadMotion(GameActivityMotionEvent* motionEvent);
    void handleEglError(EGLint error);

    bool prepareToRender();

    void doFrame();

    bool isAnimating();

    void handleGameActivityInput();

    void setupJNI();
    void setupPlatformJNI(JNIEnv* env);
    // reads JasperPlatform.displayRotation; true when it changed since the last read
    bool refreshDisplayRotation();
    // JasperPlatform's thermal state and onTrimMemory count to Device, once a frame
    void updateDeviceSignals();
    // GameActivity's display cutout insets (and the system bars when windowed), relative to the
    // surface, to Screen when they changed: every frame after the rotation and size checks
    // (cheap; APP_CMD_WINDOW_INSETS_CHANGED, the surface size and JasperPlatform's layout info
    // each arrive on their own schedule)
    void updateSafeArea();

public:
    NativeEngine(struct android_app *app);

    ~NativeEngine();

    static NativeEngine *getInstance();
    JNIEnv* getJniEnv();
    JavaVM* getJavaVM();
    GameActivity* getActivity();
    JniData& getJniData();

    void handleCommand(int32_t cmd);

    void gameLoop();

    int getSurfWidth();
    int getSurfHeight();
    // the cached Display.getRotation() degrees and the natural orientation of the device
    int getDisplayRotation();
    bool isNaturalPortrait();
    // OrientationEventListener degrees, -1 when unknown or flat
    int getDeviceDegrees();
    // frame pacing without Swappy: applied at the next frame (the context is current there)
    void setFallbackPacing(int swapInterval, double limiterFps);
    void showSoftInput(std::wstring text);
    void hideSoftInput();

    std::string getInternalDataPath();

    AAssetManager* getAssetManager();
};

#endif /* NativeEngine_H_ */
