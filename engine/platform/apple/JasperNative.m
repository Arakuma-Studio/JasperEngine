// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Objective-C helper of the native bridge (JasperNative.h). The engine's side, which
// calls plugin classes and converts values, is NativePlatformApple.mm.

#import "JasperNativeInternal.h"

#import <objc/message.h>
#import <objc/runtime.h>

// ---------------------------------------------------------------- JasperNativeCall

@implementation JasperNativeCall {
    NSString* _plugin;
    NSString* _method;
    BOOL _done;
    BOOL _cancelled;
}

- (instancetype)initWithId:(uint64_t)callId plugin:(NSString*)plugin method:(NSString*)method args:(NSArray*)args {
    if ((self = [super init])) {
        _callId = callId;
        _plugin = [plugin copy];
        _method = [method copy];
        _args = [args copy];
    }
    return self;
}

- (BOOL)isCancelled {
    @synchronized (self) {
        return _cancelled;
    }
}

- (void)markCancelled {
    @synchronized (self) {
        _cancelled = YES;
    }
}

// YES the first time (the caller completes the call), NO after
- (BOOL)takeCompletion {
    @synchronized (self) {
        if (_done) return NO;
        _done = YES;
        return YES;
    }
}

- (void)resolve:(id)value {
    if (![self takeCompletion]) {
        NSLog(@"JasperNative: %@.%@ (call %llu) was completed twice; the second result is ignored", _plugin, _method, _callId);
        return;
    }
    jasperNativeBridgeComplete(_callId, value, nil, nil);
}

- (void)reject:(NSString*)code message:(NSString*)message {
    if (![self takeCompletion]) {
        NSLog(@"JasperNative: %@.%@ (call %llu) was completed twice; the second result is ignored", _plugin, _method, _callId);
        return;
    }
    jasperNativeBridgeComplete(_callId, nil, code.length ? code : @"exception", message ?: @"");
}

- (void)failWithCode:(NSString*)code message:(NSString*)message {
    if ([self takeCompletion]) jasperNativeBridgeComplete(_callId, nil, code, message);
}

@end

// ---------------------------------------------------------------- JasperNative

@implementation JasperNative

static NSMutableArray<id<JasperLifecycleObserver>>* observers(void) {
    static NSMutableArray* list = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ list = [NSMutableArray array]; });
    return list;
}

// a copy to call without the lock (an observer may add or remove observers)
static NSArray<id<JasperLifecycleObserver>>* observersNow(void) {
    NSMutableArray* list = observers();
    @synchronized (list) {
        return [list copy];
    }
}

static void notify(SEL selector) {
    for (id<JasperLifecycleObserver> observer in observersNow()) {
        if ([observer respondsToSelector:selector]) {
            ((void (*)(id, SEL))objc_msgSend)(observer, selector);
        }
    }
}

#if TARGET_OS_IPHONE

+ (UIWindow*)window {
    // the game's scene: the foreground active window scene first
    UIWindow* fallback = nil;
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene* windowScene = (UIWindowScene*)scene;
        UIWindow* key = windowScene.keyWindow ?: windowScene.windows.firstObject;
        if (!key) continue;
        if (scene.activationState == UISceneActivationStateForegroundActive) return key;
        if (!fallback) fallback = key;
    }
    return fallback;
}

+ (UIViewController*)viewController {
    UIViewController* controller = self.window.rootViewController;
    while (controller.presentedViewController && !controller.presentedViewController.isBeingDismissed) {
        controller = controller.presentedViewController;
    }
    return controller;
}

+ (UIView*)view {
    return self.window.rootViewController.view;
}

#else

+ (NSWindow*)window {
    NSApplication* app = NSApplication.sharedApplication;
    return app.mainWindow ?: (app.keyWindow ?: app.windows.firstObject);
}

+ (NSViewController*)viewController {
    return self.window.contentViewController;
}

+ (NSView*)view {
    return self.window.contentViewController.view ?: self.window.contentView;
}

#endif

+ (void)emit:(NSString*)plugin event:(NSString*)event payload:(id)payload {
    jasperNativeBridgeEmit(plugin, event, payload);
}

+ (void)runOnMainThread:(dispatch_block_t)block {
    if (!block) return;
    if (NSThread.isMainThread) block();
    else dispatch_async(dispatch_get_main_queue(), block);
}

+ (void)runOnGameThread:(dispatch_block_t)block {
    if (block) jasperNativeBridgePost(block);
}

+ (void)pauseEngine {
    jasperNativeBridgePause(YES);
}

+ (void)resumeEngine {
    jasperNativeBridgePause(NO);
}

+ (void)addLifecycleObserver:(id<JasperLifecycleObserver>)observer {
    if (!observer) return;
    NSMutableArray* list = observers();
    @synchronized (list) {
        if (![list containsObject:observer]) [list addObject:observer];
    }
}

+ (void)removeLifecycleObserver:(id<JasperLifecycleObserver>)observer {
    NSMutableArray* list = observers();
    @synchronized (list) {
        [list removeObject:observer];
    }
}

// The images plugin classes can be in: the app and its embedded frameworks, not the system's
static BOOL isAppImage(const char* path) {
    if (!path) return NO;
    return strncmp(path, "/System/", 8) != 0 && strncmp(path, "/usr/", 5) != 0 && strncmp(path, "/Library/Apple/", 15) != 0 &&
           strstr(path, "/Xcode.app/") == NULL && strstr(path, ".simruntime/") == NULL;
}

// The class (or a superclass) adopts JasperNativePlugin; nothing is sent to it
BOOL jasperNativeIsPluginClass(Class cls) {
    Protocol* marker = @protocol(JasperNativePlugin);
    for (Class c = cls; c; c = class_getSuperclass(c)) {
        if (class_conformsToProtocol(c, marker)) return YES;
    }
    return NO;
}

// The class itself (not a superclass) has the class method `selector`. Read from the metaclass's
// method list: asking the class (class_getClassMethod, respondsToSelector:) could run its
// +initialize.
static BOOL hasOwnClassMethod(Class cls, SEL selector) {
    unsigned int count = 0;
    Method* methods = class_copyMethodList(object_getClass((id)cls), &count);
    BOOL found = NO;
    for (unsigned int i = 0; i < count && !found; i++) found = method_getName(methods[i]) == selector;
    free(methods);
    return found;
}

+ (void)dispatchInit {
    // the lifecycle, as the app's notifications (they come for scene-based apps too); once
    static dispatch_once_t observing;
    dispatch_once(&observing, ^{
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
#if TARGET_OS_IPHONE
        NSDictionary<NSNotificationName, NSString*>* names = @{
            UIApplicationDidBecomeActiveNotification: @"applicationDidBecomeActive",
            UIApplicationWillResignActiveNotification: @"applicationWillResignActive",
            UIApplicationDidEnterBackgroundNotification: @"applicationDidEnterBackground",
            UIApplicationWillEnterForegroundNotification: @"applicationWillEnterForeground",
            UIApplicationWillTerminateNotification: @"applicationWillTerminate",
        };
#else
        // a Mac app has no background state: hiding it is the closest
        NSDictionary<NSNotificationName, NSString*>* names = @{
            NSApplicationDidBecomeActiveNotification: @"applicationDidBecomeActive",
            NSApplicationWillResignActiveNotification: @"applicationWillResignActive",
            NSApplicationDidHideNotification: @"applicationDidEnterBackground",
            NSApplicationWillUnhideNotification: @"applicationWillEnterForeground",
            NSApplicationWillTerminateNotification: @"applicationWillTerminate",
        };
#endif
        [names enumerateKeysAndObjectsUsingBlock:^(NSNotificationName name, NSString* selectorName, BOOL* stop) {
            SEL selector = NSSelectorFromString(selectorName);
            [center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note) {
                notify(selector);
            }];
        }];
    });

    // every plugin class of the app's images with its own +jasperInit, once per class: called
    // again (engine start, the macOS editor's Play), it runs the classes loaded since
    static NSMutableSet<NSString*>* initialized = nil;
    if (!initialized) initialized = [NSMutableSet set];
    SEL init = @selector(jasperInit);
    unsigned int imageCount = 0;
    const char** images = objc_copyImageNames(&imageCount);
    for (unsigned int i = 0; i < imageCount; i++) {
        if (!isAppImage(images[i])) continue;
        unsigned int classCount = 0;
        const char** classNames = objc_copyClassNamesForImage(images[i], &classCount);
        for (unsigned int c = 0; c < classCount; c++) {
            Class cls = objc_getClass(classNames[c]);
            if (!cls || !jasperNativeIsPluginClass(cls) || !hasOwnClassMethod(cls, init)) continue;
            NSString* name = [NSString stringWithUTF8String:classNames[c]];
            if (!name || [initialized containsObject:name]) continue;
            [initialized addObject:name];
            @try {
                ((void (*)(id, SEL))objc_msgSend)(cls, init);
            } @catch (NSException* exception) {
                NSLog(@"JasperNative: +[%s jasperInit] threw %@: %@", classNames[c], exception.name, exception.reason);
            }
        }
        free(classNames);
    }
    free(images);
}

+ (BOOL)dispatchOpenURL:(NSURL*)url options:(NSDictionary*)options {
    BOOL handled = NO;
    for (id<JasperLifecycleObserver> observer in observersNow()) {
        if ([observer respondsToSelector:@selector(openURL:options:)] && [observer openURL:url options:options ?: @{}]) handled = YES;
    }
    return handled;
}

+ (void)dispatchRemoteNotificationToken:(NSData*)deviceToken {
    for (id<JasperLifecycleObserver> observer in observersNow()) {
        if ([observer respondsToSelector:@selector(didRegisterForRemoteNotificationsWithDeviceToken:)]) {
            [observer didRegisterForRemoteNotificationsWithDeviceToken:deviceToken];
        }
    }
}

@end
