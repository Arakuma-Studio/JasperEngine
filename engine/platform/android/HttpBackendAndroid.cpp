// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The HTTP backend of Android: java/com/jaspergameengine/jasperengine/JasperHttp.java runs each
// request (HttpURLConnection on a thread pool) and hands the response back through the native
// methods below, which feed the HttpTransfer (the same body, file, redirect and error rules as
// every platform).

#if defined(__ANDROID__)

#include "HttpBackendAndroid.h"

#include "Log.h"
#include "net/HttpBackend.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace jasperengine;

namespace {

    JavaVM* javaVm = nullptr;
    jclass httpClass = nullptr;      // global reference
    jmethodID startMethod = nullptr;
    jmethodID cancelMethod = nullptr;

    std::mutex transfersMutex;
    std::unordered_map<jlong, std::shared_ptr<HttpTransfer>> transfers;

    std::shared_ptr<HttpTransfer> findTransfer(jlong id){
        std::lock_guard<std::mutex> lock(transfersMutex);
        auto it = transfers.find(id);
        return it == transfers.end() ? nullptr : it->second;
    }

    std::shared_ptr<HttpTransfer> takeTransfer(jlong id){
        std::lock_guard<std::mutex> lock(transfersMutex);
        auto it = transfers.find(id);
        if (it == transfers.end()) return nullptr;
        std::shared_ptr<HttpTransfer> transfer = std::move(it->second);
        transfers.erase(it);
        return transfer;
    }

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

    jobjectArray newStringArray(JNIEnv* env, const std::vector<std::string>& values){
        jclass stringClass = env->FindClass("java/lang/String");
        jobjectArray array = env->NewObjectArray((jsize)values.size(), stringClass, nullptr);
        for (size_t i = 0; array && i < values.size(); i++){
            jstring value = newString(env, values[i]);
            env->SetObjectArrayElement(array, (jsize)i, value);
            env->DeleteLocalRef(value);
        }
        env->DeleteLocalRef(stringClass);
        return array;
    }

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

    // ---- JasperHttp's native methods (its pool threads)

    jboolean JNICALL nativeResponse(JNIEnv* env, jclass, jlong id, jint status, jstring finalUrl, jobjectArray headers){
        std::shared_ptr<HttpTransfer> transfer = findTransfer(id);
        if (!transfer) return JNI_FALSE;
        HttpHeaders list;
        const jsize count = headers ? env->GetArrayLength(headers) : 0;
        for (jsize i = 0; i + 1 < count; i += 2){
            jstring name = (jstring)env->GetObjectArrayElement(headers, i);
            jstring value = (jstring)env->GetObjectArrayElement(headers, i + 1);
            list.emplace_back(toUtf8(env, name), toUtf8(env, value));
            env->DeleteLocalRef(name);
            env->DeleteLocalRef(value);
        }
        return transfer->begin((int)status, std::move(list), toUtf8(env, finalUrl)) ? JNI_TRUE : JNI_FALSE;
    }

    jboolean JNICALL nativeData(JNIEnv* env, jclass, jlong id, jbyteArray data, jint length){
        std::shared_ptr<HttpTransfer> transfer = findTransfer(id);
        if (!transfer || !data || length < 0) return JNI_FALSE;
        static thread_local std::vector<uint8_t> buffer;
        buffer.resize((size_t)length);
        if (length > 0) env->GetByteArrayRegion(data, 0, length, reinterpret_cast<jbyte*>(buffer.data()));
        return transfer->write(buffer.data(), buffer.size()) ? JNI_TRUE : JNI_FALSE;
    }

    void JNICALL nativeComplete(JNIEnv* env, jclass, jlong id, jstring errorCode, jstring message){
        std::shared_ptr<HttpTransfer> transfer = takeTransfer(id);
        if (transfer) transfer->finish(toUtf8(env, errorCode), toUtf8(env, message));
    }

    jobjectArray JNICALL nativeRedirect(JNIEnv* env, jclass, jlong id, jint status, jstring location){
        std::shared_ptr<HttpTransfer> transfer = findTransfer(id);
        if (!transfer) return newStringArray(env, {"fail"});
        switch (transfer->redirect((int)status, toUtf8(env, location))){
            case HttpRedirect::Action::Stop:
                return nullptr;
            case HttpRedirect::Action::Fail:
                return newStringArray(env, {"fail"});
            case HttpRedirect::Action::Follow: {
                std::vector<std::string> next = {"follow", transfer->url, transfer->method, transfer->body.empty() ? "0" : "1"};
                for (const auto& [name, value] : transfer->headers){
                    next.push_back(name);
                    next.push_back(value);
                }
                return newStringArray(env, next);
            }
        }
        return nullptr;
    }

    class AndroidBackend : public HttpBackend {
    public:
        bool available(std::string& whyNot) override{
            if (httpClass && startMethod && cancelMethod) return true;
            whyNot = "the Java side of HTTP (JasperHttp) is not set up";
            return false;
        }

        void start(const std::shared_ptr<HttpTransfer>& transfer) override{
            JNIEnv* env = currentEnv();
            if (!env || !httpClass){
                transfer->finish("unsupported", "no JNI environment");
                return;
            }
            {
                std::lock_guard<std::mutex> lock(transfersMutex);
                transfers[(jlong)transfer->id] = transfer;
            }
            std::vector<std::string> headerValues;
            for (const auto& [name, value] : transfer->headers){
                headerValues.push_back(name);
                headerValues.push_back(value);
            }
            jstring method = newString(env, transfer->method);
            jstring url = newString(env, transfer->url);
            jobjectArray headers = newStringArray(env, headerValues);
            jbyteArray body = env->NewByteArray((jsize)transfer->body.size());
            if (body && !transfer->body.empty()){
                env->SetByteArrayRegion(body, 0, (jsize)transfer->body.size(), reinterpret_cast<const jbyte*>(transfer->body.data()));
            }
            const double total = transfer->totalTimeout() > 0.0 ? transfer->remainingTimeout() : 0.0;
            const jint timeoutMs = total > 0.0 ? (jint)std::min(2147483647.0, std::max(1.0, total * 1000.0)) : 0;
            const jint connectMs = transfer->connectTimeout > 0.0 ? (jint)std::min(2147483647.0, transfer->connectTimeout * 1000.0) : 0;
            env->CallStaticVoidMethod(httpClass, startMethod, (jlong)transfer->id, method, url, headers, body, connectMs, timeoutMs,
                                      transfer->verifyTls ? JNI_TRUE : JNI_FALSE);
            const bool failed = clearException(env);
            env->DeleteLocalRef(method);
            env->DeleteLocalRef(url);
            env->DeleteLocalRef(headers);
            env->DeleteLocalRef(body);
            if (failed){
                std::shared_ptr<HttpTransfer> started = takeTransfer((jlong)transfer->id);
                if (started) started->finish("network", "JasperHttp.start failed");
            }
        }

        void cancel(uint64_t id) override{
            JNIEnv* env = currentEnv();
            if (!env || !httpClass) return;
            env->CallStaticVoidMethod(httpClass, cancelMethod, (jlong)id);
            clearException(env);
        }

        void shutdown() override{
            // the Java pool's threads are daemons; running requests end as cancelled one by one
        }
    };

}

void jasperengine::httpAndroidSetup(JNIEnv* env, jobject activity){
    if (!env || !activity) return;
    env->GetJavaVM(&javaVm);

    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getClassLoader = env->GetMethodID(activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = getClassLoader ? env->CallObjectMethod(activity, getClassLoader) : nullptr;
    jclass loaderClass = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClass = loaderClass ? env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
    jstring name = env->NewStringUTF("com.jaspergameengine.jasperengine.JasperHttp");
    jclass found = (loader && loadClass) ? (jclass)env->CallObjectMethod(loader, loadClass, name) : nullptr;
    if (clearException(env)) found = nullptr;
    env->DeleteLocalRef(name);
    if (loaderClass) env->DeleteLocalRef(loaderClass);
    if (loader) env->DeleteLocalRef(loader);
    env->DeleteLocalRef(activityClass);
    if (!found){
        Log::error("Http: com.jaspergameengine.jasperengine.JasperHttp not found; HTTP requests will fail");
        return;
    }

    // an activity made again (setupJNI runs again): the new class reference replaces the old one
    if (httpClass) env->DeleteGlobalRef(httpClass);
    httpClass = (jclass)env->NewGlobalRef(found);
    env->DeleteLocalRef(found);

    static const JNINativeMethod natives[] = {
        {"nativeResponse", "(JILjava/lang/String;[Ljava/lang/String;)Z", reinterpret_cast<void*>(&nativeResponse)},
        {"nativeData", "(J[BI)Z", reinterpret_cast<void*>(&nativeData)},
        {"nativeComplete", "(JLjava/lang/String;Ljava/lang/String;)V", reinterpret_cast<void*>(&nativeComplete)},
        {"nativeRedirect", "(JILjava/lang/String;)[Ljava/lang/String;", reinterpret_cast<void*>(&nativeRedirect)},
    };
    if (env->RegisterNatives(httpClass, natives, sizeof(natives) / sizeof(natives[0])) != JNI_OK){
        clearException(env);
        Log::error("Http: registering the JasperHttp natives failed; HTTP requests will fail");
        env->DeleteGlobalRef(httpClass);
        httpClass = nullptr;
        return;
    }
    startMethod = env->GetStaticMethodID(httpClass, "start", "(JLjava/lang/String;Ljava/lang/String;[Ljava/lang/String;[BIIZ)V");
    cancelMethod = env->GetStaticMethodID(httpClass, "cancel", "(J)V");
    if (clearException(env) || !startMethod || !cancelMethod){
        Log::error("Http: JasperHttp.start/cancel not found; HTTP requests will fail");
        startMethod = nullptr;
        cancelMethod = nullptr;
    }
}

HttpBackend* jasperengine::createPlatformHttpBackend(){
    return new AndroidBackend();
}

#endif
