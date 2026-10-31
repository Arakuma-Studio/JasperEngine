// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "SystemMac.h"
#include "WindowMac.h"
#include "MacTextInputClient.h"
#include "io/FileUtils.h"

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#if defined(SOKOL_METAL)
#import <MetalKit/MetalKit.h>
#elif defined(SOKOL_VULKAN)
#include "VulkanContext.h"
#endif

using namespace jasperengine;

namespace {
    // Finder gives a bare executable the home folder as working directory, so
    // relative paths never resolve there. The main bundle points at the folder
    // holding the executable, or at the resources of a real .app.
    bool bundleResourcePath(NSString* name, std::string& out) {
        NSString* path = [[NSBundle mainBundle] pathForResource:name ofType:nil];
        if (!path) return false;

        out = [path UTF8String];
        return true;
    }
}

int SystemMac::getScreenWidth() {
    return WindowMac::getDrawableWidth();
}

int SystemMac::getScreenHeight() {
    return WindowMac::getDrawableHeight();
}

#if defined(SOKOL_METAL)
sg_environment SystemMac::getSokolEnvironment() {
    MTKView* view = (__bridge MTKView*)WindowMac::contentView();
    sg_environment env = {};

    env.defaults.sample_count = (int)view.sampleCount;
    env.defaults.color_format = SG_PIXELFORMAT_BGRA8;
    env.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    env.metal.device = (__bridge const void*)view.device;

    return env;
}

// Only valid inside the view's draw callback, where currentDrawable is set
sg_swapchain SystemMac::getSokolSwapchain() {
    MTKView* view = (__bridge MTKView*)WindowMac::contentView();
    sg_swapchain swapchain = {};

    swapchain.width = (int)view.drawableSize.width;
    swapchain.height = (int)view.drawableSize.height;
    swapchain.sample_count = (int)view.sampleCount;
    swapchain.color_format = SG_PIXELFORMAT_BGRA8;
    swapchain.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
    swapchain.metal.current_drawable = (__bridge const void*)view.currentDrawable;
    swapchain.metal.depth_stencil_texture = (__bridge const void*)view.depthStencilTexture;
    swapchain.metal.msaa_color_texture = (__bridge const void*)view.multisampleColorTexture;

    return swapchain;
}
#elif defined(SOKOL_VULKAN)
sg_environment SystemMac::getSokolEnvironment() {
    return VulkanContext::environment();
}

sg_swapchain SystemMac::getSokolSwapchain() {
    return VulkanContext::swapchain();
}
#endif

bool SystemMac::isFullscreen() {
    return WindowMac::isFullscreen();
}

void SystemMac::requestFullscreen() {
    WindowMac::requestFullscreen();
}

void SystemMac::exitFullscreen() {
    WindowMac::exitFullscreen();
}

bool SystemMac::isWindowMaximized() {
    return WindowMac::isMaximized();
}

void SystemMac::maximizeWindow() {
    WindowMac::maximize();
}

void SystemMac::restoreWindow() {
    WindowMac::restore();
}

void SystemMac::setWindowSize(int width, int height) {
    WindowMac::setSize(width, height);
}

bool SystemMac::isWindowResizable() {
    return WindowMac::isResizable();
}

void SystemMac::setWindowResizable(bool resizable) {
    WindowMac::setResizable(resizable);
}

void SystemMac::setWindowTitle(const std::string& title) {
    WindowMac::setTitle(title);
}

void SystemMac::quit() {
    WindowMac::quit();
}

void SystemMac::setMouseCursor(CursorType type) {
    WindowMac::setMouseCursor(type);
}

void SystemMac::setMouseMode(MouseMode mode) {
    WindowMac::setMouseMode(mode);
}

void SystemMac::setMousePosition(float x, float y) {
    WindowMac::setMousePosition(x, y);
}

// The engine runs on the main thread here (the frame timer is on the main run
// loop), so the AppKit calls below need no dispatch.
void SystemMac::setClipboardText(const std::string& utf8) {
    NSString* text = [[NSString alloc] initWithBytes:utf8.data()
                                              length:utf8.size()
                                            encoding:NSUTF8StringEncoding];
    if (!text) return;

    NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
    [pasteboard clearContents];
    [pasteboard setString:text forType:NSPasteboardTypeString];
}

std::string SystemMac::getClipboardText() {
    NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    const char* utf8 = [text UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

// Engine pixels are the content view's backing pixels, as the mouse reports them
void SystemMac::setTextInputArea(float x, float y, float width, float height) {
    macSetTextInputArea((__bridge NSView*)WindowMac::contentView(), x, y, width, height);
}

void SystemMac::hideVirtualKeyboard() {
    macDiscardMarkedText((__bridge NSView*)WindowMac::contentView());
}

// The bundle lookup covers an exported game, which ships its assets next to the
// executable. A project run from outside the editor keeps them in the project
// directory, so the editor bakes that path in (Generator::getPlatformCMakeConfig).
std::string SystemMac::getAssetPath() {
    std::string bundlePath;
    if (bundleResourcePath(@"assets", bundlePath)) return bundlePath;

#ifdef JASPERENGINE_ASSET_PATH
    return JASPERENGINE_ASSET_PATH;
#else
    return "assets";
#endif
}

// An export (the Exporter defines JASPERENGINE_APP_IDENTIFIER) saves per user, as an app may
// not write inside its bundle: ~/Library/Application Support/<identifier>. Other builds (a
// project run from outside the editor, engine tests) keep the working directory.
std::string SystemMac::getUserDataPath() {
#ifdef JASPERENGINE_APP_IDENTIFIER
    static const std::string path = []() {
        NSArray* paths = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
        if ([paths count] == 0) return std::string(".");
        NSString* dir = [[paths objectAtIndex:0] stringByAppendingPathComponent:[NSString stringWithUTF8String:JASPERENGINE_APP_IDENTIFIER]];
        const std::string utf8 = [dir UTF8String];
        return FileUtils::prepareUserDataFolder(utf8) ? utf8 : std::string(".");
    }();
    return path;
#else
    return ".";
#endif
}

std::string SystemMac::getCachePath() {
    // An app bundle gets its folder in ~/Library/Caches; a bare executable has no
    // identity to name one after and keeps its caches with its data.
    NSString* bundleId = [[NSBundle mainBundle] bundleIdentifier];
    NSArray* paths = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    if (!bundleId || [paths count] == 0) return getUserDataPath();

    NSString* dir = [[paths objectAtIndex:0] stringByAppendingPathComponent:bundleId];
    return [dir UTF8String];
}

std::string SystemMac::getLuaPath() {
    std::string bundlePath;
    if (bundleResourcePath(@"lua", bundlePath)) return bundlePath;

#ifdef JASPERENGINE_LUA_PATH
    return JASPERENGINE_LUA_PATH;
#else
    return "lua";
#endif
}

std::string SystemMac::getShaderPath() {
    std::string bundlePath;
    if (bundleResourcePath(@"shaders", bundlePath)) return bundlePath;

#ifdef JASPERENGINE_SHADER_PATH
    return JASPERENGINE_SHADER_PATH;
#else
    return System::getShaderPath();
#endif
}
