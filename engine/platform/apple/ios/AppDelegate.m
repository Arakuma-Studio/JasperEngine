// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#import "AppDelegate.h"
#import "JasperNative.h"

@implementation AppDelegate


- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    return YES;
}

// native plugins' lifecycle observers (JasperNative.h); the scene delegate forwards URLs
- (void)application:(UIApplication *)application didRegisterForRemoteNotificationsWithDeviceToken:(NSData *)deviceToken {
    [JasperNative dispatchRemoteNotificationToken:deviceToken];
}

// Every orientation: the view controller narrows it to Screen.orientation / autorotateTo*, and
// Info.plist (Player > Resolution and Presentation) only sets the launch orientation
- (UIInterfaceOrientationMask)application:(UIApplication *)application supportedInterfaceOrientationsForWindow:(UIWindow *)window {
    return UIInterfaceOrientationMaskAll;
}

@end
