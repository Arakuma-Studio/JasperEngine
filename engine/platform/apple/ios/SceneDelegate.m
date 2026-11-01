// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "SceneDelegate.h"
#import "JasperNative.h"

// One for the app's life, so a reconnected scene doesn't start another engine
static UIViewController *engineViewController(void) {
    static UIViewController *controller = nil;
    if (!controller) {
        controller = [[UIStoryboard storyboardWithName:@"Main" bundle:nil] instantiateInitialViewController];
    }
    return controller;
}

@implementation SceneDelegate

- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    if (![scene isKindOfClass:UIWindowScene.class]) return;

    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = engineViewController();
    [self.window makeKeyAndVisible];

    // a URL that launched the app reaches native plugins' observers (JasperNative.h)
    [self scene:scene openURLContexts:connectionOptions.URLContexts];
}

- (void)scene:(UIScene *)scene openURLContexts:(NSSet<UIOpenURLContext *> *)URLContexts {
    for (UIOpenURLContext *context in URLContexts) {
        NSMutableDictionary *options = [NSMutableDictionary dictionary];
        if (context.options.sourceApplication) options[UIApplicationOpenURLOptionsSourceApplicationKey] = context.options.sourceApplication;
        if (context.options.annotation) options[UIApplicationOpenURLOptionsAnnotationKey] = context.options.annotation;
        options[UIApplicationOpenURLOptionsOpenInPlaceKey] = @(context.options.openInPlace);
        [JasperNative dispatchOpenURL:context.URL options:options];
    }
}

- (void)sceneDidDisconnect:(UIScene *)scene {
    // frees the view controller for the window of the next connection
    self.window.rootViewController = nil;
    self.window = nil;
}

@end
