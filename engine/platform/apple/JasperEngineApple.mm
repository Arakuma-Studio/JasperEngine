// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "JasperEngineApple.h"
#include "PlatformConfig.h"

#include "Engine.h"
#include "Log.h"

#import "Renderer.h"
#import <Foundation/Foundation.h>

#include "util/PlatformRules.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <cmath>
#include <cstdlib>

#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

// The macOS EngineView is a MacViewMetal, whose text input client this reaches
#include "MacTextInputClient.h"
#endif

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <MetalKit/MetalKit.h>
#import <AudioToolbox/AudioToolbox.h>

#import "ios/EngineView.h"

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

JasperEngineApple::JasperEngineApple(){
}

JasperEngineApple::~JasperEngineApple(){
#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
    CGAssociateMouseAndMouseCursorPosition(true);
    appleMouseLocked = false;
    setAppleCursorHidden(false);
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

namespace {
    // A bundle resource's path as UTF-8: an app under a folder named "Jogos" or "ゲーム" starts
    // (ASCII would give NULL there)
    std::string bundlePath(NSString* path){
        const char* utf8 = [path UTF8String];
        return utf8 ? std::string(utf8) : std::string();
    }
}

std::string JasperEngineApple::getAssetPath(){
    // a prebuilt player's content root (its bundle's Content/, or --content)
    if (!jasperengine::platformConfig().contentRoot.empty())
        return jasperengine::platformConfig().contentRoot + "/assets";
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

    return bundlePath(path);
}

std::string JasperEngineApple::getUserDataPath(){
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *documentsDirectory = [paths objectAtIndex:0];
    
    return [documentsDirectory UTF8String];
}

std::string JasperEngineApple::getCrashReportPath(){
#if TARGET_OS_IPHONE
    // Library/Caches/crash: out of the backed-up Documents, inside the app's sandbox
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    if ([paths count] == 0)
        return getUserDataPath() + "/crash";
    return std::string([[paths objectAtIndex:0] UTF8String]) + "/crash";
#else
    return getCachePath() + "/crash";
#endif
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
    if (!jasperengine::platformConfig().contentRoot.empty())
        return jasperengine::platformConfig().contentRoot + "/lua";
    NSString* path = [[NSBundle mainBundle] pathForResource:@"lua" ofType:nil];

    if (!path){
#ifdef JASPERENGINE_LUA_PATH
        return JASPERENGINE_LUA_PATH;
#else
        return "lua";
#endif
    }

    return bundlePath(path);
}

std::string JasperEngineApple::getShaderPath(){
    if (!jasperengine::platformConfig().contentRoot.empty())
        return System::getShaderPath();
    NSString* path = [[NSBundle mainBundle] pathForResource:@"shaders" ofType:nil];

    if (!path){
#ifdef JASPERENGINE_SHADER_PATH
        return JASPERENGINE_SHADER_PATH;
#else
        return System::getShaderPath();
#endif
    }

    return bundlePath(path);
}

// ---- UserSettings: NSUserDefaults. Values keep their type there; one read with another type
// than it was written with gives the default (a number read as a string, data read as a
// number), never an exception. A key or value that is no valid UTF-8 has no NSString: the
// read gives the default, the write is refused (logged).

namespace {

    // standardUserDefaults is shared with the app's SDKs (ads consent: UMP, IABTCF_* strings):
    // deleteAll must only remove the game's keys. The engine keeps their names in this list:
    // every key it writes, and every key of an older build the game reads back. Other SDKs'
    // keys are never adopted from a read, only when the game itself writes one.
    NSString* const KEY_LIST = @"jasperengine.userSettings.keys";

    bool isForeignKey(NSString* name){
        static NSArray* prefixes = @[@"IABTCF_", @"IABGPP_", @"IABUSPrivacy", @"UMP", @"gad_", @"GAD", @"GoogleAds",
                                     @"com.google.", @"com.apple.", @"Apple", @"NS", @"WebKit",
                                     @"jasperengine.userSettings."];
        for (NSString* prefix in prefixes){
            if ([name hasPrefix:prefix]) return true;
        }
        return false;
    }

    NSMutableSet* gameKeys(){
        static NSMutableSet* keys = nil;
        if (!keys){
            keys = [NSMutableSet set];
            NSArray* stored = [[NSUserDefaults standardUserDefaults] arrayForKey:KEY_LIST];
            for (id name in stored){
                if ([name isKindOfClass:[NSString class]]) [keys addObject:name];
            }
        }
        return keys;
    }

    void rememberKey(NSString* name){
        @synchronized (KEY_LIST) {
            NSMutableSet* keys = gameKeys();
            if ([keys containsObject:name]) return;
            [keys addObject:name];
            [[NSUserDefaults standardUserDefaults] setObject:[keys allObjects] forKey:KEY_LIST];
        }
    }

    void forgetKey(NSString* name){
        @synchronized (KEY_LIST) {
            NSMutableSet* keys = gameKeys();
            if (![keys containsObject:name]) return;
            [keys removeObject:name];
            [[NSUserDefaults standardUserDefaults] setObject:[keys allObjects] forKey:KEY_LIST];
        }
    }

    NSString* userSettingsKey(const char* key){
        return key ? [NSString stringWithUTF8String:key] : nil;
    }

    // The stored object of a key the game reads: a key of an older build joins the list
    id userSettingsObject(NSString* name){
        if (!name) return nil;
        id value = [[NSUserDefaults standardUserDefaults] objectForKey:name];
        if (value && !isForeignKey(name)) rememberKey(name);
        return value;
    }

    // The stored number: an NSNumber, or a numeric NSString (an older build's string)
    NSNumber* userSettingsNumber(const char* key){
        id value = userSettingsObject(userSettingsKey(key));
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
        rememberKey(name);
    }

}

bool JasperEngineApple::getBoolForKey(const char *key, bool defaultValue){
    id value = userSettingsObject(userSettingsKey(key));
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
    userSettingsObject(name);
    NSData *data = name ? [[NSUserDefaults standardUserDefaults] dataForKey:name] : nil;
    if (data && data.length > 0){
        return jasperengine::Data((unsigned char*)data.bytes, (unsigned int)data.length, true, true);
    }

    return defaultValue;
}

std::string JasperEngineApple::getStringForKey(const char *key, const std::string& defaultValue){
    NSString* name = userSettingsKey(key);
    userSettingsObject(name);
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
    if (name){
        [[NSUserDefaults standardUserDefaults] removeObjectForKey:name];
        forgetKey(name);
    }
}

bool JasperEngineApple::hasKey(const char *key){
    return userSettingsObject(userSettingsKey(key)) != nil;
}

void JasperEngineApple::removeAllKeys(){
    // the game's keys only (KEY_LIST), never the SDKs' (consent state lives here too)
    @synchronized (KEY_LIST) {
        NSMutableSet* keys = gameKeys();
        for (NSString* name in keys)
            [[NSUserDefaults standardUserDefaults] removeObjectForKey:name];
        [keys removeAllObjects];
        [[NSUserDefaults standardUserDefaults] removeObjectForKey:KEY_LIST];
    }
}

void JasperEngineApple::saveUserSettings(){
    [[NSUserDefaults standardUserDefaults] synchronize];
}

// ------------------------------------------------------------------ platform services
// The engine runs on the main thread (MTKView's draw callback, UIKit and AppKit notifications),
// where every UIKit and AppKit call below belongs.

namespace {

NSString* appleString(const std::string& utf8){
    return [[NSString alloc] initWithBytes:utf8.data() length:utf8.size() encoding:NSUTF8StringEncoding];
}

NSString* bundleValue(NSString* key){
    id value = [[NSBundle mainBundle] objectForInfoDictionaryKey:key];
    return [value isKindOfClass:[NSString class]] ? (NSString*)value : nil;
}

#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
// keeps the display awake while held
id<NSObject> keepScreenOnActivity = nil;
#endif

// an address that is not link-local (fe80::/10, 169.254/16): the interface is on a network
bool routableAddress(const sockaddr* address){
    if (address->sa_family == AF_INET){
        const uint32_t ip = ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr);
        return (ip >> 16) != 0xA9FE && ip != 0;
    }
    if (address->sa_family == AF_INET6){
        const uint8_t* bytes = reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr.s6_addr;
        return !(bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80);
    }
    return false;
}

} // namespace

bool JasperEngineApple::platformOpenURL(const std::string& url){
    NSString* text = appleString(url);
    NSURL* nsurl = text ? [NSURL URLWithString:text] : nil;
    if (!nsurl){
        jasperengine::Log::error("System.openURL: \"%s\" is not a URL", url.c_str());
        return false;
    }
#if TARGET_OS_IPHONE
    // true when the request was made: canOpenURL needs LSApplicationQueriesSchemes for other apps
    [[UIApplication sharedApplication] openURL:nsurl options:@{} completionHandler:^(BOOL success){
        if (!success) NSLog(@"System.openURL: nothing opened %@", nsurl);
    }];
    return true;
#else
    return [[NSWorkspace sharedWorkspace] openURL:nsurl];
#endif
}

std::vector<std::string> JasperEngineApple::getPreferredLocales(){
    std::vector<std::string> tags;
    for (NSString* language in [NSLocale preferredLanguages]){
        const std::string tag = jasperengine::LocaleRules::normalize(appleUtf8String(language));
        if (!tag.empty()) tags.push_back(tag);
    }
    return tags.empty() ? System::getPreferredLocales() : tags;
}

std::string JasperEngineApple::getRegionOverride(){
    NSLocale* locale = [NSLocale currentLocale];
    NSString* region = nil;
#if TARGET_OS_IPHONE
    if (@available(iOS 17, *)){
        region = locale.regionCode;
    }else{
        region = locale.countryCode;
    }
#else
    if (@available(macOS 14, *)){
        region = locale.regionCode;
    }else{
        region = locale.countryCode;
    }
#endif
    return region ? appleUtf8String(region) : std::string();
}

std::string JasperEngineApple::getAppVersion(){
    NSString* version = bundleValue(@"CFBundleShortVersionString");
    return version ? appleUtf8String(version) : System::getAppVersion();
}

int64_t JasperEngineApple::getAppBuild(){
    NSString* build = bundleValue(@"CFBundleVersion");
    // "42" -> 42, "1.2" -> 1
    return build ? jasperengine::OsRules::parseBuildNumber(appleUtf8String(build)) : System::getAppBuild();
}

std::string JasperEngineApple::getAppIdentifier(){
    NSString* identifier = [[NSBundle mainBundle] bundleIdentifier];
    return identifier ? appleUtf8String(identifier) : System::getAppIdentifier();
}

std::string JasperEngineApple::getAppName(){
    NSString* name = bundleValue(@"CFBundleDisplayName") ?: bundleValue(@"CFBundleName");
    return name ? appleUtf8String(name) : System::getAppName();
}

std::string JasperEngineApple::getOperatingSystem(){
#if TARGET_OS_IPHONE
    // Unity's format: "iOS 17.1" (iPadOS reports "iPadOS")
    UIDevice* device = [UIDevice currentDevice];
    return appleUtf8String(device.systemName) + " " + appleUtf8String(device.systemVersion);
#else
    return "Mac OS X " + getOperatingSystemVersion();
#endif
}

std::string JasperEngineApple::getOperatingSystemVersion(){
#if TARGET_OS_IPHONE
    return appleUtf8String([UIDevice currentDevice].systemVersion);
#else
    const NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
    return std::to_string((long)version.majorVersion) + "." + std::to_string((long)version.minorVersion) + "." +
           std::to_string((long)version.patchVersion);
#endif
}

std::string JasperEngineApple::getDeviceModel(){
#if TARGET_OS_IPHONE
    // the simulator runs on the Mac's CPU and names the device it simulates
    if (const char* simulated = std::getenv("SIMULATOR_MODEL_IDENTIFIER")){
        if (simulated[0]) return simulated;
    }
    utsname name{};
    if (uname(&name) == 0 && name.machine[0]) return name.machine;
    return appleUtf8String([UIDevice currentDevice].model);
#else
    char model[256] = {0};
    size_t size = sizeof(model);
    if (sysctlbyname("hw.model", model, &size, nullptr, 0) == 0 && model[0]) return model;
    return "Mac";
#endif
}

jasperengine::DeviceType JasperEngineApple::getDeviceType(){
#if TARGET_OS_IPHONE
    return jasperengine::DeviceType::Handheld;
#else
    return jasperengine::DeviceType::Desktop;
#endif
}

int JasperEngineApple::getSystemMemorySize(){
    return (int)([[NSProcessInfo processInfo] physicalMemory] / (1024ULL * 1024ULL));
}

jasperengine::NetworkReachability JasperEngineApple::getInternetReachability(){
    // a network interface that is up with a routable address: Wi-Fi or Ethernet (en*) is the
    // local area network, pdp_ip* the carrier's data network (Unity's two kinds)
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return jasperengine::NetworkReachability::NotReachable;
    bool lan = false;
    bool carrier = false;
    for (ifaddrs* it = addresses; it; it = it->ifa_next){
        if (!it->ifa_addr || !it->ifa_name) continue;
        const unsigned flags = it->ifa_flags;
        if (!(flags & IFF_UP) || !(flags & IFF_RUNNING) || (flags & IFF_LOOPBACK)) continue;
        if (!routableAddress(it->ifa_addr)) continue;
        const std::string name = it->ifa_name;
        if (name.compare(0, 6, "pdp_ip") == 0) carrier = true;
        else if (name.compare(0, 2, "en") == 0 || name.compare(0, 6, "bridge") == 0) lan = true;
#if defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
        // VPNs and other tunnels of a Mac still need a network under them
        else if (name.compare(0, 4, "utun") != 0 && name.compare(0, 4, "ipsec") != 0) lan = true;
#endif
    }
    freeifaddrs(addresses);
    if (lan) return jasperengine::NetworkReachability::ReachableViaLocalAreaNetwork;
    if (carrier) return jasperengine::NetworkReachability::ReachableViaCarrierDataNetwork;
    return jasperengine::NetworkReachability::NotReachable;
}

bool JasperEngineApple::vibrate(int milliseconds){
#if TARGET_OS_IPHONE
    // iPads have no vibration motor; the length is fixed on iOS
    if ([UIDevice currentDevice].userInterfaceIdiom != UIUserInterfaceIdiomPhone) return false;
    AudioServicesPlaySystemSound(kSystemSoundID_Vibrate);
    return true;
#else
    return false;
#endif
}

bool JasperEngineApple::hapticFeedback(jasperengine::HapticFeedback type){
#if TARGET_OS_IPHONE
    if ([UIDevice currentDevice].userInterfaceIdiom != UIUserInterfaceIdiomPhone) return false;
    switch (type){
        case jasperengine::HapticFeedback::Light:
        case jasperengine::HapticFeedback::Medium:
        case jasperengine::HapticFeedback::Heavy: {
            const UIImpactFeedbackStyle style = type == jasperengine::HapticFeedback::Light ? UIImpactFeedbackStyleLight
                : type == jasperengine::HapticFeedback::Medium ? UIImpactFeedbackStyleMedium : UIImpactFeedbackStyleHeavy;
            UIImpactFeedbackGenerator* generator = [[UIImpactFeedbackGenerator alloc] initWithStyle:style];
            [generator impactOccurred];
            return true;
        }
        case jasperengine::HapticFeedback::Selection: {
            UISelectionFeedbackGenerator* generator = [[UISelectionFeedbackGenerator alloc] init];
            [generator selectionChanged];
            return true;
        }
        default: {
            const UINotificationFeedbackType notification = type == jasperengine::HapticFeedback::NotificationSuccess
                ? UINotificationFeedbackTypeSuccess
                : type == jasperengine::HapticFeedback::NotificationWarning ? UINotificationFeedbackTypeWarning : UINotificationFeedbackTypeError;
            UINotificationFeedbackGenerator* generator = [[UINotificationFeedbackGenerator alloc] init];
            [generator notificationOccurred:notification];
            return true;
        }
    }
#else
    // Force Touch trackpads only
    id<NSHapticFeedbackPerformer> performer = [NSHapticFeedbackManager defaultPerformer];
    if (!performer) return false;
    const NSHapticFeedbackPattern pattern = (type == jasperengine::HapticFeedback::Selection || type == jasperengine::HapticFeedback::Light)
        ? NSHapticFeedbackPatternAlignment
        : (type == jasperengine::HapticFeedback::NotificationSuccess || type == jasperengine::HapticFeedback::NotificationWarning ||
           type == jasperengine::HapticFeedback::NotificationError) ? NSHapticFeedbackPatternLevelChange : NSHapticFeedbackPatternGeneric;
    [performer performFeedbackPattern:pattern performanceTime:NSHapticFeedbackPerformanceTimeNow];
    return true;
#endif
}

float JasperEngineApple::getScreenDpi(){
#if TARGET_OS_IPHONE
    // UIKit tells no physical size: 163 points per inch on iPhones, 132 on iPads (an approximation)
    const bool pad = [UIDevice currentDevice].userInterfaceIdiom == UIUserInterfaceIdiomPad;
    return (float)([UIScreen mainScreen].scale * (pad ? 132.0 : 163.0));
#else
    NSScreen* screen = Renderer.view.window.screen ?: [NSScreen mainScreen];
    NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
    if (!number) return 0.0f;
    const CGDirectDisplayID display = (CGDirectDisplayID)[number unsignedIntValue];
    const CGSize millimeters = CGDisplayScreenSize(display);
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    if (!mode) return 0.0f;
    const size_t pixels = CGDisplayModeGetPixelWidth(mode);
    CGDisplayModeRelease(mode);
    if (millimeters.width <= 0.0 || pixels == 0) return 0.0f;
    return (float)((double)pixels * 25.4 / millimeters.width);
#endif
}

float JasperEngineApple::getRefreshRate(){
#if TARGET_OS_IPHONE
    const NSInteger fps = [UIScreen mainScreen].maximumFramesPerSecond;
#else
    NSScreen* screen = Renderer.view.window.screen ?: [NSScreen mainScreen];
    const NSInteger fps = screen ? screen.maximumFramesPerSecond : 0;
#endif
    return fps > 0 ? (float)fps : 60.0f;
}

void JasperEngineApple::setKeepScreenOn(bool keepOn){
#if TARGET_OS_IPHONE
    [UIApplication sharedApplication].idleTimerDisabled = keepOn ? YES : NO;
#else
    if (keepOn && !keepScreenOnActivity){
        keepScreenOnActivity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:(NSActivityIdleDisplaySleepDisabled | NSActivityUserInitiatedAllowingIdleSystemSleep)
                              reason:@"Screen.sleepTimeout = SleepTimeout.NeverSleep"];
    }else if (!keepOn && keepScreenOnActivity){
        [[NSProcessInfo processInfo] endActivity:keepScreenOnActivity];
        keepScreenOnActivity = nil;
    }
#endif
}

void JasperEngineApple::applyOrientationPolicy(jasperengine::ScreenOrientation requested, uint8_t allowedMask){
#if TARGET_OS_IPHONE
    // the view controller reads Screen's policy (Renderer supportedInterfaceOrientations)
    [[NSNotificationCenter defaultCenter] postNotificationName:JasperEngineOrientationPolicyNotification object:nil];
#endif
}

jasperengine::ScreenOrientation JasperEngineApple::getScreenOrientation(){
#if TARGET_OS_IPHONE
    UIWindowScene* scene = Renderer.view.window.windowScene;
    if (scene && scene.interfaceOrientation != UIInterfaceOrientationUnknown){
        return jasperengine::PlatformRules::orientationFromIos((int)scene.interfaceOrientation);
    }
#endif
    return System::getScreenOrientation();
}

jasperengine::DeviceOrientation JasperEngineApple::getDeviceOrientation(){
#if TARGET_OS_IPHONE
    // UIDeviceOrientation has Unity's values (FaceUp 5, FaceDown 6 included)
    const UIDeviceOrientation orientation = [UIDevice currentDevice].orientation;
    if (orientation >= UIDeviceOrientationPortrait && orientation <= UIDeviceOrientationFaceDown){
        return (jasperengine::DeviceOrientation)orientation;
    }
    return jasperengine::DeviceOrientation::Unknown;
#else
    return jasperengine::DeviceOrientation::Unknown;
#endif
}

void JasperEngineApple::applyFramePacing(const jasperengine::FramePacing& pacing){
    [Renderer applyFramePacing];
}

bool JasperEngineApple::canDisableVSync(){
    return false;
}
