// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The native bridge for plugin code on iOS and macOS (docs/mcp/guide/native-bridge.md).
//
// A plugin is an Objective-C class (Swift: `@objc(Name) class Name: NSObject`) whose class
// methods Lua calls by name:
//   Native.call("Device", "getCode", ...)       -> + (id)getCode:(NSArray*)args   (or + (id)getCode with no args)
//   Native.callAsync("Device", "login", {...})  -> + (void)login:(NSArray*)args call:(JasperNativeCall*)call
// Arguments and results are JSON values as Foundation objects: NSNull, NSNumber (booleans are
// @YES / @NO), NSString, NSArray, NSDictionary with string keys. A class may implement
// + (void)jasperInit: it runs once, on the main thread, when the game's view has loaded (SDK
// initialization). Swift files see this header through the export's bridging header.

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif

NS_ASSUME_NONNULL_BEGIN

// One Native.callAsync from Lua. Complete it once, from any thread, at any time: the first
// resolve/reject wins, later ones are ignored with a warning. The result reaches the Lua
// callback in the next frame.
@interface JasperNativeCall : NSObject
// the Lua arguments
@property (nonatomic, readonly) NSArray* args;
@property (nonatomic, readonly) uint64_t callId;
// the Lua side no longer waits (cancelled, its owner went, timed out, the game stopped)
@property (atomic, readonly) BOOL isCancelled;
- (instancetype)init NS_UNAVAILABLE;
// a JSON value as Foundation objects (nil: null)
- (void)resolve:(nullable id)value;
// code: "exception", "cancelled", "not_ready" or your own ("user_cancelled", ...)
- (void)reject:(NSString*)code message:(nullable NSString*)message;
@end

// The app's lifecycle for plugin code (an SDK that needs it); every method is optional and
// called on the main thread
@protocol JasperLifecycleObserver <NSObject>
@optional
- (void)applicationDidBecomeActive;
- (void)applicationWillResignActive;
- (void)applicationDidEnterBackground;
- (void)applicationWillEnterForeground;
- (void)applicationWillTerminate;
// YES: handled (the other observers still get it)
- (BOOL)openURL:(NSURL*)url options:(NSDictionary*)options;
- (void)didRegisterForRemoteNotificationsWithDeviceToken:(NSData*)deviceToken;
@end

@interface JasperNative : NSObject

#if TARGET_OS_IPHONE
// the topmost presented view controller of the game's window (present SDK screens on it)
@property (class, nonatomic, readonly, nullable) UIViewController* viewController;
@property (class, nonatomic, readonly, nullable) UIWindow* window;
// the game's view (add a banner view to it)
@property (class, nonatomic, readonly, nullable) UIView* view;
#else
@property (class, nonatomic, readonly, nullable) NSViewController* viewController;
@property (class, nonatomic, readonly, nullable) NSWindow* window;
@property (class, nonatomic, readonly, nullable) NSView* view;
#endif

// Fires the Lua handlers of Native.on(plugin, event, ...) in the next frame (any thread)
+ (void)emit:(NSString*)plugin event:(NSString*)event payload:(nullable id)payload;
// Now when called on the main thread, else dispatched there
+ (void)runOnMainThread:(dispatch_block_t)block;
// At the start of the next frame's update, where Lua runs (the place to touch engine state)
+ (void)runOnGameThread:(dispatch_block_t)block;
// While a full-screen SDK view covers the game (an ad): the engine pauses at the next frame
// (audio paused, Engine.onPause). Counted: as many resumeEngine as pauseEngine.
+ (void)pauseEngine;
+ (void)resumeEngine;

+ (void)addLifecycleObserver:(id<JasperLifecycleObserver>)observer;
+ (void)removeLifecycleObserver:(id<JasperLifecycleObserver>)observer;

// ---- the app (ViewController, SceneDelegate, AppDelegate)

// ViewController.viewDidLoad, before the engine starts: every plugin class's +jasperInit
// (once) and the lifecycle notifications
+ (void)dispatchInit;
// YES when an observer handled it
+ (BOOL)dispatchOpenURL:(NSURL*)url options:(NSDictionary*)options;
+ (void)dispatchRemoteNotificationToken:(NSData*)deviceToken;

@end

NS_ASSUME_NONNULL_END
