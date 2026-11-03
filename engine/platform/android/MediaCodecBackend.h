// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef MEDIACODECBACKEND_H
#define MEDIACODECBACKEND_H

struct AAssetManager;

namespace jasperengine {

    // NativeEngine hands the APK's asset manager to the video decoders (MediaCodecBackend.cpp:
    // file descriptors of uncompressed video assets below API 28)
    void mediaCodecSetAssetManager(AAssetManager* manager);

}

#endif //MEDIACODECBACKEND_H
