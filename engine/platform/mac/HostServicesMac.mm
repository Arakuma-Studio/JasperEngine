// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "HostServices.h"

#include "util/PlatformRules.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    std::string utf8(NSString* text){
        const char* value = [text UTF8String];
        return value ? std::string(value) : std::string();
    }

    NSScreen* currentScreen(){
        NSWindow* window = [NSApp keyWindow] ?: [NSApp mainWindow];
        return (window && window.screen) ? window.screen : [NSScreen mainScreen];
    }

    // keeps the display awake while held (NSProcessInfo's activity, no IOKit needed)
    id<NSObject> keepScreenOnActivity = nil;

}

bool HostServices::openURL(const std::string& url){
    NSString* text = [NSString stringWithUTF8String:url.c_str()];
    NSURL* nsurl = text ? [NSURL URLWithString:text] : nil;
    if (!nsurl) return false;
    return [[NSWorkspace sharedWorkspace] openURL:nsurl];
}

std::vector<std::string> HostServices::preferredLocales(){
    std::vector<std::string> out;
    for (NSString* language in [NSLocale preferredLanguages]){
        const std::string tag = LocaleRules::normalize(utf8(language));
        if (!tag.empty()) out.push_back(tag);
    }
    return out;
}

std::string HostServices::regionOverride(){
    NSLocale* locale = [NSLocale currentLocale];
    NSString* region = nil;
    if (@available(macOS 14, *)){
        region = locale.regionCode;
    }else{
        region = locale.countryCode;
    }
    return region ? utf8(region) : std::string();
}

std::string HostServices::operatingSystem(){
    // Unity's format: "Mac OS X 14.1.0"
    return "Mac OS X " + operatingSystemVersion();
}

std::string HostServices::operatingSystemVersion(){
    const NSOperatingSystemVersion version = [[NSProcessInfo processInfo] operatingSystemVersion];
    return std::to_string((long)version.majorVersion) + "." + std::to_string((long)version.minorVersion) + "." +
           std::to_string((long)version.patchVersion);
}

std::string HostServices::deviceModel(){
    char model[256] = {0};
    size_t size = sizeof(model);
    if (sysctlbyname("hw.model", model, &size, nullptr, 0) == 0 && model[0]){
        return model;
    }
    return "Mac";
}

int HostServices::systemMemoryMiB(){
    return (int)([[NSProcessInfo processInfo] physicalMemory] / (1024ULL * 1024ULL));
}

NetworkReachability HostServices::internetReachability(){
    // an interface that is up, running and has an address (not the loopback): Unity's
    // reachability says a network is there, not that a server answers
    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0) return NetworkReachability::NotReachable;
    bool reachable = false;
    for (ifaddrs* it = addresses; it && !reachable; it = it->ifa_next){
        if (!it->ifa_addr) continue;
        const unsigned flags = it->ifa_flags;
        if (!(flags & IFF_UP) || !(flags & IFF_RUNNING) || (flags & IFF_LOOPBACK)) continue;
        const int family = it->ifa_addr->sa_family;
        if (family == AF_INET || family == AF_INET6) reachable = true;
    }
    freeifaddrs(addresses);
    return reachable ? NetworkReachability::ReachableViaLocalAreaNetwork : NetworkReachability::NotReachable;
}

float HostServices::screenDpi(){
    NSScreen* screen = currentScreen();
    if (!screen) return 0.0f;
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
}

float HostServices::refreshRate(){
    NSScreen* screen = currentScreen();
    const NSInteger fps = screen ? screen.maximumFramesPerSecond : 0;
    return fps > 0 ? (float)fps : 60.0f;
}

void HostServices::setKeepScreenOn(bool keepOn){
    if (keepOn && !keepScreenOnActivity){
        keepScreenOnActivity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:(NSActivityIdleDisplaySleepDisabled | NSActivityUserInitiatedAllowingIdleSystemSleep)
                              reason:@"Screen.sleepTimeout = SleepTimeout.NeverSleep"];
    }else if (!keepOn && keepScreenOnActivity){
        [[NSProcessInfo processInfo] endActivity:keepScreenOnActivity];
        keepScreenOnActivity = nil;
    }
}
