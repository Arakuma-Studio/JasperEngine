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

struct NativeEngineSavedState {
    bool mHasFocus;
};

struct JniData{
    jobject gameActivityObjRef;
    jclass gameActivityClsRef;

    jmethodID getUserSettingsRef;
    jmethodID getAdMobWrapperRef;

    jobject userSettingsObjRef;
    jclass userSettingsClsRef;

    // null when the export leaves AdMob out
    jobject adMobWrapperObjRef;
    jclass adMobWrapperClsRef;

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

    jmethodID initializeAdMob;
    jmethodID setMaxAdContentRating;
    jmethodID loadInterstitialAd;
    jmethodID isInterstitialAdLoaded;
    jmethodID showInterstitialAd;

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
    EGLContext mEglContext;
    EGLConfig mEglConfig;

    int mSurfWidth, mSurfHeight;
    std::string mInternalDataPath;

    // Display.getRotation() in degrees as last read, and whether rotation 0 is portrait
    int mDisplayRotation;
    bool mNaturalPortrait;

    JNIEnv *mJniEnv;
    JniData mJniData;

    struct NativeEngineSavedState mState;

    bool initDisplay();
    bool initSurface();
    bool initContext();

    void killContext();
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
    void showSoftInput(std::wstring text);
    void hideSoftInput();

    std::string getInternalDataPath();

    AAssetManager* getAssetManager();
};

#endif /* NativeEngine_H_ */
