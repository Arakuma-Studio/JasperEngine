// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Linux, Windows (and any platform without a native layer): C++ plugins and Lua mocks only

#include "native/NativePlatform.h"

jasperengine::NativePlatform* jasperengine::createNativePlatform(){
    return nullptr;
}
