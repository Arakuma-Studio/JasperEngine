// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "AppDelegate.h"
#import "JasperNative.h"

@implementation AppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return YES;
}

// native plugins' lifecycle observers (JasperNative.h)
- (void)application:(NSApplication *)application openURLs:(NSArray<NSURL *> *)urls {
    for (NSURL *url in urls) [JasperNative dispatchOpenURL:url options:@{}];
}

- (void)application:(NSApplication *)application didRegisterForRemoteNotificationsWithDeviceToken:(NSData *)deviceToken {
    [JasperNative dispatchRemoteNotificationToken:deviceToken];
}


@end
