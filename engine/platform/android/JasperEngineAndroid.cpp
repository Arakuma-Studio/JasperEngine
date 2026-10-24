// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineAndroid.h"

#include <errno.h>
#include <android/log.h>
#include <stdarg.h>
#include <android/asset_manager.h>
#include <vector>
#include "NativeEngine.h"
#include "Log.h"
#include "util/StringUtils.h"

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
        // wchar_t is 32 bits on Android; toUTF8 replaces surrogates with U+FFFD
        out += jasperengine::StringUtils::toUTF8(static_cast<wchar_t>(cp));
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

void JasperEngineAndroid::platformLog(const int type, const char *fmt, va_list args){
    int priority = ANDROID_LOG_VERBOSE;

    if (type == D_LOG_VERBOSE){
        priority = ANDROID_LOG_VERBOSE;
    }else if (type == D_LOG_DEBUG){
        priority = ANDROID_LOG_DEBUG;
    }else if (type == D_LOG_WARN){
        priority = ANDROID_LOG_WARN;
    }else if (type == D_LOG_ERROR){
        priority = ANDROID_LOG_ERROR;
    }

    __android_log_vprint(priority, "JasperEngine", fmt, args);
}

bool JasperEngineAndroid::getBoolForKey(const char *key, bool defaultValue){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    bool value = env->CallBooleanMethod(jniData.userSettingsObjRef, jniData.getBoolForKeyRef, strKey, defaultValue);
    env->DeleteLocalRef(strKey);
    return value;
}

int JasperEngineAndroid::getIntegerForKey(const char *key, int defaultValue){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    int value = env->CallIntMethod(jniData.userSettingsObjRef, jniData.getIntegerForKeyRef, strKey, defaultValue);
    env->DeleteLocalRef(strKey);
    return value;
}

long JasperEngineAndroid::getLongForKey(const char *key, long defaultValue){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    long value = env->CallLongMethod(jniData.userSettingsObjRef, jniData.getLongForKeyRef, strKey, defaultValue);
    env->DeleteLocalRef(strKey);
    return value;
}

float JasperEngineAndroid::getFloatForKey(const char *key, float defaultValue){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    float value = env->CallFloatMethod(jniData.userSettingsObjRef, jniData.getFloatForKeyRef, strKey, defaultValue);
    env->DeleteLocalRef(strKey);
    return value;
}

double JasperEngineAndroid::getDoubleForKey(const char *key, double defaultValue){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    double value = env->CallDoubleMethod(jniData.userSettingsObjRef, jniData.getDoubleForKeyRef, strKey, defaultValue);
    env->DeleteLocalRef(strKey);
    return value;
}

std::string JasperEngineAndroid::getStringForKey(const char *key, const std::string& defaultValue){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    jstring strDefaultValue = env->NewStringUTF(defaultValue.c_str());
    jstring rv = (jstring)env->CallObjectMethod(jniData.userSettingsObjRef, jniData.getStringForKeyRef, strKey, strDefaultValue);
    std::string value = env->GetStringUTFChars(rv, 0);
    env->DeleteLocalRef(strKey);
    env->DeleteLocalRef(strDefaultValue);
    env->DeleteLocalRef(rv);
    return value;
}

void JasperEngineAndroid::setBoolForKey(const char *key, bool value){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    env->CallVoidMethod(jniData.userSettingsObjRef, jniData.setBoolForKeyRef, strKey, value);
    env->DeleteLocalRef(strKey);
}

void JasperEngineAndroid::setIntegerForKey(const char *key, int value){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    env->CallVoidMethod(jniData.userSettingsObjRef, jniData.setIntegerForKeyRef, strKey, value);
    env->DeleteLocalRef(strKey);
}

void JasperEngineAndroid::setLongForKey(const char *key, long value){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    env->CallVoidMethod(jniData.userSettingsObjRef, jniData.setLongForKeyRef, strKey, value);
    env->DeleteLocalRef(strKey);
}

void JasperEngineAndroid::setFloatForKey(const char *key, float value){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    env->CallVoidMethod(jniData.userSettingsObjRef, jniData.setFloatForKeyRef, strKey, value);
    env->DeleteLocalRef(strKey);
}

void JasperEngineAndroid::setDoubleForKey(const char *key, double value){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    env->CallVoidMethod(jniData.userSettingsObjRef, jniData.setDoubleForKeyRef, strKey, value);
    env->DeleteLocalRef(strKey);
}

void JasperEngineAndroid::setStringForKey(const char* key, const std::string& value){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    jstring strValue = env->NewStringUTF(value.c_str());
    env->CallVoidMethod(jniData.userSettingsObjRef, jniData.setStringForKeyRef, strKey, strValue);
    env->DeleteLocalRef(strKey);
    env->DeleteLocalRef(strValue);
}

void JasperEngineAndroid::removeKey(const char* key){
    JniData& jniData = NativeEngine::getInstance()->getJniData();
    JNIEnv* env = NativeEngine::getInstance()->getJniEnv();

    jstring strKey = env->NewStringUTF(key);
    env->CallVoidMethod(jniData.userSettingsObjRef, jniData.removeKeyRef, strKey);
    env->DeleteLocalRef(strKey);
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
