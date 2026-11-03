// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PlatformConfig.h"

jasperengine::PlatformConfig& jasperengine::platformConfig() {
    static PlatformConfig config;
    return config;
}
