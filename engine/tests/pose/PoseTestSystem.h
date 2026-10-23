// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The file helpers read through the engine file API (Data, File), which asks the engine's System
// for the file open call. The pose tests run without an Engine, so a file test installs this
// minimal System first (only when no System is installed yet). System::setSystemInstance is
// private to Engine, hence the access trick, confined to this test header.

#ifndef POSE_TEST_SYSTEM_H
#define POSE_TEST_SYSTEM_H

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define private public
#include "System.h"
#undef private

namespace jasperengine { namespace test {

    class PoseTestSystem : public System {
    public:
        int getScreenWidth() override { return 640; }
        int getScreenHeight() override { return 360; }
    };

    inline void ensurePoseTestSystem(){
        static PoseTestSystem system;
        if (!System::systemInstance){
            System::setSystemInstance(&system);
        }
    }

}}

#endif // POSE_TEST_SYSTEM_H
