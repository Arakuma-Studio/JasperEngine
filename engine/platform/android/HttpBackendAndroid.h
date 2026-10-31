// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTTPBACKENDANDROID_H
#define HTTPBACKENDANDROID_H

#include <jni.h>

namespace jasperengine {

    // NativeEngine::setupJNI: finds com.jaspergameengine.jasperengine.JasperHttp through the
    // activity's class loader (FindClass on a native thread only sees the system classes),
    // keeps it as a global reference and registers its native callbacks. Until it ran, Http
    // requests fail with "unsupported".
    void httpAndroidSetup(JNIEnv* env, jobject activity);

}

#endif // HTTPBACKENDANDROID_H
