// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The native bridge's platform layer on Android (core/native/NativePlatform.h): Java/Kotlin
// static methods, reached through com.jaspergameengine.bridge.JasperNative
// (engine/platform/android/java-bridge), which resolves the plugin class, converts the JSON
// arguments and invokes by reflection. JSON crosses JNI as UTF-8 text in Java strings. The
// natives below bring async completions, events, engine pause requests and game-thread tasks
// back from any Java thread.

#if defined(__ANDROID__)

#include "NativePlatformAndroid.h"

#include "Log.h"
#include "native/NativePlatform.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    JavaVM* javaVm = nullptr;
    jclass bridgeClass = nullptr;      // global reference to JasperNative
    jmethodID hasMethod = nullptr;
    jmethodID callMethod = nullptr;
    jmethodID callAsyncMethod = nullptr;
    jmethodID cancelMethod = nullptr;
    jmethodID shutdownMethod = nullptr;
    jmethodID runTaskMethod = nullptr;

    // ---- strings (UTF-8 <-> Java's UTF-16; NewStringUTF wants modified UTF-8)

    jstring newString(JNIEnv* env, const std::string& utf8){
        std::vector<jchar> out;
        out.reserve(utf8.size());
        size_t i = 0;
        while (i < utf8.size()){
            const unsigned char c = (unsigned char)utf8[i];
            uint32_t cp = 0xFFFD;
            size_t n = 1;
            if (c < 0x80){
                cp = c;
            } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()){
                cp = ((c & 0x1Fu) << 6) | (utf8[i + 1] & 0x3Fu);
                n = 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()){
                cp = ((c & 0x0Fu) << 12) | ((utf8[i + 1] & 0x3Fu) << 6) | (utf8[i + 2] & 0x3Fu);
                n = 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()){
                cp = ((c & 0x07u) << 18) | ((utf8[i + 1] & 0x3Fu) << 12) | ((utf8[i + 2] & 0x3Fu) << 6) | (utf8[i + 3] & 0x3Fu);
                n = 4;
            }
            i += n;
            if (cp >= 0x10000u && cp <= 0x10FFFFu){
                cp -= 0x10000u;
                out.push_back((jchar)(0xD800u + (cp >> 10)));
                out.push_back((jchar)(0xDC00u + (cp & 0x3FFu)));
            } else {
                out.push_back((jchar)(cp > 0x10FFFFu ? 0xFFFD : cp));
            }
        }
        static const jchar empty = 0;
        return env->NewString(out.empty() ? &empty : out.data(), (jsize)out.size());
    }

    // a lone surrogate becomes U+FFFD
    std::string toUtf8(JNIEnv* env, jstring text){
        std::string out;
        if (!text) return out;
        const jsize length = env->GetStringLength(text);
        const jchar* chars = env->GetStringChars(text, nullptr);
        if (!chars) return out;
        for (jsize i = 0; i < length; i++){
            uint32_t cp = chars[i];
            if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < length && chars[i + 1] >= 0xDC00u && chars[i + 1] <= 0xDFFFu){
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (chars[i + 1] - 0xDC00u);
                i++;
            } else if (cp >= 0xD800u && cp <= 0xDFFFu){
                cp = 0xFFFD;
            }
            if (cp < 0x80){
                out += (char)cp;
            } else if (cp < 0x800){
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000){
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xF0 | (cp >> 18));
                out += (char)(0x80 | ((cp >> 12) & 0x3F));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }
        env->ReleaseStringChars(text, chars);
        return out;
    }

    // logs and clears a pending Java exception; true when there was one
    bool clearException(JNIEnv* env){
        if (!env->ExceptionCheck()) return false;
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
    }

    // The calling thread's JNIEnv: the game thread is attached (NativeEngine); another one is
    // attached once and detached when it ends
    struct ThreadAttachment {
        bool attached = false;
        ~ThreadAttachment(){
            if (attached && javaVm) javaVm->DetachCurrentThread();
        }
    };

    JNIEnv* currentEnv(){
        if (!javaVm) return nullptr;
        JNIEnv* env = nullptr;
        const jint result = javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (result == JNI_OK) return env;
        if (result != JNI_EDETACHED) return nullptr;
        static thread_local ThreadAttachment attachment;
        if (javaVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
        attachment.attached = true;
        return env;
    }

    // JSON text from Java -> a value; unsupported_type when it does not parse (never expected:
    // JasperNative writes it)
    bool parseJson(const std::string& text, NativeJson& out, NativeError& err){
        if (text.empty()){
            out = nullptr;
            return true;
        }
        out = NativeJson::parse(text, nullptr, false);
        if (out.is_discarded()){
            out = nullptr;
            err = NativeError("unsupported_type", "the Java side returned text that is not JSON");
            return false;
        }
        return true;
    }

    // ---- natives (any Java thread)

    void JNICALL nativeComplete(JNIEnv* env, jclass, jlong id, jstring json, jstring code, jstring message){
        const std::string errorCode = toUtf8(env, code);
        if (!errorCode.empty()){
            const NativeError err(errorCode, toUtf8(env, message));
            NativeBridge::completeFromPlatform((uint64_t)id, nullptr, &err);
            return;
        }
        NativeJson value;
        NativeError err;
        if (!parseJson(toUtf8(env, json), value, err)){
            NativeBridge::completeFromPlatform((uint64_t)id, nullptr, &err);
            return;
        }
        NativeBridge::completeFromPlatform((uint64_t)id, std::move(value), nullptr);
    }

    void JNICALL nativeEmit(JNIEnv* env, jclass, jstring plugin, jstring event, jstring json){
        NativeJson payload;
        NativeError err;
        if (!parseJson(toUtf8(env, json), payload, err)){
            Log::error("Native: an event of %s is not JSON; it is dropped", toUtf8(env, plugin).c_str());
            return;
        }
        NativeBridge::emit(toUtf8(env, plugin), toUtf8(env, event), std::move(payload));
    }

    void JNICALL nativePauseEngine(JNIEnv*, jclass){
        NativeBridge::pauseEngine();
    }

    void JNICALL nativeResumeEngine(JNIEnv*, jclass){
        NativeBridge::resumeEngine();
    }

    void JNICALL nativePostToGameThread(JNIEnv*, jclass, jlong token){
        // paused frames too (a resumeEngine from the task must run)
        NativeBridge::postToGameThread([token]() {
            JNIEnv* env = currentEnv();
            if (!env || !bridgeClass || !runTaskMethod) return;
            env->CallStaticVoidMethod(bridgeClass, runTaskMethod, token);
            clearException(env);
        });
    }

    class AndroidNativePlatform : public NativePlatform {
    public:
        const char* name() const override{ return "Android"; }

        bool has(const std::string& plugin, const std::string& method) override{
            JNIEnv* env = currentEnv();
            if (!env || !bridgeClass || !hasMethod) return false;
            jstring p = newString(env, plugin);
            jstring m = newString(env, method);
            const jboolean found = env->CallStaticBooleanMethod(bridgeClass, hasMethod, p, m);
            const bool failed = clearException(env);
            env->DeleteLocalRef(p);
            env->DeleteLocalRef(m);
            return !failed && found == JNI_TRUE;
        }

        NativeJson call(const std::string& plugin, const std::string& method, const NativeJson& args, NativeError& err) override{
            JNIEnv* env = currentEnv();
            if (!env || !bridgeClass || !callMethod){
                err = NativeError("unavailable", "the Java side of the native bridge (JasperNative) is not set up");
                return nullptr;
            }
            jstring p = newString(env, plugin);
            jstring m = newString(env, method);
            jstring a = newString(env, args.dump());
            jobjectArray result = (jobjectArray)env->CallStaticObjectMethod(bridgeClass, callMethod, p, m, a);
            const bool failed = clearException(env);
            env->DeleteLocalRef(p);
            env->DeleteLocalRef(m);
            env->DeleteLocalRef(a);
            if (failed || !result || env->GetArrayLength(result) < 3){
                if (result) env->DeleteLocalRef(result);
                err = NativeError("exception", "JasperNative.call failed for " + plugin + "." + method);
                return nullptr;
            }
            jstring json = (jstring)env->GetObjectArrayElement(result, 0);
            jstring code = (jstring)env->GetObjectArrayElement(result, 1);
            jstring message = (jstring)env->GetObjectArrayElement(result, 2);
            const std::string errorCode = toUtf8(env, code);
            NativeJson value;
            if (!errorCode.empty()){
                err = NativeError(errorCode, toUtf8(env, message));
            } else {
                parseJson(toUtf8(env, json), value, err);
            }
            if (json) env->DeleteLocalRef(json);
            if (code) env->DeleteLocalRef(code);
            if (message) env->DeleteLocalRef(message);
            env->DeleteLocalRef(result);
            return err.isSet() ? NativeJson() : value;
        }

        void callAsync(uint64_t id, const std::string& plugin, const std::string& method, const NativeJson& args, bool background) override{
            JNIEnv* env = currentEnv();
            if (!env || !bridgeClass || !callAsyncMethod){
                const NativeError err("unavailable", "the Java side of the native bridge (JasperNative) is not set up");
                NativeBridge::completeFromPlatform(id, nullptr, &err);
                return;
            }
            jstring p = newString(env, plugin);
            jstring m = newString(env, method);
            jstring a = newString(env, args.dump());
            env->CallStaticVoidMethod(bridgeClass, callAsyncMethod, (jlong)id, p, m, a, background ? JNI_TRUE : JNI_FALSE);
            const bool failed = clearException(env);
            env->DeleteLocalRef(p);
            env->DeleteLocalRef(m);
            env->DeleteLocalRef(a);
            if (failed){
                const NativeError err("exception", "JasperNative.callAsync failed for " + plugin + "." + method);
                NativeBridge::completeFromPlatform(id, nullptr, &err);
            }
        }

        void cancel(uint64_t id) override{
            JNIEnv* env = currentEnv();
            if (!env || !bridgeClass || !cancelMethod) return;
            env->CallStaticVoidMethod(bridgeClass, cancelMethod, (jlong)id);
            clearException(env);
        }

        void shutdown() override{
            JNIEnv* env = currentEnv();
            if (!env || !bridgeClass || !shutdownMethod) return;
            env->CallStaticVoidMethod(bridgeClass, shutdownMethod);
            clearException(env);
        }
    };

}

namespace {

    // a static method id, or null with the exception cleared and logged
    jmethodID staticMethod(JNIEnv* env, const char* name, const char* signature){
        jmethodID id = env->GetStaticMethodID(bridgeClass, name, signature);
        if (clearException(env) || !id){
            Log::error("Native: JasperNative.%s%s is missing", name, signature);
            return nullptr;
        }
        return id;
    }

    // keeps `found` (JasperNative) and registers its natives; true when everything is there
    bool registerBridge(JNIEnv* env, jclass found){
        if (bridgeClass) env->DeleteGlobalRef(bridgeClass);
        bridgeClass = (jclass)env->NewGlobalRef(found);
        static const JNINativeMethod natives[] = {
            {"nativeComplete", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void*>(&nativeComplete)},
            {"nativeEmit", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void*>(&nativeEmit)},
            {"nativePauseEngine", "()V", reinterpret_cast<void*>(&nativePauseEngine)},
            {"nativeResumeEngine", "()V", reinterpret_cast<void*>(&nativeResumeEngine)},
            {"nativePostToGameThread", "(J)V", reinterpret_cast<void*>(&nativePostToGameThread)},
        };
        if (env->RegisterNatives(bridgeClass, natives, sizeof(natives) / sizeof(natives[0])) != JNI_OK){
            clearException(env);
            Log::error("Native: registering the JasperNative natives failed; Java native plugins will not be reached");
            env->DeleteGlobalRef(bridgeClass);
            bridgeClass = nullptr;
            return false;
        }
        hasMethod = staticMethod(env, "has", "(Ljava/lang/String;Ljava/lang/String;)Z");
        callMethod = staticMethod(env, "call", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;");
        callAsyncMethod = staticMethod(env, "callAsync", "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V");
        cancelMethod = staticMethod(env, "cancel", "(J)V");
        shutdownMethod = staticMethod(env, "shutdown", "()V");
        runTaskMethod = staticMethod(env, "runGameThreadTask", "(J)V");
        if (!hasMethod || !callMethod || !callAsyncMethod || !cancelMethod || !shutdownMethod || !runTaskMethod){
            env->DeleteGlobalRef(bridgeClass);
            bridgeClass = nullptr;
            return false;
        }
        return true;
    }

}

// System.loadLibrary (MainActivity's static initializer, on the UI thread, with the app's class
// loader): the natives exist before MainActivity.onCreate runs the plugins' onJasperInit, which
// may already emit or pause the engine
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*){
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) return JNI_VERSION_1_6;
    javaVm = vm;
    jclass found = env->FindClass("com/jaspergameengine/bridge/JasperNative");
    if (clearException(env) || !found) return JNI_VERSION_1_6;   // NativeEngine::setupJNI tries again
    registerBridge(env, found);
    env->DeleteLocalRef(found);
    return JNI_VERSION_1_6;
}

void jasperengine::nativeBridgeAndroidSetup(JNIEnv* env, jobject activity){
    if (!env || !activity) return;
    env->GetJavaVM(&javaVm);
    // registered at load already
    if (bridgeClass) return;

    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getClassLoader = env->GetMethodID(activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = getClassLoader ? env->CallObjectMethod(activity, getClassLoader) : nullptr;
    jclass loaderClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = loaderClass ? env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
    jstring name = env->NewStringUTF("com.jaspergameengine.bridge.JasperNative");
    jclass found = (loader && loadClass) ? (jclass)env->CallObjectMethod(loader, loadClass, name) : nullptr;
    if (clearException(env)) found = nullptr;
    env->DeleteLocalRef(name);
    if (loaderClass) env->DeleteLocalRef(loaderClass);
    if (loader) env->DeleteLocalRef(loader);
    env->DeleteLocalRef(activityClass);
    if (!found){
        Log::error("Native: com.jaspergameengine.bridge.JasperNative not found; Java native plugins will not be reached");
        return;
    }
    registerBridge(env, found);
    env->DeleteLocalRef(found);
}

NativePlatform* jasperengine::createNativePlatform(){
    return new AndroidNativePlatform();
}

#endif
