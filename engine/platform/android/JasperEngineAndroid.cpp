// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineAndroid.h"
#include "PlatformConfig.h"

#include <errno.h>
#include <android/log.h>
#include <stdarg.h>
#include <android/asset_manager.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include "NativeEngine.h"
#include "Log.h"
#include "util/PlatformRules.h"
#include "util/StringUtils.h"

#include <android/window.h>
#include <game-activity/GameActivity.h>
#include <swappy/swappyGL.h>

static int android_read(void* cookie, char* buf, int size) {
    return AAsset_read((AAsset*)cookie, buf, size);
}

static int android_write(void* cookie, const char* buf, int size) {
    return EACCES; // can't provide write access to the apk
}

static fpos_t android_seek(void* cookie, fpos_t offset, int whence) {
    return AAsset_seek((AAsset*)cookie, offset, whence);
}

static int android_close(void* cookie) {
    AAsset_close((AAsset*)cookie);
    return 0;
}

namespace {

// The calling thread's JNIEnv. The game thread stays attached (NativeEngine::getJniEnv);
// any other thread is attached for the lifetime of this object only.
class ScopedJniEnv {
public:
    explicit ScopedJniEnv(JavaVM* vm): vm(vm) {
        jint result = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (result == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&env, NULL) == JNI_OK) {
                attached = true;
            } else {
                env = NULL;
            }
        } else if (result != JNI_OK) {
            env = NULL;
        }
    }

    ~ScopedJniEnv() {
        if (attached)
            vm->DetachCurrentThread();
    }

    ScopedJniEnv(const ScopedJniEnv&) = delete;
    ScopedJniEnv& operator=(const ScopedJniEnv&) = delete;

    JNIEnv* get() const { return env; }

private:
    JavaVM* vm;
    JNIEnv* env = NULL;
    bool attached = false;
};

// Logs and clears a pending Java exception; true when there was one
bool clearJavaException(JNIEnv* env, const char* what) {
    if (!env->ExceptionCheck())
        return false;
    env->ExceptionDescribe();
    env->ExceptionClear();
    jasperengine::Log::error("Java exception in %s", what);
    return true;
}

// NewStringUTF takes modified UTF-8, where characters outside the BMP are two encoded
// surrogates, so UTF-8 goes through UTF-16 and NewString instead
jstring newJavaString(JNIEnv* env, const std::string& utf8) {
    bool hadInvalid = false;
    std::vector<uint32_t> codepoints = jasperengine::StringUtils::decodeUtf8ToCodepoints(utf8, hadInvalid);

    std::vector<jchar> utf16;
    utf16.reserve(codepoints.size());
    for (uint32_t cp : codepoints) {
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            utf16.push_back(static_cast<jchar>(0xD800u + (cp >> 10)));
            utf16.push_back(static_cast<jchar>(0xDC00u + (cp & 0x3FFu)));
        } else {
            utf16.push_back(static_cast<jchar>(cp));
        }
    }

    static const jchar empty = 0;
    return env->NewString(utf16.empty() ? &empty : utf16.data(), static_cast<jsize>(utf16.size()));
}

// UTF-16 of a Java string to UTF-8; a lone surrogate becomes U+FFFD
std::string javaStringToUtf8(JNIEnv* env, jstring str) {
    std::string out;
    if (!str)
        return out;

    jsize length = env->GetStringLength(str);
    const jchar* chars = env->GetStringChars(str, NULL);
    if (!chars)
        return out;

    out.reserve(length);
    for (jsize i = 0; i < length; i++) {
        uint32_t cp = chars[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < length &&
                chars[i + 1] >= 0xDC00u && chars[i + 1] <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (chars[i + 1] - 0xDC00u);
            i++;
        }
        // toUTF8 replaces lone surrogates with U+FFFD
        out += jasperengine::StringUtils::toUTF8(static_cast<char32_t>(cp));
    }

    env->ReleaseStringChars(str, chars);
    return out;
}

} // namespace

int JasperEngineAndroid::getScreenWidth(){
    return NativeEngine::getInstance()->getSurfWidth();
}

int JasperEngineAndroid::getScreenHeight(){
    return NativeEngine::getInstance()->getSurfHeight();
}

bool JasperEngineAndroid::isTouchDevice(){
    return true;
}

void JasperEngineAndroid::showVirtualKeyboard(std::wstring text){
    NativeEngine::getInstance()->showSoftInput(text);
}

void JasperEngineAndroid::hideVirtualKeyboard(){
    NativeEngine::getInstance()->hideSoftInput();
}

void JasperEngineAndroid::setClipboardText(const std::string& utf8){
    NativeEngine* nativeEngine = NativeEngine::getInstance();
    if (!nativeEngine || !nativeEngine->getJniData().setClipboardText){
        System::setClipboardText(utf8);
        return;
    }

    JniData& jniData = nativeEngine->getJniData();
    ScopedJniEnv scopedEnv(nativeEngine->getJavaVM());
    JNIEnv* env = scopedEnv.get();
    if (!env)
        return;

    jstring strText = newJavaString(env, utf8);
    if (!strText){
        clearJavaException(env, "setClipboardText");
        return;
    }
    env->CallVoidMethod(jniData.gameActivityObjRef, jniData.setClipboardText, strText);
    clearJavaException(env, "setClipboardText");
    env->DeleteLocalRef(strText);
}

std::string JasperEngineAndroid::getClipboardText(){
    NativeEngine* nativeEngine = NativeEngine::getInstance();
    if (!nativeEngine || !nativeEngine->getJniData().getClipboardText){
        return System::getClipboardText();
    }

    JniData& jniData = nativeEngine->getJniData();
    ScopedJniEnv scopedEnv(nativeEngine->getJavaVM());
    JNIEnv* env = scopedEnv.get();
    if (!env)
        return "";

    jstring rv = (jstring)env->CallObjectMethod(jniData.gameActivityObjRef, jniData.getClipboardText);
    std::string value;
    if (!clearJavaException(env, "getClipboardText")){
        value = javaStringToUtf8(env, rv);
        clearJavaException(env, "getClipboardText");
    }
    if (rv)
        env->DeleteLocalRef(rv);
    return value;
}

void JasperEngineAndroid::quit(){
    GameActivity_finish(NativeEngine::getInstance()->getActivity());
}

std::string JasperEngineAndroid::getAssetPath() {
    if (!jasperengine::platformConfig().contentRoot.empty())
        return jasperengine::platformConfig().contentRoot + "/assets";
    return System::getAssetPath();
}

std::string JasperEngineAndroid::getLuaPath() {
    if (!jasperengine::platformConfig().contentRoot.empty())
        return jasperengine::platformConfig().contentRoot + "/lua";
    return System::getLuaPath();
}

std::string JasperEngineAndroid::getUserDataPath() {
    return NativeEngine::getInstance()->getInternalDataPath();
}

FILE* JasperEngineAndroid::platformFopen(const char* fname, const char* mode) {
    std::string path = fname;

    //Return regular fopen if writable path is in path
    if (!NativeEngine::getInstance() || path.find(getUserDataPath()) != std::string::npos) {
        return fopen(path.c_str(), mode);
    }

    if (path[0] == '/'){
        path = path.substr(1, path.length());
    }

    if(mode[0] == 'w') return NULL;
    AAsset* asset = AAssetManager_open(NativeEngine::getInstance()->getAssetManager(), path.c_str(), 0);
    if(!asset) return NULL;
    return funopen(asset, android_read, android_write, android_seek, android_close);
}

// ---- UserSettings: SharedPreferences through UserSettings.java (it catches its own
// exceptions: a key read with another type than it was written with gives the default).
// Strings cross as UTF-16 (newJavaString / javaStringToUtf8): NewStringUTF and
// GetStringUTFChars use modified UTF-8, which differs outside the BMP.

namespace {

    // The game thread's JNIEnv and the key as a Java string, released at the end of the call
    class UserSettingsCall {
    public:
        explicit UserSettingsCall(const char* key)
            : jniData(NativeEngine::getInstance()->getJniData()),
              env(NativeEngine::getInstance()->getJniEnv()),
              key(newJavaString(env, key ? key : "")) {
        }

        ~UserSettingsCall() {
            if (key)
                env->DeleteLocalRef(key);
        }

        UserSettingsCall(const UserSettingsCall&) = delete;
        UserSettingsCall& operator=(const UserSettingsCall&) = delete;

        // false (logged) when the call raised; the caller uses the default then
        bool done(const char* what) {
            return !clearJavaException(env, what);
        }

        JniData& jniData;
        JNIEnv* env;
        jstring key;
    };

}

bool JasperEngineAndroid::getBoolForKey(const char *key, bool defaultValue){
    UserSettingsCall call(key);
    const bool value = call.env->CallBooleanMethod(call.jniData.userSettingsObjRef, call.jniData.getBoolForKeyRef, call.key, defaultValue);
    return call.done("UserSettings.getBoolForKey") ? value : defaultValue;
}

int JasperEngineAndroid::getIntegerForKey(const char *key, int defaultValue){
    UserSettingsCall call(key);
    const int value = call.env->CallIntMethod(call.jniData.userSettingsObjRef, call.jniData.getIntegerForKeyRef, call.key, defaultValue);
    return call.done("UserSettings.getIntegerForKey") ? value : defaultValue;
}

long JasperEngineAndroid::getLongForKey(const char *key, long defaultValue){
    UserSettingsCall call(key);
    const long value = static_cast<long>(call.env->CallLongMethod(call.jniData.userSettingsObjRef, call.jniData.getLongForKeyRef, call.key,
                                                                  static_cast<jlong>(defaultValue)));
    return call.done("UserSettings.getLongForKey") ? value : defaultValue;
}

float JasperEngineAndroid::getFloatForKey(const char *key, float defaultValue){
    UserSettingsCall call(key);
    const float value = call.env->CallFloatMethod(call.jniData.userSettingsObjRef, call.jniData.getFloatForKeyRef, call.key, defaultValue);
    return call.done("UserSettings.getFloatForKey") ? value : defaultValue;
}

double JasperEngineAndroid::getDoubleForKey(const char *key, double defaultValue){
    UserSettingsCall call(key);
    const double value = call.env->CallDoubleMethod(call.jniData.userSettingsObjRef, call.jniData.getDoubleForKeyRef, call.key, defaultValue);
    return call.done("UserSettings.getDoubleForKey") ? value : defaultValue;
}

std::string JasperEngineAndroid::getStringForKey(const char *key, const std::string& defaultValue){
    UserSettingsCall call(key);
    jstring strDefaultValue = newJavaString(call.env, defaultValue);
    jstring rv = (jstring)call.env->CallObjectMethod(call.jniData.userSettingsObjRef, call.jniData.getStringForKeyRef, call.key, strDefaultValue);
    std::string value = defaultValue;
    if (call.done("UserSettings.getStringForKey") && rv)
        value = javaStringToUtf8(call.env, rv);
    if (strDefaultValue)
        call.env->DeleteLocalRef(strDefaultValue);
    if (rv)
        call.env->DeleteLocalRef(rv);
    return value;
}

void JasperEngineAndroid::setBoolForKey(const char *key, bool value){
    UserSettingsCall call(key);
    call.env->CallVoidMethod(call.jniData.userSettingsObjRef, call.jniData.setBoolForKeyRef, call.key, value);
    call.done("UserSettings.setBoolForKey");
}

void JasperEngineAndroid::setIntegerForKey(const char *key, int value){
    UserSettingsCall call(key);
    call.env->CallVoidMethod(call.jniData.userSettingsObjRef, call.jniData.setIntegerForKeyRef, call.key, value);
    call.done("UserSettings.setIntegerForKey");
}

void JasperEngineAndroid::setLongForKey(const char *key, long value){
    UserSettingsCall call(key);
    call.env->CallVoidMethod(call.jniData.userSettingsObjRef, call.jniData.setLongForKeyRef, call.key, static_cast<jlong>(value));
    call.done("UserSettings.setLongForKey");
}

void JasperEngineAndroid::setFloatForKey(const char *key, float value){
    UserSettingsCall call(key);
    call.env->CallVoidMethod(call.jniData.userSettingsObjRef, call.jniData.setFloatForKeyRef, call.key, value);
    call.done("UserSettings.setFloatForKey");
}

void JasperEngineAndroid::setDoubleForKey(const char *key, double value){
    UserSettingsCall call(key);
    call.env->CallVoidMethod(call.jniData.userSettingsObjRef, call.jniData.setDoubleForKeyRef, call.key, value);
    call.done("UserSettings.setDoubleForKey");
}

void JasperEngineAndroid::setStringForKey(const char* key, const std::string& value){
    if (!key || !key[0]){
        jasperengine::Log::error("UserSettings: a key must be a non-empty string");
        return;
    }
    UserSettingsCall call(key);
    jstring strValue = newJavaString(call.env, value);
    call.env->CallVoidMethod(call.jniData.userSettingsObjRef, call.jniData.setStringForKeyRef, call.key, strValue);
    call.done("UserSettings.setStringForKey");
    if (strValue)
        call.env->DeleteLocalRef(strValue);
}

void JasperEngineAndroid::removeKey(const char* key){
    UserSettingsCall call(key);
    call.env->CallVoidMethod(call.jniData.userSettingsObjRef, call.jniData.removeKeyRef, call.key);
    call.done("UserSettings.removeKey");
}

bool JasperEngineAndroid::hasKey(const char* key){
    if (!key) return false;
    UserSettingsCall call(key);
    const bool value = call.env->CallBooleanMethod(call.jniData.userSettingsObjRef, call.jniData.hasKeyRef, call.key);
    return call.done("UserSettings.hasKey") && value;
}

void JasperEngineAndroid::removeAllKeys(){
    UserSettingsCall call(nullptr);
    call.env->CallVoidMethod(call.jniData.userSettingsObjRef, call.jniData.removeAllKeysRef);
    call.done("UserSettings.removeAllKeys");
}

void JasperEngineAndroid::saveUserSettings(){
    UserSettingsCall call(nullptr);
    call.env->CallBooleanMethod(call.jniData.userSettingsObjRef, call.jniData.saveUserSettingsRef);
    call.done("UserSettings.save");
}

// The export compiles the AdMob wrapper in only when the project enables AdMob
static bool hasAdMob(const JniData& jniData){
    if (jniData.adMobWrapperObjRef)
        return true;
    static bool warned = false;
    if (!warned){
        warned = true;
        jasperengine::Log::warn("AdMob is not in this build: enable Google AdMob in Project Settings > Player > Android");
    }
    return false;
}

void JasperEngineAndroid::initializeAdMob(bool tagForChildDirectedTreatment, bool tagForUnderAgeOfConsent){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();
    if (!hasAdMob(jniData)) return;

    env->CallVoidMethod(jniData.adMobWrapperObjRef, jniData.initializeAdMob, tagForChildDirectedTreatment, tagForUnderAgeOfConsent);
}

void JasperEngineAndroid::setMaxAdContentRating(jasperengine::AdMobRating rating){
    int irating = 0;
    if (rating == jasperengine::AdMobRating::General){
        irating = 1;
    }else if (rating == jasperengine::AdMobRating::ParentalGuidance){
        irating = 2;
    }else if (rating == jasperengine::AdMobRating::Teen){
        irating = 3;
    }else if (rating == jasperengine::AdMobRating::MatureAudience){
        irating = 4;
    }

    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();
    if (!hasAdMob(jniData)) return;

    env->CallVoidMethod(jniData.adMobWrapperObjRef, jniData.setMaxAdContentRating, irating);
}

void JasperEngineAndroid::loadInterstitialAd(const std::string& adUnitID){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();
    if (!hasAdMob(jniData)) return;

    jstring strAdUnitID = env->NewStringUTF(adUnitID.c_str());
    env->CallVoidMethod(jniData.adMobWrapperObjRef, jniData.loadInterstitialAd, strAdUnitID);
    env->DeleteLocalRef(strAdUnitID);
}

bool JasperEngineAndroid::isInterstitialAdLoaded(){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();
    if (!hasAdMob(jniData)) return false;

    bool value = env->CallBooleanMethod(jniData.adMobWrapperObjRef, jniData.isInterstitialAdLoaded);
    return value;
}

void JasperEngineAndroid::showInterstitialAd(){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();
    if (!hasAdMob(jniData)) return;

    env->CallVoidMethod(jniData.adMobWrapperObjRef, jniData.showInterstitialAd);
}

// ------------------------------------------------------------------ platform services

namespace {

// The calling thread's env for a JasperPlatform call: NULL when the class or method is missing
JNIEnv* platformEnv(jmethodID method){
    NativeEngine* nativeEngine = NativeEngine::getInstance();
    if (!nativeEngine || !method || !nativeEngine->getJniData().platformClsRef) return NULL;
    return nativeEngine->getJniEnv();
}

std::string callPlatformString(jmethodID method, bool withActivity){
    JNIEnv* env = platformEnv(method);
    if (!env) return "";
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    jstring value = withActivity
        ? (jstring)env->CallStaticObjectMethod(jniData.platformClsRef, method, jniData.gameActivityObjRef)
        : (jstring)env->CallStaticObjectMethod(jniData.platformClsRef, method);
    std::string out;
    if (!clearJavaException(env, "JasperPlatform")){
        out = javaStringToUtf8(env, value);
    }
    if (value) env->DeleteLocalRef(value);
    return out;
}

int callPlatformInt(jmethodID method, bool withActivity, int fallback){
    JNIEnv* env = platformEnv(method);
    if (!env) return fallback;
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    const jint value = withActivity
        ? env->CallStaticIntMethod(jniData.platformClsRef, method, jniData.gameActivityObjRef)
        : env->CallStaticIntMethod(jniData.platformClsRef, method);
    return clearJavaException(env, "JasperPlatform") ? fallback : (int)value;
}

jasperengine::DeviceOrientation lastDeviceOrientation = jasperengine::DeviceOrientation::Unknown;

} // namespace

bool JasperEngineAndroid::platformOpenURL(const std::string& url){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = platformEnv(jniData.platformOpenURL);
    if (!env) return System::platformOpenURL(url);
    jstring text = newJavaString(env, url);
    const jboolean opened = env->CallStaticBooleanMethod(jniData.platformClsRef, jniData.platformOpenURL, jniData.gameActivityObjRef, text);
    env->DeleteLocalRef(text);
    return !clearJavaException(env, "openURL") && opened == JNI_TRUE;
}

std::vector<std::string> JasperEngineAndroid::getPreferredLocales(){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = platformEnv(jniData.platformGetPreferredLocales);
    if (!env) return System::getPreferredLocales();
    jobjectArray array = (jobjectArray)env->CallStaticObjectMethod(jniData.platformClsRef, jniData.platformGetPreferredLocales);
    std::vector<std::string> tags;
    if (!clearJavaException(env, "getPreferredLocales") && array){
        const jsize count = env->GetArrayLength(array);
        for (jsize i = 0; i < count; i++){
            jstring tag = (jstring)env->GetObjectArrayElement(array, i);
            const std::string value = jasperengine::LocaleRules::normalize(javaStringToUtf8(env, tag));
            if (!value.empty()) tags.push_back(value);
            if (tag) env->DeleteLocalRef(tag);
        }
    }
    if (array) env->DeleteLocalRef(array);
    return tags.empty() ? System::getPreferredLocales() : tags;
}

std::string JasperEngineAndroid::getRegionOverride(){
    return callPlatformString(NativeEngine::getInstance()->getJniData().platformGetRegion, false);
}

std::string JasperEngineAndroid::getAppVersion(){
    static const std::string value = callPlatformString(NativeEngine::getInstance()->getJniData().platformGetAppVersion, true);
    return value.empty() ? System::getAppVersion() : value;
}

int64_t JasperEngineAndroid::getAppBuild(){
    static int64_t value = -1;
    if (value < 0){
        JniData& jniData = NativeEngine::getInstance()->getJniData();
        JNIEnv* env = platformEnv(jniData.platformGetAppBuild);
        value = 0;
        if (env){
            const jlong build = env->CallStaticLongMethod(jniData.platformClsRef, jniData.platformGetAppBuild, jniData.gameActivityObjRef);
            if (!clearJavaException(env, "getAppBuild")) value = (int64_t)build;
        }
    }
    return value > 0 ? value : System::getAppBuild();
}

std::string JasperEngineAndroid::getAppIdentifier(){
    static const std::string value = callPlatformString(NativeEngine::getInstance()->getJniData().platformGetAppIdentifier, true);
    return value.empty() ? System::getAppIdentifier() : value;
}

std::string JasperEngineAndroid::getAppName(){
    static const std::string value = callPlatformString(NativeEngine::getInstance()->getJniData().platformGetAppName, true);
    return value.empty() ? System::getAppName() : value;
}

std::string JasperEngineAndroid::getOperatingSystem(){
    static const std::string value = [this](){
        JniData& jniData = NativeEngine::getInstance()->getJniData();
        return jasperengine::OsRules::androidOperatingSystem(
            callPlatformString(jniData.platformGetOsRelease, false), getAndroidApiLevel(),
            callPlatformString(jniData.platformGetBuildDisplay, false),
            callPlatformString(jniData.platformGetBuildIncremental, false));
    }();
    return value;
}

std::string JasperEngineAndroid::getOperatingSystemVersion(){
    static const std::string value = callPlatformString(NativeEngine::getInstance()->getJniData().platformGetOsRelease, false);
    return value;
}

int JasperEngineAndroid::getAndroidApiLevel(){
    static const int value = callPlatformInt(NativeEngine::getInstance()->getJniData().platformGetApiLevel, false, 0);
    return value;
}

std::string JasperEngineAndroid::getDeviceModel(){
    static const std::string value = callPlatformString(NativeEngine::getInstance()->getJniData().platformGetDeviceModel, false);
    return value;
}

jasperengine::DeviceType JasperEngineAndroid::getDeviceType(){
    return jasperengine::DeviceType::Handheld;
}

int JasperEngineAndroid::getSystemMemorySize(){
    static const int value = callPlatformInt(NativeEngine::getInstance()->getJniData().platformGetSystemMemorySize, true, 0);
    return value;
}

jasperengine::NetworkReachability JasperEngineAndroid::getInternetReachability(){
    const int value = callPlatformInt(NativeEngine::getInstance()->getJniData().platformGetInternetReachability, true, 0);
    if (value == 1) return jasperengine::NetworkReachability::ReachableViaCarrierDataNetwork;
    if (value == 2) return jasperengine::NetworkReachability::ReachableViaLocalAreaNetwork;
    return jasperengine::NetworkReachability::NotReachable;
}

bool JasperEngineAndroid::vibrate(int milliseconds){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = platformEnv(jniData.platformVibrate);
    if (!env) return false;
    const jboolean done = env->CallStaticBooleanMethod(jniData.platformClsRef, jniData.platformVibrate, jniData.gameActivityObjRef, (jint)milliseconds);
    return !clearJavaException(env, "vibrate") && done == JNI_TRUE;
}

bool JasperEngineAndroid::hapticFeedback(jasperengine::HapticFeedback type){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = platformEnv(jniData.platformHapticFeedback);
    if (!env) return false;
    const jboolean done = env->CallStaticBooleanMethod(jniData.platformClsRef, jniData.platformHapticFeedback, jniData.gameActivityObjRef, (jint)type);
    return !clearJavaException(env, "hapticFeedback") && done == JNI_TRUE;
}

float JasperEngineAndroid::getScreenDpi(){
    return (float)callPlatformInt(NativeEngine::getInstance()->getJniData().platformGetScreenDpi, true, 0);
}

float JasperEngineAndroid::getRefreshRate(){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = platformEnv(jniData.platformGetRefreshRate);
    if (!env) return 60.0f;
    const jfloat rate = env->CallStaticFloatMethod(jniData.platformClsRef, jniData.platformGetRefreshRate, jniData.gameActivityObjRef);
    if (clearJavaException(env, "getRefreshRate") || rate <= 1.0f) return 60.0f;
    return (float)rate;
}

void JasperEngineAndroid::setKeepScreenOn(bool keepOn){
    GameActivity* activity = NativeEngine::getInstance()->getActivity();
    if (keepOn){
        GameActivity_setWindowFlags(activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);
    }else{
        GameActivity_setWindowFlags(activity, 0, AWINDOW_FLAG_KEEP_SCREEN_ON);
    }
}

void JasperEngineAndroid::applyOrientationPolicy(jasperengine::ScreenOrientation requested, uint8_t allowedMask){
    bool unsupported = false;
    const int orientation = jasperengine::PlatformRules::androidRequestedOrientation(requested, allowedMask, &unsupported);
    static bool warned = false;
    if (unsupported && !warned){
        warned = true;
        jasperengine::Log::warn("Android cannot restrict auto-rotation to this set of Screen.autorotateTo* orientations; all four are allowed");
    }
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = platformEnv(jniData.platformSetOrientationPolicy);
    if (!env) return;
    env->CallStaticVoidMethod(jniData.platformClsRef, jniData.platformSetOrientationPolicy, jniData.gameActivityObjRef, (jint)orientation);
    clearJavaException(env, "setOrientationPolicy");
}

jasperengine::ScreenOrientation JasperEngineAndroid::getScreenOrientation(){
    NativeEngine* nativeEngine = NativeEngine::getInstance();
    if (!nativeEngine->getJniData().platformDisplayRotation) return System::getScreenOrientation();
    return jasperengine::PlatformRules::orientationFromRotation(nativeEngine->getDisplayRotation(), nativeEngine->isNaturalPortrait());
}

jasperengine::DeviceOrientation JasperEngineAndroid::getDeviceOrientation(){
    NativeEngine* nativeEngine = NativeEngine::getInstance();
    lastDeviceOrientation = jasperengine::PlatformRules::deviceOrientationFromDegrees(
        nativeEngine->getDeviceDegrees(), nativeEngine->isNaturalPortrait(), lastDeviceOrientation);
    return lastDeviceOrientation;
}

void JasperEngineAndroid::applyFramePacing(const jasperengine::FramePacing& pacing){
    const int fps = pacing.mobileFps > 0 ? pacing.mobileFps : 60;
    if (SwappyGL_isEnabled()){
        // Swappy picks the nearest multiple of the display period
        SwappyGL_setSwapIntervalNS((uint64_t)(1000000000.0 / (double)fps));
        NativeEngine::getInstance()->setFallbackPacing(1, 0.0);
        return;
    }
    // without Swappy (devices it refuses): the swap interval nearest the rate, and the frame
    // limiter for what an interval cannot give (45 fps at 60 Hz). The swap interval needs the
    // context: systemViewLoaded applies the pacing again once it exists.
    const double refresh = getRefreshRate();
    const int interval = std::max(1, (int)std::lround(refresh / (double)fps));
    const double intervalFps = refresh / (double)interval;
    NativeEngine::getInstance()->setFallbackPacing(interval, std::fabs(intervalFps - (double)fps) > 1.0 ? (double)fps : 0.0);
}
