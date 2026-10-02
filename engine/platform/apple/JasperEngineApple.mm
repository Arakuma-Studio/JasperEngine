// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineApple.h"

#include "Engine.h"

#import "Renderer.h"
#import <Foundation/Foundation.h>

#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#endif

#if TARGET_OS_IPHONE
#import <MetalKit/MetalKit.h>

#import "ios/AdMobAdapter.h"

static AdMobAdapter* admob = nil;
#endif

#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
static bool appleMouseLocked = false;
static bool appleCursorHidden = false;

// [NSCursor hide]/[NSCursor unhide] are reference-counted and not idempotent,
// so track the net state and only toggle on an actual transition.
static void setAppleCursorHidden(bool hidden){
    if (hidden == appleCursorHidden)
        return;
    if (hidden){
        [NSCursor hide];
    }else{
        [NSCursor unhide];
    }
    appleCursorHidden = hidden;
}
#endif

JasperEngineApple::JasperEngineApple(){
#if TARGET_OS_IPHONE
    if (!admob)
        admob = [[AdMobAdapter alloc]init];
#endif
}

JasperEngineApple::~JasperEngineApple(){
#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
    CGAssociateMouseAndMouseCursorPosition(true);
    appleMouseLocked = false;
    setAppleCursorHidden(false);
#endif
#if TARGET_OS_IPHONE
    admob = nil;
#endif
}

sg_environment JasperEngineApple::getSokolEnvironment(){
    return (sg_environment) {
        .defaults = {
            .color_format = SG_PIXELFORMAT_BGRA8,
            .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
            .sample_count = (int)Renderer.view.sampleCount,
        },
        .metal = {
            .device = (__bridge const void*) Renderer.view.device,
        }
    };
}

sg_swapchain JasperEngineApple::getSokolSwapchain(){
    return (sg_swapchain) {
        .width = (int)Renderer.screenSize.width,
        .height = (int)Renderer.screenSize.height,
        .sample_count = (int)Renderer.view.sampleCount,
        .color_format = SG_PIXELFORMAT_BGRA8,
        .depth_format = SG_PIXELFORMAT_DEPTH_STENCIL,
        .metal = {
            .current_drawable = (__bridge const void*) [Renderer.view currentDrawable],
            .depth_stencil_texture = (__bridge const void*) [Renderer.view depthStencilTexture],
            .msaa_color_texture = (__bridge const void*) [Renderer.view multisampleColorTexture],
        }
    };
}

#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
// undocumented methods for creating cursors (see GLFW 3.4 and imgui_impl_osx.mm)
@interface NSCursor()
+ (id)_windowResizeNorthWestSouthEastCursor;
+ (id)_windowResizeNorthEastSouthWestCursor;
+ (id)_windowResizeNorthSouthCursor;
+ (id)_windowResizeEastWestCursor;
@end
#endif

void JasperEngineApple::setMouseCursor(jasperengine::CursorType type){
#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
    NSCursor* cursor;
    if (type == jasperengine::CursorType::ARROW){
        cursor = [NSCursor arrowCursor];
    }else if (type == jasperengine::CursorType::IBEAM){
        cursor = [NSCursor IBeamCursor];
    }else if (type == jasperengine::CursorType::CROSSHAIR){
        cursor = [NSCursor crosshairCursor];
    }else if (type == jasperengine::CursorType::POINTING_HAND){
        cursor = [NSCursor pointingHandCursor];
    }else if (type == jasperengine::CursorType::RESIZE_EW){
        cursor = [NSCursor respondsToSelector:@selector(_windowResizeEastWestCursor)] ? [NSCursor _windowResizeEastWestCursor] : [NSCursor resizeLeftRightCursor];
    }else if (type == jasperengine::CursorType::RESIZE_NS){
        cursor = [NSCursor respondsToSelector:@selector(_windowResizeNorthSouthCursor)] ? [NSCursor _windowResizeNorthSouthCursor] : [NSCursor resizeUpDownCursor];
    }else if (type == jasperengine::CursorType::RESIZE_NWSE){
        cursor = [NSCursor respondsToSelector:@selector(_windowResizeNorthWestSouthEastCursor)] ? [NSCursor _windowResizeNorthWestSouthEastCursor] : [NSCursor closedHandCursor];
    }else if (type == jasperengine::CursorType::RESIZE_NESW){
        cursor = [NSCursor respondsToSelector:@selector(_windowResizeNorthEastSouthWestCursor)] ? [NSCursor _windowResizeNorthEastSouthWestCursor] : [NSCursor closedHandCursor];
    }else if (type == jasperengine::CursorType::RESIZE_ALL){
        cursor = [NSCursor closedHandCursor];
    }else if (type == jasperengine::CursorType::NOT_ALLOWED){
        cursor = [NSCursor operationNotAllowedCursor];
    }

    [cursor set];
#endif
}

void JasperEngineApple::setMouseMode(jasperengine::MouseMode mode){
#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
    // macOS has no built-in cursor confinement, so CONFINED behaves as NORMAL.
    const bool locked = (mode == jasperengine::MouseMode::CAPTURED);
    if (appleMouseLocked != locked){
        CGAssociateMouseAndMouseCursorPosition(!locked);
        appleMouseLocked = locked;
    }

    const bool hidden = (mode == jasperengine::MouseMode::HIDDEN || mode == jasperengine::MouseMode::CAPTURED);
    setAppleCursorHidden(hidden);
#endif
}

int JasperEngineApple::getScreenWidth(){
    return Renderer.screenSize.width;
}

int JasperEngineApple::getScreenHeight(){
    return Renderer.screenSize.height;
}

int JasperEngineApple::getSampleCount(){
    return (int)Renderer.view.sampleCount;
}

bool JasperEngineApple::isTouchDevice(){
#if TARGET_OS_IPHONE
    return true;
#else
    return false;
#endif
}

void JasperEngineApple::showVirtualKeyboard(std::wstring text){
#if TARGET_OS_IPHONE
    [Renderer.view becomeFirstResponder];
#endif
}

void JasperEngineApple::hideVirtualKeyboard(){
#if TARGET_OS_IPHONE
    [Renderer.view resignFirstResponder];
#endif
}

void JasperEngineApple::quit(){
#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
    // deferred to not close inside a script call
    dispatch_async(dispatch_get_main_queue(), ^{
        [[Renderer.view window] close];
    });
#else
    // iOS apps should not terminate themselves
    jasperengine::System::quit();
#endif
}

std::string JasperEngineApple::getAssetPath(){
    NSString* path = [[NSBundle mainBundle] pathForResource:@"assets" ofType:nil];

    if (!path){
        // No bundle resource: either a plain (non-.app) build, or a project run
        // from outside the editor, which leaves the assets in the project
        // directory and bakes its path in (see Generator::getPlatformCMakeConfig).
#ifdef JASPERENGINE_ASSET_PATH
        return JASPERENGINE_ASSET_PATH;
#else
        return "assets";
#endif
    }

    return [path cStringUsingEncoding:NSASCIIStringEncoding];
}

std::string JasperEngineApple::getUserDataPath(){
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *documentsDirectory = [paths objectAtIndex:0];
    
    return [documentsDirectory UTF8String];
}

std::string JasperEngineApple::getCachePath(){
    // Caches stay out of Documents, which iOS backs up; macOS shares ~/Library/Caches
    // between apps, so each one keeps to its own folder there
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    if ([paths count] == 0)
        return getUserDataPath();

    NSString *cachesDirectory = [paths objectAtIndex:0];
    NSString *bundleId = [[NSBundle mainBundle] bundleIdentifier];
    if (bundleId)
        cachesDirectory = [cachesDirectory stringByAppendingPathComponent:bundleId];

    return [cachesDirectory UTF8String];
}

std::string JasperEngineApple::getLuaPath(){
    NSString* path = [[NSBundle mainBundle] pathForResource:@"lua" ofType:nil];

    if (!path){
#ifdef JASPERENGINE_LUA_PATH
        return JASPERENGINE_LUA_PATH;
#else
        return "lua";
#endif
    }

    return [path cStringUsingEncoding:NSASCIIStringEncoding];
}

std::string JasperEngineApple::getShaderPath(){
    NSString* path = [[NSBundle mainBundle] pathForResource:@"shaders" ofType:nil];

    if (!path){
#ifdef JASPERENGINE_SHADER_PATH
        return JASPERENGINE_SHADER_PATH;
#else
        return System::getShaderPath();
#endif
    }

    return [path cStringUsingEncoding:NSASCIIStringEncoding];
}

bool JasperEngineApple::getBoolForKey(const char *key, bool defaultValue){
    NSNumber *value = [[NSUserDefaults standardUserDefaults] objectForKey:[NSString stringWithUTF8String:key]];
    if (value) {
        return [value boolValue];
    }

    return defaultValue;
}

int JasperEngineApple::getIntegerForKey(const char *key, int defaultValue){
    NSNumber *value = [[NSUserDefaults standardUserDefaults] objectForKey:[NSString stringWithUTF8String:key]];
    if (value) {
        return [value intValue];
    }

    return defaultValue;
}

long JasperEngineApple::getLongForKey(const char *key, long defaultValue){
    NSNumber *value = [[NSUserDefaults standardUserDefaults] objectForKey:[NSString stringWithUTF8String:key]];
    if (value) {
        return [value longValue];
    }

    return defaultValue;
}

float JasperEngineApple::getFloatForKey(const char *key, float defaultValue){
    NSNumber *value = [[NSUserDefaults standardUserDefaults] objectForKey:[NSString stringWithUTF8String:key]];
    if (value) {
        return [value floatValue];
    }

    return defaultValue;
}

double JasperEngineApple::getDoubleForKey(const char *key, double defaultValue){
    NSNumber *value = [[NSUserDefaults standardUserDefaults] objectForKey:[NSString stringWithUTF8String:key]];
    if (value) {
        return [value doubleValue];
    }

    return defaultValue;
}

jasperengine::Data JasperEngineApple::getDataForKey(const char* key, const jasperengine::Data& defaultValue){
    NSData *data = [[NSUserDefaults standardUserDefaults] dataForKey:[NSString stringWithUTF8String:key]];
    if (data){
        return jasperengine::Data((unsigned char*)data.bytes, (unsigned int)data.length, true, true);
    }
    
     return defaultValue;
}

std::string JasperEngineApple::getStringForKey(const char *key, const std::string& defaultValue){
    NSString *str = [[NSUserDefaults standardUserDefaults] stringForKey:[NSString stringWithUTF8String:key]];
    if (str){
        return [str UTF8String];
    }
    
    return defaultValue;
}

void JasperEngineApple::setBoolForKey(const char *key, bool value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithBool:value] forKey:[NSString stringWithUTF8String:key]];
}

void JasperEngineApple::setIntegerForKey(const char *key, int value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithInt:value] forKey:[NSString stringWithUTF8String:key]];
}

void JasperEngineApple::setLongForKey(const char *key, long value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithLong:value] forKey:[NSString stringWithUTF8String:key]];
}

void JasperEngineApple::setFloatForKey(const char *key, float value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithFloat:value] forKey:[NSString stringWithUTF8String:key]];
}

void JasperEngineApple::setDoubleForKey(const char *key, double value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSNumber numberWithDouble:value] forKey:[NSString stringWithUTF8String:key]];
}

void JasperEngineApple::setDataForKey(const char* key, jasperengine::Data& value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSData dataWithBytes: value.getMemPtr() length: value.length()] forKey:[NSString stringWithUTF8String:key]];
}

void JasperEngineApple::setStringForKey(const char* key, const std::string& value){
    [[NSUserDefaults standardUserDefaults] setObject:[NSString stringWithUTF8String:value.c_str()] forKey:[NSString stringWithUTF8String:key]];
}

void JasperEngineApple::removeKey(const char *key){
    [[NSUserDefaults standardUserDefaults] removeObjectForKey:[NSString stringWithUTF8String:key]];
}

void JasperEngineApple::initializeAdMob(bool tagForChildDirectedTreatment, bool tagForUnderAgeOfConsent){
#if TARGET_OS_IPHONE
    [admob initializeAdMob: tagForChildDirectedTreatment and:tagForUnderAgeOfConsent];
#endif
}

void JasperEngineApple::setMaxAdContentRating(jasperengine::AdMobRating rating){
    int irating = 0;
    if (rating == jasperengine::AdMobRating::General){
        irating = 1;
    }else if (rating == jasperengine::AdMobRating::ParentalGuidance){
        irating = 2;
    }else if (rating == jasperengine::AdMobRating::Teen){
        irating = 3;
    }else if (rating == jasperengine::AdMobRating::MatureAudience){
        irating = 4;
    }
#if TARGET_OS_IPHONE
    [admob setMaxAdContentRating: irating];
#endif
}

void JasperEngineApple::loadInterstitialAd(const std::string& adUnitID){
#if TARGET_OS_IPHONE
    [admob loadInterstitial:[NSString stringWithUTF8String:adUnitID.c_str()]];
#endif
}

bool JasperEngineApple::isInterstitialAdLoaded(){
#if TARGET_OS_IPHONE
    return [admob isInterstitialAdLoaded];
#else
    return false;
#endif
}

void JasperEngineApple::showInterstitialAd(){
#if TARGET_OS_IPHONE
    [admob showInterstitial];
#endif
}
