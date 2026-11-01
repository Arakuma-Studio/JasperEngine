// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// macOS: NSColorSampler (10.15+), the system loupe. One asynchronous pick, converted to sRGB;
// no live preview and no Screen Recording permission (OpenEngine's CGWindowListCreateImage
// read is not used: it needs that permission and the API is obsolete).

#include "platform/ScreenColor.h"

#import <AppKit/AppKit.h>

#include <cstring>

using namespace jasperengine::editor;

// the sampler of the pick in progress, kept alive until it answers (ARC)
static NSColorSampler* activeSampler API_AVAILABLE(macos(10.15)) = nil;

bool screencolor::liveSupported() {
    return false;
}

bool screencolor::waylandSession() {
    return false;
}

bool screencolor::begin(std::string& why) {
    why = "macOS picks with the system colour sampler";
    return false;
}

void screencolor::end() {
}

bool screencolor::active() {
    return false;
}

bool screencolor::cursor(int& x, int& y) {
    const NSPoint point = [NSEvent mouseLocation];
    x = (int)point.x;
    y = (int)point.y;
    return true;
}

bool screencolor::read9x9(int, int, uint8_t rgb[kLoupePixels * 3]) {
    std::memset(rgb, 0, kLoupePixels * 3);
    return false;
}

screencolor::Button screencolor::pollButton() {
    return Button::Idle;
}

bool screencolor::toWindow(void*, int, int, float&, float&) {
    return false;
}

bool screencolor::pickOnceSupported() {
    if (@available(macOS 10.15, *)) return true;
    return false;
}

void screencolor::pickOnce(std::function<void(bool, float, float, float)> done) {
    if (@available(macOS 10.15, *)) {
        activeSampler = [[NSColorSampler alloc] init];
        __block std::function<void(bool, float, float, float)> callback = std::move(done);
        [activeSampler showSamplerWithSelectionHandler:^(NSColor* picked) {
            NSColor* srgb = picked ? [picked colorUsingColorSpace:[NSColorSpace sRGBColorSpace]] : nil;
            dispatch_async(dispatch_get_main_queue(), ^{
                activeSampler = nil;
                if (!callback) return;
                if (srgb) {
                    callback(true, (float)[srgb redComponent], (float)[srgb greenComponent], (float)[srgb blueComponent]);
                } else {
                    callback(false, 0.0f, 0.0f, 0.0f);
                }
            });
        }];
        return;
    }
    if (done) done(false, 0.0f, 0.0f, 0.0f);
}
