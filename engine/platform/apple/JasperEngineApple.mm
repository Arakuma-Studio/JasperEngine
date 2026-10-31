// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineApple.h"

#include "Engine.h"
#include "Log.h"

#import "Renderer.h"
#import <Foundation/Foundation.h>

#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

// The macOS EngineView is a MacViewMetal, whose text input client this reaches
#include "MacTextInputClient.h"
#endif

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>

#import "ios/EngineView.h"

// Google Mobile Ads is linked only when the project enables AdMob
#ifdef JASPERENGINE_ADMOB
#import "ios/AdMobAdapter.h"

static AdMobAdapter* admob = nil;
#define JASPERENGINE_IOS_ADMOB 1
#endif

// The text input lives on the view, which owns the hidden text field
static EngineView* appleEngineView(){
    MTKView* view = Renderer.view;
    return [view isKindOfClass:[EngineView class]] ? (EngineView*)view : nil;
}
#endif

static std::string appleUtf8String(NSString* text){
    const char* utf8 = [text UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

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

#if TARGET_OS_IPHONE && !defined(JASPERENGINE_IOS_ADMOB)
static void warnNoAdMob(){
    static bool warned = false;
    if (!warned){
        warned = true;
        jasperengine::Log::warn("AdMob is not in this build: enable Google AdMob in Project Settings > Player > iOS");
    }
}
#endif

JasperEngineApple::JasperEngineApple(){
#if defined(JASPERENGINE_IOS_ADMOB)
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
#if defined(JASPERENGINE_IOS_ADMOB)
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
    // NSCursor has no public busy or help cursor, so WAIT, PROGRESS and HELP stay arrows
    NSCursor* cursor = [NSCursor arrowCursor];
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
    }else if (type == jasperengine::CursorType::GRAB){
        cursor = [NSCursor openHandCursor];
    }else if (type == jasperengine::CursorType::GRABBING){
        cursor = [NSCursor closedHandCursor];
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

// The engine runs inside the MTKView's draw callback, on the main thread, so the
// UIKit and AppKit calls below need no dispatch.

// The hidden text field starts empty (its sentinel): the engine keeps the text being
// edited, so the current text passed here is not needed.
void JasperEngineApple::showVirtualKeyboard(std::wstring text){
#if TARGET_OS_IPHONE
    [appleEngineView() showTextInput];
#endif
}

void JasperEngineApple::hideVirtualKeyboard(){
#if TARGET_OS_IPHONE
    [appleEngineView() hideTextInput];
#else
    // No virtual keyboard on macOS: end the IME session the engine dropped
    jasperengine::macDiscardMarkedText(Renderer.view);
#endif
}

void JasperEngineApple::setClipboardText(const std::string& utf8){
    NSString* text = [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
    if (!text)
        return;
#if TARGET_OS_IPHONE
    [UIPasteboard generalPasteboard].string = text;
#else
    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    [pasteboard setString:text forType:NSPasteboardTypeString];
#endif
}

std::string JasperEngineApple::getClipboardText(){
#if TARGET_OS_IPHONE
    return appleUtf8String([UIPasteboard generalPasteboard].string);
#else
    return appleUtf8String([[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString]);
#endif
}

// Engine pixels, top left: the drawable's pixels, as touches and the mouse report them
void JasperEngineApple::setTextInputArea(float x, float y, float width, float height){
#if TARGET_OS_IPHONE
    [appleEngineView() setTextInputArea:CGRectMake(x, y, width, height)];
#else
    jasperengine::macSetTextInputArea(Renderer.view, x, y, width, height);
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

// ---- UserSettings: NSUserDefaults. Values keep their type there; one read with another type
// than it was written with gives the default (a number read as a string, data read as a
// number), never an exception. A key or value that is no valid UTF-8 has no NSString: the
// read gives the default, the write is refused (logged).

namespace {

    NSString* userSettingsKey(const char* key){
        return key ? [NSString stringWithUTF8String:key] : nil;
    }

    // The stored number: an NSNumber, or a numeric NSString (an older build's string)
    NSNumber* userSettingsNumber(const char* key){
        NSString* name = userSettingsKey(key);
        if (!name) return nil;
        id value = [[NSUserDefaults standardUserDefaults] objectForKey:name];
        if ([value isKindOfClass:[NSNumber class]]) return (NSNumber*)value;
        if ([value isKindOfClass:[NSString class]]){
            NSScanner* scanner = [NSScanner scannerWithString:(NSString*)value];
            [scanner setLocale:[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]];
            double number = 0;
            if ([scanner scanDouble:&number] && [scanner isAtEnd]) return [NSNumber numberWithDouble:number];
        }
        return nil;
    }

    void setUserSettingsObject(const char* key, id object){
        NSString* name = userSettingsKey(key);
        if (!name || [name length] == 0 || !object){
            jasperengine::Log::error("UserSettings: the key or the value is not valid UTF-8 (or the key is empty); not stored");
            return;
        }
        [[NSUserDefaults standardUserDefaults] setObject:object forKey:name];
    }

}

bool JasperEngineApple::getBoolForKey(const char *key, bool defaultValue){
    NSString* name = userSettingsKey(key);
    id value = name ? [[NSUserDefaults standardUserDefaults] objectForKey:name] : nil;
    // a number, or a string as NSString reads it ("YES", "true", "1")
    if ([value isKindOfClass:[NSNumber class]] || [value isKindOfClass:[NSString class]]) {
        return [value boolValue];
    }

    return defaultValue;
}

int JasperEngineApple::getIntegerForKey(const char *key, int defaultValue){
    NSNumber *value = userSettingsNumber(key);
    if (value) {
        return [value intValue];
    }

    return defaultValue;
}

long JasperEngineApple::getLongForKey(const char *key, long defaultValue){
    NSNumber *value = userSettingsNumber(key);
    if (value) {
        return [value longValue];
    }

    return defaultValue;
}

float JasperEngineApple::getFloatForKey(const char *key, float defaultValue){
    NSNumber *value = userSettingsNumber(key);
    if (value) {
        return [value floatValue];
    }

    return defaultValue;
}

double JasperEngineApple::getDoubleForKey(const char *key, double defaultValue){
    NSNumber *value = userSettingsNumber(key);
    if (value) {
        return [value doubleValue];
    }

    return defaultValue;
}

jasperengine::Data JasperEngineApple::getDataForKey(const char* key, const jasperengine::Data& defaultValue){
    NSString* name = userSettingsKey(key);
    NSData *data = name ? [[NSUserDefaults standardUserDefaults] dataForKey:name] : nil;
    if (data && data.length > 0){
        return jasperengine::Data((unsigned char*)data.bytes, (unsigned int)data.length, true, true);
    }

    return defaultValue;
}

std::string JasperEngineApple::getStringForKey(const char *key, const std::string& defaultValue){
    NSString* name = userSettingsKey(key);
    // stringForKey: gives a stored number as text too (as before); data and others are nil
    NSString* str = name ? [[NSUserDefaults standardUserDefaults] stringForKey:name] : nil;
    NSData* utf8 = [str dataUsingEncoding:NSUTF8StringEncoding];
    if (utf8){
        // the bytes, embedded zeros included (a stored "" is returned as "")
        if (utf8.length == 0) return std::string();
        return std::string(static_cast<const char*>(utf8.bytes), utf8.length);
    }

    return defaultValue;
}

void JasperEngineApple::setBoolForKey(const char *key, bool value){
    setUserSettingsObject(key, [NSNumber numberWithBool:value]);
}

void JasperEngineApple::setIntegerForKey(const char *key, int value){
    setUserSettingsObject(key, [NSNumber numberWithInt:value]);
}

void JasperEngineApple::setLongForKey(const char *key, long value){
    setUserSettingsObject(key, [NSNumber numberWithLong:value]);
}

void JasperEngineApple::setFloatForKey(const char *key, float value){
    setUserSettingsObject(key, [NSNumber numberWithFloat:value]);
}

void JasperEngineApple::setDoubleForKey(const char *key, double value){
    setUserSettingsObject(key, [NSNumber numberWithDouble:value]);
}

void JasperEngineApple::setDataForKey(const char* key, jasperengine::Data& value){
    if (!value.getMemPtr()){
        jasperengine::Log::error("No data to add for key: %s", key ? key : "(null)");
        return;
    }
    setUserSettingsObject(key, [NSData dataWithBytes: value.getMemPtr() length: value.length()]);
}

void JasperEngineApple::setStringForKey(const char* key, const std::string& value){
    // initWithBytes keeps embedded zeros; nil when the bytes are not UTF-8
    NSString* text = [[NSString alloc] initWithBytes:value.data() length:value.size() encoding:NSUTF8StringEncoding];
    setUserSettingsObject(key, text);
}

void JasperEngineApple::removeKey(const char *key){
    NSString* name = userSettingsKey(key);
    if (name)
        [[NSUserDefaults standardUserDefaults] removeObjectForKey:name];
}

bool JasperEngineApple::hasKey(const char *key){
    NSString* name = userSettingsKey(key);
    return name && [[NSUserDefaults standardUserDefaults] objectForKey:name] != nil;
}

void JasperEngineApple::removeAllKeys(){
    // the app's own domain: what the game stored, not the system's defaults
    NSString* domain = [[NSBundle mainBundle] bundleIdentifier];
    if (domain){
        [[NSUserDefaults standardUserDefaults] removePersistentDomainForName:domain];
    } else {
        // a bare executable has no domain of its own: its keys, one by one
        NSDictionary* all = [[NSUserDefaults standardUserDefaults] persistentDomainForName:[[NSProcessInfo processInfo] processName]];
        for (NSString* name in all)
            [[NSUserDefaults standardUserDefaults] removeObjectForKey:name];
    }
}

void JasperEngineApple::saveUserSettings(){
    [[NSUserDefaults standardUserDefaults] synchronize];
}

void JasperEngineApple::initializeAdMob(bool tagForChildDirectedTreatment, bool tagForUnderAgeOfConsent){
#if defined(JASPERENGINE_IOS_ADMOB)
    [admob initializeAdMob: tagForChildDirectedTreatment and:tagForUnderAgeOfConsent];
#elif TARGET_OS_IPHONE
    warnNoAdMob();
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
#if defined(JASPERENGINE_IOS_ADMOB)
    [admob setMaxAdContentRating: irating];
#elif TARGET_OS_IPHONE
    warnNoAdMob();
#endif
}

void JasperEngineApple::loadInterstitialAd(const std::string& adUnitID){
#if defined(JASPERENGINE_IOS_ADMOB)
    [admob loadInterstitial:[NSString stringWithUTF8String:adUnitID.c_str()]];
#elif TARGET_OS_IPHONE
    warnNoAdMob();
#endif
}

bool JasperEngineApple::isInterstitialAdLoaded(){
#if defined(JASPERENGINE_IOS_ADMOB)
    return [admob isInterstitialAdLoaded];
#else
    return false;
#endif
}

void JasperEngineApple::showInterstitialAd(){
#if defined(JASPERENGINE_IOS_ADMOB)
    [admob showInterstitial];
#elif TARGET_OS_IPHONE
    warnNoAdMob();
#endif
}
