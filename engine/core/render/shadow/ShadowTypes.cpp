// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "ShadowTypes.h"

using namespace jasperengine;

const char* jasperengine::shadowSlotPathName(ShadowSlotPath path){
    switch (path){
        case ShadowSlotPath::SKIP: return "skip";
        case ShadowSlotPath::RESTORE_DYNAMIC: return "restore";
        case ShadowSlotPath::RENDER_STATIC: return "static";
        case ShadowSlotPath::FULL: return "full";
        case ShadowSlotPath::UNCACHED: return "uncached";
    }
    return "skip";
}

const char* jasperengine::shadowDirtyCauseName(ShadowDirtyCause cause){
    switch (cause){
        case ShadowDirtyCause::NONE: return "none";
        case ShadowDirtyCause::FIRST: return "first";
        case ShadowDirtyCause::LIGHT: return "light";
        case ShadowDirtyCause::FIT: return "fit";
        case ShadowDirtyCause::STATIC_CASTER: return "static_caster";
        case ShadowDirtyCause::DYNAMIC_CASTER: return "dynamic_caster";
        case ShadowDirtyCause::CAMERA_CASTER: return "camera_caster";
        case ShadowDirtyCause::ATLAS: return "atlas";
        case ShadowDirtyCause::FORCED: return "forced";
    }
    return "none";
}
