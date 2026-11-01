// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef NATIVEPLATFORMANDROID_H
#define NATIVEPLATFORMANDROID_H

#include <jni.h>

namespace jasperengine {

    // NativeEngine::setupJNI: finds com.jaspergameengine.bridge.JasperNative through the
    // activity's class loader (FindClass on a native thread only sees the system classes),
    // keeps it as a global reference and registers its natives. Until it ran, Native calls
    // see no Java plugins.
    void nativeBridgeAndroidSetup(JNIEnv* env, jobject activity);

}

#endif // NATIVEPLATFORMANDROID_H
