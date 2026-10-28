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
    void showSoftInput(std::wstring text);
    void hideSoftInput();

    std::string getInternalDataPath();

    AAssetManager* getAssetManager();
};

#endif /* NativeEngine_H_ */
