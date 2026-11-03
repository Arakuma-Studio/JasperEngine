// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The test binary's Metal device on macOS: the system default device, no window, no layer.
// Frames render into engine Framebuffers only; nothing is presented.

#include "HeadlessContext.h"

#import <Metal/Metal.h>

#include <cstdio>
#include <string>

using namespace jasperengine::test;

namespace {
    id<MTLDevice> device = nil;
    std::string rendererName;
}

bool HeadlessContext::create(int width, int height){
    (void)width;
    (void)height;
    if (device)
        return true;

    device = MTLCreateSystemDefaultDevice();
    if (!device){
        std::fprintf(stderr, "HeadlessContext: no Metal device\n");
        return false;
    }
    rendererName = [[device name] UTF8String];
    return true;
}

void HeadlessContext::destroy(){
    device = nil;
}

bool HeadlessContext::isCreated(){
    return device != nil;
}

const char* HeadlessContext::getBackendName(){
    return "metal";
}

const char* HeadlessContext::getRendererName(){
    return rendererName.c_str();
}

const void* HeadlessContext::getMetalDevice(){
    return (__bridge const void*)device;
}
